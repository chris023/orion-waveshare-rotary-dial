/*
 * sim_state.c — from-scratch, single-threaded stand-in for
 * components/dial_state/dial_state.c (which drags in a FreeRTOS mutex,
 * esp_timer, and nvs_flash — none of which exist on the host).
 *
 * Implements exactly the dial_state.h entry points the compiled dial_ui
 * screens call (verified against every scr_*.c + ui_router.c): dial_state_get,
 * set_ui_temp, set_zone_on, set_ui_zone, set_welcomed, set_side_picked,
 * set_units_c, set_rel_mode, set_haptics_level, set_rotation, set_wifi_join,
 * clear_wifi_join_failed, set_phase, stamp_input, get/set_bri_day_pct,
 * get/set_bri_night_pct, get/set_bri_night_clock_pct,
 * get/set_screen_timeout_s, set_beta,
 * set_sched_follow, set_ota_auto,
 * set_ota_defer, set_ota_skip, clear_ota_prompt_due, and dial_cmd_post (a
 * logging no-op — there is no worker task here to drain the queue).
 * set_ota_shown is deliberately NOT here: only main.c's worker calls it
 * (docs/SPEC-update-prompt.md's idle-loop gate evaluation), and the
 * simulator has no worker task / doesn't link main.c at all.
 *
 * No mutex: this whole simulator is one thread pumping the LVGL tick, so
 * "under the store mutex" collapses to "just mutate the global."
 */
#include <string.h>
#include <stdio.h>
#include "sim_state.h"

static app_state_t s_state;

app_state_t *sim_state_ptr(void) { return &s_state; }

void sim_state_reset(void)
{
    memset(&s_state, 0, sizeof(s_state));
    s_state.ui_temp_f[ZONE_A] = -1;
    s_state.ui_temp_f[ZONE_B] = -1;
    // Matches dial_state_init()'s real-firmware default: -1 = "Holding" on
    // scr_dial.c's status pill (§3). There is no worker task here to derive
    // this from a schedule (compute_hold_until_min lives in main.c, which
    // the simulator never links) — scenarios that want "Until H:MM" poke it
    // directly, same as every other worker-computed field this file seeds
    // by hand (see this file's own header comment).
    s_state.zones[ZONE_A].hold_until_min = -1;
    s_state.zones[ZONE_B].hold_until_min = -1;
    // Matches dial_state_init(): -1 = "not yet discovered", so
    // dial_state_temp_min_f()/_max_f() fall back to DIAL_TEMP_MIN_F/MAX_F
    // (now the real 50-113F rails themselves) — 0 would be misread as a
    // discovered-but-degenerate range, not "unknown", so this can't be left
    // to the memset above.
    s_state.temp_min_f = -1;
    s_state.temp_max_f = -1;
    s_state.wifi_join_idx = -1;
    // Fresh-device defaults, matching dial_state_init exactly so screenshots
    // show what a new dial actually ships with.
    s_state.bri_day_pct = 30;
    s_state.bri_night_pct = 0;
    s_state.bri_night_clock_pct = 20;   // its own default, not derived from night
    s_state.screen_timeout_s = 90;  // matches dial_state_init's default; the row
                                   // renders it as "1m", the nearest offered choice
    s_state.haptics_level = 1;    // HAPTIC_LEVEL_LOW — matches dial_state_init's default
    s_state.sched_follow = true;  // "Dial adjusts" default — matches dial_state_init's default
    s_state.ota_auto = 0;         // Off — matches dial_state_init's fresh-device default
    // ota_defer/ota_shown/ota_skip/ota_prompt_due all default to 0/""/false
    // via the memset above, same as dial_state_init.
    s_state.generation = 1;
}

void dial_state_get(app_state_t *out) { *out = s_state; }

void dial_state_set_ui_temp(zone_idx_t zone, int temp_f)
{
    s_state.ui_temp_f[zone] = temp_f;
    s_state.generation++;
}

void dial_state_set_zone_on(zone_idx_t zone, bool on)
{
    s_state.zones[zone].on = on;
    s_state.generation++;
}

void dial_state_set_ui_zone(zone_idx_t zone)
{
    s_state.ui_zone = zone;
    s_state.generation++;
}

void dial_state_set_welcomed(void)
{
    s_state.welcomed = true;
    s_state.generation++;
}

void dial_state_set_side_picked(void)
{
    s_state.side_picked = true;
    s_state.generation++;
}

void dial_state_set_units_c(bool units_c)
{
    s_state.units_c = units_c;
    s_state.generation++;
}

void dial_state_set_rel_mode(bool rel_mode)
{
    s_state.rel_mode = rel_mode;
    s_state.generation++;
}

// Boost-duration pref (NVS-backed on hardware, default 15 min); the sim keeps it
// in memory so the boost screen and rail-push gesture read a stable value.
static uint8_t s_boost_minutes = 15;
uint8_t dial_state_get_boost_minutes(void) { return s_boost_minutes; }
void dial_state_set_boost_minutes(uint8_t minutes) { s_boost_minutes = minutes; s_state.generation++; }

// Optimistic relief write: the simulator has no worker to reconcile against,
// so this just mutates the store the same way the firmware's does.
void dial_state_set_relief_optimistic(int zone, bool active, bool heat, int64_t end_ms)
{
    for (int z = 0; z < ZONE_COUNT; z++) {
        if (zone >= 0 && z != zone) continue;
        if (!active && s_state.zones[z].relief_active) {
            s_state.zones[z].on     = s_state.zones[z].relief_prev_on;
            s_state.zones[z].temp_c = s_state.zones[z].relief_prev_temp_c;
        }
        s_state.zones[z].relief_active = active;
        s_state.zones[z].relief_heat   = heat;
        s_state.zones[z].relief_end_ms = active ? end_ms : 0;
    }
    s_state.generation++;
}

void dial_state_set_haptics_level(uint8_t level)
{
    s_state.haptics_level = level;
    s_state.generation++;
}

void dial_state_set_rotation(uint8_t quarters)
{
    s_state.rotation = quarters;
    s_state.generation++;
}

uint8_t dial_state_get_bri_day_pct(void) { return s_state.bri_day_pct; }
uint8_t dial_state_get_bri_night_pct(void) { return s_state.bri_night_pct; }

void dial_state_set_bri_day_pct(uint8_t pct)
{
    s_state.bri_day_pct = pct;
    s_state.generation++;
}

void dial_state_set_bri_night_pct(uint8_t pct)
{
    s_state.bri_night_pct = pct;
    s_state.generation++;
}

uint8_t dial_state_get_bri_night_clock_pct(void) { return s_state.bri_night_clock_pct; }

void dial_state_set_bri_night_clock_pct(uint8_t pct)
{
    s_state.bri_night_clock_pct = pct;
    s_state.generation++;
}

uint16_t dial_state_get_screen_timeout_s(void) { return s_state.screen_timeout_s; }

void dial_state_set_screen_timeout_s(uint16_t seconds)
{
    s_state.screen_timeout_s = seconds;
    s_state.generation++;
}

void dial_state_set_beta(bool enabled)
{
    s_state.beta = enabled;
    s_state.generation++;
}

void dial_state_set_sched_follow(bool follow)
{
    s_state.sched_follow = follow;
    s_state.generation++;
}

void dial_state_set_ota_auto(uint8_t mode)
{
    s_state.ota_auto = (mode <= 1) ? mode : 0;
    s_state.generation++;
}

void dial_state_set_ota_defer(uint32_t epoch)
{
    s_state.ota_defer = epoch;
    s_state.generation++;
}

void dial_state_set_ota_skip(const char *version)
{
    strncpy(s_state.ota_skip, version ? version : "", sizeof(s_state.ota_skip) - 1);
    s_state.ota_skip[sizeof(s_state.ota_skip) - 1] = '\0';
    s_state.generation++;
}

void dial_state_clear_ota_prompt_due(void)
{
    s_state.ota_prompt_due = false;
    s_state.generation++;
}

void dial_state_set_battery(int mv, int pct, bool charging)
{
    // No ADC here, so scenarios set the fields through sim_state_ptr()
    // directly. This exists so the link resolves and so a scenario CAN drive
    // it the way the firmware does if it wants to.
    s_state.batt_mv       = mv;
    s_state.batt_pct      = pct;
    s_state.batt_charging = charging;
    s_state.generation++;
}

void dial_state_set_wifi_join(int idx, const char *ssid)
{
    s_state.wifi_join_idx = (int8_t)idx;
    if (ssid) {
        strncpy(s_state.wifi_join_ssid, ssid, sizeof(s_state.wifi_join_ssid) - 1);
        s_state.wifi_join_ssid[sizeof(s_state.wifi_join_ssid) - 1] = '\0';
    }
    s_state.wifi_join_failed = false;
    s_state.generation++;
}

void dial_state_clear_wifi_join_failed(void)
{
    s_state.wifi_join_failed = false;
    s_state.generation++;
}

void dial_state_set_phase(conn_phase_t phase, const char *err)
{
    s_state.phase = phase;
    if (err) {
        strncpy(s_state.phase_err, err, sizeof(s_state.phase_err) - 1);
        s_state.phase_err[sizeof(s_state.phase_err) - 1] = '\0';
    }
    s_state.generation++;
}

void dial_state_stamp_input(void)
{
    // No quiet-period gate to feed here — there's no polling worker in the
    // simulator whose resync this would ever unblock. Kept as a no-op entry
    // point purely so callers (ui_router.c, scr_dial.c) link.
}

void dial_cmd_post(const app_cmd_t *cmd)
{
    // Kept aligned by hand with dial_state.h's cmd_kind_t ordinals — this is
    // a debug label only (dial_cmd_post has no worker to actually drain into
    // here), but a stale table silently prints the wrong name forever, which
    // is how this one drifted after CMD_MATCH_PARTNER's removal (§4).
    static const char *KIND[] = {
        "SET_TEMP", "TOGGLE_ON", "BOOST_START", "BOOST_CANCEL", "BED_OFF",
        "AWAY", "RELINK", "WIFI_RESET", "FACTORY_RESET", "OTA_CHECK",
        "OTA_APPLY", "OTA_CLEAR_FAILED",
    };
    const char *k = (cmd->kind >= 0 && (size_t)cmd->kind < sizeof(KIND) / sizeof(KIND[0]))
                        ? KIND[cmd->kind] : "?";
    printf("[cmd] %s zone=%d a=%d b=%d temp_f=%d\n", k, cmd->zone, cmd->a, cmd->b, cmd->temp_f);
}
