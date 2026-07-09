/*
 * Orion dial — app wiring.
 *
 * Structure:
 *   dial_display  LCD/touch/LVGL bring-up + the LVGL task and lock
 *   dial_state    snapshot store + UI->worker command queue + input stamp
 *   dial_ui       screen router (LVGL task) + screens
 *   worker_task   (here) the single network task: Wi-Fi -> OAuth -> MCP ->
 *                 command/poll loop, as a supervisor state machine. Every
 *                 failure is a phase + backoff, never a dead end.
 *
 * Threading: the worker never touches LVGL; it commits to dial_state and the
 * router's dispatcher renders. Knob callbacks (esp_timer task) only feed the
 * router's atomic accumulator. LVGL event callbacks already hold the LVGL
 * mutex and must not re-lock it.
 */
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "dial_display.h"
#include "dial_state.h"
#include "ui_router.h"
#include "ui_screens.h"
#include "dial_wifi.h"
#include "dial_oauth.h"
#include "dial_mcp.h"
#include "dial_time.h"
#include "bidi_switch_knob.h"
#include "secrets.h"
#include "cJSON.h"

static const char *TAG = "app";

// Device resync is gated on a quiet period: every user input stamps the store,
// and the poll only reads the bed back once there's been no input for a while
// (so an update can never land mid-interaction).
#define KNOB_SETTLE_US    2500000    // 2.5s of no input before the bed is read back
#define POLL_INTERVAL_US 10000000    // and at most every ~10s when idle

#define BACKOFF_MIN_S  5
#define BACKOFF_MAX_S 60

static const char *zone_id_str(zone_idx_t z) { return z == ZONE_A ? "zone_a" : "zone_b"; }

/* ---- rotary knob ------------------------------------------------------ */
// GPIO8 (A) / GPIO7 (B); only electrically live under the full board init.
// Callbacks run in the decoder's esp_timer task: feed the router's atomic
// accumulator and nothing else (blocking here stalls lv_tick_inc).
#define KNOB_A 8
#define KNOB_B 7
static knob_handle_t s_knob;

static void knob_left_cb(void *arg, void *data)  { (void)arg; (void)data; ui_router_knob_input(-1); }
static void knob_right_cb(void *arg, void *data) { (void)arg; (void)data; ui_router_knob_input(+1); }

static void knob_init(void)
{
    knob_config_t cfg = { .gpio_encoder_a = KNOB_A, .gpio_encoder_b = KNOB_B };
    s_knob = iot_knob_create(&cfg);
    if (!s_knob) { ESP_LOGE(TAG, "knob create failed"); return; }
    iot_knob_register_cb(s_knob, KNOB_LEFT, knob_left_cb, NULL);
    iot_knob_register_cb(s_knob, KNOB_RIGHT, knob_right_cb, NULL);
    ESP_LOGI(TAG, "knob ready on GPIO%d/%d", KNOB_A, KNOB_B);
}

/* ---- navigation policy (runs in the LVGL task) ------------------------ */

static screen_id_t nav_policy(const app_state_t *st, void **arg)
{
    switch (st->phase) {
    case PH_WIFI_PORTAL:        return SCR_WIFI_PORTAL;
    case PH_OAUTH_WAIT_CONSENT: return SCR_OAUTH_QR;
    case PH_READY:
    case PH_DEGRADED:
    case PH_WIFI_LOST:
        // Once we have device state, stay on the dial (with its staleness dot)
        // through transient outages rather than yanking the user to a status
        // screen mid-interaction.
        if (st->have_state) {
            *arg = (void *)(uintptr_t)st->ui_zone;
            return SCR_DIAL;
        }
        return st->phase == PH_READY ? SCR_CONNECTING : SCR_ERROR;
    default:                    return SCR_CONNECTING;
    }
}

/* ---- store mutators (file-scope: no GCC nested-fn trampolines) --------- */

typedef struct {
    zone_state_t zones[ZONE_COUNT];
    bool online;
    bool safety_error;
    char safety_desc[96];
    char water_fill[12];
} device_snapshot_t;

static void mut_device_state(app_state_t *st, void *arg)
{
    device_snapshot_t *d = arg;
    for (int z = 0; z < ZONE_COUNT; z++) {
        // Names come from list_devices, not the state poll — keep them.
        char keep[sizeof(st->zones[z].user_name)];
        memcpy(keep, st->zones[z].user_name, sizeof(keep));
        st->zones[z] = d->zones[z];
        memcpy(st->zones[z].user_name, keep, sizeof(keep));
        st->ui_temp_f[z] = -1;              // device truth is the new baseline
    }
    st->device_online = d->online;
    st->safety.error = d->safety_error;
    strlcpy(st->safety.desc, d->safety_desc, sizeof(st->safety.desc));
    strlcpy(st->water_fill, d->water_fill, sizeof(st->water_fill));
    st->have_state = true;
}

typedef struct { char names[ZONE_COUNT][24]; char serial[16]; } device_identity_t;

static void mut_identity(app_state_t *st, void *arg)
{
    device_identity_t *n = arg;
    for (int z = 0; z < ZONE_COUNT; z++)
        strlcpy(st->zones[z].user_name, n->names[z], sizeof(st->zones[z].user_name));
    strlcpy(st->serial, n->serial, sizeof(st->serial));
}

typedef struct { int zone; bool on; } zone_on_t;
static void mut_zone_on(app_state_t *st, void *arg)
{
    zone_on_t *u = arg;
    st->zones[u->zone].on = u->on;                  // optimistic
}

typedef struct { int zone; float temp_c; } zone_temp_t;
static void mut_zone_temp(app_state_t *st, void *arg)
{
    zone_temp_t *u = arg;
    st->zones[u->zone].temp_c = u->temp_c;          // optimistic
    st->ui_temp_f[u->zone] = -1;
}

static void mut_oauth_url(app_state_t *st, void *arg) { strlcpy(st->oauth_url, arg, sizeof(st->oauth_url)); }
static void mut_retry_in(app_state_t *st, void *arg)  { st->retry_in_s = *(int *)arg; }
static void mut_ap_ssid(app_state_t *st, void *arg)   { strlcpy(st->ap_ssid, arg, sizeof(st->ap_ssid)); }

/* ---- Orion MCP calls (worker task only) -------------------------------- */

static char s_serial[16];

static zone_idx_t zone_idx_from_id(const char *id)
{
    return (id && strcmp(id, "zone_b") == 0) ? ZONE_B : ZONE_A;
}

static bool orion_refresh_state(void)
{
    char args[48];
    snprintf(args, sizeof(args), "{\"serial\":\"%s\"}", s_serial);
    char *json = NULL;
    if (!dial_mcp_call_tool("get_device_state", args, &json) || !json) return false;
    cJSON *root = cJSON_Parse(json);
    free(json);
    if (!root) return false;

    device_snapshot_t d = { 0 };
    for (int z = 0; z < ZONE_COUNT; z++) d.zones[z].actual_c = -1.0f;
    strlcpy(d.water_fill, "unknown", sizeof(d.water_fill));

    cJSON *z;
    cJSON_ArrayForEach(z, cJSON_GetObjectItem(root, "zones")) {
        cJSON *id = cJSON_GetObjectItem(z, "id");
        if (!id || !id->valuestring) continue;
        zone_state_t *zs = &d.zones[zone_idx_from_id(id->valuestring)];
        cJSON *t = cJSON_GetObjectItem(z, "temp");
        if (cJSON_IsNumber(t)) zs->temp_c = (float)t->valuedouble;
        zs->on = cJSON_IsTrue(cJSON_GetObjectItem(z, "on"));
    }

    cJSON *status = cJSON_GetObjectItem(root, "status");
    if (status) {
        d.online = cJSON_IsTrue(cJSON_GetObjectItem(status, "online"));
        cJSON_ArrayForEach(z, cJSON_GetObjectItem(status, "zones")) {
            cJSON *id = cJSON_GetObjectItem(z, "id");
            if (!id || !id->valuestring) continue;
            zone_state_t *zs = &d.zones[zone_idx_from_id(id->valuestring)];
            cJSON *t = cJSON_GetObjectItem(z, "temp");
            if (cJSON_IsNumber(t)) zs->actual_c = (float)t->valuedouble;
            cJSON *ts = cJSON_GetObjectItem(z, "thermal_state");
            if (ts && ts->valuestring)
                strlcpy(zs->thermal_state, ts->valuestring, sizeof(zs->thermal_state));
        }
        cJSON *safety = cJSON_GetObjectItem(status, "safety");
        if (safety) {
            d.safety_error = cJSON_IsTrue(cJSON_GetObjectItem(safety, "error"));
            cJSON *descs = cJSON_GetObjectItem(safety, "error_descriptions");
            cJSON *first = cJSON_IsArray(descs) ? cJSON_GetArrayItem(descs, 0) : NULL;
            if (first && first->valuestring)
                strlcpy(d.safety_desc, first->valuestring, sizeof(d.safety_desc));
        }
    }
    cJSON *wf = cJSON_GetObjectItem(root, "water_fill");
    if (wf && wf->valuestring) strlcpy(d.water_fill, wf->valuestring, sizeof(d.water_fill));
    cJSON_Delete(root);

    dial_state_commit(mut_device_state, &d);
    return true;
}

static bool orion_set_zone(zone_idx_t zone, const char *field_json)
{
    char args[96];
    snprintf(args, sizeof(args), "{\"serial\":\"%s\",\"zone_id\":\"%s\",%s}",
             s_serial, zone_id_str(zone), field_json);
    char *r = NULL;
    bool ok = dial_mcp_call_tool("set_zone", args, &r);
    if (!ok) ESP_LOGW(TAG, "set_zone %s failed: %s", field_json, dial_mcp_last_error());
    free(r);
    return ok;
}

static bool orion_discover_device(void)
{
    char *devices = NULL;
    if (!dial_mcp_call_tool("list_devices", "{}", &devices) || !devices) return false;

    cJSON *root = cJSON_Parse(devices);
    free(devices);
    if (!root) return false;

    bool ok = false;
    cJSON *arr  = cJSON_GetObjectItem(root, "devices");
    cJSON *dev0 = cJSON_IsArray(arr) ? cJSON_GetArrayItem(arr, 0) : NULL;
    if (dev0) {
        cJSON *serial = cJSON_GetObjectItem(dev0, "serial_number");
        if (serial && serial->valuestring) {
            strlcpy(s_serial, serial->valuestring, sizeof(s_serial));
            ok = true;
        }
        cJSON *tz = cJSON_GetObjectItem(dev0, "timezone");
        if (tz && tz->valuestring)
            dial_time_set_iana_tz(tz->valuestring);

        device_identity_t ident = { 0 };
        strlcpy(ident.serial, s_serial, sizeof(ident.serial));
        cJSON *zn;
        cJSON_ArrayForEach(zn, cJSON_GetObjectItem(dev0, "zones")) {
            cJSON *id = cJSON_GetObjectItem(zn, "id");
            cJSON *user = cJSON_GetObjectItem(zn, "user");
            cJSON *fn = user ? cJSON_GetObjectItem(user, "first_name") : NULL;
            if (id && id->valuestring && fn && fn->valuestring)
                strlcpy(ident.names[zone_idx_from_id(id->valuestring)],
                        fn->valuestring, sizeof(ident.names[0]));
        }
        if (ok) dial_state_commit(mut_identity, &ident);
    }
    cJSON_Delete(root);
    return ok;
}

/* ---- worker supervisor ------------------------------------------------- */

// Sleep `seconds` while publishing a countdown for the error screen.
static void backoff_wait(int seconds)
{
    for (int s = seconds; s > 0; s--) {
        dial_state_commit(mut_retry_in, &s);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    int zero = 0;
    dial_state_commit(mut_retry_in, &zero);
}

// 401-aware call wrapper: on failure, refresh the token, reopen the MCP
// session, retry once. Used for polls AND writes so an expired token never
// silently drops a command.
static bool with_auth_retry(bool (*call)(void), const oauth_disc_t *disc, const char *client_id)
{
    if (call()) return true;
    if (dial_oauth_refresh(disc, client_id)) {
        dial_mcp_connect(NULL);
        return call();
    }
    return false;
}

static bool poll_call(void) { return orion_refresh_state(); }

static void worker_task(void *arg)
{
    (void)arg;
    oauth_disc_t disc;
    char client_id[96];
    int backoff_s = BACKOFF_MIN_S;

    // ---- Wi-Fi (blocking bringup; portal phase published via events) ----
    dial_state_set_phase(PH_WIFI_CONNECTING, NULL);
    dial_net_bringup();
    dial_time_start();

    // ---- OAuth + MCP with retry/backoff on every step ----
    for (;;) {
        dial_state_set_phase(PH_OAUTH_DISCOVER, NULL);

        char ip[16], redirect_uri[48];
        if (!dial_net_ip(ip, sizeof(ip))) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        snprintf(redirect_uri, sizeof(redirect_uri), "http://%s/callback", ip);

        if (!dial_oauth_discover(&disc) ||
            !dial_oauth_ensure_client(&disc, redirect_uri, client_id, sizeof(client_id))) {
            dial_state_set_phase(PH_DEGRADED, "Orion unreachable");
            backoff_wait(backoff_s);
            if (backoff_s < BACKOFF_MAX_S) backoff_s *= 2;
            continue;
        }

        if (!dial_oauth_have_valid_access() && !dial_oauth_refresh(&disc, client_id)) {
            // Interactive consent: QR on screen; on timeout, a fresh QR — no dead end.
            char url[600];
            if (!dial_oauth_start_authorize(&disc, client_id, redirect_uri, url, sizeof(url))) {
                dial_state_set_phase(PH_DEGRADED, dial_oauth_last_error());
                backoff_wait(backoff_s);
                continue;
            }
            dial_state_commit(mut_oauth_url, url);
            dial_state_set_phase(PH_OAUTH_WAIT_CONSENT, NULL);
            bool ok = dial_oauth_finish_authorize(&disc, client_id, redirect_uri, 300000);
            dial_oauth_stop_authorize();
            if (!ok) {
                ESP_LOGW(TAG, "consent window elapsed (%s) — restarting authorize",
                         dial_oauth_last_error());
                continue;
            }
        }

        dial_state_set_phase(PH_MCP_CONNECTING, NULL);
        if (!dial_mcp_connect(NULL) || !orion_discover_device()) {
            dial_state_set_phase(PH_DEGRADED, dial_mcp_last_error());
            backoff_wait(backoff_s);
            if (backoff_s < BACKOFF_MAX_S) backoff_s *= 2;
            continue;
        }
        backoff_s = BACKOFF_MIN_S;
        break;
    }
    ESP_LOGI(TAG, "device linked; %d tools", dial_mcp_list_tools_count());

    with_auth_retry(poll_call, &disc, client_id);
    dial_state_set_phase(PH_READY, NULL);
    knob_init();   // safe now: full board init done, router running

    // ---- steady state: drain commands (coalescing per zone), gated poll ----
    int64_t last_poll_us = esp_timer_get_time();
    int poll_failures = 0;
    for (;;) {
        app_cmd_t cmd;
        if (dial_cmd_receive(&cmd, 300)) {
            // Coalesce a burst: per zone, at most one net toggle + final temp.
            int last_temp[ZONE_COUNT] = { -1, -1 };
            int toggles[ZONE_COUNT]   = { 0, 0 };
            do {
                if (cmd.kind == CMD_SET_TEMP) last_temp[cmd.zone] = cmd.temp_f;
                else                          toggles[cmd.zone]++;
            } while (dial_cmd_receive(&cmd, 0));

            for (int z = 0; z < ZONE_COUNT; z++) {
                if (toggles[z] & 1) {
                    app_state_t st;
                    dial_state_get(&st);
                    zone_on_t up = { z, !st.zones[z].on };
                    char f[24];
                    snprintf(f, sizeof(f), "\"on\":%s", up.on ? "true" : "false");
                    if (orion_set_zone((zone_idx_t)z, f))
                        dial_state_commit(mut_zone_on, &up);
                }
                if (last_temp[z] >= 0) {
                    zone_temp_t up = { z, dial_f_to_c(last_temp[z]) };
                    char f[24];
                    snprintf(f, sizeof(f), "\"temp\":%.1f", up.temp_c);
                    if (orion_set_zone((zone_idx_t)z, f))
                        dial_state_commit(mut_zone_temp, &up);
                }
            }
            // Push the poll's T0 out so the resync waits for quiet after this.
            last_poll_us = esp_timer_get_time();
            continue;
        }

        // No command this tick. Resync only when quiet AND due.
        int64_t now = esp_timer_get_time();
        if (now - dial_state_last_input_us() < KNOB_SETTLE_US) continue;
        if (now - last_poll_us < POLL_INTERVAL_US) continue;

        if (!dial_wifi_is_connected()) {
            // dial_net auto-reconnects; reflect the outage and wait it out.
            dial_state_set_phase(PH_WIFI_LOST, NULL);
            vTaskDelay(pdMS_TO_TICKS(1000));
            last_poll_us = esp_timer_get_time();
            continue;
        }

        if (with_auth_retry(poll_call, &disc, client_id)) {
            poll_failures = 0;
            dial_state_set_phase(PH_READY, NULL);
        } else if (++poll_failures >= 3) {
            dial_state_set_phase(PH_DEGRADED, dial_mcp_last_error());
        }
        last_poll_us = esp_timer_get_time();
    }
}

/* ---- app entry --------------------------------------------------------- */

static void net_event_cb(dial_net_event_t ev)
{
    // Runs on the Wi-Fi event task: only phase bookkeeping, nothing blocking.
    app_state_t st;
    switch (ev) {
    case DIAL_NET_EV_PORTAL:
        dial_state_set_phase(PH_WIFI_PORTAL, NULL);
        break;
    case DIAL_NET_EV_LOST:
        dial_state_get(&st);
        if (st.phase == PH_READY || st.phase == PH_DEGRADED)
            dial_state_set_phase(PH_WIFI_LOST, NULL);
        break;
    case DIAL_NET_EV_GOT_IP:
        dial_state_get(&st);
        if (st.phase == PH_WIFI_LOST)
            dial_state_set_phase(st.have_state ? PH_READY : PH_WIFI_CONNECTING, NULL);
        break;
    default:
        break;
    }
}

void app_main(void)
{
    dial_display_start();
    dial_state_init();

    // Router + screens live in the LVGL task from here on. ui_router_start
    // needs the LVGL lock because the LVGL task is already running.
    ui_screens_register_all();
    ui_router_set_nav_policy(nav_policy);
    if (dial_display_lock(-1)) {
        ui_router_start(SCR_CONNECTING, NULL);
        dial_display_unlock();
    }

    dial_net_on_event(net_event_cb);
    dial_net_init();
    dial_state_restore_prefs();   // last shown side (needs NVS, hence after net init)
    dial_net_seed(WIFI_SSID, WIFI_PASSWORD);
    dial_state_commit(mut_ap_ssid, (void *)dial_net_ap_ssid());

    // OAuth/TLS/MCP need a big stack; everything network runs on this task.
    xTaskCreate(worker_task, "worker", 16384, NULL, 4, NULL);
}
