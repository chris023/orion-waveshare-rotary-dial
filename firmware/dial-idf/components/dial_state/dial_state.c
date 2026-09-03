#include "dial_state.h"

#include <math.h>

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "nvs.h"

#define NVS_NS "ui"

// Shared range clamp for the two brightness prefs — used on both read
// (defends against a corrupt/out-of-range NVS byte) and write (defends a
// caller that didn't already clamp, e.g. a future settings-screen bug).
// 0 is a legal Day/Night value now: on the current curve it means "dimmest
// this firmware produces", not black (dial_power's tier_duty). The old floor
// of 10 existed to keep the in-use screen legible, and tier_duty enforces
// that in duty space instead -- clamping here would only re-break the
// rescaled values the migration just produced.
static inline uint8_t clamp_bri_pct(uint8_t pct)
{
    return pct > 100 ? 100 : pct;
}

// Same defensive style, for the haptics level. Mirrors dial_haptics.h's
// haptic_level_t (0=Off, 1=Auto, 2=Low, 3=High — see app_state_t.
// haptics_level's comment). An out-of-range byte (corrupt NVS, or a future
// firmware's level this build predates) clamps to Low, not Off: silently
// going quiet is the wrong failure mode for a physical-feedback preference,
// and Low is this pref's own default besides.
static inline uint8_t clamp_haptics_level(uint8_t level)
{
    return (level <= 3) ? level : 1;
}

static inline uint8_t clamp_boost_minutes(uint8_t minutes)
{
    return (minutes >= 5 && minutes <= 120) ? minutes : 15;
}

// Defends the screen-timeout pref on read/write. A legal stored value is
// EITHER 90 (the legacy hardcoded threshold this pref defaults to — see
// app_state_t.screen_timeout_s) OR one of the five values
// DIAL_SCR_TIMEOUT_CHOICES (dial_state.h) that the Settings row can actually
// produce. Anything else (corrupt NVS byte, a future firmware's choice this
// build predates) falls back to 90 — deliberately NOT the nearest choice,
// which would silently change an already-configured timeout instead of
// falling back to the one value this build has always understood.
static inline uint16_t clamp_screen_timeout_s(uint16_t s)
{
    if (s == 90) return s;
    for (int i = 0; i < DIAL_SCR_TIMEOUT_CHOICES_N; i++)
        if (DIAL_SCR_TIMEOUT_CHOICES[i] == s) return s;
    return 90;
}

static SemaphoreHandle_t s_mux;
static QueueHandle_t     s_cmd_q;
static app_state_t       s_state;

// Written from LVGL task (touch), knob decoder task, and worker; read from the
// worker's poll gate. 64-bit stores tear on Xtensa, so guard with a SPINLOCK,
// not a mutex: the knob decoder runs in the shared esp_timer task (which also
// dispatches lv_tick_inc) and must never block on a held mutex.
static portMUX_TYPE s_input_mux = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_last_input_us;

void dial_state_init(void)
{
    s_mux = xSemaphoreCreateMutex();
    s_cmd_q = xQueueCreate(16, sizeof(app_cmd_t));
    configASSERT(s_mux && s_cmd_q);
    memset(&s_state, 0, sizeof(s_state));
    s_state.phase = PH_BOOT;
    s_state.wifi_join_idx = -1;   // no on-device join attempted yet
    s_state.haptics_level = 1;        // HAPTIC_LEVEL_LOW — the default feel (see dial_haptics.h)
    s_state.boost_minutes = 15;
    // Fresh-device brightness defaults, chosen by the owner after living with
    // the dial on a nightstand (2026-07-29): full brightness is uncomfortable
    // in a bedroom, and a device that arrives glaring is worse than one that
    // arrives dim. 0 for night is NOT off — on the current curve it is the
    // dimmest legible duty (see dial_power's tier_duty); the clock's 20 IS on
    // its own 0-means-off curve, a faint but readable glow across a room.
    s_state.bri_day_pct   = 30;
    s_state.bri_night_pct = 0;
    s_state.bri_night_clock_pct = 20;  // fresh device only,
                                        // so the clock and in-use night duty read
                                        // identically until the user changes one
                                        // — restore_prefs below is what makes an
                                        // UPGRADING device's default something
                                        // other than this plain 100
    s_state.screen_timeout_s = 90;    // matches the old hardcoded STANDBY_AFTER_US
                                        // exactly (not one of the five UI choices —
                                        // see app_state_t.screen_timeout_s), so
                                        // shipping this pref changes no device's
                                        // behavior until the user taps the row
    s_state.beta          = false;    // fresh-device default: stable channel only
    s_state.sched_follow  = true;     // fresh-device default: Follow schedule (owner decision)
    s_state.ota_auto      = 0;        // fresh-device default: Off (explicit consent required)
    // ota_defer/ota_shown/ota_skip/ota_prompt_due all default to 0/""/false
    // via the memset above -- 0 is exactly "no defer pending" and "never
    // shown" for the two epoch fields, so no explicit seed needed here.
    // Fresh-device default: relative scale (owner decision). Seeded here, in
    // RAM, exactly like haptics_level above — restore_prefs opens NVS
    // read-only and early-returns when the "ui" namespace doesn't exist yet
    // (a factory-fresh device), so a default that lived only there would never
    // apply. An upgrading device overrides this back to absolute in
    // restore_prefs when it finds a pre-existing "zone" key but no "relmode".
    s_state.rel_mode = true;
    // Device temperature range unknown until orion_discover_device() parses
    // list_devices' temperature_range -- dial_state_temp_min_f()/_max_f()
    // fall back to the DIAL_TEMP_MIN_F/MAX_F constants while these are -1.
    s_state.temp_min_f = -1;
    s_state.temp_max_f = -1;
    for (int z = 0; z < ZONE_COUNT; z++) {
        s_state.ui_temp_f[z]         = -1;
        s_state.zones[z].actual_c    = -1.0f;
        s_state.zones[z].hold_until_min = -1;   // "Holding" until the worker's first idle tick says otherwise
    }
}

void dial_state_restore_prefs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t zone = 0, units = 0, haptics = 1 /* HAPTIC_LEVEL_LOW */, rot = 0, relmode = 1;
    uint8_t boost_minutes = 15;
    uint8_t bri_day = 100, bri_night = 100, bri_nclk = 100;
    uint8_t beta = 0;
    uint8_t sched_follow = 1;   // matches init's fresh-device default (Follow)
    uint8_t  ota_auto = 0;
    uint32_t ota_defer = 0, ota_shown = 0;
    char     ota_skip[16] = "";   // matches app_state_t.ota_skip's size
    bool have_zone    = nvs_get_u8(h, "zone", &zone) == ESP_OK && zone < ZONE_COUNT;
    bool have_units   = nvs_get_u8(h, "units", &units) == ESP_OK;
    bool have_haptics = nvs_get_u8(h, "haptics", &haptics) == ESP_OK;
    bool have_boost   = nvs_get_u8(h, "boost_min", &boost_minutes) == ESP_OK;
    bool have_rot     = nvs_get_u8(h, "rot", &rot) == ESP_OK && rot < 4;
    bool have_rel     = nvs_get_u8(h, "relmode", &relmode) == ESP_OK;
    // v1.2.x moved Day/Night from a 10-100 slider to 0-100, where 0 means
    // "the dimmest this firmware ever produced" rather than black (see
    // dial_power's tier_duty). Same physical range, different labelling, so
    // stored values must be rescaled: p' = (p - 10) * 100 / 90. That is
    // EXACTLY appearance-preserving -- both curves are linear in percent
    // through the same endpoints. New keys rather than rewriting the old
    // ones in place, so the rescale cannot run twice on successive boots and
    // walk a user's setting down to nothing.
    uint8_t bri_day2 = 100, bri_night2 = 100;
    bool have_bri_day   = nvs_get_u8(h, "bri_day2", &bri_day2) == ESP_OK;
    bool have_bri_night = nvs_get_u8(h, "bri_nite2", &bri_night2) == ESP_OK;
    bool old_day   = !have_bri_day   && nvs_get_u8(h, "bri_day", &bri_day) == ESP_OK;
    bool old_night = !have_bri_night && nvs_get_u8(h, "bri_night", &bri_night) == ESP_OK;
    if (have_bri_day)   bri_day   = bri_day2;
    else if (old_day)   bri_day   = (uint8_t)((bri_day   > 10 ? (bri_day   - 10) * 100 / 90 : 0));
    if (have_bri_night) bri_night = bri_night2;
    else if (old_night) bri_night = (uint8_t)((bri_night > 10 ? (bri_night - 10) * 100 / 90 : 0));
    have_bri_day   = have_bri_day   || old_day;
    have_bri_night = have_bri_night || old_night;
    bool have_bri_nclk  = nvs_get_u8(h, "bri_nclk", &bri_nclk) == ESP_OK;
    uint16_t scr_to = 90;   // matches init's fresh-device default
    bool have_scr_to    = nvs_get_u16(h, "scr_to", &scr_to) == ESP_OK;
    bool have_beta      = nvs_get_u8(h, "beta", &beta) == ESP_OK;
    bool have_sched_follow = nvs_get_u8(h, "sched_follow", &sched_follow) == ESP_OK;
    bool have_ota_auto  = nvs_get_u8(h, "ota_auto", &ota_auto) == ESP_OK;
    bool have_ota_defer = nvs_get_u32(h, "ota_defer", &ota_defer) == ESP_OK;
    bool have_ota_shown = nvs_get_u32(h, "ota_shown", &ota_shown) == ESP_OK;
    size_t ota_skip_sz  = sizeof(ota_skip);
    bool have_ota_skip  = nvs_get_str(h, "ota_skip", ota_skip, &ota_skip_sz) == ESP_OK;
    nvs_close(h);
    if (!have_zone && !have_units && !have_haptics && !have_boost && !have_rot && !have_rel
        && !have_bri_day && !have_bri_night && !have_bri_nclk && !have_scr_to && !have_beta
        && !have_sched_follow
        && !have_ota_auto && !have_ota_defer && !have_ota_shown && !have_ota_skip) return;

    xSemaphoreTake(s_mux, portMAX_DELAY);
    if (have_zone) {
        s_state.ui_zone     = (zone_idx_t)zone;
        // The "zone" key's mere existence means some earlier session already
        // established a default side — including upgrades from before
        // side_picked existed, so those devices never re-run SCR_SIDEPICK.
        s_state.side_picked = true;
    }
    if (have_units)   s_state.units_c      = (units != 0);
    // No 0/1 -> bool translation needed: the "haptics" key's stored byte
    // already IS the new haptic_level_t encoding (0=Off, 1=Auto) for every
    // device that has ever written it — 1 ("on") already meant "the
    // adaptive day/night behavior" — so a legacy value lands on exactly the
    // right level with zero migration code. clamp defends only against a
    // corrupt byte or a future level this build predates.
    if (have_haptics) s_state.haptics_level = clamp_haptics_level(haptics);
    if (have_boost) s_state.boost_minutes = clamp_boost_minutes(boost_minutes);
    if (have_rot)     s_state.rotation      = rot;
    // Temperature scale. If we've ever persisted it, honor it. Otherwise, if
    // this device was set up before this release (has a "zone" key but no
    // "relmode"), keep it on ABSOLUTE — an unattended OTA must not change what
    // the big number means for a user who's read °F every night. A truly fresh
    // device has neither key and keeps init's relative default.
    if (have_rel)       s_state.rel_mode = (relmode != 0);
    else if (have_zone) s_state.rel_mode = false;
    if (have_bri_day)   s_state.bri_day_pct   = clamp_bri_pct(bri_day);
    if (have_bri_night) s_state.bri_night_pct = clamp_bri_pct(bri_night);
    // Night-clock brightness migration. bri_night_clock_pct did not exist
    // before this pref shipped, so no device in the field has a "bri_nclk"
    // key yet. If we simply defaulted an absent key to 100 (like a genuinely
    // fresh device), an existing user's screensaver clock would visibly
    // BRIGHTEN the instant they upgrade: e.g. someone who already turned
    // bri_night_pct down to 50% currently sees the standby duty computed as
    // scale_duty(DUTY_NIGHT.standby, 50%, FLOOR_STANDBY) every night; a bare
    // 100% night-clock default would jump that to scale_duty(..., 100%, ...)
    // — brighter, unrequested, and only noticed at 2am. So on an absent key
    // we instead seed bri_night_clock_pct from the (possibly also just-
    // restored, above) bri_night_pct value: the migration reproduces
    // tonight's exact clock duty, and the user can pull the two apart from
    // there if they want. A brand-fresh device takes neither branch: it has
    // no night key either, so it keeps dial_state_init's chosen defaults.
    if (have_bri_nclk) {
        s_state.bri_night_clock_pct = (bri_nclk <= 100) ? bri_nclk : 100;
    } else if (have_bri_night) {
        // Only an UPGRADING device (one that already had a night brightness
        // stored) gets the migration below. A fresh device has no night key
        // either, and must keep dial_state_init's chosen default instead of
        // inheriting a value computed from a placeholder. For an upgrading
        // device, seed the percent that reproduces the duty it is ALREADY
        // showing, so an update never changes the glow in someone's bedroom
        // unasked.
        //
        // Before this setting existed the night clock ran at
        // max(FLOOR_STANDBY 4, DUTY_NIGHT.standby 6 * bri_night / 100). The
        // new pref maps percent -> duty on a quadratic curve topping out at
        // 64 (dial_power_night_clock_duty), so invert it: pct = 100 *
        // sqrt(duty / 64). Duty 6 (a device at 100% night) lands on 31%;
        // duty 4 (anyone at 50% or below) lands on 25%. Deliberately NOT
        // copied from bri_night_pct -- these are now different scales, and
        // 100 on the new curve is ten times the old brightness.
        int old_duty = (6 * (int)s_state.bri_night_pct) / 100;
        if (old_duty < 4) old_duty = 4;
        s_state.bri_night_clock_pct = (uint8_t)lroundf(100.0f * sqrtf((float)old_duty / 64.0f));
    }
    if (have_scr_to)     s_state.screen_timeout_s = clamp_screen_timeout_s(scr_to);
    if (have_beta)       s_state.beta         = (beta != 0);
    if (have_sched_follow) s_state.sched_follow = (sched_follow != 0);
    if (have_ota_auto)   s_state.ota_auto  = (ota_auto <= 1) ? ota_auto : 0;
    if (have_ota_defer)  s_state.ota_defer = ota_defer;
    if (have_ota_shown)  s_state.ota_shown = ota_shown;
    if (have_ota_skip)   strlcpy(s_state.ota_skip, ota_skip, sizeof(s_state.ota_skip));
    s_state.generation++;
    xSemaphoreGive(s_mux);
}

// Screen rotation, in quarter turns clockwise. Persisted here; actually applied
// by dial_display_set_rotation (this store doesn't know about the panel).
void dial_state_set_rotation(uint8_t quarters)
{
    quarters &= 3;
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_state.rotation = quarters;
    s_state.generation++;
    xSemaphoreGive(s_mux);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "rot", quarters);
        nvs_commit(h);
        nvs_close(h);
    }
}

/*
 * thermal_state is device truth and only lands on a poll, so anything the user
 * does — switching a zone on, turning the knob past the water temperature —
 * would leave the arc and pill rendering the OLD state until the next read.
 * Predict it from the two things already known: where the water is, and where
 * they just asked it to go. The next poll overwrites this with the truth, so a
 * wrong guess self-corrects within seconds.
 *
 * Uses the optimistic target (ui_temp_f) when one is pending, because that is
 * what the user is looking at — not the setpoint the device has been told about
 * so far. Caller must hold the lock.
 */
void dial_state_predict_thermal(app_state_t *st, zone_idx_t zone)
{
    zone_state_t *zs = &st->zones[zone];
    const char *s;
    if (!zs->on)               s = "standby";
    else if (zs->actual_c < 0) s = "holding";   // nothing measured to compare against
    else {
        float target_c = (st->ui_temp_f[zone] >= 0) ? dial_f_to_c(st->ui_temp_f[zone])
                                                    : zs->temp_c;
        float delta = target_c - zs->actual_c;
        s = (delta > 0.5f) ? "heating" : (delta < -0.5f) ? "cooling" : "holding";
    }
    strlcpy(zs->thermal_state, s, sizeof(zs->thermal_state));
}

void dial_state_set_ui_temp(zone_idx_t zone, int temp_f)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_state.ui_temp_f[zone] = temp_f;
    dial_state_predict_thermal(&s_state, zone);   // the pill follows the knob, not the poll
    s_state.generation++;
    xSemaphoreGive(s_mux);
}

// Optimistic power flip, committed by the UI the instant the disc is tapped.
// The worker used to be the only one to set this — but it commits only AFTER
// the write to Orion returns, so the face sat unchanged for a whole TLS round
// trip while the user waited on a button they had already pressed.
void dial_state_set_zone_on(zone_idx_t zone, bool on)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_state.zones[zone].on = on;
    dial_state_predict_thermal(&s_state, zone);
    s_state.generation++;
    xSemaphoreGive(s_mux);
}

void dial_state_set_wifi_join(int idx, const char *ssid)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_state.wifi_join_idx = (int8_t)idx;
    strlcpy(s_state.wifi_join_ssid, ssid ? ssid : "", sizeof(s_state.wifi_join_ssid));
    s_state.wifi_join_failed = false;      // a fresh attempt is not a failed one
    s_state.generation++;
    xSemaphoreGive(s_mux);
}

void dial_state_set_wifi_join_failed(void)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_state.wifi_join_failed = true;
    s_state.generation++;
    xSemaphoreGive(s_mux);
}

void dial_state_clear_wifi_join_failed(void)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_state.wifi_join_failed = false;
    s_state.generation++;
    xSemaphoreGive(s_mux);
}

void dial_state_set_ui_zone(zone_idx_t zone)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    bool changed = (s_state.ui_zone != zone);
    s_state.ui_zone = zone;
    xSemaphoreGive(s_mux);
    // No generation bump: the caller is the screen that already navigated —
    // re-rendering here would race the transition it just started.

    // Last side chosen survives reboot (owner D3). Writes are rare (one per
    // swipe) and this runs in the LVGL task, where a ~ms NVS write is fine.
    if (changed) {
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_u8(h, "zone", (uint8_t)zone);
            nvs_commit(h);
            nvs_close(h);
        }
    }
}

void dial_state_set_welcomed(void)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_state.welcomed = true;
    s_state.generation++;
    xSemaphoreGive(s_mux);
}

void dial_state_set_side_picked(void)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_state.side_picked = true;
    bool rel = s_state.rel_mode;
    s_state.generation++;
    xSemaphoreGive(s_mux);

    // Onboarding runs only on a fresh device (nav_policy gates SCR_SIDEPICK on
    // !side_picked, and an upgrade restores side_picked from the existing
    // "zone" key so it never reaches here). Persisting "relmode" now is what
    // lets a fresh device's relative default survive a reboot even if the user
    // never opens Settings — otherwise its second boot would see a "zone" key
    // (written by the side pick) but no "relmode" and wrongly apply the
    // upgrade→absolute rule in dial_state_restore_prefs. On an upgrade this
    // code is unreachable, so a genuine ≤v1.0.6 device still lands on absolute.
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        uint8_t existing;
        if (nvs_get_u8(h, "relmode", &existing) != ESP_OK) {
            nvs_set_u8(h, "relmode", rel ? 1 : 0);
            nvs_commit(h);
        }
        nvs_close(h);
    }
}

void dial_state_set_units_c(bool units_c)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_state.units_c = units_c;
    s_state.generation++;
    xSemaphoreGive(s_mux);

    // Settings changes are rare taps in the LVGL task — a ~ms NVS write here
    // is fine (same reasoning as dial_state_set_ui_zone above).
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "units", units_c ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

void dial_state_set_rel_mode(bool rel_mode)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_state.rel_mode = rel_mode;
    s_state.generation++;
    xSemaphoreGive(s_mux);

    // Rare Settings tap in the LVGL task — a ~ms NVS write here is fine (same
    // reasoning as dial_state_set_units_c above). This is also what pins an
    // upgraded device's scale once the user has opted in, so restore_prefs
    // honors it over the upgrade→absolute default from then on.
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "relmode", rel_mode ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

void dial_state_set_relief_optimistic(int zone, bool active, bool heat, int64_t end_ms)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    for (int z = 0; z < ZONE_COUNT; z++) {
        if (zone >= 0 && z != zone) continue;
        if (active && !s_state.zones[z].relief_active) {
            // Capture what the boost is about to displace, at the instant it
            // starts. Without this, relief_prev_on held whatever the last
            // get_device_state poll left (false on a zone that has never had
            // relief this boot), and cancelling before the first poll landed
            // restored that stale value — switching a bed that was ON to OFF.
            // Doing it here covers BOTH callers, the LVGL task and the worker.
            s_state.zones[z].relief_prev_on     = s_state.zones[z].on;
            // Prefer the pending optimistic target over the setpoint the device
            // has been told about so far, the same way predict_thermal does:
            // a rail-push boost restores the settled setpoint (scr_dial's
            // s_rail_from_f) by posting it immediately before the boost, so at
            // this instant that write is still in flight and temp_c is the rail
            // the gesture walked across. Capturing temp_c here would undo the
            // restore for anyone who cancels before the server's ack lands.
            s_state.zones[z].relief_prev_temp_c =
                (s_state.ui_temp_f[z] >= 0) ? dial_f_to_c(s_state.ui_temp_f[z])
                                            : s_state.zones[z].temp_c;
        }
        if (!active && s_state.zones[z].relief_active) {
            // Cancelling restores what the relief displaced. Without this the
            // zone still looks ON for one render and the chip flashes
            // "Holding" before settling — exactly what the owner saw.
            s_state.zones[z].on     = s_state.zones[z].relief_prev_on;
            s_state.zones[z].temp_c = s_state.zones[z].relief_prev_temp_c;
        }
        s_state.zones[z].relief_active  = active;
        s_state.zones[z].relief_heat    = heat;
        s_state.zones[z].relief_end_ms  = active ? end_ms : 0;
        s_state.zones[z].relief_opt_us  = esp_timer_get_time();
    }
    s_state.generation++;
    xSemaphoreGive(s_mux);
}

uint8_t dial_state_get_boost_minutes(void)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    uint8_t v = clamp_boost_minutes(s_state.boost_minutes);
    xSemaphoreGive(s_mux);
    return v;
}

void dial_state_set_boost_minutes(uint8_t minutes)
{
    minutes = clamp_boost_minutes(minutes);
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_state.boost_minutes = minutes;
    s_state.generation++;
    xSemaphoreGive(s_mux);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "boost_min", minutes);
        nvs_commit(h);
        nvs_close(h);
    }
}

void dial_state_set_haptics_level(uint8_t level)
{
    level = clamp_haptics_level(level);
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_state.haptics_level = level;
    s_state.generation++;
    xSemaphoreGive(s_mux);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "haptics", level);
        nvs_commit(h);
        nvs_close(h);
    }
}

// Cheap single-field reads (mutex held only long enough to copy one byte) —
// dial_power's power_task calls these from its 100ms apply tick, and a full
// dial_state_get() snapshot (the app_state_t struct is large, e.g. the 600B
// oauth_url) would be needless work on that hot-ish loop.
uint8_t dial_state_get_bri_day_pct(void)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    uint8_t v = s_state.bri_day_pct;
    xSemaphoreGive(s_mux);
    return v;
}

uint8_t dial_state_get_bri_night_pct(void)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    uint8_t v = s_state.bri_night_pct;
    xSemaphoreGive(s_mux);
    return v;
}

void dial_state_set_bri_day_pct(uint8_t pct)
{
    pct = clamp_bri_pct(pct);
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_state.bri_day_pct = pct;
    s_state.generation++;
    xSemaphoreGive(s_mux);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "bri_day2", pct);
        nvs_commit(h);
        nvs_close(h);
    }
}

void dial_state_set_bri_night_pct(uint8_t pct)
{
    pct = clamp_bri_pct(pct);
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_state.bri_night_pct = pct;
    s_state.generation++;
    xSemaphoreGive(s_mux);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "bri_nite2", pct);
        nvs_commit(h);
        nvs_close(h);
    }
}

uint8_t dial_state_get_bri_night_clock_pct(void)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    uint8_t v = s_state.bri_night_clock_pct;
    xSemaphoreGive(s_mux);
    return v;
}

void dial_state_set_bri_night_clock_pct(uint8_t pct)
{
    pct = clamp_bri_pct(pct);
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_state.bri_night_clock_pct = pct;
    s_state.generation++;
    xSemaphoreGive(s_mux);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "bri_nclk", pct);
        nvs_commit(h);
        nvs_close(h);
    }
}

uint16_t dial_state_get_screen_timeout_s(void)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    uint16_t v = s_state.screen_timeout_s;
    xSemaphoreGive(s_mux);
    return v;
}

void dial_state_set_screen_timeout_s(uint16_t seconds)
{
    seconds = clamp_screen_timeout_s(seconds);
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_state.screen_timeout_s = seconds;
    s_state.generation++;
    xSemaphoreGive(s_mux);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u16(h, "scr_to", seconds);
        nvs_commit(h);
        nvs_close(h);
    }
}

bool dial_state_get_beta(void)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    bool v = s_state.beta;
    xSemaphoreGive(s_mux);
    return v;
}

void dial_state_set_beta(bool enabled)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_state.beta = enabled;
    s_state.generation++;
    xSemaphoreGive(s_mux);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "beta", enabled ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

bool dial_state_get_sched_follow(void)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    bool v = s_state.sched_follow;
    xSemaphoreGive(s_mux);
    return v;
}

void dial_state_set_sched_follow(bool follow)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_state.sched_follow = follow;
    s_state.generation++;
    xSemaphoreGive(s_mux);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "sched_follow", follow ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

void dial_state_set_ota_auto(uint8_t mode)
{
    mode = (mode <= 1) ? mode : 0;   // defend against a caller passing garbage; 0=Off is the safe fallback
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_state.ota_auto = mode;
    s_state.generation++;
    xSemaphoreGive(s_mux);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "ota_auto", mode);
        nvs_commit(h);
        nvs_close(h);
    }
}

void dial_state_set_ota_defer(uint32_t epoch)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_state.ota_defer = epoch;
    s_state.generation++;
    xSemaphoreGive(s_mux);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, "ota_defer", epoch);
        nvs_commit(h);
        nvs_close(h);
    }
}

void dial_state_set_ota_skip(const char *version)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    strlcpy(s_state.ota_skip, version ? version : "", sizeof(s_state.ota_skip));
    s_state.generation++;
    xSemaphoreGive(s_mux);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "ota_skip", version ? version : "");
        nvs_commit(h);
        nvs_close(h);
    }
}

void dial_state_set_ota_shown(uint32_t epoch)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_state.ota_shown = epoch;
    s_state.generation++;
    xSemaphoreGive(s_mux);

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, "ota_shown", epoch);
        nvs_commit(h);
        nvs_close(h);
    }
}

// Session-only, no NVS write -- see app_state_t.ota_prompt_due's comment.
void dial_state_clear_ota_prompt_due(void)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_state.ota_prompt_due = false;
    s_state.generation++;
    xSemaphoreGive(s_mux);
}

void dial_state_set_battery(int mv, int pct, bool charging)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    // The rail wanders a few mV between conversions even at rest. Committing
    // that would bump the generation every 30s forever and re-render every
    // screen for nothing, so only a real move counts. 20mV is well under one
    // percent of the usable window and well over the converter's jitter.
    bool moved = charging != s_state.batt_charging ||
                 pct != s_state.batt_pct ||
                 mv > s_state.batt_mv + 20 || mv < s_state.batt_mv - 20;
    if (moved) {
        s_state.batt_mv       = mv;
        s_state.batt_pct      = pct;
        s_state.batt_charging = charging;
        s_state.generation++;
    }
    xSemaphoreGive(s_mux);
}

void dial_cmd_post(const app_cmd_t *cmd)
{
    dial_state_stamp_input();
    if (xQueueSend(s_cmd_q, cmd, 0) != pdTRUE) {
        // Queue full (worker stuck in a slow network call through a 16-input
        // burst). For SET_TEMP the newest value must win: drop the oldest
        // command to make room. A lost TOGGLE would silently flip power, so
        // for toggles the drop-oldest is still the right bias (the oldest is
        // most likely an earlier temp step).
        app_cmd_t scrapped;
        xQueueReceive(s_cmd_q, &scrapped, 0);
        xQueueSend(s_cmd_q, cmd, 0);
    }
}

bool dial_cmd_receive(app_cmd_t *out, int timeout_ms)
{
    return xQueueReceive(s_cmd_q, out, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

// Look at the next command without taking it. The worker's long waits (consent,
// retry backoff) use this to spot the few commands they're allowed to run
// mid-wait while leaving everything else queued for the normal drain -- taking
// and re-posting instead would reorder the queue and spin the wait at 100% CPU
// whenever a command it can't run is sitting at the head.
bool dial_cmd_peek(app_cmd_t *out, int timeout_ms)
{
    return xQueuePeek(s_cmd_q, out, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void dial_state_get(app_state_t *out)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    *out = s_state;
    xSemaphoreGive(s_mux);
}

void dial_state_commit(void (*mutate)(app_state_t *st, void *arg), void *arg)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    mutate(&s_state, arg);
    s_state.generation++;
    xSemaphoreGive(s_mux);
}

void dial_state_set_phase(conn_phase_t phase, const char *err)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_state.phase = phase;
    if (err) strlcpy(s_state.phase_err, err, sizeof(s_state.phase_err));
    s_state.generation++;
    xSemaphoreGive(s_mux);
}

void dial_state_stamp_input(void)
{
    int64_t now = esp_timer_get_time();
    taskENTER_CRITICAL(&s_input_mux);
    s_last_input_us = now;
    taskEXIT_CRITICAL(&s_input_mux);
}

int64_t dial_state_last_input_us(void)
{
    taskENTER_CRITICAL(&s_input_mux);
    int64_t v = s_last_input_us;
    taskEXIT_CRITICAL(&s_input_mux);
    return v;
}
