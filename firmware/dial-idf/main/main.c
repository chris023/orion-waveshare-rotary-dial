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
#include "esp_system.h"
#include "nvs_flash.h"

#include "dial_display.h"
#include "dial_state.h"
#include "ui_router.h"
#include "ui_screens.h"
#include "dial_wifi.h"
#include "dial_oauth.h"
#include "dial_mcp.h"
#include "dial_time.h"
#include "dial_haptics.h"
#include "dial_power.h"
#include "dial_palette.h"
#include "dial_ota.h"
#include "bidi_switch_knob.h"
#include "secrets.h"
#include "cJSON.h"

static const char *TAG = "app";

// Device resync is gated on a quiet period: every user input stamps the store,
// and the poll only reads the bed back once there's been no input for a while
// (so an update can never land mid-interaction).
#define KNOB_SETTLE_US    2500000    // 2.5s of no input before the bed is read back
#define POLL_INTERVAL_US 10000000    // and at most every ~10s when idle
/*
 * ...but the interval is not one number. Right after a write the device is the
 * thing that's changing (the bed takes a few seconds to actually start heating
 * or cooling), so a 10s cadence means the dial insists nothing is happening
 * long after the user can hear the pump. Poll hard for a few rounds, then fall
 * back to idle cadence. The quiet gate above still holds the whole time, so a
 * knob spin is never interrupted by a read.
 */
#define POLL_CONFIRM_US   2000000    // 2s between the confirm polls after a write
#define POLL_CONFIRM_N          3    // how many of them before returning to idle cadence

// Sleep schedules (M5) don't change minute to minute — refresh far less
// often than device state, piggybacked on the same idle poll path.
#define SCHED_INTERVAL_US (30LL * 60 * 1000000)   // ~30 min

// Auto OTA check (M6): once per uptime-day, and only checks (never applies)
// — see the gating comment at its call site for the full safe-window rule.
#define OTA_AUTOCHECK_INTERVAL_US (24LL * 60 * 60 * 1000000)

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

// Haptic tick fires here (non-blocking queue write) so the pulse lands with
// the detent, not a dispatcher period later. A detent arriving in standby
// wakes the screen and is consumed — a 3am reach must not change the temp.
static void knob_step(int dir)
{
    dial_state_stamp_input();
    if (dial_power_wake_consumes()) return;
    dial_haptics_play(HAPTIC_TICK);
    ui_router_knob_input(dir);
}
static void knob_left_cb(void *arg, void *data)  { (void)arg; (void)data; knob_step(-1); }
static void knob_right_cb(void *arg, void *data) { (void)arg; (void)data; knob_step(+1); }

// Touch filter (LVGL task, every 20ms sample): any contact stamps activity;
// the press that wakes a standby screen is swallowed end-to-end (LVGL never
// sees it, so it can't click a button or drag the arc).
static bool touch_filter(bool pressed)
{
    static bool s_consuming;
    if (!pressed) {
        s_consuming = false;
        return false;
    }
    dial_state_stamp_input();
    if (s_consuming) return true;
    if (dial_power_wake_consumes()) {
        s_consuming = true;   // swallow until release
        return true;
    }
    return false;
}

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
    // Onboarding (M4): a genuinely fresh device (no Wi-Fi creds at boot; see
    // app_main) parks on the welcome splash through the earliest connection
    // phases until the user acknowledges it (tap/knob -> dial_state_set_
    // welcomed). Checked ahead of the phase switch below so it also
    // pre-empts PH_WIFI_PORTAL's own screen (join-the-AP QR) — the welcome
    // screen is meant to be seen first, then dismissed into that QR.
    if (st->fresh_device && !st->welcomed &&
        (st->phase == PH_BOOT || st->phase == PH_WIFI_CONNECTING || st->phase == PH_WIFI_PORTAL))
        return SCR_WELCOME;

    switch (st->phase) {
    case PH_WIFI_PORTAL: {
        // Setting up on the dial (network list, password entry) is a deliberate
        // journey inside this phase — a routine state commit must not yank the
        // user back to the instructions screen halfway through typing.
        screen_id_t cur = ui_router_current();
        if (cur == SCR_NETPICK || cur == SCR_PASSKEY) return cur;
        return SCR_WIFI_PORTAL;
    }
    case PH_OAUTH_WAIT_CONSENT: return SCR_OAUTH_QR;
    case PH_READY:
    case PH_DEGRADED:
    case PH_WIFI_LOST:
        // Once we have device state, stay on the dial (with its staleness dot)
        // through transient outages rather than yanking the user to a status
        // screen mid-interaction.
        if (st->have_state) {
            // Quick-actions, boost, and settings are transient overlays
            // reached by a deliberate action; a routine state commit (poll
            // landing, night-mode flip, ...) must not yank the user back to
            // the dial mid-flow. Returning the current screen unchanged is a
            // no-op in ui_router_go (same id + same arg), so this is safe
            // every tick.
            screen_id_t cur = ui_router_current();
            // The menu face and its passive sub-screens (TONIGHT/WIFI/ABOUT)
            // are reached by swipe/tap and join the sticky set below, but
            // unlike QUICK/BOOST/SETTINGS (which only leave via a deliberate
            // user action) they're also dismissed by the standby idle
            // timeout — someone can swipe there and fall asleep on it — so
            // that check must win over stickiness, checked BEFORE folding
            // them into the sticky-set return.
            bool passive = cur == SCR_MENU || cur == SCR_TONIGHT ||
                           cur == SCR_WIFI || cur == SCR_ABOUT;
            if (passive && dial_power_level() == DPWR_STANDBY) {
                *arg = (void *)(uintptr_t)st->ui_zone;
                return SCR_STANDBY;
            }
            if (passive || cur == SCR_QUICK || cur == SCR_BOOST || cur == SCR_SETTINGS) return cur;
            // First link on a fresh device: pick a default side before showing
            // the dial (SCR_SIDEPICK). Nothing to pick on a single-zone topper,
            // so that device goes straight to its one face. The `cur` half of
            // the OR keeps a poll from yanking the user off the picker
            // mid-decision.
            if (dial_state_is_dual(st) &&
                ((st->fresh_device && !st->side_picked) || cur == SCR_SIDEPICK))
                return SCR_SIDEPICK;
            *arg = (void *)(uintptr_t)st->ui_zone;
            return dial_power_level() == DPWR_STANDBY ? SCR_STANDBY : SCR_DIAL;
        }
        return st->phase == PH_READY ? SCR_CONNECTING : SCR_ERROR;
    default:                    return SCR_CONNECTING;
    }
}

/* ---- store mutators (file-scope: no GCC nested-fn trampolines) --------- */

typedef struct {
    zone_state_t zones[ZONE_COUNT];
    bool present[ZONE_COUNT];  // zone ids actually carried by this response
    bool online;
    bool safety_error;
    char safety_desc[96];
    char water_fill[12];
    int64_t poll_started_us;   // when the get_device_state round-trip began
} device_snapshot_t;

static void mut_device_state(app_state_t *st, void *arg)
{
    device_snapshot_t *d = arg;
    // A response that LEFT the device before the user's last input cannot know
    // about what they just did. Taking its control fields (on / setpoint /
    // thermal_state) would visibly undo the optimistic update for a beat and
    // then redo it on the following poll — the "jumpy" flicker after a tap or a
    // knob turn. Its telemetry (measured water, safety, fill) is still good:
    // the user's input didn't change those, so only the control state is held.
    bool predates_input = dial_state_last_input_us() > d->poll_started_us;

    for (int z = 0; z < ZONE_COUNT; z++) {
        // Two things about a zone don't come from THIS call and must survive
        // it: the name (list_devices) and the M5 sleep-schedule fields
        // (get_sleep_schedules, refreshed on its own much slower cadence —
        // see SCHED_INTERVAL_US) — save the whole zone, overwrite with the
        // poll's fresh values, then restore just those fields.
        zone_state_t keep = st->zones[z];
        st->zones[z] = d->zones[z];
        strlcpy(st->zones[z].user_name, keep.user_name, sizeof(st->zones[z].user_name));
        st->zones[z].sched_valid              = keep.sched_valid;
        strlcpy(st->zones[z].sched_bedtime, keep.sched_bedtime, sizeof(st->zones[z].sched_bedtime));
        st->zones[z].sched_bedtime_temp_c      = keep.sched_bedtime_temp_c;
        strlcpy(st->zones[z].sched_wakeup, keep.sched_wakeup, sizeof(st->zones[z].sched_wakeup));
        st->zones[z].sched_wakeup_temp_c        = keep.sched_wakeup_temp_c;
        st->zones[z].sched_override_available   = keep.sched_override_available;
        st->zones[z].sched_override_applied     = keep.sched_override_applied;

        if (predates_input) {                       // see the note above
            st->zones[z].on     = keep.on;
            st->zones[z].temp_c = keep.temp_c;
            strlcpy(st->zones[z].thermal_state, keep.thermal_state,
                    sizeof(st->zones[z].thermal_state));
        }
    }
    for (int z = 0; z < ZONE_COUNT; z++) st->zone_present[z] = d->present[z];
    // A single-zone topper has no partner face to show. If the persisted side
    // names a zone this device doesn't have (a dial moved between beds, or a
    // NVS value from before this field existed), fall back to the one it does —
    // otherwise nav_policy would keep routing to a face built from an empty zone.
    if (!st->zone_present[st->ui_zone])
        st->ui_zone = dial_state_primary_zone(st);
    st->device_online = d->online;
    st->safety.error = d->safety_error;
    strlcpy(st->safety.desc, d->safety_desc, sizeof(st->safety.desc));
    strlcpy(st->water_fill, d->water_fill, sizeof(st->water_fill));
    st->have_state = true;
    // Clear optimistic intent ONLY if no input arrived after this poll's
    // round-trip began (the failed-write case still converges: the next
    // quiet-period poll runs with no newer input and clears the stale intent).
    if (!predates_input)
        for (int z = 0; z < ZONE_COUNT; z++)
            st->ui_temp_f[z] = -1;
}

typedef struct { char names[ZONE_COUNT][24]; char serial[16]; } device_identity_t;

static void mut_identity(app_state_t *st, void *arg)
{
    device_identity_t *n = arg;
    for (int z = 0; z < ZONE_COUNT; z++)
        strlcpy(st->zones[z].user_name, n->names[z], sizeof(st->zones[z].user_name));
    strlcpy(st->serial, n->serial, sizeof(st->serial));
}

/*
 * Write acks. `issued_us` is when the worker STARTED the write, so these can
 * tell the difference between "the user has not touched anything since" and
 * "the user kept going while this was in flight". A write that lands after
 * newer input must not roll the display back to what it happened to send.
 */
typedef struct { int zone; bool on; int64_t issued_us; } zone_on_t;
static void mut_zone_on(app_state_t *st, void *arg)
{
    zone_on_t *u = arg;
    if (dial_state_last_input_us() > u->issued_us) return;   // user moved on; leave their state alone
    st->zones[u->zone].on = u->on;
    dial_state_predict_thermal(st, u->zone);
}

typedef struct { int zone; float temp_c; int64_t issued_us; } zone_temp_t;
static void mut_zone_temp(app_state_t *st, void *arg)
{
    zone_temp_t *u = arg;
    st->zones[u->zone].temp_c = u->temp_c;          // the device now holds this target

    // Only retire the optimistic display value if nothing newer has been dialled
    // in since this write left. Clearing it unconditionally is what made the
    // numeral run up with the knob and then SNAP BACK to whatever value the
    // in-flight write happened to carry — a number the user had already turned
    // past. The newer value has its own CMD_SET_TEMP queued behind this one.
    if (dial_state_last_input_us() <= u->issued_us)
        st->ui_temp_f[u->zone] = -1;

    dial_state_predict_thermal(st, u->zone);
}

// Response shape shared by start_thermal_relief / cancel_thermal_relief:
// {success, zones:[{id,temp,on,thermal_relief?}, ...]}. `touched` marks which
// zones this particular response actually described.
typedef struct {
    zone_state_t zones[ZONE_COUNT];
    bool touched[ZONE_COUNT];
} relief_ack_t;

// Ack-commit for start/cancel_thermal_relief: applies the response's zones[]
// (temp/on/relief) directly, not gated by poll_started_us — unlike
// mut_device_state this isn't a background poll racing user input, it's the
// direct result of a command the user just issued. Deliberately leaves
// ui_temp_f alone (these commands come from SCR_QUICK/SCR_BOOST, never from a
// knob turn on the dial, so there's normally no optimistic temp in flight for
// this zone to clobber or preserve).
static void mut_relief_ack(app_state_t *st, void *arg)
{
    relief_ack_t *r = arg;
    for (int z = 0; z < ZONE_COUNT; z++) {
        if (!r->touched[z]) continue;
        st->zones[z].temp_c            = r->zones[z].temp_c;
        st->zones[z].on                = r->zones[z].on;
        st->zones[z].relief_active     = r->zones[z].relief_active;
        st->zones[z].relief_heat       = r->zones[z].relief_heat;
        st->zones[z].relief_end_ms     = r->zones[z].relief_end_ms;
        st->zones[z].relief_prev_temp_c = r->zones[z].relief_prev_temp_c;
    }
}

static void mut_bed_off(app_state_t *st, void *arg)
{
    (void)arg;
    for (int z = 0; z < ZONE_COUNT; z++) {
        st->zones[z].on = false;                   // optimistic
        dial_state_predict_thermal(st, (zone_idx_t)z);
    }
}

static void mut_away(app_state_t *st, void *arg) { st->away = *(bool *)arg; }

// "Match my side" (M5): the worker reads the source zone's CURRENT store
// values at command-execution time (handle_immediate_cmd), not whatever was
// true when the sheet was opened, then set_zones the other zone to match —
// this commit just mirrors that same (temp_c, on) pair into the store.
typedef struct { zone_idx_t other; float temp_c; bool on; } match_args_t;
static void mut_match_partner(app_state_t *st, void *arg)
{
    match_args_t *m = arg;
    st->zones[m->other].temp_c = m->temp_c;
    st->zones[m->other].on     = m->on;
    st->ui_temp_f[m->other]    = -1;
    dial_state_predict_thermal(st, m->other);
}

// Tonight schedule (M5) snapshot from get_sleep_schedules — TODAY's entry
// only, one per zone (via the worker's uuid map, see s_zone_uuid below).
typedef struct {
    bool  valid;
    char  bedtime[6];
    float bedtime_temp_c;
    char  wakeup[6];
    float wakeup_temp_c;
    bool  override_available;
    bool  override_applied;
} sched_zone_t;
typedef struct { sched_zone_t zones[ZONE_COUNT]; } sched_snapshot_t;

static void mut_schedules(app_state_t *st, void *arg)
{
    sched_snapshot_t *s = arg;
    for (int z = 0; z < ZONE_COUNT; z++) {
        st->zones[z].sched_valid = s->zones[z].valid;
        if (!s->zones[z].valid) continue;
        strlcpy(st->zones[z].sched_bedtime, s->zones[z].bedtime, sizeof(st->zones[z].sched_bedtime));
        st->zones[z].sched_bedtime_temp_c      = s->zones[z].bedtime_temp_c;
        strlcpy(st->zones[z].sched_wakeup, s->zones[z].wakeup, sizeof(st->zones[z].sched_wakeup));
        st->zones[z].sched_wakeup_temp_c        = s->zones[z].wakeup_temp_c;
        st->zones[z].sched_override_available   = s->zones[z].override_available;
        st->zones[z].sched_override_applied     = s->zones[z].override_applied;
    }
}

static void mut_oauth_url(app_state_t *st, void *arg) { strlcpy(st->oauth_url, arg, sizeof(st->oauth_url)); }
static void mut_retry_in(app_state_t *st, void *arg)  { st->retry_in_s = *(int *)arg; }
static void mut_ap_ssid(app_state_t *st, void *arg)   { strlcpy(st->ap_ssid, arg, sizeof(st->ap_ssid)); }
static void mut_clock_valid(app_state_t *st, void *arg) { st->clock_valid = *(bool *)arg; }
static void mut_fresh_device(app_state_t *st, void *arg) { st->fresh_device = *(bool *)arg; }

// Mirrors a dial_ota_info_t snapshot into app_state_t.ota (see dial_state.h's
// comment on that field for why it's a plain-int mirror, not a #include).
static void mut_ota(app_state_t *st, void *arg)
{
    const dial_ota_info_t *info = arg;
    st->ota.status = (int)info->status;
    strlcpy(st->ota.latest, info->latest, sizeof(st->ota.latest));
    st->ota.progress_pct = info->progress_pct;
    strlcpy(st->ota.err, info->err, sizeof(st->ota.err));
}

// Fetches the fresh dial_ota_get() snapshot and commits it.
static void commit_ota_snapshot(void)
{
    dial_ota_info_t info;
    dial_ota_get(&info);
    dial_state_commit(mut_ota, &info);
}

// OTA rollback health check (M6): dial_ota_mark_valid_if_pending() is
// idempotent, but there's nothing left to confirm after the first success —
// this guards against re-reading the OTA partition state on every ~10s poll
// for the rest of the device's uptime.
static bool s_ota_confirmed;
static void ota_confirm_once(void)
{
    if (s_ota_confirmed) return;
    dial_ota_mark_valid_if_pending();
    s_ota_confirmed = true;
}

// No-op mutator: dial_state_commit() bumps the generation unconditionally, so
// this is just a way to force the dispatcher to re-render after a palette
// swap (screens re-read PAL() from on_state; they never cache day/night).
static void mut_bump(app_state_t *st, void *arg) { (void)st; (void)arg; }

// Tracks the night flag actually applied to the UI palette, separate from
// dial_power's own internal one (dial_power.c must not depend on dial_ui, so
// it can't call dial_palette_set_night itself — this is the seam instead).
static bool s_ui_night;

// Tracks the clock-valid flag actually committed to the store, mirroring
// s_ui_night's pattern, so the commit only fires on an actual transition.
static bool s_ui_clock_valid;

/* ---- Orion MCP calls (worker task only) -------------------------------- */

static char s_serial[16];

// zone -> Orion user uuid (list_devices zones[].user.id), captured once in
// orion_discover_device. get_sleep_schedules keys its "schedules" object by
// this same uuid, so it's how orion_refresh_schedules matches a schedule
// entry back to a zone. Worker-side only — deliberately NOT in app_state_t
// (dial_state stays lean; nothing outside the worker needs the raw uuid).
static char s_zone_uuid[ZONE_COUNT][40];

static zone_idx_t zone_idx_from_id(const char *id)
{
    return (id && strcmp(id, "zone_b") == 0) ? ZONE_B : ZONE_A;
}

// thermal_relief is an object|null field carried on a zone entry in three
// response shapes: get_device_state's top-level zones[], and the
// start/cancel_thermal_relief responses' zones[] — same shape every time.
// Null/absent means no active relief on that zone.
static void parse_thermal_relief(cJSON *zone_obj, zone_state_t *zs)
{
    cJSON *relief = cJSON_GetObjectItem(zone_obj, "thermal_relief");
    if (!cJSON_IsObject(relief)) {
        zs->relief_active = false;
        return;
    }
    cJSON *type = cJSON_GetObjectItem(relief, "type");
    cJSON *end  = cJSON_GetObjectItem(relief, "end_time");
    cJSON *prev = cJSON_GetObjectItem(relief, "previous_temp");
    zs->relief_active       = true;
    zs->relief_heat         = (type && type->valuestring && !strcmp(type->valuestring, "heat"));
    zs->relief_end_ms       = cJSON_IsNumber(end) ? (int64_t)end->valuedouble : 0;
    zs->relief_prev_temp_c  = cJSON_IsNumber(prev) ? (float)prev->valuedouble : zs->temp_c;
}

// Parser for start_thermal_relief / cancel_thermal_relief responses (shape:
// relief_ack_t, above) — a subset of the get_device_state shape (no
// status/actual-temp block), so only touch the fields this response carries.
static bool parse_relief_ack(const char *json, relief_ack_t *out)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;
    cJSON *z;
    cJSON_ArrayForEach(z, cJSON_GetObjectItem(root, "zones")) {
        cJSON *id = cJSON_GetObjectItem(z, "id");
        if (!id || !id->valuestring) continue;
        zone_idx_t zi = zone_idx_from_id(id->valuestring);
        zone_state_t *zs = &out->zones[zi];
        cJSON *t = cJSON_GetObjectItem(z, "temp");
        if (cJSON_IsNumber(t)) zs->temp_c = (float)t->valuedouble;
        zs->on = cJSON_IsTrue(cJSON_GetObjectItem(z, "on"));
        parse_thermal_relief(z, zs);
        out->touched[zi] = true;
    }
    cJSON_Delete(root);
    return true;
}

static bool orion_refresh_state(void)
{
    int64_t started_us = esp_timer_get_time();
    char args[48];
    snprintf(args, sizeof(args), "{\"serial\":\"%s\"}", s_serial);
    char *json = NULL;
    if (!dial_mcp_call_tool("get_device_state", args, &json) || !json) return false;
    cJSON *root = cJSON_Parse(json);
    free(json);
    if (!root) return false;

    device_snapshot_t d = { .poll_started_us = started_us };
    for (int z = 0; z < ZONE_COUNT; z++) d.zones[z].actual_c = -1.0f;
    strlcpy(d.water_fill, "unknown", sizeof(d.water_fill));

    cJSON *z;
    cJSON_ArrayForEach(z, cJSON_GetObjectItem(root, "zones")) {
        cJSON *id = cJSON_GetObjectItem(z, "id");
        if (!id || !id->valuestring) continue;
        // zones[] is the authority on how many sides this topper has: a
        // single-zone model reports one entry (see app_state_t.zone_present).
        zone_idx_t zi = zone_idx_from_id(id->valuestring);
        d.present[zi] = true;
        zone_state_t *zs = &d.zones[zi];
        cJSON *t = cJSON_GetObjectItem(z, "temp");
        if (cJSON_IsNumber(t)) zs->temp_c = (float)t->valuedouble;
        zs->on = cJSON_IsTrue(cJSON_GetObjectItem(z, "on"));
        parse_thermal_relief(z, zs);
    }
    // A response with no zones at all is not a single-zone device, it's a
    // malformed/partial payload — don't let it collapse the UI to one face.
    if (!d.present[ZONE_A] && !d.present[ZONE_B]) {
        ESP_LOGW(TAG, "get_device_state returned no zones — ignoring this poll");
        cJSON_Delete(root);
        return false;
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

    for (int z = 0; z < ZONE_COUNT; z++)
        if (d.present[z])
            ESP_LOGI(TAG, "zone %s: on=%d thermal=%s set=%.1fC water=%.1fC",
                     zone_id_str((zone_idx_t)z), d.zones[z].on,
                     d.zones[z].thermal_state[0] ? d.zones[z].thermal_state : "(none)",
                     d.zones[z].temp_c, d.zones[z].actual_c);

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

// Commits the start/cancel_thermal_relief response via mut_relief_ack (an
// ack-commit, not a poll — see that mutator's comment). Shared by both calls
// below since the response shape is identical.
static void commit_relief_response(const char *json)
{
    relief_ack_t ack = { 0 };
    if (parse_relief_ack(json, &ack)) dial_state_commit(mut_relief_ack, &ack);
}

typedef struct { zone_idx_t zone; bool heat; int minutes; } boost_args_t;
static bool orion_boost(void *arg)
{
    boost_args_t *b = arg;
    char args[128];
    snprintf(args, sizeof(args),
             "{\"serial\":\"%s\",\"type\":\"%s\",\"zones\":[\"%s\"],\"duration_minutes\":%d}",
             s_serial, b->heat ? "heat" : "cool", zone_id_str(b->zone), b->minutes);
    char *r = NULL;
    bool ok = dial_mcp_call_tool("start_thermal_relief", args, &r);
    if (ok && r) commit_relief_response(r);
    else         ESP_LOGW(TAG, "start_thermal_relief failed: %s", dial_mcp_last_error());
    free(r);
    return ok;
}

static bool orion_boost_cancel(void *arg)
{
    (void)arg;
    char args[48];
    snprintf(args, sizeof(args), "{\"serial\":\"%s\"}", s_serial);
    char *r = NULL;
    bool ok = dial_mcp_call_tool("cancel_thermal_relief", args, &r);
    if (ok && r) commit_relief_response(r);
    else         ESP_LOGW(TAG, "cancel_thermal_relief failed: %s", dial_mcp_last_error());
    free(r);
    return ok;
}

// set_zones is atomic across zones — used here so "Bed off" can never leave
// the bed with one side on and one off from a partial failure.
static bool orion_bed_off(void *arg)
{
    (void)arg;
    // Only name zones this topper actually has — a single-zone device would
    // reject (or silently ignore) a set_zones carrying a zone_b it doesn't own.
    app_state_t st;
    dial_state_get(&st);
    char zones[96] = "";
    for (int z = 0; z < ZONE_COUNT; z++) {
        if (!st.zone_present[z]) continue;
        char one[48];
        snprintf(one, sizeof(one), "%s{\"id\":\"%s\",\"on\":false}",
                 zones[0] ? "," : "", zone_id_str((zone_idx_t)z));
        strlcat(zones, one, sizeof(zones));
    }
    char args[160];
    snprintf(args, sizeof(args), "{\"serial\":\"%s\",\"zones\":[%s]}", s_serial, zones);
    char *r = NULL;
    bool ok = dial_mcp_call_tool("set_zones", args, &r);
    if (!ok) ESP_LOGW(TAG, "set_zones (bed off) failed: %s", dial_mcp_last_error());
    free(r);
    return ok;
}

typedef struct { bool away; } away_args_t;
static bool orion_set_away(void *arg)
{
    away_args_t *a = arg;
    char args[32];
    snprintf(args, sizeof(args), "{\"is_away\":%s}", a->away ? "true" : "false");
    char *r = NULL;
    bool ok = dial_mcp_call_tool("set_away", args, &r);
    if (!ok) ESP_LOGW(TAG, "set_away failed: %s", dial_mcp_last_error());
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
            if (!id || !id->valuestring) continue;
            zone_idx_t zi = zone_idx_from_id(id->valuestring);
            cJSON *fn = user ? cJSON_GetObjectItem(user, "first_name") : NULL;
            if (fn && fn->valuestring)
                strlcpy(ident.names[zi], fn->valuestring, sizeof(ident.names[0]));
            // Captured for orion_refresh_schedules (M5): get_sleep_schedules
            // keys its per-user entries by this same uuid.
            cJSON *uid = user ? cJSON_GetObjectItem(user, "id") : NULL;
            if (uid && uid->valuestring)
                strlcpy(s_zone_uuid[zi], uid->valuestring, sizeof(s_zone_uuid[0]));
        }
        if (ok) dial_state_commit(mut_identity, &ident);
    }
    cJSON_Delete(root);
    return ok;
}

// "Match my side" (M5): set_zones the OTHER zone to (temp_c, on) — atomic so
// the partner's side can never land with the temp changed but the power
// state not (or vice versa) from a partial failure.
static bool orion_match_partner(void *arg)
{
    match_args_t *m = arg;
    char args[192];
    snprintf(args, sizeof(args),
             "{\"serial\":\"%s\",\"zones\":[{\"id\":\"%s\",\"temp\":%.1f,\"on\":%s}]}",
             s_serial, zone_id_str(m->other), m->temp_c, m->on ? "true" : "false");
    char *r = NULL;
    bool ok = dial_mcp_call_tool("set_zones", args, &r);
    if (!ok) ESP_LOGW(TAG, "set_zones (match partner) failed: %s", dial_mcp_last_error());
    free(r);
    return ok;
}

// get_sleep_schedules {} returns {schedules: {"<uuid>": [ {day:0-6, bedtime,
// bedtime_temp, wakeup, wakeup_temp, is_override_available,
// is_override_applied, ...} x7 ]}} — one entry per user uuid. Pulls out
// TODAY's entry (day == dial_time_now's tm_wday) per zone, matched via
// s_zone_uuid. Requires a valid clock (to know which weekday "today" is);
// skips silently if the clock isn't set yet, same as the rest of the app
// treats an unsynced SNTP as "wait, don't guess".
static bool orion_refresh_schedules(void)
{
    struct tm lt;
    if (!dial_time_now(&lt)) return false;
    // "Tonight" belongs to the evening it started: before noon we're still in
    // (or just out of) the sleep session that began YESTERDAY evening, so key
    // by the previous weekday then. Otherwise, at 00:15 the half-hourly
    // refresh would clobber the governing schedule with the next day's entry
    // — e.g. an early-wake Tuesday would end night mode at 05:30 during
    // Monday night's 07:00 session. Noon is the natural session boundary.
    int today = lt.tm_wday;
    if (lt.tm_hour < 12) today = (today + 6) % 7;

    char *json = NULL;
    if (!dial_mcp_call_tool("get_sleep_schedules", "{}", &json) || !json) return false;
    cJSON *root = cJSON_Parse(json);
    free(json);
    if (!root) return false;

    sched_snapshot_t sc = { 0 };
    cJSON *schedules = cJSON_GetObjectItem(root, "schedules");
    if (cJSON_IsObject(schedules)) {
        for (int z = 0; z < ZONE_COUNT; z++) {
            if (!s_zone_uuid[z][0]) continue;
            cJSON *arr = cJSON_GetObjectItem(schedules, s_zone_uuid[z]);
            if (!cJSON_IsArray(arr)) continue;
            cJSON *entry;
            cJSON_ArrayForEach(entry, arr) {
                cJSON *day = cJSON_GetObjectItem(entry, "day");
                if (!cJSON_IsNumber(day) || (int)day->valuedouble != today) continue;

                sched_zone_t *zs = &sc.zones[z];
                zs->valid = true;
                cJSON *bt  = cJSON_GetObjectItem(entry, "bedtime");
                cJSON *btt = cJSON_GetObjectItem(entry, "bedtime_temp");
                cJSON *wk  = cJSON_GetObjectItem(entry, "wakeup");
                cJSON *wkt = cJSON_GetObjectItem(entry, "wakeup_temp");
                if (bt && bt->valuestring) strlcpy(zs->bedtime, bt->valuestring, sizeof(zs->bedtime));
                if (cJSON_IsNumber(btt)) zs->bedtime_temp_c = (float)btt->valuedouble;
                if (wk && wk->valuestring) strlcpy(zs->wakeup, wk->valuestring, sizeof(zs->wakeup));
                if (cJSON_IsNumber(wkt)) zs->wakeup_temp_c = (float)wkt->valuedouble;
                zs->override_available = cJSON_IsTrue(cJSON_GetObjectItem(entry, "is_override_available"));
                zs->override_applied   = cJSON_IsTrue(cJSON_GetObjectItem(entry, "is_override_applied"));
                break;   // one entry per day
            }
        }
    }
    cJSON_Delete(root);
    dial_state_commit(mut_schedules, &sc);
    return true;
}

// override_sleep_schedule_tonight {fields:{...}} — same field vocabulary as
// get_sleep_schedules, ack only. Only sends the fields the caller actually
// changed (a/b == -1 means "leave alone"). Targets the OAuth token's own
// account implicitly (no user_id in the confirmed schema) — callers must
// only invoke this for ZONE_A (see the CMD_TONIGHT_OVERRIDE comment in
// dial_state.h for why).
typedef struct { int wakeup_min; int bedtime_temp_f; } tonight_override_args_t;
static bool orion_tonight_override(void *arg)
{
    tonight_override_args_t *a = arg;
    char fields[80] = "";
    if (a->wakeup_min >= 0) {
        char buf[24];
        snprintf(buf, sizeof(buf), "\"wakeup\":\"%02d:%02d\"", a->wakeup_min / 60, a->wakeup_min % 60);
        strlcat(fields, buf, sizeof(fields));
    }
    if (a->bedtime_temp_f >= 0) {
        if (fields[0]) strlcat(fields, ",", sizeof(fields));
        char buf[32];
        snprintf(buf, sizeof(buf), "\"bedtime_temp\":%.1f", dial_f_to_c(a->bedtime_temp_f));
        strlcat(fields, buf, sizeof(fields));
    }
    if (!fields[0]) return true;   // nothing to change

    char args[128];
    snprintf(args, sizeof(args), "{\"fields\":{%s}}", fields);
    char *r = NULL;
    bool ok = dial_mcp_call_tool("override_sleep_schedule_tonight", args, &r);
    if (!ok) ESP_LOGW(TAG, "override_sleep_schedule_tonight failed: %s", dial_mcp_last_error());
    free(r);
    return ok;
}

static bool orion_tonight_revert(void *arg)
{
    (void)arg;
    char *r = NULL;
    bool ok = dial_mcp_call_tool("revert_sleep_schedule_override", "{}", &r);
    if (!ok) ESP_LOGW(TAG, "revert_sleep_schedule_override failed: %s", dial_mcp_last_error());
    free(r);
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
static bool with_auth_retry(bool (*call)(void *), void *arg,
                            const oauth_disc_t *disc, const char *client_id)
{
    if (call(arg)) return true;
    if (dial_oauth_refresh(disc, client_id)) {
        dial_mcp_connect(NULL);
        return call(arg);
    }
    return false;
}

static bool poll_call(void *arg) { (void)arg; return orion_refresh_state(); }
static bool sched_call(void *arg) { (void)arg; return orion_refresh_schedules(); }

typedef struct { zone_idx_t zone; const char *field_json; } set_zone_args_t;
static bool set_zone_call(void *arg)
{
    set_zone_args_t *a = arg;
    return orion_set_zone(a->zone, a->field_json);
}

// dial_ota_download_and_apply's progress callback: fires on every
// esp_https_ota_perform() iteration (every ~4KB read), far too often to
// commit unthrottled — a store commit bumps the generation and triggers a
// full settings-screen re-render. Only commit on a >=5-point change (or the
// final 100%), same idea as the staleness-dot fade elsewhere in the app.
static int s_ota_last_committed_pct = -100;
static void ota_progress_cb(int pct)
{
    if (pct < 100 && pct - s_ota_last_committed_pct < 5) return;
    s_ota_last_committed_pct = pct;
    commit_ota_snapshot();
}

// CMD_BOOST_START/CANCEL, CMD_BED_OFF and CMD_AWAY are rare (a quick-actions
// tap, not a knob spin) — no coalescing, just run them in arrival order.
static void handle_immediate_cmd(const app_cmd_t *cmd, const oauth_disc_t *disc,
                                  const char *client_id)
{
    switch (cmd->kind) {
    case CMD_BOOST_START: {
        boost_args_t b = { cmd->zone, cmd->a != 0, cmd->b };
        with_auth_retry(orion_boost, &b, disc, client_id);   // commits inside on success
        break;
    }
    case CMD_BOOST_CANCEL:
        with_auth_retry(orion_boost_cancel, NULL, disc, client_id);  // commits inside
        break;
    case CMD_BED_OFF:
        if (with_auth_retry(orion_bed_off, NULL, disc, client_id))
            dial_state_commit(mut_bed_off, NULL);
        break;
    case CMD_AWAY: {
        bool away = cmd->a != 0;
        away_args_t a = { away };
        if (with_auth_retry(orion_set_away, &a, disc, client_id))
            dial_state_commit(mut_away, &away);
        break;
    }
    case CMD_MATCH_PARTNER: {
        // Read the source zone's CURRENT value at execution time (not
        // whatever was true when the sheet was opened — a burst of knob
        // turns could have landed in between).
        app_state_t st;
        dial_state_get(&st);
        if (!dial_state_is_dual(&st)) break;   // no partner side to match on a single-zone topper
        zone_idx_t mine  = cmd->zone;
        zone_idx_t other = (mine == ZONE_A) ? ZONE_B : ZONE_A;
        match_args_t m = { other, st.zones[mine].temp_c, st.zones[mine].on };
        if (with_auth_retry(orion_match_partner, &m, disc, client_id))
            dial_state_commit(mut_match_partner, &m);
        break;
    }
    // Tonight schedule (M5) — the owner's own side only, see dial_state.h's
    // comment beside CMD_TONIGHT_OVERRIDE for why the partner side is dropped
    // here. That side is ZONE_A on a normal topper, but a single-zone model may
    // only have ZONE_B, so compare against the device's primary zone.
    case CMD_TONIGHT_OVERRIDE: {
        app_state_t st;
        dial_state_get(&st);
        if (cmd->zone != dial_state_primary_zone(&st)) break;
        tonight_override_args_t a = { cmd->a, cmd->b };
        if (with_auth_retry(orion_tonight_override, &a, disc, client_id))
            with_auth_retry(sched_call, NULL, disc, client_id);   // refresh so override_applied flips immediately
        break;
    }
    case CMD_TONIGHT_REVERT: {
        app_state_t st;
        dial_state_get(&st);
        if (cmd->zone != dial_state_primary_zone(&st)) break;
        if (with_auth_retry(orion_tonight_revert, NULL, disc, client_id))
            with_auth_retry(sched_call, NULL, disc, client_id);
        break;
    }
    // Settings (M4) destructive actions: each erases some NVS state and
    // reboots — there's no follow-up state commit because esp_restart()
    // never returns.
    case CMD_RELINK:
        ESP_LOGW(TAG, "settings: re-link requested — clearing Orion tokens");
        dial_oauth_forget();
        esp_restart();
        break;
    case CMD_WIFI_RESET:
        ESP_LOGW(TAG, "settings: change-network requested — rebooting into the setup portal");
        dial_net_request_setup();
        esp_restart();
        break;
    case CMD_FACTORY_RESET:
        ESP_LOGW(TAG, "settings: factory reset requested — erasing NVS");
        nvs_flash_erase();
        esp_restart();
        break;
    // Software update (M6), from Settings' "Software update" row.
    case CMD_OTA_CHECK:
        dial_ota_check();
        commit_ota_snapshot();
        break;
    case CMD_OTA_APPLY: {
        // Stale-tap guard: the row's confirm is only armed while AVAILABLE,
        // but the store could have moved on (an unrelated auto-check landed,
        // say) between the tap and the worker draining this command.
        app_state_t st;
        dial_state_get(&st);
        if (st.ota.status != OTA_AVAILABLE) break;
        s_ota_last_committed_pct = -100;   // guarantee the first progress commit fires
        bool ok = dial_ota_download_and_apply(ota_progress_cb);
        commit_ota_snapshot();
        if (ok) {
            ESP_LOGI(TAG, "OTA image ready; rebooting into it");
            esp_restart();
        }
        break;
    }
    default:
        break;   // CMD_SET_TEMP/CMD_TOGGLE_ON never reach here (see the drain loop)
    }
}

static void worker_task(void *arg)
{
    (void)arg;
    oauth_disc_t disc;
    char client_id[96];
    int backoff_s = BACKOFF_MIN_S;

    /*
     * Input comes up FIRST, before the network.
     *
     * These used to be initialised after Wi-Fi, OAuth, the MCP connect and the
     * first poll had all succeeded — which meant that during Wi-Fi setup the
     * encoder callbacks did not exist yet and the knob was simply dead. That
     * was survivable when setup was "scan a QR with your phone", and fatal the
     * moment the dial started asking you to TYPE A PASSWORD with the knob.
     *
     * Nothing here needs the network: the display, touch and I2C bus are all up
     * (app_main did them), and iot_knob only needs its two GPIOs. Haptics is
     * safe to call before this runs — dial_haptics_play() no-ops until the
     * driver is present — so the ordering was never load-bearing, just late.
     */
    knob_init();
    dial_haptics_init();
    {
        // Apply the persisted haptics preference (restored into the store at
        // boot) — the driver defaults to enabled and settings only writes on
        // taps, so without this an "Off" preference reverts every reboot.
        app_state_t st;
        dial_state_get(&st);
        dial_haptics_set_enabled(st.haptics_enabled);
    }

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
            backoff_s = (backoff_s * 2 > BACKOFF_MAX_S) ? BACKOFF_MAX_S : backoff_s * 2;
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
        bool linked = dial_mcp_connect(NULL) && orion_discover_device();
        if (!linked) {
            // The token can be dead server-side while still looking usable here:
            // dial_oauth_have_valid_access() reports that an access token EXISTS,
            // not that it's still good, and the whole design leans on refreshing
            // when a call comes back 401. Reads and writes get that from
            // with_auth_retry — this first connect never did, so an expired or
            // revoked token produced "Orion unreachable", a backoff, and then a
            // loop that skipped the refresh branch again on every pass, forever.
            // Force one refresh and retry.
            if (dial_oauth_refresh(&disc, client_id)) {
                linked = dial_mcp_connect(NULL) && orion_discover_device();
            } else {
                // The refresh token is dead too. Drop the stale access token so
                // the next pass falls through to interactive consent (the QR)
                // rather than spinning on credentials that can never work again.
                ESP_LOGW(TAG, "refresh failed on a rejected token — re-linking");
                dial_oauth_forget_access();
            }
        }
        if (!linked) {
            dial_state_set_phase(PH_DEGRADED, dial_mcp_last_error());
            backoff_wait(backoff_s);
            backoff_s = (backoff_s * 2 > BACKOFF_MAX_S) ? BACKOFF_MAX_S : backoff_s * 2;
            continue;
        }
        backoff_s = BACKOFF_MIN_S;
        break;
    }
    ESP_LOGI(TAG, "device linked; %d tools", dial_mcp_list_tools_count());

    bool first_poll_ok = with_auth_retry(poll_call, NULL, &disc, client_id);
    with_auth_retry(sched_call, NULL, &disc, client_id);   // M5: today's schedule, once up front
    dial_state_set_phase(PH_READY, NULL);
    // OTA rollback health check (M6): reaching here with a successful poll
    // proves Wi-Fi + TLS + OAuth + MCP + real device state all work on this
    // image -- cancel the bootloader's pending-verify rollback timer if this
    // boot came from an OTA install. (Also re-tried on the first successful
    // poll in the steady-state loop below, in case this exact poll hit a
    // transient failure -- ota_confirm_once() only ever does real work once.)
    if (first_poll_ok) ota_confirm_once();

    // ---- steady state: drain commands (coalescing per zone), gated poll ----
    int64_t last_poll_us      = esp_timer_get_time();
    int     poll_confirms     = 0;   // fast reads still owed after a write
    int64_t last_sched_us     = esp_timer_get_time();
    int64_t last_ota_check_us = esp_timer_get_time();   // first auto-check ~24h after boot
    int poll_failures = 0;
    for (;;) {
        app_cmd_t cmd;
        if (dial_cmd_receive(&cmd, 300)) {
            // Rare, non-coalesced commands: handle the head of the queue
            // immediately and go back around, rather than folding them into
            // the temp/toggle coalescing loop below (which only knows those
            // two kinds).
            if (cmd.kind != CMD_SET_TEMP && cmd.kind != CMD_TOGGLE_ON) {
                handle_immediate_cmd(&cmd, &disc, client_id);
                last_poll_us = 0;                  // read it back now, not in 10s
                poll_confirms = POLL_CONFIRM_N;    // ...and again while the bed acts on it
                continue;
            }

            // Coalesce a burst: per zone, at most one net toggle + final temp.
            // A burst mixing in a rare command (above) is vanishingly
            // unlikely — quick-actions requires its own screen — but if one
            // lands mid-drain, stop coalescing and handle it right after
            // rather than silently mis-treating it as a toggle.
            int last_temp[ZONE_COUNT] = { -1, -1 };
            int want_on[ZONE_COUNT]   = { -1, -1 };   // -1 = untouched this burst
            bool have_pending = false;
            app_cmd_t pending;
            // The command carries the DESIRED on state, so the last one posted
            // wins. (It used to count toggle parity and then re-derive
            // !current from the store — which now flips optimistically on tap,
            // so re-deriving would have undone the user's own press.)
            do {
                if (cmd.kind == CMD_SET_TEMP)       last_temp[cmd.zone] = cmd.temp_f;
                else if (cmd.kind == CMD_TOGGLE_ON) want_on[cmd.zone] = cmd.a ? 1 : 0;
                else { have_pending = true; pending = cmd; break; }
            } while (dial_cmd_receive(&cmd, 0));

            // Stamped BEFORE the writes: anything the user does during the
            // round trip is newer than this batch, and must not be undone by
            // the commits below (that was the knob "jumping back" to a value
            // the user had already turned past).
            int64_t issued_us = esp_timer_get_time();

            for (int z = 0; z < ZONE_COUNT; z++) {
                if (want_on[z] >= 0) {
                    zone_on_t up = { z, want_on[z] != 0, issued_us };
                    char f[24];
                    snprintf(f, sizeof(f), "\"on\":%s", up.on ? "true" : "false");
                    set_zone_args_t sa = { (zone_idx_t)z, f };
                    if (with_auth_retry(set_zone_call, &sa, &disc, client_id))
                        dial_state_commit(mut_zone_on, &up);
                }
                if (last_temp[z] >= 0) {
                    zone_temp_t up = { z, dial_f_to_c(last_temp[z]), issued_us };
                    char f[24];
                    snprintf(f, sizeof(f), "\"temp\":%.1f", up.temp_c);
                    set_zone_args_t sa = { (zone_idx_t)z, f };
                    if (with_auth_retry(set_zone_call, &sa, &disc, client_id))
                        dial_state_commit(mut_zone_temp, &up);
                }
            }
            if (have_pending) handle_immediate_cmd(&pending, &disc, client_id);

            // We just changed the bed, so read it back as soon as the user
            // stops touching it, then keep reading for a few rounds while it
            // acts on the command. This used to set last_poll_us = now, which
            // pushed the confirming read a FULL interval away — so switching a
            // zone on left the dial showing the old state for ~10s even though
            // the quiet gate below was already all the protection a knob spin
            // needed.
            last_poll_us = 0;
            poll_confirms = POLL_CONFIRM_N;
            continue;
        }

        // Publish clock validity so the dial's boost countdown (mm:ss, needs
        // real wall time) can fall back to a bare "BOOST" before SNTP syncs.
        bool clock_valid = dial_time_valid();
        if (clock_valid != s_ui_clock_valid) {
            s_ui_clock_valid = clock_valid;
            dial_state_commit(mut_clock_valid, &clock_valid);
        }

        // Night mode: warm-dim + quiet haptics while the household sleeps.
        // Real window (M5): bedtime-30min -> wake+30min from ZONE_A's
        // schedule (the dial's own side — see CMD_TONIGHT_OVERRIDE's comment
        // for why only ZONE_A's schedule is trusted); falls back to a fixed
        // 21:00-07:00 window until that schedule is known.
        struct tm lt;
        if (dial_time_now(&lt)) {
            int now_min = lt.tm_hour * 60 + lt.tm_min;
            bool night;
            app_state_t sched_st;
            dial_state_get(&sched_st);
            const zone_state_t *za = &sched_st.zones[ZONE_A];
            int bed_min, wake_min;
            if (za->sched_valid &&
                dial_parse_hhmm(za->sched_bedtime, &bed_min) &&
                dial_parse_hhmm(za->sched_wakeup, &wake_min)) {
                int start = ((bed_min - 30) % 1440 + 1440) % 1440;
                int end   = (wake_min + 30) % 1440;
                // The window almost always crosses midnight (bedtime ~21:00,
                // wake ~07:00 next day); handle the wrap explicitly.
                night = (start <= end) ? (now_min >= start && now_min < end)
                                       : (now_min >= start || now_min < end);
            } else {
                night = (lt.tm_hour >= 21 || lt.tm_hour < 7);
            }
            dial_power_set_night(night);
            // Swap the UI palette too, and force a re-render — screens read
            // PAL() from on_state, so a bare palette swap without a commit
            // would sit unapplied until the next unrelated state change.
            if (night != s_ui_night) {
                s_ui_night = night;
                dial_palette_set_night(night);
                dial_state_commit(mut_bump, NULL);
            }
        }

        // No command this tick. Resync only when quiet AND due — "due" being
        // sooner while we're still confirming a write the user just made.
        int64_t now = esp_timer_get_time();
        if (now - dial_state_last_input_us() < KNOB_SETTLE_US) continue;
        int64_t due = poll_confirms > 0 ? POLL_CONFIRM_US : POLL_INTERVAL_US;
        if (now - last_poll_us < due) continue;
        if (poll_confirms > 0) poll_confirms--;

        if (!dial_wifi_is_connected()) {
            // dial_net auto-reconnects; reflect the outage and wait it out.
            dial_state_set_phase(PH_WIFI_LOST, NULL);
            vTaskDelay(pdMS_TO_TICKS(1000));
            last_poll_us = esp_timer_get_time();
            continue;
        }

        if (with_auth_retry(poll_call, NULL, &disc, client_id)) {
            poll_failures = 0;
            dial_state_set_phase(PH_READY, NULL);
            ota_confirm_once();
        } else if (++poll_failures >= 3) {
            dial_state_set_phase(PH_DEGRADED, dial_mcp_last_error());
        }
        last_poll_us = esp_timer_get_time();

        // Sleep schedules change far less often than device state — piggyback
        // on this same quiet-idle gate, just at a much longer interval.
        if (esp_timer_get_time() - last_sched_us >= SCHED_INTERVAL_US) {
            with_auth_retry(sched_call, NULL, &disc, client_id);   // commits inside on success
            last_sched_us = esp_timer_get_time();
        }

        // Auto-check for firmware updates (M6): at most once per uptime-day,
        // and only CHECKS (never applies) -- the settings row just gets a
        // badge; the user still has to tap-confirm to install. Gated to a
        // window that can never coincide with sleep or an in-progress boost:
        // clock known, local hour in a daytime band, no zone mid-relief, and
        // steady state. Re-evaluated (cheaply) every idle tick once due, but
        // only actually calls dial_ota_check() -- and re-arms the 24h timer
        // -- once all of those hold.
        if (esp_timer_get_time() - last_ota_check_us >= OTA_AUTOCHECK_INTERVAL_US) {
            app_state_t ota_st;
            dial_state_get(&ota_st);
            struct tm ota_lt;
            bool in_window = ota_st.clock_valid && dial_time_now(&ota_lt) &&
                              ota_lt.tm_hour >= 10 && ota_lt.tm_hour < 16;
            bool relief_any = ota_st.zones[ZONE_A].relief_active ||
                               ota_st.zones[ZONE_B].relief_active;
            if (in_window && !relief_any && ota_st.phase == PH_READY) {
                dial_ota_check();
                commit_ota_snapshot();
                last_ota_check_us = esp_timer_get_time();
            }
        }
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
    dial_display_set_touch_filter(touch_filter);
    dial_power_start();

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
    // Onboarding (M4): "fresh" means no Wi-Fi creds were ever stored — read
    // BEFORE dial_net_seed()'s dev convenience below can inject any, so a
    // fresh-flashed dev build still exercises the real onboarding flow. A
    // user-requested network change also leaves NVS credential-less, but that
    // device is NOT fresh (it keeps its tokens, side, and prefs) — it should
    // land straight on the portal QR, not replay the welcome splash.
    bool fresh = !dial_net_have_creds() && !dial_net_setup_requested();
    dial_state_commit(mut_fresh_device, &fresh);
    dial_state_restore_prefs();   // last shown side + rotation (needs NVS, hence after net init)

    // Apply the saved rotation to the panel. The store only remembers it; the
    // display is what has to act on it.
    {
        app_state_t st;
        dial_state_get(&st);
        if (st.rotation && dial_display_lock(-1)) {
            dial_display_set_rotation(st.rotation);
            dial_display_unlock();
        }
    }
    dial_net_seed(WIFI_SSID, WIFI_PASSWORD);
    dial_state_commit(mut_ap_ssid, (void *)dial_net_ap_ssid());

    // OAuth/TLS/MCP need a big stack; everything network runs on this task.
    // Priority 3 and pinned to core 0 — BELOW the LVGL task (5, core 1), which
    // the user is actually looking at. It used to be 4, outranking the UI, so a
    // TLS handshake froze the screen and swallowed taps. Core 0 is where Wi-Fi
    // and lwIP already live, so the network work stays on the network core and
    // leaves the UI a core of its own.
    xTaskCreatePinnedToCore(worker_task, "worker", 16384, NULL, 3, NULL, 0);
}
