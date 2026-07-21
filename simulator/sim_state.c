/*
 * sim_state.c — from-scratch, single-threaded stand-in for
 * components/dial_state/dial_state.c (which drags in a FreeRTOS mutex,
 * esp_timer, and nvs_flash — none of which exist on the host).
 *
 * Implements exactly the dial_state.h entry points the compiled dial_ui
 * screens call (verified against every scr_*.c + ui_router.c): dial_state_get,
 * set_ui_temp, set_zone_on, set_ui_zone, set_welcomed, set_side_picked,
 * set_units_c, set_haptics_enabled, set_rotation, set_wifi_join,
 * clear_wifi_join_failed, set_phase, stamp_input, and dial_cmd_post (a
 * logging no-op — there is no worker task here to drain the queue).
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
    s_state.wifi_join_idx = -1;
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

void dial_state_set_haptics_enabled(bool enabled)
{
    s_state.haptics_enabled = enabled;
    s_state.generation++;
}

void dial_state_set_rotation(uint8_t quarters)
{
    s_state.rotation = quarters;
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
    static const char *KIND[] = {
        "SET_TEMP", "TOGGLE_ON", "BOOST_START", "BOOST_CANCEL", "BED_OFF",
        "AWAY", "MATCH_PARTNER", "TONIGHT_OVERRIDE", "TONIGHT_REVERT",
        "RELINK", "WIFI_RESET", "FACTORY_RESET", "OTA_CHECK", "OTA_APPLY",
    };
    const char *k = (cmd->kind >= 0 && (size_t)cmd->kind < sizeof(KIND) / sizeof(KIND[0]))
                        ? KIND[cmd->kind] : "?";
    printf("[cmd] %s zone=%d a=%d b=%d temp_f=%d\n", k, cmd->zone, cmd->a, cmd->b, cmd->temp_f);
}
