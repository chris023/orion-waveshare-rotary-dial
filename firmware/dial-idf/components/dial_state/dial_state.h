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

// ABSOLUTE display range in °F (Fahrenheit/Celsius modes). The device's real
// range is 10–45°C ≈ 50–113°F, but absolute mode keeps the familiar 55–110
// the product shipped with (owner decision: widen only in relative mode, so an
// existing °F user sees no arc/indicator change). Relative mode uses the full
// 50–113 rails below so all 21 levels are reachable. Orion takes °C; the °F
// lookup is the linear formula rounded to 0.1°C, so conversion round-trips.
#define DIAL_TEMP_MIN_F 55
#define DIAL_TEMP_MAX_F 110

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
// Returns false (leaving *out_min untouched) on any malformed input. Shared
// by the worker (real night-window calc) and SCR_TONIGHT (display + the wake
// picker), both of which only ever look at zone_state_t's sched_bedtime/
// sched_wakeup strings below.
static inline bool dial_parse_hhmm(const char *s, int *out_min)
{
    int hh, mm;
    if (!s || sscanf(s, "%d:%d", &hh, &mm) != 2) return false;
    if (hh < 0 || hh > 23 || mm < 0 || mm > 59) return false;
    *out_min = hh * 60 + mm;
    return true;
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
    bool  sched_override_applied;
    bool  sched_override_available;
} zone_state_t;

typedef struct {
    // Connection / lifecycle
    conn_phase_t phase;
    char    phase_err[128];   // last human-readable error (offline/error screens)
    int     retry_in_s;       // seconds until the supervisor's next retry (0 = n/a)
    char    oauth_url[600];   // authorize URL while PH_OAUTH_WAIT_CONSENT
    char    ap_ssid[33];      // SoftAP name while PH_WIFI_PORTAL

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
    // page dots, "Match my side", the side picker) must gate on this rather
    // than on ZONE_COUNT. Valid once have_state; see dial_state_is_dual().
    bool    zone_present[ZONE_COUNT];
    struct { bool error; char desc[96]; } safety;
    char    water_fill[12];
    bool    away;             // session-optimistic (set_away has no readback)

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
    // Haptics master enable, mirrored here so screens can read the current
    // setting; dial_haptics_set_enabled() is the actual enforcement point.
    bool haptics_enabled;

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
    } ota;

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
// whose schedule the Tonight face speaks for). ZONE_A unless the device reports
// only ZONE_B.
static inline zone_idx_t dial_state_primary_zone(const app_state_t *st)
{
    return (!st->zone_present[ZONE_A] && st->zone_present[ZONE_B]) ? ZONE_B : ZONE_A;
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
// Set the haptics-enabled preference; persists to NVS "ui"/"haptics". Does
// NOT itself call dial_haptics_set_enabled() — dial_state has no business
// knowing about the haptics driver, so callers do both.
void dial_state_set_haptics_enabled(bool enabled);

// Screen rotation, quarter turns clockwise (0..3). Persisted; apply it to the
// panel with dial_display_set_rotation.
void dial_state_set_rotation(uint8_t quarters);

// On-device Wi-Fi setup: what the dial is currently trying to join (called just
// before the credentials are handed over), and whether that attempt was
// rejected. See app_state_t's wifi_join_* fields.
void dial_state_set_wifi_join(int idx, const char *ssid);
void dial_state_set_wifi_join_failed(void);
void dial_state_clear_wifi_join_failed(void);

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
    CMD_BED_OFF,       // zone ignored — both zones off, atomically
    CMD_AWAY,          // a=1 away / 0 home
    CMD_MATCH_PARTNER, // zone = mine; worker reads the store at execution
                       // time and set_zones the OTHER zone to my (temp_c, on)

    // Tonight schedule (M5). ONLY ZONE_A is supported: override_sleep_
    // schedule_tonight's confirmed field vocabulary has no user_id — it
    // implicitly targets the OAuth token's own account, and with two users
    // sharing a bed we have no reliable way to tell which of the two uuids
    // get_sleep_schedules returns is the token owner. Simplest safe answer:
    // only the dial's own side (ZONE_A, per D3) offers the override control.
    // Both commands are dropped by the worker if cmd->zone != ZONE_A.
    CMD_TONIGHT_OVERRIDE, // zone (must be ZONE_A); a = new wakeup minutes-
                          // from-midnight (0-1439) or -1 to keep; b = new
                          // bedtime_temp_f or -1 to keep (unused by today's
                          // wake-only picker; reserved for a future control)
    CMD_TONIGHT_REVERT,   // zone (must be ZONE_A) — reverts today's override

    // Settings (M4) destructive actions — each erases some NVS state and
    // reboots. Handled in main.c's handle_immediate_cmd like the others
    // above; none of them return (esp_restart()).
    CMD_RELINK,          // clear Orion tokens, keep Wi-Fi + client_id
    CMD_WIFI_RESET,      // clear Wi-Fi credentials
    CMD_FACTORY_RESET,   // erase all of NVS

    // Software update (M6), from Settings' "Software update" row.
    CMD_OTA_CHECK,       // -> dial_ota_check(); zone/a/b unused
    CMD_OTA_APPLY,       // -> dial_ota_download_and_apply() + reboot on
                         // success; worker drops this unless ota.status is
                         // already OTA_AVAILABLE (stale-tap guard)
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
