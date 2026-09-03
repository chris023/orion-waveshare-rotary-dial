#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>

/*
 * The single app-state store. The network worker (and only it) commits device
 * truth; input handlers commit optimistic UI intent; the LVGL-side dispatcher
 * polls the generation counter and re-renders the active screen on change.
 *
 * Rules:
 *  - Readers take a full snapshot with dial_state_get() — never hold pointers
 *    into the store.
 *  - Writers mutate inside dial_state_commit()'s callback — the store mutex is
 *    held for the duration, so keep mutators tiny and never block in them.
 *  - The quiet-period input gate (dial_state_stamp_input / last_input_us) is
 *    the proven resync mechanism: the worker only reads the bed back after
 *    2.5s of no input, so a poll can never land mid-interaction.
 */

typedef enum {
    PH_BOOT = 0,
    PH_WIFI_CONNECTING,
    PH_WIFI_PORTAL,        // SoftAP captive portal is up, waiting for creds
    PH_WIFI_LOST,          // had Wi-Fi, lost it; supervisor is retrying
    PH_OAUTH_DISCOVER,     // discovery + client registration
    PH_OAUTH_WAIT_CONSENT, // QR on screen, waiting for phone approval
    PH_MCP_CONNECTING,     // token ok; opening MCP + finding the device
    PH_READY,              // steady state: command + poll loop
    PH_DEGRADED,           // net up but Orion calls failing; retrying w/ backoff
} conn_phase_t;

typedef enum { ZONE_A = 0, ZONE_B = 1, ZONE_COUNT = 2 } zone_idx_t;

// ABSOLUTE display range in °F (Fahrenheit/Celsius modes). Superseded at
// runtime by the device's OWN reported range (list_devices'
// temperature_range, °C on the wire) — main.c's orion_discover_device()
// parses and commits it into app_state_t.temp_min_f/temp_max_f; screens read
// it through dial_state_temp_min_f()/dial_state_temp_max_f() below, never
// these macros directly, except as the fallback those two functions use
// before discovery has ever run (fresh boot, offline). Earlier firmware
// (<=v1.0.x) hardcoded 55-110 here — narrower than the device's real
// 10-45°C (50-113°F) range on purpose, so an existing °F user's arc wouldn't
// visibly change — but the owner has since asked for the real range
// everywhere, so the fallback now matches it exactly: even a dial that has
// never completed discovery gets the full range, not a narrowed one. Orion
// takes °C; the °F lookup is the linear formula rounded to 0.1°C, so
// conversion round-trips.
#define DIAL_TEMP_MIN_F 50
#define DIAL_TEMP_MAX_F 113

static inline int   dial_c_to_f(float c) { return (int)lroundf(c * 1.8f + 32.0f); }
static inline float dial_f_to_c(int f)   { return roundf(((f - 32) / 1.8f) * 10.0f) / 10.0f; }

/*
 * RELATIVE temperature scale (Orion's third temperature_scale table). A signed
 * −10…+10 "level" scale where 0 = 27.5°C (81.5°F), the midpoint of the device
 * range; negative is cooler, positive warmer. It is purely a display/input
 * convention on our side — the wire is always °C (set_zone takes Celsius; there
 * is no scale/level parameter on any Orion tool, confirmed by live probe).
 *
 * We keep the internal setpoint in whole °F either way (dial_state's temp is
 * °C on the wire, °F for UI). Each level is carried by the whole °F below,
 * chosen so its dial_f_to_c() lands strictly inside that level's Celsius
 * bracket — so a device poll can never nudge the displayed level, and every
 * value we write is on Orion's grid. These two tables are the ONLY source of
 * truth; the discover-time tripwire in main.c re-checks them against the live
 * temperature_scale.relative and logs loudly on any mismatch.
 *
 *   DIAL_REL_F[L+10]  = whole °F carrying level L (−10…+10).
 *   DIAL_REL_LO_F[i]  = lowest whole °F that belongs to level (−9 + i); the
 *                       nearest-level boundaries, derived from the CELSIUS
 *                       midpoints (not midpoints of the °F carriers, which
 *                       disagree at a few boundaries). Ties resolve toward the
 *                       WARMER level, consistently (that single rule also picks
 *                       the level-0 carrier 82 over 81 and the +8 boundary 104).
 */
#define DIAL_REL_MIN (-10)
#define DIAL_REL_MAX ( 10)
#define DIAL_REL_MIN_F  50   // level −10 rail (= 10.0°C)
#define DIAL_REL_MAX_F 113   // level +10 rail (= 45.0°C)

static const uint8_t DIAL_REL_F[21] = {
    50, 54, 57, 61, 64, 66, 69, 73, 76, 79, 82,
    84, 87, 90, 92, 95, 99, 102, 106, 109, 113
};
static const uint8_t DIAL_REL_LO_F[20] = {
    52, 56, 59, 63, 65, 68, 72, 75, 78, 81,
    83, 86, 89, 91, 94, 97, 101, 104, 108, 112
};

// Nearest relative level for a whole °F (clamped to −10…+10). Uses the boundary
// table: f is at least level (−9 + i) iff f ≥ DIAL_REL_LO_F[i].
static inline int dial_rel_from_f(int f)
{
    int lvl = DIAL_REL_MIN;
    for (int i = 0; i < 20; i++)
        if (f >= DIAL_REL_LO_F[i]) lvl = DIAL_REL_MIN + 1 + i;
    return lvl;
}

// The whole °F carrying a level (level clamped to range).
static inline int dial_rel_to_f(int level)
{
    if (level < DIAL_REL_MIN) level = DIAL_REL_MIN;
    if (level > DIAL_REL_MAX) level = DIAL_REL_MAX;
    return DIAL_REL_F[level - DIAL_REL_MIN];
}

// One detent = exactly one level in the turned direction, from whatever level
// is currently DISPLAYED (so an off-grid device value snaps onto the grid in
// the direction the user turned — the numeral and the bed always move together
// or not at all). Returns the new carrier °F; equals f only when pinned at a
// rail (caller treats that as the range stop).
static inline int dial_rel_step(int f, int detents)
{
    int cur = dial_rel_from_f(f);
    int nl  = cur + detents;
    if (nl < DIAL_REL_MIN) nl = DIAL_REL_MIN;
    if (nl > DIAL_REL_MAX) nl = DIAL_REL_MAX;
    return (nl == cur) ? f : dial_rel_to_f(nl);
}

// Parse a schedule "HH:MM" (24h) time string into minutes-from-midnight.
// Returns false (leaving *out_min untouched) on any malformed input. Used by
// the worker's real night-window calc (main.c's sleep_phase_now()), which
// only ever looks at zone_state_t's sched_bedtime/sched_wakeup strings below.
static inline bool dial_parse_hhmm(const char *s, int *out_min)
{
    int hh, mm;
    if (!s || sscanf(s, "%d:%d", &hh, &mm) != 2) return false;
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return false;
    *out_min = hh * 60 + mm;
    return true;
}

/*
 * Screen (lock/standby) timeout: the fixed set of idle durations Settings'
 * "Screen timeout" row offers, seconds. Deliberately NOT "Never" — this is a
 * bedside device whose whole standby design (night dimming, the standby
 * clock face, dial_power_wake_consumes' wake-eats-first-input rule) exists
 * to stop it glowing at 3am; an always-on face would be a footgun the rest
 * of the firmware is built to avoid.
 *
 * app_state_t.screen_timeout_s (below) holds this preference; its own
 * default (90) is deliberately NOT one of these five — see that field's
 * comment for why. dial_scr_timeout_label treats any value that isn't an
 * exact member (today only that 90s default) as "nearest to one of these
 * five, ties toward the LONGER one", so the row always shows one of its own
 * five labels (a fresh device reads "2m", the nearest to the true 90s).
 * dial_scr_timeout_next always advances one step PAST that nearest choice —
 * never just snaps onto it — so even a device's very first tap visibly
 * changes the label instead of silently reproducing the same nearest choice
 * the row was already (mis)reporting; only every tap thereafter is a plain
 * one-step cycle, same idiom as Rotation's modulo.
 */
#define DIAL_SCR_TIMEOUT_CHOICES_N 4
// Seconds only, no "Off": an always-on option was tried and removed as not
// worth its complexity (it needed an infinite-threshold special case in
// dial_power, and it defeats the night dimming and clock face this device is
// built around). Anything above 1m was cut too — on a dial you glance at and
// walk away from, the useful range is seconds. A device that somehow stored
// the old 0 sentinel falls back to the 90s default via clamp_screen_timeout_s.
static const uint16_t DIAL_SCR_TIMEOUT_CHOICES[DIAL_SCR_TIMEOUT_CHOICES_N] = { 5, 15, 30, 60 };
static const char *const DIAL_SCR_TIMEOUT_LABELS[DIAL_SCR_TIMEOUT_CHOICES_N] = { "5s", "15s", "30s", "1m" };

static inline int dial_scr_timeout_nearest_idx(uint16_t s)
{
    int idx = 0, best_d = -1;
    for (int i = 0; i < DIAL_SCR_TIMEOUT_CHOICES_N; i++) {
        int d = (int)s - (int)DIAL_SCR_TIMEOUT_CHOICES[i];
        if (d < 0) d = -d;
        if (best_d < 0 || d < best_d ||
            (d == best_d && DIAL_SCR_TIMEOUT_CHOICES[i] > DIAL_SCR_TIMEOUT_CHOICES[idx])) {
            best_d = d;
            idx = i;
        }
    }
    return idx;
}

// Display label for the Screen timeout row — always one of the five choices'
// own strings (see the block comment above for how an off-menu value like
// the 90s default maps onto one).
static inline const char *dial_scr_timeout_label(uint16_t s)
{
    return DIAL_SCR_TIMEOUT_LABELS[dial_scr_timeout_nearest_idx(s)];
}

// Tap-to-cycle step for the Screen timeout row — same idiom as Rotation's
// plain modulo, just over an irregular set, and ALWAYS moves one step past
// the nearest choice (see the block comment above) so a tap can never be a
// silent no-op, even from the off-menu 90s default.
static inline uint16_t dial_scr_timeout_next(uint16_t cur)
{
    int idx = (dial_scr_timeout_nearest_idx(cur) + 1) % DIAL_SCR_TIMEOUT_CHOICES_N;
    return DIAL_SCR_TIMEOUT_CHOICES[idx];
}

typedef struct {
    float temp_c;            // setpoint (top-level zones[].temp)
    float actual_c;          // measured water temp (status.zones[].temp); <0 = unknown
    bool  on;
    char  thermal_state[12]; // "standby" | "holding" | (heating/cooling presumed)
    char  user_name[24];     // first name from list_devices zones[].user ("" = unknown)

    // Thermal relief ("boost"), parsed from top-level zones[].thermal_relief
    // (get_device_state) or the start/cancel_thermal_relief response's zones[]
    // — same shape in all three. Null/absent thermal_relief = relief_active
    // false and the rest of these are stale/don't-care.
    bool    relief_active;
    bool    relief_heat;      // true = heat, false = cool
    int64_t relief_end_ms;    // epoch MILLISECONDS (API's units, not seconds)
    float   relief_prev_temp_c;
    bool    relief_prev_on;   // the zone's on/off BEFORE the relief started —
                              // cancelling restores it, so the UI can predict
                              // the post-cancel state instead of flashing an
                              // intermediate one (owner: cancel briefly showed
                              // "Holding" before settling to off).
    // Dial-local, never carried on the wire: esp_timer_get_time() when the
    // four relief_* fields above were last set OPTIMISTICALLY (main.c's
    // handle_immediate_cmd, CMD_BOOST_START/CANCEL, BEFORE the MCP round
    // trip) rather than from a get_device_state poll or a relief ack that
    // already agreed with us. 0 = no optimistic guess in flight. main.c's
    // mut_device_state and mut_relief_ack both consult this (via
    // relief_should_preserve_optimistic) to keep the guess alive for a short
    // window while a poll or a lagging ack that hasn't caught up yet would
    // otherwise flicker the boost chip back — see RELIEF_OPTIMISTIC_WINDOW_US.
    int64_t relief_opt_us;

    // Tonight's sleep schedule (M5), from get_sleep_schedules — TODAY's entry
    // only (day == dial_time_now's tm_wday), matched to this zone via the
    // worker's zone->uuid map (list_devices zones[].user.id). sched_valid
    // false means either the clock isn't set yet or the worker hasn't
    // resolved this zone's schedule (no user uuid, or no entry for today).
    bool  sched_valid;
    char  sched_bedtime[6];        // "HH:MM", 24h
    float sched_bedtime_temp_c;
    char  sched_wakeup[6];         // "HH:MM", 24h
    float sched_wakeup_temp_c;

    // Smart-temperature phase schedule ("Dial adjusts" / M8), same
    // get_sleep_schedules entry as the fields above. is_smart_temperature_active
    // says whether this schedule's phase engine is even on for tonight (if
    // false, only sched_bedtime_temp_c/sched_wakeup_temp_c above are
    // meaningful — there's no phase_1/phase_2 step to speak of). The offsets
    // are MINUTES AFTER BEDTIME (not clock times), so together these mark
    // tonight's phase boundaries: bedtime -> bedtime+phase_1_offset ->
    // bedtime+phase_2_offset -> wakeup, with temps bedtime_temp / phase_1_temp
    // / phase_2_temp / wakeup_temp. See main.c's sleep_phase_now() for how
    // "which phase is active right now" is resolved from these.
    bool  sched_smart_temp_active;
    int   sched_phase1_offset_min;
    float sched_phase1_temp_c;
    int   sched_phase2_offset_min;
    float sched_phase2_temp_c;

    // Ecobee-style "will this hold, or expire?" for scr_dial.c's status pill.
    // Computed by the WORKER (main.c's compute_hold_until_min(), which wraps
    // sleep_phase_now()) mirroring temp_write_phase()'s exact preconditions
    // (Adjustment mode = Follow schedule, a captured Orion user uuid for this
    // zone, a valid wall clock, and an active sleep-schedule phase right now)
    // so the pill states the WRITE PATH'S BEHAVIOUR, not the raw preference —
    // Schedule mode with no active phase (outside the window, smart temp off,
    // or no usable schedule) still writes a plain hold, and must say so.
    // -1 = the setpoint holds until the user changes it ("Holding");
    // otherwise minutes-from-midnight of the CURRENT phase's own end
    // boundary ("Until H:MM"). Recomputed every idle tick alongside the
    // night-window calc (same block, same fresh clock+state read) so a phase
    // boundary crossing, a fresh schedule fetch, or an Adjustment-mode flip
    // all land within one tick; committed to the store only on an actual
    // per-zone change. dial_state_init() seeds this -1 so a fresh boot (or a
    // screen render before the worker's first idle tick) never shows a bogus
    // "Until 0:00".
    int16_t hold_until_min;
} zone_state_t;

/*
 * Honest, actionable phase_err text for a TLS certificate-verification
 * failure (the device's embedded trust anchors are too old for whatever the
 * server presents now) — this hardware can outlive its maintainer, so a
 * stale anchor must not read as the same routine "Orion unreachable" outage
 * as a Wi-Fi blip. dial_oauth/dial_mcp set this verbatim as the phase_err
 * whenever their last_err_cert() getter is true; scr_connecting.c recognizes
 * DIAL_CERT_ERR_TITLE as the first line and promotes it to the screen's
 * headline instead of folding it into the usual retry subtitle.
 *
 * The repo line omits the "github.com/" host: at the error screen's sub
 * label (300px @ lv_font_montserrat_16) the full URL measures ~400px and
 * wraps awkwardly, while the bare "owner/repo" form fits on one line.
 */
#define DIAL_CERT_ERR_TITLE "Secure connection failed"
#define DIAL_CERT_ERR_MSG \
    DIAL_CERT_ERR_TITLE "\n" \
    "This firmware may be too old\n" \
    "chris023/orion-waveshare-rotary-dial"

typedef struct {
    // Connection / lifecycle
    conn_phase_t phase;
    char    phase_err[128];   // last human-readable error (offline/error screens)
    int     retry_in_s;       // seconds until the supervisor's next retry (0 = n/a)
    char    oauth_url[600];   // authorize URL while PH_OAUTH_WAIT_CONSENT
    char    ap_ssid[33];      // SoftAP name while PH_WIFI_PORTAL
    // The dial's own HOME network SSID (dial_net_sta_ssid() mirror, committed
    // once worker_task's dial_net_bringup() returns). NOT ap_ssid above --
    // this is the network SCR_OAUTH_QR tells the user their PHONE must also be
    // on, since the OAuth callback is a LAN redirect to the dial and cannot
    // reach it over cellular or a different network. "" if unknown.
    char    sta_ssid[33];

    /*
     * On-device Wi-Fi setup. A rejected password must not throw the user back
     * to the start of setup with no explanation — it has to say what went wrong
     * and put them back on the password screen for the SAME network. So the
     * store remembers which network the dial is trying, and whether the last
     * attempt came back rejected.
     */
    char    wifi_join_ssid[33];
    int8_t  wifi_join_idx;    // index into the scan list; -1 = came from the captive portal
    bool    wifi_join_failed;

    // Device truth (valid once have_state)
    bool    have_state;
    char    serial[16];
    bool    device_online;
    zone_state_t zones[ZONE_COUNT];
    // Which zones the device actually reports (get_device_state's zones[]).
    // Orion also sells SINGLE-ZONE toppers, so a zone entry existing is not a
    // given: everything that assumes a partner side (the side-swap chain, the
    // page dots, the side picker) must gate on this rather than on
    // ZONE_COUNT. Valid once have_state; see dial_state_is_dual().
    bool    zone_present[ZONE_COUNT];
    struct { bool error; char desc[96]; } safety;
    char    water_fill[12];
    bool    away;             // session-optimistic (set_away has no readback)
    // Device-reported absolute temperature range (list_devices'
    // temperature_range, °C on the wire), converted once via dial_c_to_f()
    // and mirrored here as whole °F by main.c's orion_discover_device() —
    // this, not the DIAL_TEMP_MIN_F/MAX_F constants, is what the arc range /
    // knob clamp / drag clamp use in ABSOLUTE mode (owner: "use the Orion
    // reported min/max for limits, not our own"). Relative mode's own
    // 21-level table (DIAL_REL_MIN_F/MAX_F above) is untouched by this —
    // that table is tied to Orion's temperature_scale.relative, a different
    // field, validated by its own discover-time tripwire. -1 = not yet known
    // (fresh boot, before the first successful list_devices, or a device
    // that omits the field) — dial_state_temp_min_f()/_max_f() below fall
    // back to the DIAL_TEMP_MIN_F/MAX_F constants then, which now equal the
    // device's real rails, so the fallback no longer narrows anything.
    int     temp_min_f;
    int     temp_max_f;

    // Wall clock
    bool    clock_valid;

    // UI intent (optimistic layer, kept apart from device truth)
    int     ui_temp_f[ZONE_COUNT];  // shown setpoint °F; -1 = follow device
    zone_idx_t ui_zone;             // which side the UI is showing (persisted)

    // --- Onboarding (M4) ---
    // True for the whole session when the device booted with no stored Wi-Fi
    // credentials (set once in app_main from !dial_net_have_creds(), before
    // dial_net_bringup runs the portal). Gates SCR_WELCOME/SCR_SIDEPICK so an
    // already-provisioned device (upgraded firmware, never picked a side) is
    // never routed through onboarding again.
    bool fresh_device;
    // SCR_WELCOME dismissed (tap or knob). Session-only, deliberately NOT
    // persisted — the point is just to stop nav_policy from pinning the
    // welcome screen once the user acknowledges it.
    bool welcomed;
    // True once a default side is known: either the user picked one on
    // SCR_SIDEPICK, or (upgrade path) NVS already had a "zone" key from
    // before this flag existed. Restored from that key's *existence* in
    // dial_state_restore_prefs, not its value.
    bool side_picked;

    // --- Settings (M4) ---
    // Display units: false = °F (canonical/internal — the store's temp_c is
    // always °C regardless), true = °C for display only. In RELATIVE mode this
    // governs only the absolute readouts that persist there (the measured water
    // caption), never the setpoint hero.
    bool units_c;
    // Temperature SCALE for the setpoint hero: false = absolute (°F/°C per
    // units_c), true = relative levels (−10…+10). Default is relative on a
    // fresh device, absolute on one upgrading from ≤v1.0.6 (see
    // dial_state_restore_prefs — an existing user must not have the big number
    // silently change meaning under an OTA). The wire is °C regardless; the
    // internal setpoint stays whole °F in both scales.
    bool rel_mode;
    // Screen rotation in quarter turns clockwise (0..3), persisted. The dial
    // sits on a nightstand and its cable exits one edge, so which way is "up"
    // is a property of the room, not of the device.
    uint8_t rotation;
    // Haptics feedback level, mirrored here so screens can read the current
    // setting; dial_haptics_set_level() is the actual enforcement point.
    // Mirrors dial_haptics.h's haptic_level_t values field-for-value without
    // depending on that header (same int-mirror trick as ota.status below):
    // 0=Off (nothing plays), 1=Auto (FIRM by day, SOFT during the sleep
    // window — today's pre-M7 "enabled" behavior, unchanged), 2=Low (SOFT
    // always), 3=High (FIRM always, including at 3am — a deliberate, global
    // user choice, not time-of-day-gated). The NVS key "haptics" is
    // unchanged from the old On/Off bool pref — every device in the field
    // already stores 0 or 1, and 1 already meant "the adaptive behavior", so
    // it maps to exactly Off/Auto with no migration; 2 (Low) and 3 (High)
    // are new. See dial_state_set_haptics_level.
    uint8_t haptics_level;
    // Last started Boost duration, in minutes — recorded when the user starts
    // one, not when the device confirms it. The Boost picker opens at this
    // value, and rail shortcuts use the same remembered duration.
    // Persisted to NVS "ui"/"boost_min"; defaults to 15.
    uint8_t boost_minutes;
    // Day/night backlight brightness, 10..100 (percent), 10% steps. dial_power
    // scales its whole duty table (active/dimmed/standby together) by this at
    // apply time; dial_power_start() is what actually enforces it. Defaults to
    // 100 (today's exact brightness) both here and in NVS-absent restores —
    // see dial_state_get_bri_day_pct/set_bri_day_pct.
    uint8_t bri_day_pct;
    uint8_t bri_night_pct;
    // Night-only override for JUST the standby (screensaver clock) duty —
    // the face that glows in a dark bedroom all night, independent of the
    // brightness used while the dial is actually being interacted with at
    // night. Day is untouched: day's standby tier still follows bri_day_pct
    // like the rest of the day table. Same 10..100/10%-step range and
    // clamp-on-read contract as the pair above. Defaults to 100 on a fresh
    // device (matches bri_day_pct/bri_night_pct's own fresh-device default),
    // but an UPGRADING device must NOT see this default — see
    // dial_state_restore_prefs's migration comment: on that path it is
    // seeded from the device's own bri_night_pct instead, so a device
    // already dimmed at night keeps exactly the clock glow it has tonight
    // until the user deliberately pulls the two apart. See
    // dial_state_get_bri_night_clock_pct/set_bri_night_clock_pct.
    uint8_t bri_night_clock_pct;
    // Screen (lock/standby) timeout: idle seconds before dial_power drops the
    // display to DPWR_STANDBY (its "STANDBY" threshold — see dial_power.c's
    // power_task, which reads this live every 100ms tick, same as the
    // brightness prefs above). Settings' "Screen timeout" row offers exactly
    // DIAL_SCR_TIMEOUT_CHOICES above (30s/1m/2m/5m/10m — deliberately no
    // "Never", see that table's comment). Default 90 is deliberately NOT one
    // of those five: it's the exact value this firmware hardcoded as
    // STANDBY_AFTER_US before this preference existed, so introducing it
    // changes zero devices' behavior until the user taps the row — see
    // dial_scr_timeout_label/_next for how the row displays and steps off
    // that off-menu default. Persisted to NVS "ui"/"scr_to"; see
    // dial_state_get_screen_timeout_s/set_screen_timeout_s.
    uint16_t screen_timeout_s;
    // Beta OTA channel opt-in (SCR_UPDATE's "Beta builds" toggle), persisted
    // to NVS "ui"/"beta". dial_state has no business knowing about dial_ota,
    // so this is just the stored preference -- the worker (main.c) reads it
    // out of its app_state_t snapshot and passes it to dial_ota_check(),
    // same division of labor as haptics_level/dial_haptics_set_level.
    // Default false (stable channel only) both here and in an NVS-absent
    // restore -- see dial_state_get_beta/dial_state_set_beta.
    bool beta;
    // "Dial adjusts" preference: true = Follow schedule (a knob turn during
    // tonight's active sleep-schedule phase retargets THAT PHASE via
    // override_sleep_schedule_tonight, leaving the rest of tonight's
    // schedule in control — matches the Orion phone app); false = Hold
    // tonight (a knob turn always set_zones a plain hold for the rest of the
    // night, this dial's original behavior). main.c's worker reads this out
    // of its app_state_t snapshot the same way it reads beta above — dial_state
    // has no business knowing about MCP tool calls. Persisted to NVS
    // "ui"/"sched_follow". Default TRUE (owner decision) both here and in an
    // NVS-absent restore -- see dial_state_get_sched_follow/
    // dial_state_set_sched_follow.
    bool sched_follow;

    // --- OTA (M6) ---
    // Mirrors dial_ota_info_t (components/dial_ota/dial_ota.h) field-for-
    // field, without a direct dependency on it: dial_state is a leaf
    // component (no REQUIRES beyond esp_timer/nvs_flash), so it can't
    // #include dial_ota.h. The worker (main.c, which includes both) commits
    // this after every dial_ota_* call; `status` holds a dial_ota_status_t
    // value (int-cast, same enum ordering/values on both sides).
    struct {
        int  status;
        char latest[16];
        int  progress_pct;
        char err[96];
        // Mirrors dial_ota_info_t.pending_verify -- true while this boot is
        // still ESP_OTA_IMG_PENDING_VERIFY (a fresh OTA install the
        // bootloader will roll back on the next reset/power-cycle unless it
        // survives to confirm). bool needs no int-cast trick like `status`
        // above: it's a plain stdbool.h type on both sides, not an enum
        // defined in dial_ota.h. Drives scr_connecting's/scr_dial's
        // "Finalizing update -- keep powered" notice.
        bool pending_verify;
        // True while an install the USER never asked for is running (the
        // overnight auto-updater). nav_policy uses it to leave a SLEEPING
        // dial dark rather than lighting the takeover screen at a bedside
        // for two minutes to narrate an update nobody is watching. Manual
        // installs clear it and always take over — the user is standing
        // there, and a dial that reboots under their hands unexplained is
        // worse than the interruption.
        bool unattended;
    } ota;

    // --- Update prompt / auto-update (docs/SPEC-update-prompt.md) ---
    // Auto-update setting (SCR_UPDATE's "Auto-update" row): 0 = Off,
    // 1 = Overnight. Persisted to NVS "ui"/"ota_auto". Default 0 (Off) both
    // here and on an NVS-absent restore -- spec's explicit consent
    // requirement (a device that silently reboots itself must not surprise
    // someone who never asked for it). Not a bool/haptics_level-style enum
    // mirror of anything in dial_ota.h -- this preference is dial_state's own,
    // dial_ota has no concept of "overnight".
    uint8_t ota_auto;
    // "Later" defer timestamp: an epoch-seconds wall clock value (survives
    // reboots correctly, unlike an uptime timer) before which the update
    // prompt must not be shown again. Persisted to NVS "ui"/"ota_defer".
    // Default 0, which is always in the past -- a fresh device has nothing
    // to defer. Set to now+23h (deliberately not 24h -- see the spec) by
    // scr_update_prompt.c's "Later" action and its swipe-dismiss equivalent.
    uint32_t ota_defer;
    // Exact version string ("X.Y.Z"/"X.Y.Z-beta.N") the user chose to skip
    // via SCR_UPDATE's "Skip this version" row -- an EXACT match against
    // ota.latest suppresses the prompt for that version only; anything
    // newer (a different string) prompts again. Persisted to NVS
    // "ui"/"ota_skip". Default "" (nothing skipped).
    char ota_skip[16];
    // Epoch seconds the update prompt was last actually shown -- the
    // once-per-24h ceiling on prompt frequency, independent of "Later"'s own
    // defer window (a device that's never been shown the prompt has this at
    // 0, which is always >=24h in the past). Persisted to NVS "ui"/"ota_shown".
    // Committed by the worker (main.c) the instant it raises ota_prompt_due
    // below -- see dial_state_set_ota_shown.
    uint32_t ota_shown;
    // Session-only (deliberately NOT persisted): "is the update prompt sheet
    // raised right now" (docs/SPEC-update-prompt.md's 2026-07-29 rework).
    // Raised by the worker (main.c) on a WAKE EDGE
    // (DPWR_STANDBY/DPWR_DIMMED -> DPWR_ACTIVE) when every entry gate holds
    // at that instant -- NOT a continuously re-evaluated condition; once
    // raised it stays true until the worker's own three separate exit
    // checks fire (display back to STANDBY, night begins, update no longer
    // available) or scr_update_prompt.c clears it directly on a deliberate
    // user action. Mirrors s_ui_night's edge-triggered commit pattern (only
    // touched on an actual change), but the edge it tracks is now "just
    // woke", not "gate table still holds" -- the earlier continuous
    // re-evaluation was the mid-reach-withdrawal bug this rework fixed (a
    // touch resets idle time, which used to be one of the gates).
    // nav_policy routes to SCR_UPDATE_PROMPT exactly when this is true (and
    // the dial face is what's showing -- see nav_policy's own comment for
    // why it's scoped to SCR_DIAL). Cleared immediately by
    // scr_update_prompt.c on every exit path (Update now / Later / Update
    // options / swipe-dismiss) via dial_state_clear_ota_prompt_due(), so a
    // deliberate dismissal can never race the worker's own next tick.
    bool ota_prompt_due;

    // Battery, sampled off ADC1 ch0 (GPIO1 / BATT_ADC) by dial_battery.
    // Session-only, never persisted: a stale level read out of NVS at boot
    // would be worse than the half-second of zeroes before the first sample.
    //
    // batt_mv is the `5V` rail, which is the charger while USB is attached
    // and the cell once it is not. batt_charging is that distinction, taken
    // with hysteresis around DIAL_BATTERY_MV_USB_ON/OFF. batt_pct is
    // DIAL_BATTERY_PCT_UNKNOWN (-1) whenever batt_charging is true, because
    // the pin cannot see the cell through the charger and a number there
    // would be fiction. Zero until the first sample lands.
    int  batt_mv;
    int  batt_pct;
    bool batt_charging;

    // Bumped on every commit; the UI dispatcher re-renders when it changes.
    uint32_t generation;
} app_state_t;

/*
 * Zone-count helpers (single-zone toppers). Before have_state nothing is known
 * about the device, so these answer for the layout the UI is currently drawing:
 * dial_state_is_dual() is false until the device says otherwise, which keeps a
 * booting dial from flashing a partner face that may not exist.
 */
static inline bool dial_state_is_dual(const app_state_t *st)
{
    return st->zone_present[ZONE_A] && st->zone_present[ZONE_B];
}

// The zone a single-zone device actually has (and, on a dual device, the side
// whose schedule the worker's overnight write-path logic follows — see
// sleep_phase_now()/temp_write_phase() in main.c). ZONE_A unless the device
// reports only ZONE_B.
static inline zone_idx_t dial_state_primary_zone(const app_state_t *st)
{
    return (!st->zone_present[ZONE_A] && st->zone_present[ZONE_B]) ? ZONE_B : ZONE_A;
}

// Effective ABSOLUTE-mode temperature range: the device's own reported rails
// once discovery has found them, else the DIAL_TEMP_MIN_F/MAX_F fallback
// (see app_state_t.temp_min_f/temp_max_f's comment). Every absolute-mode
// consumer (scr_dial.c's arc range, knob clamp, drag clamp) reads the range
// through these two, never the macros or the raw fields directly, so there
// is exactly one place that decides "known vs. fallback".
static inline int dial_state_temp_min_f(const app_state_t *st)
{
    return st->temp_min_f >= 0 ? st->temp_min_f : DIAL_TEMP_MIN_F;
}
static inline int dial_state_temp_max_f(const app_state_t *st)
{
    return st->temp_max_f >= 0 ? st->temp_max_f : DIAL_TEMP_MAX_F;
}

// Initialize the store (mutex + defaults). Call once before any other call.
void dial_state_init(void);

// Restore persisted UI preferences (last shown side). Requires NVS to be
// initialized, so call after dial_net_init, before the first real screen.
void dial_state_restore_prefs(void);

// Copy the current state under the store mutex.
void dial_state_get(app_state_t *out);

// Run `mutate` on the live state under the mutex, then bump the generation.
void dial_state_commit(void (*mutate)(app_state_t *st, void *arg), void *arg);

// Convenience: set the connection phase (+ optional error text, NULL to keep).
void dial_state_set_phase(conn_phase_t phase, const char *err);

// Hot-path setter used by the dial screen during knob/drag interaction.
void dial_state_set_ui_temp(zone_idx_t zone, int temp_f);

// Optimistic power flip — call from the UI on tap, so the face answers the
// press instead of waiting for the write to Orion to come back. The next poll
// reconciles (and reverts it, if the write failed).
void dial_state_set_zone_on(zone_idx_t zone, bool on);

// Derive a zone's thermal_state from its target vs. its measured water temp.
// Used by the optimistic setters above and by the worker's own commits, so the
// UI and the worker never disagree about what a pending change means. Caller
// must hold the store lock (i.e. call it from inside a dial_state_commit
// mutator, or from dial_state.c itself).
void dial_state_predict_thermal(app_state_t *st, zone_idx_t zone);

// Record which side the UI is showing. The nav policy follows this, so any
// screen that switches sides MUST commit it here (or the next state commit
// navigates right back — the side choice lives in the store, not the router).
void dial_state_set_ui_zone(zone_idx_t zone);

// --- Onboarding / settings setters (M4) ---
// Dismiss SCR_WELCOME. Not persisted (see app_state_t.welcomed).
void dial_state_set_welcomed(void);
// Mark that a default side is known (see app_state_t.side_picked). Callers
// that pick a side also call dial_state_set_ui_zone() to persist it.
void dial_state_set_side_picked(void);
// Set the display-units preference; persists to NVS "ui"/"units".
void dial_state_set_units_c(bool units_c);
// Set the temperature-scale preference (relative vs absolute); persists to NVS
// "ui"/"relmode".
void dial_state_set_rel_mode(bool rel_mode);
// Set the haptics feedback level (0=Off/1=High/2=Low — see app_state_t.
// haptics_level above); persists to NVS "ui"/"haptics" immediately. Does NOT
// itself call dial_haptics_set_level() — dial_state has no business knowing
// about the haptics driver, so callers do both. `level` is not typed
// haptic_level_t: dial_state can't #include dial_haptics.h (leaf component),
// same reasoning as ota.status's int mirror.
void dial_state_set_haptics_level(uint8_t level);

// Optimistic thermal-relief write, callable from the LVGL task the instant a
// user taps. The worker ALSO commits this before issuing the call, but that
// is not enough on its own: the worker is single-threaded, so a tap landing
// while it sits in a slow MCP round-trip (observed at 10-20s) waits in the
// command queue before anything reaches the screen -- which is exactly the
// lag the owner reported on cancel. Committing here makes the chip react on
// the next render regardless of what the worker is busy with; the worker's
// own commit then reconciles, and reverts if the call fails.
// zone < 0 applies to every zone (cancel_thermal_relief is device-wide).
void dial_state_set_relief_optimistic(int zone, bool active, bool heat, int64_t end_ms);

// Last-started Boost duration preference, minutes. Legal values are the Boost
// picker's 5..120 minute range; corrupt/out-of-range values fall back to 15.
uint8_t dial_state_get_boost_minutes(void);
void    dial_state_set_boost_minutes(uint8_t minutes);

// Day/night backlight brightness preference, 10..100 (percent, 10% steps).
// Getters always return a value in that range (clamped on read; 100 when no
// "bri_day"/"bri_night" NVS key has ever been written). Setters clamp too and
// persist immediately to NVS "ui"/"bri_day" and "ui"/"bri_night" — same
// immediate-commit pattern as dial_state_set_haptics_level above. dial_state
// has no business knowing about the backlight driver, so callers that want the
// new brightness to actually take effect must also call
// dial_power_brightness_changed() (dial_power.h), same division of labor as
// haptics_level/dial_haptics_set_level().
uint8_t dial_state_get_bri_day_pct(void);
uint8_t dial_state_get_bri_night_pct(void);
void    dial_state_set_bri_day_pct(uint8_t pct);
void    dial_state_set_bri_night_pct(uint8_t pct);

// Night-clock (screensaver-at-night) brightness override — see app_state_t.
// bri_night_clock_pct's comment for what it governs and how it migrates.
// Same getter/setter/clamp/NVS-immediate-commit shape as the pair above,
// persisted to NVS "ui"/"bri_nclk".
uint8_t dial_state_get_bri_night_clock_pct(void);
void    dial_state_set_bri_night_clock_pct(uint8_t pct);

// Screen (lock/standby) timeout preference, seconds (see app_state_t.
// screen_timeout_s above for the choice set/default/NVS key). Same
// clamp-on-read/immediate-commit shape as the brightness pair, except the
// clamp snaps an out-of-range/corrupt stored value to 90 (today's legacy
// behavior), not to the nearest UI choice — a corrupt byte should fail back
// to the ORIGINAL fixed threshold, not to whatever choice happens to be
// numerically closest. The setter does NOT itself call into dial_power —
// power_task reads this getter fresh every 100ms tick (same as the
// brightness pair), so a change reaches the standby/dim decision within one
// tick with no separate "changed" hook required; see dial_power.c.
uint16_t dial_state_get_screen_timeout_s(void);
void     dial_state_set_screen_timeout_s(uint16_t seconds);

// Beta OTA channel preference (see app_state_t.beta above). Same
// getter+setter shape as the brightness pair; setter persists immediately to
// NVS "ui"/"beta".
bool dial_state_get_beta(void);
void dial_state_set_beta(bool enabled);

// "Dial adjusts" preference (see app_state_t.sched_follow above). Same
// getter+setter shape as beta; setter persists immediately to NVS
// "ui"/"sched_follow".
bool dial_state_get_sched_follow(void);
void dial_state_set_sched_follow(bool follow);

// --- Update prompt / auto-update (see app_state_t's comments above for what
// each field means and its NVS key). All four setters persist immediately,
// same shape as dial_state_set_beta -- callers are rare taps in the LVGL
// task (SCR_UPDATE's Auto-update/Skip rows, scr_update_prompt.c's Later
// action), except dial_state_set_ota_shown, which the WORKER calls the
// instant it raises ota_prompt_due (main.c's idle loop; a ~ms NVS write
// there is fine, it happens at most once a day).
void dial_state_set_ota_auto(uint8_t mode);
void dial_state_set_ota_defer(uint32_t epoch);
void dial_state_set_ota_skip(const char *version);
void dial_state_set_ota_shown(uint32_t epoch);

// Clears the session-only ota_prompt_due flag (see app_state_t's comment).
// Called directly from the LVGL task by scr_update_prompt.c's every exit
// path -- NOT persisted, no NVS write, just an immediate commit (same shape
// as dial_state_set_welcomed).
void dial_state_clear_ota_prompt_due(void);

// Screen rotation, quarter turns clockwise (0..3). Persisted; apply it to the
// panel with dial_display_set_rotation.
void dial_state_set_rotation(uint8_t quarters);

// On-device Wi-Fi setup: what the dial is currently trying to join (called just
// before the credentials are handed over), and whether that attempt was
// rejected. See app_state_t's wifi_join_* fields.
void dial_state_set_wifi_join(int idx, const char *ssid);
void dial_state_set_wifi_join_failed(void);
void dial_state_clear_wifi_join_failed(void);

// Battery sample from dial_battery's task. Not persisted (see app_state_t's
// batt_* comment). Skips the commit when nothing moved, so a dial sitting on
// the charger does not wake every screen's on_state every 30 seconds for a
// reading that did not change.
void dial_state_set_battery(int mv, int pct, bool charging);

// --- Input quiet-period gate (torn-read-safe on 32-bit) ---
void    dial_state_stamp_input(void);   // call on EVERY user input
int64_t dial_state_last_input_us(void);

/*
 * UI -> worker command queue. Screens post; the (single) network worker
 * drains, coalescing bursts (a knob spin collapses to one set_zone).
 */
typedef enum {
    CMD_SET_TEMP,      // zone + temp_f
    CMD_TOGGLE_ON,     // zone + a = the DESIRED on state (1/0), not "flip it".
                       // The UI flips the store optimistically before posting,
                       // so a worker that re-derived !current would undo it.
    CMD_BOOST_START,   // zone + a=heat?1:0 + b=minutes
    CMD_BOOST_CANCEL,  // zone ignored — cancels relief on every zone
    CMD_AWAY,          // a=1 away / 0 home

    // Settings (M4) destructive actions — each erases some NVS state and
    // reboots. Handled in main.c's handle_immediate_cmd like the others
    // above; none of them return (esp_restart()).
    CMD_RELINK,          // clear Orion tokens, keep Wi-Fi + client_id
    CMD_WIFI_RESET,      // clear Wi-Fi credentials
    CMD_FACTORY_RESET,   // erase all of NVS

    // Software update (M6/M7), from SCR_UPDATE's "Check for updates" row.
    CMD_OTA_CHECK,       // -> dial_ota_check(st.beta); zone/a/b unused
    CMD_OTA_APPLY,       // -> dial_ota_download_and_apply() + reboot on
                         // success; worker drops this unless ota.status is
                         // already OTA_AVAILABLE (stale-tap guard)
    // Posted by scr_update.c's destroy() (screen teardown, M7 -- scr_about.c
    // before it) so a FAILED "Check for updates" row never survives to the
    // next visit -- OTA_FAILED isn't allowed to be terminal (field bug: it
    // used to stay wedged until a manual power cycle). ->
    // dial_ota_clear_stale_failure(0); zone/a/b unused. A no-op if status
    // has already moved off OTA_FAILED. See also main.c's periodic
    // idle-loop call for the time-based ~25s auto-clear that covers the
    // case where the user never leaves the screen at all.
    CMD_OTA_CLEAR_FAILED,
} cmd_kind_t;

typedef struct {
    cmd_kind_t kind;
    zone_idx_t zone;
    int        temp_f;  // CMD_SET_TEMP
    int        a, b;    // generic args: CMD_BOOST_START (a=heat, b=minutes),
                         // CMD_AWAY (a=away)
} app_cmd_t;

void dial_cmd_post(const app_cmd_t *cmd);
bool dial_cmd_receive(app_cmd_t *out, int timeout_ms);

// Next queued command, left in place. For waits that can only act on some of
// them -- see the note above the definition.
bool dial_cmd_peek(app_cmd_t *out, int timeout_ms);
