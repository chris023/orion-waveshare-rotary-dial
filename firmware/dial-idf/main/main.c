#include <stdio.h>

#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_timer.h"
#include "esp_log.h"

#include "lvgl.h"
#include "dial_display.h"
#include "dial_wifi.h"
#include "dial_oauth.h"
#include "dial_mcp.h"
#include "bidi_switch_knob.h"
#include "secrets.h"
#include "cJSON.h"
static const char *TAG = "example";

// Full-screen setup screen: instruction label + a QR of the authorize URL.
static void ui_show_qr(const char *title, const char *url)
{
    if (!dial_display_lock(-1)) return;
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);

    lv_obj_t *label = lv_label_create(scr);
    lv_label_set_text(label, title);
    lv_obj_set_style_text_color(label, lv_color_hex(0x0b6a4a), 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, 44);

    lv_obj_t *qr = lv_qrcode_create(scr, 220, lv_color_black(), lv_color_white());
    lv_qrcode_update(qr, url, strlen(url));
    lv_obj_center(qr);
    dial_display_unlock();
}

// Centered status message.
static void ui_show_msg(const char *msg)
{
    if (!dial_display_lock(-1)) return;
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_t *label = lv_label_create(scr);
    lv_obj_set_width(label, 300);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(label, msg);
    lv_obj_set_style_text_color(label, lv_color_hex(0x0b6a4a), 0);
    lv_obj_center(label);
    dial_display_unlock();
}

// OAuth: get a usable access token (stored, refreshed, or via on-screen consent).
/* ========================= Orion control ============================== *
 * The rotary encoder lives on the companion MCU, so for now this dial is
 * driven by the working touchscreen: an LVGL arc you drag to set the target
 * temperature and a button to toggle the zone on/off. Touch events post
 * commands to a worker (the oauth_task loop) that owns the MCP session; the
 * worker polls get_device_state and pushes the truth back to the UI.
 */

#define ORION_ZONE  "zone_a"      // this dial controls zone_a (owner side)
#define TEMP_MIN_F  55
#define TEMP_MAX_F  110

typedef enum { CMD_SET_TEMP, CMD_TOGGLE_ON } orion_cmd_kind_t;
typedef struct { orion_cmd_kind_t kind; int temp_f; } orion_cmd_t;

static QueueHandle_t     s_cmd_q;
static SemaphoreHandle_t s_state_mux;
static char  s_serial[16];
static float s_temp_c;       // last known device temp for our zone (°C)
static bool  s_on;
static bool  s_online;
static bool  s_have_state;

static lv_obj_t *s_arc, *s_temp_lbl, *s_state_lbl;

// Rotary encoder on GPIO8 (A) / GPIO7 (B). The encoder is only electrically
// live under the full board init (display+touch up) — bare-GPIO probes read it
// dead. Decoded by Waveshare's bidi_switch_knob (iot_knob), the same decoder the
// stock firmware uses for this exact detented part. Each detent -> a left/right
// event -> a 1°F step on the setpoint.
#define KNOB_A 8
#define KNOB_B 7
static knob_handle_t s_knob;
static volatile int  s_ui_temp_f = -1;   // current shown setpoint (°F); -1 = unknown

// Device resync is gated on a quiet period: every user input stamps this, and
// the poll only reads the bed back once there's been no input for a while (so an
// update can never land mid-interaction). The poll interval is measured from T0
// = the last input, not wall-clock.
static volatile int64_t s_last_input_us;
#define KNOB_SETTLE_US    2500000    // 2.5s of no input before the bed is read back
#define POLL_INTERVAL_US 10000000    // and at most every ~10s when idle

// Orion works in °C; this US user's app shows °F. The device's own conversion
// table is the linear formula rounded to 0.1°C, so linear round-trips exactly.
static inline int   c_to_f(float c) { return (int)lroundf(c * 1.8f + 32.0f); }
static inline float f_to_c(int f)   { return roundf(((f - 32) / 1.8f) * 10.0f) / 10.0f; }

// ---- UI (all callers below hold the LVGL mux where noted) ----

// Update the center readout. Caller must hold the LVGL mux.
static void ui_set_center(int temp_f, bool on, bool online)
{
    char t[8];
    snprintf(t, sizeof(t), "%d", temp_f);
    lv_label_set_text(s_temp_lbl, t);
    lv_label_set_text(s_state_lbl, !online ? "OFFLINE" : (on ? "ON" : "OFF"));
    lv_color_t c = !online ? lv_color_hex(0x666666)
                           : (on ? lv_color_hex(0xff7043) : lv_color_hex(0x5b8def));
    lv_obj_set_style_text_color(s_temp_lbl, c, 0);
    lv_obj_set_style_arc_color(s_arc, c, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(s_arc, on ? LV_OPA_COVER : LV_OPA_40, LV_PART_INDICATOR);
}

// LVGL event callbacks run inside lv_timer_handler, which already holds the
// LVGL mux — so they must NOT re-lock. They only touch LVGL + post to the queue.
static void arc_event_cb(lv_event_t *e)
{
    s_last_input_us = esp_timer_get_time();
    int f = lv_arc_get_value(s_arc);
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        char t[8];
        snprintf(t, sizeof(t), "%d", f);
        lv_label_set_text(s_temp_lbl, t);          // live feedback while dragging
    } else {                                        // LV_EVENT_RELEASED
        orion_cmd_t cmd = { .kind = CMD_SET_TEMP, .temp_f = f };
        xQueueSend(s_cmd_q, &cmd, 0);
    }
}

static void power_event_cb(lv_event_t *e)
{
    s_last_input_us = esp_timer_get_time();
    orion_cmd_t cmd = { .kind = CMD_TOGGLE_ON };
    xQueueSend(s_cmd_q, &cmd, 0);
}

static void ui_build_dial(void)
{
    if (!dial_display_lock(-1)) return;
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    s_arc = lv_arc_create(scr);
    lv_obj_set_size(s_arc, 320, 320);
    lv_obj_center(s_arc);
    lv_arc_set_rotation(s_arc, 135);
    lv_arc_set_bg_angles(s_arc, 0, 270);
    lv_arc_set_range(s_arc, TEMP_MIN_F, TEMP_MAX_F);
    lv_arc_set_value(s_arc, (TEMP_MIN_F + TEMP_MAX_F) / 2);
    lv_obj_set_style_arc_width(s_arc, 16, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 16, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(0x2a2a2a), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_arc, lv_color_hex(0xffffff), LV_PART_KNOB);
    lv_obj_add_event_cb(s_arc, arc_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_arc, arc_event_cb, LV_EVENT_RELEASED, NULL);

    lv_obj_t *zone = lv_label_create(scr);
    lv_obj_set_style_text_font(zone, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(zone, lv_color_hex(0x888888), 0);
    lv_label_set_text(zone, "MY SIDE");
    lv_obj_align(zone, LV_ALIGN_CENTER, 0, -66);

    s_temp_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_temp_lbl, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_temp_lbl, lv_color_hex(0xff7043), 0);
    lv_label_set_text(s_temp_lbl, "--");
    lv_obj_align(s_temp_lbl, LV_ALIGN_CENTER, 0, -8);

    lv_obj_t *unit = lv_label_create(scr);
    lv_obj_set_style_text_font(unit, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(unit, lv_color_hex(0xaaaaaa), 0);
    lv_label_set_text(unit, "\xC2\xB0" "F");        // UTF-8 degree sign + F
    lv_obj_align_to(unit, s_temp_lbl, LV_ALIGN_OUT_RIGHT_TOP, 4, 8);

    lv_obj_t *btn = lv_btn_create(scr);
    lv_obj_set_size(btn, 104, 44);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 62);
    lv_obj_set_style_radius(btn, 22, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x1e1e1e), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x444444), 0);
    lv_obj_add_event_cb(btn, power_event_cb, LV_EVENT_CLICKED, NULL);
    s_state_lbl = lv_label_create(btn);
    lv_obj_set_style_text_font(s_state_lbl, &lv_font_montserrat_16, 0);
    lv_label_set_text(s_state_lbl, "--");
    lv_obj_center(s_state_lbl);

    dial_display_unlock();
}

// Push the shared device state into the UI (called from the worker task).
static void ui_apply_state(void)
{
    xSemaphoreTake(s_state_mux, portMAX_DELAY);
    int  f      = c_to_f(s_temp_c);
    bool on     = s_on;
    bool online = s_online;
    bool have   = s_have_state;
    if (have) s_ui_temp_f = f;     // keep the knob's baseline synced to the device
    xSemaphoreGive(s_state_mux);
    if (!have) return;
    if (!dial_display_lock(-1)) return;
    lv_arc_set_value(s_arc, f);
    ui_set_center(f, on, online);
    dial_display_unlock();
}

// ---- Rotary knob -> temperature setpoint ----
// A knob detent adjusts the setpoint by 1°F: update UI optimistically and queue
// a set_zone. Runs in the decoder's esp_timer task, so it only touches the
// queue + the two mutexes (never blocks on the network).
static void knob_apply_step(int delta_f)
{
    if (!s_cmd_q) return;
    s_last_input_us = esp_timer_get_time();
    xSemaphoreTake(s_state_mux, portMAX_DELAY);
    int cur = (s_ui_temp_f >= 0) ? s_ui_temp_f : c_to_f(s_temp_c);
    int nf  = cur + delta_f;
    if (nf < TEMP_MIN_F) nf = TEMP_MIN_F;
    if (nf > TEMP_MAX_F) nf = TEMP_MAX_F;
    s_ui_temp_f = nf;
    bool on = s_on;
    xSemaphoreGive(s_state_mux);

    if (dial_display_lock(50)) {
        lv_arc_set_value(s_arc, nf);
        ui_set_center(nf, on, true);
        dial_display_unlock();
    }
    orion_cmd_t cmd = { .kind = CMD_SET_TEMP, .temp_f = nf };
    xQueueSend(s_cmd_q, &cmd, 0);
}
static void knob_left_cb(void *arg, void *data)  { knob_apply_step(-1); }  // CCW = cooler
static void knob_right_cb(void *arg, void *data) { knob_apply_step(+1); }  // CW  = warmer
static void knob_init(void)
{
    knob_config_t cfg = { .gpio_encoder_a = KNOB_A, .gpio_encoder_b = KNOB_B };
    s_knob = iot_knob_create(&cfg);
    if (!s_knob) { ESP_LOGE(TAG, "knob create failed"); return; }
    iot_knob_register_cb(s_knob, KNOB_LEFT, knob_left_cb, NULL);
    iot_knob_register_cb(s_knob, KNOB_RIGHT, knob_right_cb, NULL);
    ESP_LOGI(TAG, "knob ready on GPIO%d/%d", KNOB_A, KNOB_B);
}

// ---- Orion MCP tool calls (run on the worker task) ----

static bool orion_refresh_state(void)
{
    char args[48];
    snprintf(args, sizeof(args), "{\"serial\":\"%s\"}", s_serial);
    char *json = NULL;
    if (!dial_mcp_call_tool("get_device_state", args, &json) || !json) return false;
    cJSON *root = cJSON_Parse(json);
    free(json);
    if (!root) return false;

    bool online = false, on = false, found = false;
    float temp = 0;
    cJSON *status = cJSON_GetObjectItem(root, "status");
    if (status) online = cJSON_IsTrue(cJSON_GetObjectItem(status, "online"));
    cJSON *z;
    cJSON_ArrayForEach(z, cJSON_GetObjectItem(root, "zones")) {
        cJSON *id = cJSON_GetObjectItem(z, "id");
        if (id && id->valuestring && strcmp(id->valuestring, ORION_ZONE) == 0) {
            cJSON *t = cJSON_GetObjectItem(z, "temp");
            if (cJSON_IsNumber(t)) temp = (float)t->valuedouble;
            on = cJSON_IsTrue(cJSON_GetObjectItem(z, "on"));
            found = true;
        }
    }
    cJSON_Delete(root);
    if (!found) return false;

    xSemaphoreTake(s_state_mux, portMAX_DELAY);
    s_temp_c = temp; s_on = on; s_online = online; s_have_state = true;
    xSemaphoreGive(s_state_mux);
    return true;
}

static bool orion_set_zone(const char *field_json)
{
    char args[96];
    snprintf(args, sizeof(args), "{\"serial\":\"%s\",\"zone_id\":\"%s\",%s}",
             s_serial, ORION_ZONE, field_json);
    char *r = NULL;
    bool ok = dial_mcp_call_tool("set_zone", args, &r);
    ESP_LOGI(TAG, "set_zone %s -> %s (%s)", field_json, ok ? "ok" : "fail",
             ok ? "" : dial_mcp_last_error());
    free(r);
    return ok;
}

static void oauth_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1000));
    char ip[16];
    if (!dial_net_ip(ip, sizeof(ip))) { vTaskDelete(NULL); return; }
    char redirect_uri[48];
    snprintf(redirect_uri, sizeof(redirect_uri), "http://%s/callback", ip);

    oauth_disc_t disc;
    char client_id[96];
    if (!dial_oauth_discover(&disc) ||
        !dial_oauth_ensure_client(&disc, redirect_uri, client_id, sizeof(client_id))) {
        ui_show_msg("Setup error:\nOrion unreachable");
        vTaskDelete(NULL);
        return;
    }

    if (dial_oauth_have_valid_access()) {
        ESP_LOGI(TAG, "using stored access token");
    } else if (dial_oauth_refresh(&disc, client_id)) {
        ESP_LOGI(TAG, "refreshed access token");
    } else {
        char url[600];
        if (dial_oauth_start_authorize(&disc, client_id, redirect_uri, url, sizeof(url))) {
            ESP_LOGI(TAG, "authorize URL: %s", url);
            ui_show_qr("Scan to link your dial", url);
            bool ok = dial_oauth_finish_authorize(&disc, client_id, redirect_uri, 300000);
            dial_oauth_stop_authorize();
            if (ok) {
                ui_show_msg("Dial linked!");
            } else {
                char msg[256];
                snprintf(msg, sizeof(msg), "Link failed:\n%s", dial_oauth_last_error());
                ui_show_msg(msg);
                vTaskDelete(NULL);
                return;
            }
        }
    }

    ui_show_msg("Connecting to Orion...");

    // ---- Open the MCP session ----
    char *server = NULL;
    if (!dial_mcp_connect(&server)) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Orion connect failed:\n%s", dial_mcp_last_error());
        ui_show_msg(msg);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "MCP server: %s, %d tools", server ? server : "?",
             dial_mcp_list_tools_count());
    free(server);

    // ---- Find this bed's serial from list_devices ----
    char *devices = NULL;
    if (dial_mcp_call_tool("list_devices", "{}", &devices) && devices) {
        cJSON *root = cJSON_Parse(devices);
        cJSON *arr  = root ? cJSON_GetObjectItem(root, "devices") : NULL;
        cJSON *dev0 = cJSON_IsArray(arr) ? cJSON_GetArrayItem(arr, 0) : NULL;
        cJSON *serial = dev0 ? cJSON_GetObjectItem(dev0, "serial_number") : NULL;
        if (serial && serial->valuestring)
            strlcpy(s_serial, serial->valuestring, sizeof(s_serial));
        if (root) cJSON_Delete(root);
    }
    free(devices);
    if (!s_serial[0]) {
        ui_show_msg("No Orion device\non this account");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "controlling serial %s zone %s", s_serial, ORION_ZONE);

    // ---- Build the touch dial, start the knob, enter the command + poll loop ----
    s_cmd_q     = xQueueCreate(16, sizeof(orion_cmd_t));
    s_state_mux = xSemaphoreCreateMutex();
    orion_refresh_state();
    ui_build_dial();
    ui_apply_state();
    knob_init();     // safe now: queue + mutex + UI all exist

    int64_t last_poll_us = esp_timer_get_time();
    for (;;) {
        orion_cmd_t cmd;
        // Short tick so the loop stays responsive to commands; the device
        // readback is gated separately on the quiet-period below.
        if (xQueueReceive(s_cmd_q, &cmd, pdMS_TO_TICKS(300)) == pdTRUE) {
            // Coalesce a burst (e.g. spinning the knob): collapse to at most one
            // net on/off toggle + the final target temperature.
            int  last_temp = -1, toggles = 0;
            do {
                if (cmd.kind == CMD_SET_TEMP) last_temp = cmd.temp_f;
                else toggles++;
            } while (xQueueReceive(s_cmd_q, &cmd, 0) == pdTRUE);

            if (toggles & 1) {
                xSemaphoreTake(s_state_mux, portMAX_DELAY);
                bool cur = s_on;
                xSemaphoreGive(s_state_mux);
                if (orion_set_zone(cur ? "\"on\":false" : "\"on\":true")) {
                    xSemaphoreTake(s_state_mux, portMAX_DELAY);
                    s_on = !cur;                 // optimistic: trust the change
                    xSemaphoreGive(s_state_mux);
                }
            }
            if (last_temp >= 0) {
                char f[24];
                snprintf(f, sizeof(f), "\"temp\":%.1f", f_to_c(last_temp));
                if (orion_set_zone(f)) {
                    xSemaphoreTake(s_state_mux, portMAX_DELAY);
                    s_temp_c = f_to_c(last_temp);  // optimistic: trust the change
                    xSemaphoreGive(s_state_mux);
                }
            }
            // Reflect the (optimistic) local state. Deliberately NO device
            // readback here: the bed lags a beat. Also push the poll's T0 out so
            // the resync waits for the quiet period after this input.
            ui_apply_state();
            last_poll_us = esp_timer_get_time();
            continue;
        }

        // No command this tick. Resync to the device truth only when it's been
        // quiet (no input for KNOB_SETTLE_US) AND a poll interval has elapsed —
        // so an update can never land mid-interaction.
        int64_t now = esp_timer_get_time();
        if (now - s_last_input_us < KNOB_SETTLE_US) continue;
        if (now - last_poll_us < POLL_INTERVAL_US) continue;

        if (!dial_oauth_have_valid_access())
            dial_oauth_refresh(&disc, client_id);
        if (!orion_refresh_state() && dial_oauth_refresh(&disc, client_id)) {
            dial_mcp_connect(NULL);       // token rotated — reopen the session
            orion_refresh_state();
        }
        ui_apply_state();
        last_poll_us = esp_timer_get_time();
    }
}

void app_main(void)
{
    dial_display_start();

    ui_show_msg("Connecting to Wi-Fi...");

    // Bring up Wi-Fi: connect with stored creds, else run the SoftAP captive
    // portal. Seed from secrets.h in dev so we can skip the portal while iterating.
    dial_net_init();
    dial_net_seed(WIFI_SSID, WIFI_PASSWORD);
    if (!dial_net_have_creds())
        ESP_LOGW(TAG, "no Wi-Fi creds — join AP \"%s\" to set up", dial_net_ap_ssid());
    ui_show_msg("Connecting to Wi-Fi...");
    dial_net_bringup();
    ESP_LOGI(TAG, "Wi-Fi ready");
    ui_show_msg("Linking to Orion...");

    // OAuth runs in its own task with a generous stack: the TLS handshake plus
    // the token exchange (large JSON) overflow smaller stacks. The rotary knob
    // is started from inside that task once the UI + command queue exist.
    xTaskCreate(oauth_task, "oauth", 16384, NULL, 4, NULL);
}
