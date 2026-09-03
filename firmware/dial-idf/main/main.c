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
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_mac.h"
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
#include "dial_battery.h"
#include "dial_power.h"
#include "dial_palette.h"
#include "dial_ota.h"
#include "bidi_switch_knob.h"
#include "mdns.h"
// secrets.h is an optional dev convenience (git-ignored) that pre-seeds Wi-Fi
// creds so a developer build skips on-device provisioning; a fresh clone has
// none, and WIFI_SSID falls back to dial_net_seed's own placeholder string so
// the seed is a provable no-op and the dial provisions on-device as normal.
#if __has_include("secrets.h")
#include "secrets.h"
#else
#define WIFI_SSID     "your-wifi-ssid"
#define WIFI_PASSWORD ""
#endif
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

// Thermal-relief ("boost") optimistic window: CMD_BOOST_START/CANCEL commit
// their guess of relief_active/heat/end/prev BEFORE issuing the MCP call (see
// handle_immediate_cmd), so the chip reacts on the same tick as the tap
// instead of after a multi-second (sometimes 10-20s) round trip. The server
// can be slow to actually reflect a just-issued relief change -- even in the
// call's OWN response -- so both mut_device_state's poll and mut_relief_ack's
// own ack keep the optimistic guess alive, via relief_should_preserve_
// optimistic(), while it's younger than this AND the incoming data disagrees
// with it. Long enough to outlast a slow ack or the fast post-write confirm-
// poll cadence (POLL_CONFIRM_US * POLL_CONFIRM_N ~= 6s), short enough that a
// genuine disagreement (relief actually expired, or was cancelled from
// elsewhere) still resolves quickly instead of wedging the chip.
#define RELIEF_OPTIMISTIC_WINDOW_US 9000000

// Sleep schedules (M5) don't change minute to minute — refresh far less
// often than device state, piggybacked on the same idle poll path.
#define SCHED_INTERVAL_US (30LL * 60 * 1000000)   // ~30 min
// A refresh that misses — no synced clock yet, or a failed call — re-tries on
// this much shorter cadence instead of costing a full interval. The FIRST
// fetch after a boot is the one that matters: until it lands, sched_valid is
// false and temp_write_phase(), the night window and the pill's hold/until
// grammar all take their "no schedule" branch.
#define SCHED_RETRY_US (60LL * 1000000)           // ~1 min

// Auto OTA check (M6): once per uptime-day, and only checks (never applies)
// — see the gating comment at its call site for the full safe-window rule.
// 6h, not 24h (owner asked what it costs to check more often, 2026-08-04).
// One check is a single GitHub request against a 60/hour per-IP limit, so
// frequency is free on that side; the only real cost is briefly handing the
// TLS session over from the MCP client, which is noise a few times a day.
// The old 24h interval combined with a narrow daytime band meant a release
// took up to a full day to be noticed — the band, not the interval, was the
// binding constraint (see the window gate below, now widened to "any time
// outside the sleep window").
#define OTA_AUTOCHECK_INTERVAL_US (6LL * 60 * 60 * 1000000)

// OTA_FAILED is not allowed to be terminal (field bug: it used to stay
// wedged until a manual power cycle) — see dial_ota_clear_stale_failure()'s
// comment. This is the time-based half of that fix; CMD_OTA_CLEAR_FAILED
// (scr_settings.c's screen teardown) is the immediate half.
#define OTA_FAILED_AUTOCLEAR_US (25LL * 1000000)

#define BACKOFF_MIN_S  5
#define BACKOFF_MAX_S 60

// How long the QR stays up waiting for consent before it's refreshed, and how
// finely that wait is chopped. The slice is only a responsiveness knob — it
// bounds how long a reboot command posted from the link screen waits to be
// noticed, so it wants to be well under the ~300ms that reads as instant, and
// costs one queue peek each time round.
#define CONSENT_WINDOW_MS 300000
#define CONSENT_SLICE_MS     250
// Poll the relay mailbox at most this often. Reboot servicing still runs every
// CONSENT_SLICE_MS; decoupling the poll cadence from it keeps a fresh TLS
// handshake off the wire ~4x/s (the 2026-07-28 router flood-protection incident)
// while leaving the link screen's reboot controls responsive between polls.
#define RELAY_POLL_INTERVAL_MS 1500

// Discovery + anonymous client registration are the steps that must complete
// before the Orion-link QR can even be built (the authorize URL needs the
// authorization_endpoint from discovery and the client_id from registration).
// They're quick public-metadata/registration calls that have nothing to do with
// the user's account yet. The very first HTTPS request after a fresh Wi-Fi
// association routinely misses (DNS/ARP/TLS warmup), so retry these fast and
// quietly — staying on "Linking to Orion..." — for the first several attempts
// before treating it as a real outage. Without this, a freshly flashed dial fell
// into the steady-state "Orion unreachable" 5->60s backoff and made the user sit
// through several retry cycles before the QR appeared.
#define PREP_FAST_RETRIES 3      // fast attempts before falling to slow backoff
// 5s, not the old 1.5s: rapid fresh TLS connections are exactly what home-
// router flood protection rate-limits per device (field incident 2026-07-28
// — the dial's own retry pace kept the throttle tripped for hours). Still
// covers the boot-time SNTP race the fast window exists for, at a cadence
// that reads as a polite client instead of an attack.
#define PREP_RETRY_MS  5000

static const char *zone_id_str(zone_idx_t z) { return z == ZONE_A ? "zone_a" : "zone_b"; }

/* ---- rotary knob ------------------------------------------------------ */
// GPIO8 (A) / GPIO7 (B); only electrically live under the full board init.
// Callbacks run in the decoder's esp_timer task: feed the router's atomic
// accumulator and nothing else (blocking here stalls lv_tick_inc).
#define KNOB_A 8
#define KNOB_B 7
static knob_handle_t s_knob;

// No haptic per detent: the encoder is mechanically detented, so a motor
// pulse on top of each click is redundant and reads as noise (owner,
// 2026-07-29). This is where that pulse used to fire -- in the decoder's own
// task, ahead of the router -- which is why muting the router's dispatch
// alone did not silence it. Range-end feedback survives, but it is raised by
// the screens themselves via dial_haptics_play_soft().
//
// A detent arriving in standby wakes the screen and is consumed — a 3am
// reach must not change the temp.
static void knob_step(int dir)
{
    dial_state_stamp_input();
    if (dial_power_wake_consumes()) return;
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
    // OTA install takeover (M6 UX hardening): once the confirmed install on
    // SCR_UPDATE actually starts pulling bytes, lock the user onto a
    // dedicated full-screen progress ring until the device reboots (success
    // — dial_ota_download_and_apply()'s esp_restart() never returns, so this
    // never even gets a chance to route away) or the install fails. Checked
    // ahead of everything else, the same forced-navigation trick the welcome
    // splash below uses, so a routine poll or phase blip can never surface a
    // normal screen mid-download. READY_REBOOT (image written + verified,
    // about to restart) stays on the same screen — it's the tail of this
    // same flow, not a new one.
    if (st->ota.status == OTA_DOWNLOADING || st->ota.status == OTA_READY_REBOOT) {
        // An UNATTENDED install on a sleeping dial stays dark: nobody asked
        // for it, and lighting a bedside screen for two minutes to narrate an
        // update nobody is watching is exactly the interruption this
        // firmware's night handling exists to avoid. If the display is awake
        // — the user is present, or wakes it mid-install — the takeover still
        // wins, because a dial that reboots under someone's hands with no
        // explanation is worse than the interruption.
        // ...and once it IS showing, it stays for the rest of the install.
        // Without the ui_router_current() clause an unattended install that a
        // user woke, looked at, and walked away from would drop back to the
        // clock face mid-download and then reboot with no explanation.
        if (!st->ota.unattended || dial_power_level() != DPWR_STANDBY ||
            ui_router_current() == SCR_UPDATING)
            return SCR_UPDATING;
    }
    // Status moved off the takeover's two states while we were still showing
    // it — only OTA_FAILED does this in practice (the stale-tap guard on
    // CMD_OTA_APPLY keeps a race from reaching here any other way). Back to
    // Update (M7: moved off scr_about.c), where the stacked error line under
    // the row says why.
    if (ui_router_current() == SCR_UPDATING)
        return SCR_UPDATE;

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
        // A password the router rejected sends the user straight back to the
        // password screen FOR THE SAME NETWORK, which is the only place they can
        // do anything about it. Landing them at the start of setup — as this
        // did — makes them re-pick the network to fix a typo.
        if (st->wifi_join_failed && st->wifi_join_idx >= 0) {
            *arg = (void *)(uintptr_t)st->wifi_join_idx;
            return SCR_PASSKEY;
        }
        // Setting up on the dial (network list, password entry) is a deliberate
        // journey inside this phase — a routine state commit must not yank the
        // user back to the instructions screen halfway through typing.
        //
        // SCR_CONNECTING is deliberately NOT in this set: it is also the boot
        // screen, so it is "current" every time the portal first comes up, and
        // making it sticky here pinned the UI on "Connecting to Wi-Fi..." while
        // the portal ran underneath, forever. The connect attempt shows the
        // connecting screen through the PHASE instead (PH_WIFI_CONNECTING),
        // which the passkey screen sets before it hands the credentials over.
        screen_id_t cur = ui_router_current();
        if (cur == SCR_NETPICK || cur == SCR_PASSKEY) return cur;
        return SCR_WIFI_PORTAL;
    }
    case PH_OAUTH_WAIT_CONSENT: {
        // Same "never trap the user" rule the no-device-state block below
        // spells out, applied to the link step. This phase used to pin the QR
        // outright, which made the link screen the one genuinely modal place
        // in the UI — and the worst possible place for that, because the
        // OAuth callback can only arrive over the dial's OWN LAN. "My phone
        // is on the wrong network" and "this dial is on the wrong network"
        // are the two likeliest reasons to be stuck staring at this code, and
        // the second is fixed from SCR_WIFI, which was unreachable from here.
        // A deliberately opened menu or sub-screen stays put; everything else
        // still falls back to the QR, so swiping back off the menu returns to
        // the code on the next tick with nothing to re-arm.
        screen_id_t cur = ui_router_current();
        if (cur == SCR_MENU || cur == SCR_SETTINGS || cur == SCR_ABOUT ||
            cur == SCR_WIFI || cur == SCR_BRIGHTNESS ||
            cur == SCR_BRIGHTNESS_MENU || cur == SCR_UPDATE)
            return cur;
        return SCR_OAUTH_QR;
    }
    case PH_READY:
    case PH_DEGRADED:
    case PH_WIFI_LOST:
        // Once we have device state, stay on the dial (with its staleness dot)
        // through transient outages rather than yanking the user to a status
        // screen mid-interaction.
        if (st->have_state) {
            // Boost, the Schedule/Hold picker, and settings are transient
            // overlays reached by a deliberate action; a routine state
            // commit (poll landing, night-mode flip, ...) must not yank the
            // user back to the dial mid-flow. Returning the current screen
            // unchanged is a no-op in ui_router_go (same id + same arg), so
            // this is safe every tick.
            screen_id_t cur = ui_router_current();

            // Update prompt (docs/SPEC-update-prompt.md, reworked): the
            // worker raises ota_prompt_due on a WAKE EDGE (STANDBY/DIMMED ->
            // ACTIVE) when every entry gate holds at that instant, not a
            // continuously re-evaluated condition -- the earlier design
            // re-checked its gates every idle tick, so a touch (which resets
            // idle time) withdrew the sheet mid-reach. The worker itself
            // still withdraws the offer, but only on one of three explicit
            // exit checks (display back to STANDBY, night begins, update no
            // longer available -- see that gate's own "exit" comment), so it
            // survives being touched. When it does withdraw, this falls
            // through to the normal fallback below and lands back on
            // SCR_DIAL/SCR_STANDBY same as any other abandoned sub-screen.
            // ENTRY is scoped to SCR_DIAL specifically -- not Settings/Menu/
            // Wi-Fi/etc. -- so a routine background flag can never yank the
            // user out of a screen they navigated to on purpose (the same
            // restraint every other branch below already applies to a
            // routine poll landing); cur == SCR_UPDATE_PROMPT keeps it
            // sticky once shown, same shape as QUICK/BOOST/SETTINGS below,
            // so a poll landing mid-decision can't yank it away either.
            if (st->ota_prompt_due && (cur == SCR_DIAL || cur == SCR_UPDATE_PROMPT)) {
                *arg = (void *)(uintptr_t)st->ui_zone;
                return SCR_UPDATE_PROMPT;
            }

            // The menu face and its passive sub-screens (WIFI/ABOUT/UPDATE)
            // are reached by swipe/tap and join the sticky set below, but
            // unlike QUICK/BOOST/SETTINGS/BRIGHTNESS_MENU/ADJUST_MODE (which
            // only leave via a deliberate user action) they're also
            // dismissed by the standby idle timeout — someone can swipe
            // there and fall asleep on it — so that check must win over
            // stickiness, checked BEFORE folding them into the sticky-set
            // return. UPDATE joined this set at M7 (moved off ABOUT, which
            // was already here) — it's the one sub-screen where getting
            // yanked away mid-check/mid-confirm by a routine poll commit
            // would be user-visibly broken, not just an inconvenience.
            // DIAG belongs here for the same reason ABOUT does: it is a
            // read-only face someone swipes to and can fall asleep on, and a
            // routine poll commit must not yank them off it mid-read.
            bool passive = cur == SCR_MENU ||
                           cur == SCR_WIFI || cur == SCR_ABOUT || cur == SCR_UPDATE ||
                           cur == SCR_DIAG;
            if (passive && dial_power_level() == DPWR_STANDBY) {
                *arg = (void *)(uintptr_t)st->ui_zone;
                return SCR_STANDBY;
            }
            // ADJUST_MODE joins BRIGHTNESS_MENU here (not the idle-dismissed
            // passive set above): both are Settings sub-screens reached by a
            // deliberate tap, and a routine poll landing mid-choice must not
            // yank the user off either one.
            if (passive || cur == SCR_BOOST || cur == SCR_SETTINGS ||
                cur == SCR_BRIGHTNESS_MENU || cur == SCR_ADJUST_MODE) return cur;
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
        // Never trap the user (field incident 2026-07-28): with no device
        // state the connect/error screen used to own the display outright,
        // making Settings' Re-link, Wi-Fi change, and About's update —
        // the only tools that FIX a stuck dial — unreachable. A deliberately
        // opened menu face or sub-screen stays put; scr_connecting offers
        // the swipe that gets there.
        {
            screen_id_t cur = ui_router_current();
            if (cur == SCR_MENU || cur == SCR_SETTINGS || cur == SCR_ABOUT ||
                cur == SCR_WIFI || cur == SCR_BRIGHTNESS ||
                cur == SCR_BRIGHTNESS_MENU || cur == SCR_UPDATE)
                return cur;
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

// Shared by mut_device_state (a poll) and mut_relief_ack (the direct result
// of the boost call we just issued): true when `keep` -- the zone's
// currently-committed relief_active/heat/end/prev -- was set optimistically
// (relief_opt_us != 0) more recently than RELIEF_OPTIMISTIC_WINDOW_US, AND
// `fresh` -- the incoming poll/ack data -- disagrees with it on the one bit
// the chip actually renders (relief_active). Disagreeing on anything else
// (exact end time, say) is not treated as a conflict -- once the two sides
// agree the zone is (in)active, the incoming data's own end/heat/prev is
// trusted immediately, no reason to hold those back too. `as_of_us` is
// passed in (poll_started_us for a poll, a freshly-read "now" for an ack)
// rather than re-read here, so every zone this call touches agrees on the
// same instant.
static bool relief_should_preserve_optimistic(const zone_state_t *keep,
                                               const zone_state_t *fresh,
                                               int64_t as_of_us)
{
    if (!keep->relief_opt_us) return false;
    if (as_of_us - keep->relief_opt_us >= RELIEF_OPTIMISTIC_WINDOW_US) return false;
    return keep->relief_active != fresh->relief_active;
}

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
        st->zones[z].sched_smart_temp_active    = keep.sched_smart_temp_active;
        st->zones[z].sched_phase1_offset_min    = keep.sched_phase1_offset_min;
        st->zones[z].sched_phase1_temp_c        = keep.sched_phase1_temp_c;
        st->zones[z].sched_phase2_offset_min    = keep.sched_phase2_offset_min;
        st->zones[z].sched_phase2_temp_c        = keep.sched_phase2_temp_c;
        // hold_until_min is worker-computed (compute_hold_until_min), not
        // carried by get_device_state, so it belongs in this preserve list
        // like every sched_* field above. Without it each ~10s poll zeroed
        // the field -- and 0 is a LEGAL value (midnight), so the pill read
        // "Until 12:00" on an idle dial with no active phase at all, which
        // is exactly what the owner saw. The worker only re-commits on a
        // CHANGE, so once the poll clobbered it to 0 it stayed there.
        st->zones[z].hold_until_min             = keep.hold_until_min;

        if (predates_input) {                       // see the note above
            st->zones[z].on     = keep.on;
            st->zones[z].temp_c = keep.temp_c;
            strlcpy(st->zones[z].thermal_state, keep.thermal_state,
                    sizeof(st->zones[z].thermal_state));
        }

        // relief_opt_us is dial-local (see its comment in dial_state.h) and
        // never carried by get_device_state, so -- like hold_until_min above
        // -- it must survive the wholesale copy or every poll would zero it
        // and permanently disable the optimistic-preserve check below.
        st->zones[z].relief_opt_us = keep.relief_opt_us;
        if (relief_should_preserve_optimistic(&keep, &d->zones[z], d->poll_started_us)) {
            // This poll's relief_active hasn't caught up to the boost start/
            // cancel we just optimistically committed -- keep our guess
            // (the wholesale copy above already applied the poll's version;
            // put ours back) rather than let the chip flicker to the stale
            // answer and then flicker again on the next poll once the server
            // does catch up.
            st->zones[z].relief_active      = keep.relief_active;
            st->zones[z].relief_heat        = keep.relief_heat;
            st->zones[z].relief_end_ms      = keep.relief_end_ms;
            st->zones[z].relief_prev_temp_c = keep.relief_prev_temp_c;
            st->zones[z].relief_prev_on     = keep.relief_prev_on;
        } else {
            st->zones[z].relief_opt_us = 0;   // poll agrees, or the window lapsed -- server wins from here
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

// temp_min_f/temp_max_f: -1 = temperature_range wasn't present/numeric this
// discovery (see orion_discover_device()) -- mut_identity below leaves the
// store's existing value alone then, rather than clobbering a previously
// good range with "unknown".
typedef struct {
    char names[ZONE_COUNT][24];
    char serial[16];
    int  temp_min_f, temp_max_f;
} device_identity_t;

static void mut_identity(app_state_t *st, void *arg)
{
    device_identity_t *n = arg;
    for (int z = 0; z < ZONE_COUNT; z++)
        strlcpy(st->zones[z].user_name, n->names[z], sizeof(st->zones[z].user_name));
    strlcpy(st->serial, n->serial, sizeof(st->serial));
    // Device-reported absolute temperature range (owner: "use the Orion
    // reported min/max for limits, not our own") -- see app_state_t.temp_min_f's
    // comment for what reads this and the DIAL_TEMP_MIN_F/MAX_F fallback.
    if (n->temp_min_f >= 0 && n->temp_max_f >= 0) {
        st->temp_min_f = n->temp_min_f;
        st->temp_max_f = n->temp_max_f;
    }
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
// ui_temp_f alone (these commands come from the dial face's boost buttons /
// SCR_BOOST, never from a knob turn on the dial, so there's normally no
// optimistic temp in flight for this zone to clobber or preserve).
//
// This IS normally the authoritative settling point for the optimistic guess
// handle_immediate_cmd commits before issuing the call — CMD_BOOST_START/
// CANCEL only need the guess to survive until this ack lands. But the owner
// has observed the server take a beat to actually reflect a just-issued
// relief change, sometimes even in this call's OWN response — an ack that
// arrives saying "not active yet" would otherwise stomp the correct
// optimistic state right back out. So this is gated by the same
// relief_should_preserve_optimistic() check as mut_device_state's poll: if
// the response disagrees with a still-fresh optimistic guess, keep the guess
// (still apply temp/on — this ack IS the authoritative answer for those) and
// let a later poll or a retried ack settle it; only once they agree (or the
// window lapses) does the ack's own relief_* win and retire the guess.
static void mut_relief_ack(app_state_t *st, void *arg)
{
    relief_ack_t *r = arg;
    int64_t now_us = esp_timer_get_time();
    for (int z = 0; z < ZONE_COUNT; z++) {
        if (!r->touched[z]) continue;
        zone_state_t keep = st->zones[z];
        st->zones[z].temp_c = r->zones[z].temp_c;
        st->zones[z].on     = r->zones[z].on;
        if (relief_should_preserve_optimistic(&keep, &r->zones[z], now_us))
            continue;   // relief_active/heat/end/prev + relief_opt_us stay as they are (the optimistic guess)
        st->zones[z].relief_active      = r->zones[z].relief_active;
        st->zones[z].relief_heat        = r->zones[z].relief_heat;
        st->zones[z].relief_end_ms      = r->zones[z].relief_end_ms;
        st->zones[z].relief_prev_temp_c = r->zones[z].relief_prev_temp_c;
        st->zones[z].relief_opt_us      = 0;   // ack agrees — server wins from here
    }
}

// Optimistic relief commit for CMD_BOOST_START/CANCEL (main.c's
// handle_immediate_cmd) — mirrors dial_state_set_zone_on's "flip it before
// the round trip" shape for the boost chip instead of the power disc.
// `optimistic` distinguishes the two callers: true stamps relief_opt_us to
// esp_timer_get_time() (a guess made ahead of the call, which
// mut_device_state/mut_relief_ack must protect for a while — see
// RELIEF_OPTIMISTIC_WINDOW_US); false is a revert-on-failure back to the
// pre-tap truth, which needs no protecting (it isn't a guess, and stamping it
// would only block the very next poll from correcting a genuinely stale
// revert).
typedef struct {
    zone_idx_t zone;
    bool    active;
    bool    heat;
    int64_t end_ms;
    float   prev_temp_c;
    bool    optimistic;
} relief_optimistic_t;

static void mut_relief_optimistic(app_state_t *st, void *arg)
{
    relief_optimistic_t *o = arg;
    zone_state_t *zs = &st->zones[o->zone];
    zs->relief_active      = o->active;
    zs->relief_heat        = o->heat;
    zs->relief_end_ms      = o->end_ms;
    zs->relief_prev_temp_c = o->prev_temp_c;
    zs->relief_opt_us      = o->optimistic ? esp_timer_get_time() : 0;
}


static void mut_away(app_state_t *st, void *arg) { st->away = *(bool *)arg; }

// Tonight schedule (M5) snapshot from get_sleep_schedules — TODAY's entry
// only, one per zone (via the worker's uuid map, see s_zone_uuid below).
typedef struct {
    bool  valid;
    char  bedtime[6];
    float bedtime_temp_c;
    char  wakeup[6];
    float wakeup_temp_c;
    // Smart-temperature phase fields — see zone_state_t's comment in
    // dial_state.h for what these mean.
    bool  smart_temp_active;
    int   phase1_offset_min;
    float phase1_temp_c;
    int   phase2_offset_min;
    float phase2_temp_c;
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
        st->zones[z].sched_smart_temp_active    = s->zones[z].smart_temp_active;
        st->zones[z].sched_phase1_offset_min    = s->zones[z].phase1_offset_min;
        st->zones[z].sched_phase1_temp_c        = s->zones[z].phase1_temp_c;
        st->zones[z].sched_phase2_offset_min    = s->zones[z].phase2_offset_min;
        st->zones[z].sched_phase2_temp_c        = s->zones[z].phase2_temp_c;
    }
}

/*
 * "Dial adjusts" (Follow schedule vs. Hold tonight): which segment of
 * tonight's sleep schedule is active RIGHT NOW, so a knob turn during that
 * segment can retarget just that segment's temp field via
 * override_sleep_schedule_tonight instead of blindly holding for the rest of
 * the night (see orion_set_temp below). Schedule markers, in order from
 * bedtime: bedtime -> bedtime+phase_1_offset -> bedtime+phase_2_offset ->
 * wakeup; each marker's temp field governs from itself up to the next one.
 * wakeup's own temp is treated as active for a grace window after the wake
 * clock time too (SLEEP_PHASE_WAKE_GRACE_MIN, the same +30min slop the
 * steady-state loop below already uses to call the palette/haptics window
 * "still nighttime") — someone who turns the dial in the few minutes after
 * their alarm is still adjusting tonight's session, not starting a fresh
 * daytime hold. Past that grace, or before bedtime, this is SLEEP_PHASE_NONE
 * ("outside the sleep window") and the caller must fall back to a plain hold.
 *
 * All arithmetic is done in minutes-SINCE-BEDTIME on a rolling 24h wheel, not
 * raw clock minutes, so a boundary crossing midnight — bedtime 21:30 -> wake
 * 08:00 is the normal case, and a phase offset can independently push its own
 * boundary past midnight too — is not a special case: every boundary just
 * wraps the same way.
 */
typedef enum {
    SLEEP_PHASE_NONE = 0,   // outside tonight's sleep window, or schedule unusable
    SLEEP_PHASE_BEDTIME,
    SLEEP_PHASE_1,
    SLEEP_PHASE_2,
    SLEEP_PHASE_WAKEUP,
} sleep_phase_t;

#define SLEEP_PHASE_WAKE_GRACE_MIN 30

// `out_boundary_since` (nullable): when a phase IS active, receives the
// clock-minutes-from-midnight at which it hands off to the next one — the
// dial's status pill (§3, scr_dial.c) reads this as "Until H:MM" via
// compute_hold_until_min() below. NULL for callers (temp_write_phase) that
// only care which field to override, not when it expires.
static sleep_phase_t sleep_phase_now(const zone_state_t *z, int now_min, int *out_boundary_since)
{
    if (!z->sched_valid || !z->sched_smart_temp_active) return SLEEP_PHASE_NONE;
    int bed_min, wake_min;
    if (!dial_parse_hhmm(z->sched_bedtime, &bed_min)) return SLEEP_PHASE_NONE;
    if (!dial_parse_hhmm(z->sched_wakeup, &wake_min))  return SLEEP_PHASE_NONE;

    int since      = ((now_min  - bed_min) % 1440 + 1440) % 1440;   // now,     minutes-since-bedtime
    int wake_since = ((wake_min - bed_min) % 1440 + 1440) % 1440;   // wakeup,  minutes-since-bedtime
    if (wake_since == 0) wake_since = 1440;   // wakeup == bedtime clock time: a full 24h window, not zero-length

    // Clamp the offsets so a bad/duplicate/overrunning value from the API can
    // never order phase_2 before phase_1 or push either past the window it
    // lives in — worst case a clamped offset just collapses that phase to
    // zero width rather than misreporting which one is active.
    int p1 = z->sched_phase1_offset_min < 0 ? 0 : z->sched_phase1_offset_min;
    int p2 = z->sched_phase2_offset_min < p1 ? p1 : z->sched_phase2_offset_min;
    if (p1 > wake_since) p1 = wake_since;
    if (p2 > wake_since) p2 = wake_since;

    sleep_phase_t phase;
    int boundary_since;   // minutes-since-bedtime the ACTIVE phase hands off at
    if (since < p1)                                       { phase = SLEEP_PHASE_BEDTIME; boundary_since = p1; }
    else if (since < p2)                                  { phase = SLEEP_PHASE_1;       boundary_since = p2; }
    else if (since < wake_since)                          { phase = SLEEP_PHASE_2;       boundary_since = wake_since; }
    else if (since < wake_since + SLEEP_PHASE_WAKE_GRACE_MIN)
                                                           { phase = SLEEP_PHASE_WAKEUP;  boundary_since = wake_since + SLEEP_PHASE_WAKE_GRACE_MIN; }
    else return SLEEP_PHASE_NONE;

    if (out_boundary_since) *out_boundary_since = ((bed_min + boundary_since) % 1440 + 1440) % 1440;
    return phase;
}

// The exact get_sleep_schedules/override_sleep_schedule_tonight field name
// for a phase's temp — the ONLY field an override write may carry (see
// orion_set_temp below): sending bedtime/wakeup alongside it would silently
// move the schedule's clock times too.
static const char *sleep_phase_field(sleep_phase_t p)
{
    switch (p) {
    case SLEEP_PHASE_BEDTIME: return "bedtime_temp";
    case SLEEP_PHASE_1:       return "phase_1_temp";
    case SLEEP_PHASE_2:       return "phase_2_temp";
    case SLEEP_PHASE_WAKEUP:  return "wakeup_temp";
    default:                  return NULL;
    }
}

// Commits one zone's compute_hold_until_min() result (§3) into the store —
// see zone_state_t.hold_until_min's own comment for what it drives.
typedef struct { zone_idx_t zone; int16_t hold_until_min; } hold_until_t;
static void mut_hold_until(app_state_t *st, void *arg)
{
    hold_until_t *u = arg;
    st->zones[u->zone].hold_until_min = u->hold_until_min;
}

static void mut_oauth_url(app_state_t *st, void *arg) { strlcpy(st->oauth_url, arg, sizeof(st->oauth_url)); }
static void mut_retry_in(app_state_t *st, void *arg)  { st->retry_in_s = *(int *)arg; }
static void mut_ap_ssid(app_state_t *st, void *arg)   { strlcpy(st->ap_ssid, arg, sizeof(st->ap_ssid)); }
static void mut_sta_ssid(app_state_t *st, void *arg)  { strlcpy(st->sta_ssid, arg, sizeof(st->sta_ssid)); }
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
    st->ota.pending_verify = info->pending_verify;
}

// Fetches the fresh dial_ota_get() snapshot and commits it.
static void commit_ota_snapshot(void)
{
    dial_ota_info_t info;
    dial_ota_get(&info);
    dial_state_commit(mut_ota, &info);
}

// Update prompt (docs/SPEC-update-prompt.md). The worker RAISES
// ota_prompt_due on a wake edge (via this mutator, see the "Update prompt:
// entry" gate below) and LOWERS it itself on one of three exit conditions
// (see "Update prompt: exit", same use site); scr_update_prompt.c also
// LOWERS it directly (dial_state_clear_ota_prompt_due) the instant the user
// acts — the same asymmetric set/clear split this file already uses for
// fresh_device (set here) / welcomed (cleared by scr_welcome.c).
static void mut_ota_unattended(app_state_t *st, void *arg) { st->ota.unattended = *(bool *)arg; }

static void mut_ota_prompt_due(app_state_t *st, void *arg) { st->ota_prompt_due = *(bool *)arg; }

// Auto-update two-strikes tracking (spec): worker-only, deliberately NOT
// persisted to NVS or mirrored into app_state_t — a device that fails an
// overnight install doesn't reboot (only a SUCCESSFUL apply does, via
// esp_restart() below), so this naturally survives every retry across many
// nights within one boot session; a rare manual power cycle just re-arms
// it, which is fine either way ("retry the next day" already covers it).
// The failure itself still surfaces on SCR_UPDATE — see the idle loop's
// stale-failure auto-clear gate below — by simply leaving dial_ota's own
// OTA_FAILED/.err alone (no new UI surface needed).
static char s_ota_auto_fail_ver[16];
static int  s_ota_auto_fail_count;
// At most one auto-install attempt per overnight-window OCCURRENCE (success
// or fail): latched the instant an attempt starts, re-armed when the clock
// walks back outside the window so tomorrow's occurrence gets its own try —
// without this, a failed attempt would retry every ~300ms for the rest of
// the ~2h window instead of "the next day" (spec).
static bool s_ota_auto_attempted;
// Live "is the prompt sheet currently raised" flag, mirrored into
// app_state_t.ota_prompt_due only on a false<->true transition — same
// edge-triggered shape as s_ui_night/s_ui_clock_valid below, so a tick where
// nothing changed doesn't bump the generation (and re-render everything) for
// no reason. Unlike the shipped v1.0-1.2 design, this is no longer a live
// re-evaluation of a gate table: it's set true exactly once per wake (see
// s_ota_prev_pwr_level below) and only ever cleared by the three explicit
// exit checks (or by scr_update_prompt.c itself, on a deliberate user
// action) — see the "Update prompt: entry" / "Update prompt: exit" comments
// at this flag's use site for the full story.
static bool s_ota_prompt_live;

// dial_power_level() as of the PREVIOUS idle tick, sampled every tick
// regardless of clock validity — the wake-edge gate below needs a
// STANDBY/DIMMED -> ACTIVE transition, which a level re-read at a single
// instant can't tell from "has been ACTIVE the whole time". Seeded ACTIVE
// (the real boot-time level, dial_power.c) so the very first tick can never
// look like a wake — the spec explicitly wants the prompt to never raise on
// the initial boot transition, only on an OBSERVED prior standby.
static dial_power_level_t s_ota_prev_pwr_level = DPWR_ACTIVE;

// OTA rollback health check (M6): dial_ota_mark_valid_if_pending() is
// idempotent, but there's nothing left to confirm after the first success —
// this guards against re-reading the OTA partition state on every ~10s poll
// for the rest of the device's uptime.
//
// Field incident: this used to be the ONLY confirm path, reached solely via
// "first successful Orion poll" below (and its steady-state twin) — 30-60+s
// after boot, and hostage to Wi-Fi + TLS + OAuth + MCP + a poll ALL
// succeeding. A user power-cycled inside that window and the bootloader
// silently reverted a good install because it never got the chance to
// confirm; worse, on a network outage post-update it could never confirm at
// all. Rollback exists to catch a genuinely BROKEN image, not to hold a good
// one hostage to a cloud outage — a broken image crash-loops well inside 30
// seconds. See the 30s ota_confirm_timer_cb fallback armed in app_main:
// whichever of that timer or a real poll success gets here first wins (this
// function is idempotent via s_ota_confirmed), and the loser's call becomes
// a no-op.
static bool s_ota_confirmed;
static void ota_confirm_once(void)
{
    if (s_ota_confirmed) return;
    dial_ota_mark_valid_if_pending();
    s_ota_confirmed = true;
    // Push the fresh snapshot (pending_verify very likely just flipped
    // false) so scr_connecting/scr_dial's "Finalizing update" notice drops
    // immediately, instead of waiting for some unrelated OTA-mirror commit.
    commit_ota_snapshot();
}

// The 30s stable-boot fallback itself (armed once from app_main). Runs in
// the esp_timer task, not an ISR -- esp_timer's default ESP_TIMER_TASK
// dispatch method, so calling into esp_ota_ops (via ota_confirm_once ->
// dial_ota_mark_valid_if_pending) here is safe, same as calling it from the
// worker task. The only cross-task hazard is s_ota_confirmed itself, which
// is a plain bool read-then-write from two possible callers (this timer's
// task and the worker task) with no lock; the benign race is a redundant
// dial_ota_mark_valid_if_pending() call if both sides read it false before
// either sets it true -- that function is itself idempotent (re-marking an
// already-valid partition, or re-reading a state that's already settled), so
// the worst case is one harmless extra flash-state read, never a double
// confirm or a lost one.
static void ota_confirm_timer_cb(void *arg)
{
    (void)arg;
    ota_confirm_once();
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

// Per-zone hold_until_min actually committed to the store (§3's pill),
// mirroring s_ui_night's edge-triggered shape so a tick where neither zone's
// value moved doesn't bump the generation for no reason. -2 (not a legal
// minutes-from-midnight value, nor -1's "Holding") so the very first idle
// tick always commits the real answer instead of assuming it agrees with
// dial_state_init()'s -1 default.
static int16_t s_ui_hold_until[ZONE_COUNT] = { -2, -2 };

/* ---- Orion MCP calls (worker task only) -------------------------------- */

static char s_serial[16];

// Set by orion_discover_device: list_devices answered (no transport/auth
// failure) but the account has no topper registered at all. Distinct from
// every other discovery failure, which is why the supervisor loop below
// gives it its own phase_err instead of dial_mcp_last_error()'s generic text.
static bool s_no_orion_devices;

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
    cJSON *prev_on = cJSON_GetObjectItem(relief, "previous_on");
    zs->relief_active       = true;
    zs->relief_heat         = (type && type->valuestring && !strcmp(type->valuestring, "heat"));
    zs->relief_end_ms       = cJSON_IsNumber(end) ? (int64_t)end->valuedouble : 0;
    zs->relief_prev_temp_c  = cJSON_IsNumber(prev) ? (float)prev->valuedouble : zs->temp_c;
    zs->relief_prev_on      = cJSON_IsBool(prev_on) ? cJSON_IsTrue(prev_on) : zs->on;
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

// "Dial adjusts": which sleep-schedule phase (if any) a temp write for `zone`
// should retarget right now. Every precondition must hold, or this returns
// SLEEP_PHASE_NONE (the caller then falls back to a plain hold) — see the
// worked list of preconditions in orion_set_temp's own comment below. Reads
// s_zone_uuid (worker-side zone->uuid map, populated in orion_discover_device)
// because override_sleep_schedule_tonight targets a specific Orion user_id —
// with no captured uuid for this zone there is nothing safe to target.
static sleep_phase_t temp_write_phase(const app_state_t *st, zone_idx_t zone)
{
    if (!st->sched_follow) return SLEEP_PHASE_NONE;
    if (!s_zone_uuid[zone][0]) return SLEEP_PHASE_NONE;
    struct tm lt;
    if (!dial_time_now(&lt)) return SLEEP_PHASE_NONE;   // no real wall clock yet -- don't guess
    return sleep_phase_now(&st->zones[zone], lt.tm_hour * 60 + lt.tm_min, NULL);
}

// UI mirror of temp_write_phase() (§3, scr_dial.c's status pill): the exact
// same four preconditions (Follow schedule, a captured uuid for THIS zone, a
// real clock, an active phase right now), so the pill reports the WRITE
// PATH'S BEHAVIOUR rather than just echoing the Adjustment-mode preference —
// Schedule mode with no active phase still falls back to a plain hold, and
// this must say so too. -1 = "Holding"; otherwise the active phase's own end
// boundary, in clock-minutes-from-midnight, for the pill's "Until H:MM".
// `now_min` is passed in (not re-read via dial_time_now) so every zone this
// tick agrees on exactly the same "now" the caller's night-window calc used.
static int16_t compute_hold_until_min(const app_state_t *st, zone_idx_t zone, int now_min)
{
    if (!st->sched_follow) return -1;
    if (!s_zone_uuid[zone][0]) return -1;
    int boundary_min;
    sleep_phase_t phase = sleep_phase_now(&st->zones[zone], now_min, &boundary_min);
    return (phase == SLEEP_PHASE_NONE) ? -1 : (int16_t)boundary_min;
}

typedef struct {
    zone_idx_t zone;
    float      temp_c;
    bool       used_override;   // OUT: true if this write went the schedule-override route
} set_temp_args_t;

// The temp write behind a knob turn / SCR_DIAL edit ("Dial adjusts", M8).
// Follow schedule: if temp_write_phase() finds a phase actually active right
// now, retarget ONLY that phase's own temp field via
// override_sleep_schedule_tonight (never bedtime/wakeup times, never the
// other phase's temp — sending more than the one field would silently move
// the rest of the schedule too) — this leaves the schedule engine in control
// of tonight's later phases, matching the Orion app. Hold tonight, no usable
// schedule, smart-temp off, outside the window, or no captured uuid — any of
// those makes temp_write_phase return NONE — falls straight to the plain
// set_zone hold, unchanged from pre-M8 behavior. And if the override call
// itself fails for any reason, this still falls back to set_zone rather than
// silently dropping the write: a missed override is a minor annoyance, but a
// dropped knob turn (or a wrong-field write at 3am) is not.
static bool orion_set_temp(void *arg)
{
    set_temp_args_t *a = arg;
    a->used_override = false;

    app_state_t st;
    dial_state_get(&st);
    const char *field = sleep_phase_field(temp_write_phase(&st, a->zone));

    if (field) {
        char args[160];
        snprintf(args, sizeof(args), "{\"user_id\":\"%s\",\"fields\":{\"%s\":%.1f}}",
                 s_zone_uuid[a->zone], field, a->temp_c);
        char *r = NULL;
        bool ok = dial_mcp_call_tool("override_sleep_schedule_tonight", args, &r);
        free(r);
        if (ok) { a->used_override = true; return true; }
        ESP_LOGW(TAG, "override_sleep_schedule_tonight (%s) failed, falling back to set_zone: %s",
                 field, dial_mcp_last_error());
        // fall through to the plain hold below
    }

    char f[32];
    snprintf(f, sizeof(f), "\"temp\":%.1f", a->temp_c);
    return orion_set_zone(a->zone, f);
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
    s_no_orion_devices = false;
    char *devices = NULL;
    if (!dial_mcp_call_tool("list_devices", "{}", &devices) || !devices) return false;

    cJSON *root = cJSON_Parse(devices);
    free(devices);
    if (!root) return false;

    bool ok = false;
    cJSON *arr  = cJSON_GetObjectItem(root, "devices");
    cJSON *dev0 = cJSON_IsArray(arr) ? cJSON_GetArrayItem(arr, 0) : NULL;
    if (!dev0 && cJSON_IsArray(arr)) {
        // list_devices is a well-formed, successful answer — the account
        // just doesn't have a topper on it yet. Not a transport/auth error,
        // so it shouldn't spin in the generic "unreachable" backoff forever.
        s_no_orion_devices = true;
    }
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
        ident.temp_min_f = ident.temp_max_f = -1;   // set below only if temperature_range parses
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

        // Relative-scale tripwire. Our −10…+10 tables (dial_state.h) are
        // compiled constants: relative mode is a client-side view, and Orion's
        // set_zone only takes °C, so there is nothing to adopt at runtime. But
        // if Orion ever reshapes temperature_scale.relative or its range, the
        // dial would silently show wrong levels. Re-validate the live payload
        // against the compiled tables and log LOUDLY on any mismatch — a
        // release-blocking signal to regenerate the tables, NOT a runtime
        // adoption (which would mean the worker mutating tables the LVGL task
        // reads). Log-only; touches no state, bumps no generation.
        cJSON *scale = cJSON_GetObjectItem(dev0, "temperature_scale");
        cJSON *rel   = scale ? cJSON_GetObjectItem(scale, "relative") : NULL;
        if (cJSON_IsArray(rel)) {
            int n = cJSON_GetArraySize(rel), mismatch = 0;
            if (n != 21)
                ESP_LOGE(TAG, "relative scale has %d entries, expected 21 — compiled table is stale", n);
            cJSON *e;
            cJSON_ArrayForEach(e, rel) {
                cJSON *in  = cJSON_GetObjectItem(e, "in");
                cJSON *out = cJSON_GetObjectItem(e, "out");
                if (!cJSON_IsNumber(in) || !cJSON_IsNumber(out)) continue;
                int lvl = in->valueint;
                int got = dial_rel_from_f(dial_c_to_f((float)out->valuedouble));
                if (got != lvl) {
                    mismatch++;
                    ESP_LOGE(TAG, "relative scale MISMATCH: level %d = %.1f C maps to our level %d "
                                  "-- regenerate DIAL_REL_F/DIAL_REL_LO_F", lvl, out->valuedouble, got);
                }
            }
            if (n == 21 && mismatch == 0)
                ESP_LOGD(TAG, "relative scale table matches compiled DIAL_REL");
        } else {
            ESP_LOGW(TAG, "list_devices has no temperature_scale.relative -- "
                          "relative mode uses the compiled fallback table");
        }
        // Device-reported ABSOLUTE range (owner: use Orion's own min/max, not
        // a hardcoded guess) -- captured into ident.temp_min_f/max_f below,
        // committed via mut_identity, and read everywhere absolute mode needs
        // it through dial_state_temp_min_f()/_max_f() (dial_state.h). The
        // lo/hi != DIAL_REL_MIN_F/MAX_F check right after is a SEPARATE
        // concern (unchanged): it's the relative-scale tripwire validating
        // the compiled DIAL_REL_F/DIAL_REL_LO_F level table, not this range.
        cJSON *range = cJSON_GetObjectItem(dev0, "temperature_range");
        if (range) {
            cJSON *mn = cJSON_GetObjectItem(range, "min");
            cJSON *mx = cJSON_GetObjectItem(range, "max");
            if (cJSON_IsNumber(mn) && cJSON_IsNumber(mx)) {
                int lo = dial_c_to_f((float)mn->valuedouble);
                int hi = dial_c_to_f((float)mx->valuedouble);
                ident.temp_min_f = lo;
                ident.temp_max_f = hi;
                if (lo != DIAL_REL_MIN_F || hi != DIAL_REL_MAX_F)
                    ESP_LOGE(TAG, "temperature_range %.0f-%.0f C = %d-%d F != relative rails %d-%d F",
                             mn->valuedouble, mx->valuedouble, lo, hi, DIAL_REL_MIN_F, DIAL_REL_MAX_F);
            }
        }

        if (ok) dial_state_commit(mut_identity, &ident);
    }
    cJSON_Delete(root);
    return ok;
}

// get_sleep_schedules {} returns {schedules: {"<uuid>": [ {day:0-6, bedtime,
// bedtime_temp, wakeup, wakeup_temp, is_override_available,
// is_override_applied, ...} x7 ]}} — one entry per user uuid. The override
// flags are documented here because the response carries them, but nothing
// reads them since the Tonight face was removed; they are deliberately not
// parsed. Pulls out
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

                // "Dial adjusts" (M8) phase fields — see sleep_phase_now()'s
                // comment above for what these mean.
                zs->smart_temp_active = cJSON_IsTrue(cJSON_GetObjectItem(entry, "is_smart_temperature_active"));
                cJSON *p1o = cJSON_GetObjectItem(entry, "phase_1_offset_minutes");
                cJSON *p1t = cJSON_GetObjectItem(entry, "phase_1_temp");
                cJSON *p2o = cJSON_GetObjectItem(entry, "phase_2_offset_minutes");
                cJSON *p2t = cJSON_GetObjectItem(entry, "phase_2_temp");
                if (cJSON_IsNumber(p1o)) zs->phase1_offset_min = (int)p1o->valuedouble;
                if (cJSON_IsNumber(p1t)) zs->phase1_temp_c     = (float)p1t->valuedouble;
                if (cJSON_IsNumber(p2o)) zs->phase2_offset_min = (int)p2o->valuedouble;
                if (cJSON_IsNumber(p2t)) zs->phase2_temp_c     = (float)p2t->valuedouble;
                break;   // one entry per day
            }
        }
    }
    cJSON_Delete(root);
    dial_state_commit(mut_schedules, &sc);
    return true;
}

/* ---- worker supervisor ------------------------------------------------- */

// Settings' three destructive actions: each erases some NVS state and reboots.
// There's no follow-up state commit because esp_restart() never returns — which
// is also what makes them the only commands a mid-wait worker can safely run
// (see wait_servicing_reboots): they need no MCP session, and nothing after the
// call site has to cope with having been interrupted.
static void run_reboot_cmd(const app_cmd_t *cmd)
{
    switch (cmd->kind) {
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
    default:
        break;
    }
}

static bool cmd_reboots(cmd_kind_t k)
{
    return k == CMD_RELINK || k == CMD_WIFI_RESET || k == CMD_FACTORY_RESET;
}

// Sleep `ms`, but honour a queued reboot command instead of sleeping through it.
//
// This task is the only thing that drains the command queue, and it only gets
// round to it once it has a live MCP session. So every long wait before that
// point — the OAuth consent window (five minutes) and the retry backoff — made
// Settings' reboot actions look broken: the screen posted the command, said
// "Restarting…", and then just sat there. Owner-reported against Change network
// from the link screen, which the new Wi-Fi link there made easy to reach.
//
// Only the reboot trio is run here; anything else needs state this task hasn't
// built yet and stays queued, untouched, for the normal drain. Hence peek
// rather than receive — taking a command we can't run would reorder the queue,
// and re-posting it would spin this loop for the rest of the wait.
static void wait_servicing_reboots(int ms)
{
    int64_t end = esp_timer_get_time() + (int64_t)ms * 1000;
    for (;;) {
        int64_t left_ms = (end - esp_timer_get_time()) / 1000;
        if (left_ms <= 0) return;
        app_cmd_t cmd;
        if (!dial_cmd_peek(&cmd, (int)left_ms)) return;   // window elapsed
        if (!cmd_reboots(cmd.kind)) {
            // Something we can't run is parked at the head and will stay there.
            // Serve out the rest of the wait in one sleep rather than peeking
            // at it again and again.
            left_ms = (end - esp_timer_get_time()) / 1000;
            if (left_ms > 0) vTaskDelay(pdMS_TO_TICKS(left_ms));
            return;
        }
        run_reboot_cmd(&cmd);   // does not return
    }
}

// Sleep `seconds` while publishing a countdown for the error screen.
static void backoff_wait(int seconds)
{
    for (int s = seconds; s > 0; s--) {
        dial_state_commit(mut_retry_in, &s);
        wait_servicing_reboots(1000);
    }
    int zero = 0;
    dial_state_commit(mut_retry_in, &zero);
}

// Consecutive refresh failures classified PERMANENT (dial_oauth_last_token_err_
// permanent -- RFC 6749 §5.2 invalid_grant). Two in a row, not one, so a single
// server-side fluke can't force a re-link; reset on any success or
// transient-classified failure. Steady state only calls with_auth_retry every
// ~10s (poll) or on a rare write, so two hits is at most ~20s to recover.
static int s_perm_refresh_failures = 0;

// 401-aware call wrapper: on failure, refresh the token, reopen the MCP
// session, retry once. Used for polls AND writes so an expired token never
// silently drops a command. If the refresh token itself is dead (not just
// this call), that never clears on its own -- after two consecutive
// permanent-classified refresh failures, forget the tokens and reboot into
// the QR consent screen, exactly like the manual CMD_RELINK path, instead of
// presenting the same dead token forever.
static bool with_auth_retry(bool (*call)(void *), void *arg,
                            const oauth_disc_t *disc, const char *client_id)
{
    if (call(arg)) { s_perm_refresh_failures = 0; return true; }
    if (dial_oauth_refresh(disc, client_id)) {
        s_perm_refresh_failures = 0;
        dial_oauth_release_connection();   // one connection to the host at a time
        dial_mcp_connect(NULL);
        return call(arg);
    }
    if (dial_oauth_last_token_err_permanent()) {
        if (++s_perm_refresh_failures >= 2) {
            ESP_LOGE(TAG, "refresh token permanently rejected (x%d) — re-linking", s_perm_refresh_failures);
            dial_oauth_forget();
            esp_restart();
        }
    } else {
        s_perm_refresh_failures = 0;   // transient -- e.g. network/5xx -- don't count it
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

// CMD_BOOST_START/CANCEL and CMD_AWAY are rare (a deliberate
// tap on a boost icon/pill/settings row, not a knob spin) — no coalescing,
// just run them in arrival order.
static void handle_immediate_cmd(const app_cmd_t *cmd, const oauth_disc_t *disc,
                                  const char *client_id)
{
    switch (cmd->kind) {
    case CMD_BOOST_START: {
        dial_power_inhibit(DPWR_INHIBIT_TASK, true);
        boost_args_t b = { cmd->zone, cmd->a != 0, cmd->b };

        // Optimistic (owner-reported: boost was the one control that didn't
        // react until the whole MCP round trip landed, sometimes 10-20s) —
        // commit the requested relief state BEFORE issuing the call, exactly
        // like dial_state_set_zone_on does for the power disc, so the chip
        // (and SCR_BOOST's hand-off back to the dial face) shows the
        // countdown on this same tick. end_ms uses the wall clock (time(),
        // matching how scr_dial.c's pill computes remain_ms), not
        // esp_timer_get_time() — harmless even before clock_valid, since the
        // pill only renders a countdown once the clock is valid anyway.
        // mut_relief_ack (inside orion_boost, on success) is the normal
        // settling point — see RELIEF_OPTIMISTIC_WINDOW_US's comment for how
        // it and the next poll avoid fighting this guess in the meantime; on
        // failure below, revert to exactly what was showing before the tap.
        app_state_t pre;
        dial_state_get(&pre);
        relief_optimistic_t opt = {
            .zone = cmd->zone, .active = true, .heat = b.heat,
            .end_ms = (int64_t)time(NULL) * 1000 + (int64_t)b.minutes * 60000,
            // Same "prefer the pending optimistic target" rule as
            // dial_state_set_relief_optimistic — see its comment. The rail-push
            // restore is queued just ahead of this command, so temp_c can still
            // be the rail the gesture crossed.
            .prev_temp_c = (pre.ui_temp_f[cmd->zone] >= 0)
                               ? dial_f_to_c(pre.ui_temp_f[cmd->zone])
                               : pre.zones[cmd->zone].temp_c,
            .optimistic = true,
        };
        dial_state_commit(mut_relief_optimistic, &opt);

        if (!with_auth_retry(orion_boost, &b, disc, client_id)) {
            relief_optimistic_t revert = {
                .zone = cmd->zone,
                .active = pre.zones[cmd->zone].relief_active,
                .heat = pre.zones[cmd->zone].relief_heat,
                .end_ms = pre.zones[cmd->zone].relief_end_ms,
                .prev_temp_c = pre.zones[cmd->zone].relief_prev_temp_c,
                .optimistic = false,
            };
            dial_state_commit(mut_relief_optimistic, &revert);
        }
        dial_power_inhibit(DPWR_INHIBIT_TASK, false);
        break;
    }
    case CMD_BOOST_CANCEL:
        dial_power_inhibit(DPWR_INHIBIT_TASK, true); {
        // Optimistic, mirroring START above — and, like the underlying
        // cancel_thermal_relief call itself (no zone arg — see cmd_kind_t's
        // comment), applied to BOTH zones: whichever one actually had relief
        // running clears now; the other is already inactive, so clearing it
        // too is a no-op.
        app_state_t pre;
        dial_state_get(&pre);
        for (int z = 0; z < ZONE_COUNT; z++) {
            relief_optimistic_t opt = { .zone = (zone_idx_t)z, .optimistic = true };
            dial_state_commit(mut_relief_optimistic, &opt);
        }
        if (!with_auth_retry(orion_boost_cancel, NULL, disc, client_id)) {
            for (int z = 0; z < ZONE_COUNT; z++) {
                relief_optimistic_t revert = {
                    .zone = (zone_idx_t)z,
                    .active = pre.zones[z].relief_active,
                    .heat = pre.zones[z].relief_heat,
                    .end_ms = pre.zones[z].relief_end_ms,
                    .prev_temp_c = pre.zones[z].relief_prev_temp_c,
                    .optimistic = false,
                };
                dial_state_commit(mut_relief_optimistic, &revert);
            }
        }
        dial_power_inhibit(DPWR_INHIBIT_TASK, false);
        break;
    }
    case CMD_AWAY: {
        dial_power_inhibit(DPWR_INHIBIT_TASK, true);
        bool away = cmd->a != 0;
        away_args_t a = { away };
        if (with_auth_retry(orion_set_away, &a, disc, client_id))
            dial_state_commit(mut_away, &away);
        dial_power_inhibit(DPWR_INHIBIT_TASK, false);
        break;
    }
    // Settings (M4) destructive actions — see run_reboot_cmd. Shared with the
    // long waits before this loop exists, so a "Restarting…" tap during setup
    // or a retry backoff isn't stuck behind them.
    case CMD_RELINK:
    case CMD_WIFI_RESET:
    case CMD_FACTORY_RESET:
        run_reboot_cmd(cmd);
        break;
    // Software update (M6/M7), from SCR_UPDATE's "Check for updates" row.
    // Gated on clock_valid the same as the M6 auto-check above
    // (dial_ota_check() is all HTTPS, and mbedTLS cert validation needs a
    // real wall clock) — but a manual tap is a user waiting on feedback,
    // not a background sweep, so a blocked check must say why rather than
    // silently doing nothing. st.beta selects the channel (M7) — see
    // dial_ota_check()'s own doc comment.
    case CMD_OTA_CHECK: {
        app_state_t st;
        dial_state_get(&st);
        // A MANUAL check re-arms the prompt: tapping "Check for updates" is a
        // user saying they care about updates right now, which outranks an
        // earlier "Later" (23h defer) or the once-a-day shown ceiling. Without
        // this there is no way back to the prompt short of waiting out the
        // timers — the owner lost it for a day to an accidental dismissal and
        // had no way to summon it again. The background check deliberately
        // does NOT do this; only a deliberate tap.
        dial_state_set_ota_defer(0);
        dial_state_set_ota_shown(0);
        // One TLS session at a time (see dial_mcp_release_connection): the
        // OTA client is about to open its own connection to GitHub, and a
        // second concurrent session fails its handshake on this build.
        dial_mcp_release_connection();
        // Hold the screen for the duration: the user tapped this and is
        // waiting on the answer. Released in the same handler below, which
        // restarts the idle clock so the result is readable even on a 5s
        // timeout.
        dial_power_inhibit(DPWR_INHIBIT_TASK, true);
        if (st.clock_valid) {
            // Show "Checking..." for the duration of the call instead of
            // flicking straight to the answer -- a tap with no visible
            // response reads as a dead button (owner feedback).
            dial_ota_mark_checking();
            commit_ota_snapshot();
            dial_ota_check(st.beta);
        }
        else dial_ota_set_blocked("waiting for time sync - try again shortly");
        commit_ota_snapshot();
        dial_power_inhibit(DPWR_INHIBIT_TASK, false);
        break;
    }
    case CMD_OTA_APPLY: {
        // Held from the tap, not from the moment DOWNLOADING is published:
        // esp_https_ota_begin can take seconds (and retries once), and until
        // status flips, nav_policy hasn't yet forced the SCR_UPDATING screen
        // whose own inhibit would cover this.
        dial_power_inhibit(DPWR_INHIBIT_TASK, true);
        // Stale-tap guard: the row's confirm is only armed while AVAILABLE,
        // but the store could have moved on (an unrelated auto-check landed,
        // say) between the tap and the worker draining this command.
        app_state_t st;
        dial_state_get(&st);
        if (st.ota.status != OTA_AVAILABLE) {
            dial_power_inhibit(DPWR_INHIBIT_TASK, false);   // early out must not leak the hold
            break;
        }
        // This command only ever arrives from a deliberate, confirmed user
        // tap (scr_update.c's tap-twice, or scr_update_prompt.c's "Update
        // now") — never from the unattended overnight path below, which
        // calls dial_ota_download_and_apply() directly. A human actively
        // watching this install is exactly the case the auto-updater's
        // two-failed-attempts brake (docs/SPEC-update-prompt.md) doesn't
        // need to protect against, so a manual attempt always gets to try,
        // and resets the strike count for next time the auto-updater looks.
        s_ota_auto_fail_count = 0;
        s_ota_auto_fail_ver[0] = 0;
        s_ota_last_committed_pct = -100;   // guarantee the first progress commit fires
        // Both clients hand their sockets back: the downloader opens its own
        // TLS session with bigger buffers, and it should not have to compete
        // with either of ours for memory (see dial_mcp_release_connection).
        dial_mcp_release_connection();
        dial_oauth_release_connection();
        bool attended = false;
        dial_state_commit(mut_ota_unattended, &attended);
        bool ok = dial_ota_download_and_apply(ota_progress_cb);
        commit_ota_snapshot();
        if (ok) {
            ESP_LOGI(TAG, "OTA image ready; rebooting into it");
            esp_restart();
        }
        dial_power_inhibit(DPWR_INHIBIT_TASK, false);
        break;
    }
    // scr_settings.c posts this from destroy() (screen teardown) so a FAILED
    // row never survives to the next visit — see dial_ota_clear_stale_failure()'s
    // comment. max_age_us=0: clear immediately regardless of how recently it
    // failed. A no-op (no commit) if status already moved off OTA_FAILED.
    case CMD_OTA_CLEAR_FAILED:
        if (dial_ota_clear_stale_failure(0)) commit_ota_snapshot();
        break;
    default:
        break;   // CMD_SET_TEMP/CMD_TOGGLE_ON never reach here (see the drain loop)
    }
}

/* ---- mDNS ---------------------------------------------------------------
 * NOTE: since the code-relay migration, LINKING NO LONGER USES mDNS. The OAuth
 * redirect_uri is now the hosted relay's /cb (a fleet-wide constant, reached
 * outbound over the internet), not a per-device .local hostname — so none of
 * the rationale below is load-bearing for onboarding any more. The bringup is
 * kept only as harmless network presence; linking was its sole consumer, so a
 * reviewer may delete mdns_bringup()/s_mdns_ok outright. The original reasoning
 * is preserved below for that decision.
 *
 * A stable orion-dial-xxxxxx.local hostname stands in for the DHCP IP in the
 * OAuth redirect_uri below. An IP-based redirect_uri silently invalidates the
 * registered OAuth client the moment the router hands out a new lease — the
 * DCR client_id is registered per redirect_uri, so a changed IP meant a
 * changed URI, which meant dial_oauth_ensure_client's cache-compare (below)
 * quietly re-registered a NEW client and orphaned the one the user had
 * already consented to, forcing them back through the QR flow. This bit the
 * owner repeatedly. mDNS names don't change with the lease, so once
 * registered this class of forced re-link can't happen again.
 *
 * Phones resolve ".local" out of the box — Bonjour on iOS/macOS, NSD on
 * Android — with no app install, which is exactly what's on hand to scan the
 * on-screen QR, so it's the right redirect host for this flow.
 */
static bool s_mdns_ok;

// Registered once, right after Wi-Fi comes up. Formerly this had to precede the
// OAuth callback server so the redirect_uri could point at the mDNS hostname;
// the relay migration removed both, so s_mdns_ok now has no reader and this is
// vestigial network presence (see the block note above). A registration
// failure (some networks block/filter multicast) is no longer load-bearing.
static void mdns_bringup(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns_init failed (%s)", esp_err_to_name(err));
        return;
    }
    mdns_hostname_set(dial_net_hostname());
    mdns_instance_name_set("Orion Dial");
    s_mdns_ok = true;
    ESP_LOGI(TAG, "mDNS up: %s.local", dial_net_hostname());
}

static void worker_task(void *arg)
{
    (void)arg;
    oauth_disc_t disc;
    char client_id[96];
    int backoff_s = BACKOFF_MIN_S;
    int prep_fast_retries = 0;   // see PREP_FAST_RETRIES: fast, quiet retries for
                                 // the pre-QR discovery + registration steps

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
        // boot) — the driver defaults to Auto and settings only writes on
        // taps, so without this an Off/Low/High preference reverts every reboot.
        app_state_t st;
        dial_state_get(&st);
        dial_haptics_set_level((haptic_level_t)st.haptics_level);
    }

    // ---- Wi-Fi (blocking bringup; portal phase published via events) ----
    dial_state_set_phase(PH_WIFI_CONNECTING, NULL);
    dial_net_bringup();
    // Bringup only returns once connected, so this is the home network's real
    // name -- SCR_OAUTH_QR names it explicitly (the callback that finishes
    // linking is a LAN redirect to the dial; a phone on cellular or a guest
    // network can never deliver it, and that trap has already cost a real
    // debugging session -- see the QR screen's own comment).
    dial_state_commit(mut_sta_ssid, (void *)dial_net_sta_ssid());
    dial_time_start();
    mdns_bringup();   // vestigial since the relay migration — see its note above

    // ---- OAuth + MCP with retry/backoff on every step ----
    for (;;) {
        dial_state_set_phase(PH_OAUTH_DISCOVER, NULL);

        // The redirect_uri is now the hosted relay's /cb — one stable constant
        // for the WHOLE FLEET (dial_link_config.h). The phone reaches it over
        // the internet (outbound-only), so linking no longer depends on the
        // phone being able to reach the dial INBOUND on the LAN — the failure
        // that stranded users on cellular / guest / client-isolated / weak
        // Wi-Fi. No more mDNS-hostname or DHCP-IP redirect construction.
        const char *redirect_uri = ORION_DIAL_RELAY_REDIRECT;

        // Backward-compat: an ALREADY-LINKED device keeps whatever redirect_uri
        // its client was registered with (even the old on-LAN .local/IP form).
        // DCR is per-redirect_uri, so presenting a different URI here would mint
        // a fresh client_id and strand the stored refresh token ("Client ID
        // mismatch"), forcing every updating user back through the QR flow. Only
        // an unlinked device (fresh, factory-reset, or already re-linking)
        // adopts the relay redirect; everyone else keeps refreshing untouched
        // and migrates to it the next time they re-link for their own reasons.
        // The migration hinges on dial_oauth_forget() clearing the cached
        // client_id+redirect (it does): after a Re-link or a dead-token
        // auto-relink, cached_redirect() returns false here, so the relay
        // constant above stands and a fresh client is registered against it.
        char cached_uri[160];
        if (dial_oauth_cached_redirect(cached_uri, sizeof(cached_uri)))
            redirect_uri = cached_uri;

        if (!dial_oauth_discover(&disc) ||
            !dial_oauth_ensure_client(&disc, redirect_uri, client_id, sizeof(client_id))) {
            // Keep the reassuring "Linking to Orion..." (PH_OAUTH_DISCOVER, set at
            // the top of the loop) and retry quickly for the first few misses —
            // the warmup window after a fresh association — only then falling into
            // the slow, user-visible "Orion unreachable" backoff. See PREP_FAST_RETRIES.
            if (++prep_fast_retries <= PREP_FAST_RETRIES) {
                vTaskDelay(pdMS_TO_TICKS(PREP_RETRY_MS));
                continue;
            }
            // A stale trust anchor fails right here (discovery is the first
            // HTTPS call after Wi-Fi) — a device rebooting years from now must
            // say so honestly instead of "Orion unreachable" (see DIAL_CERT_ERR_MSG).
            dial_state_set_phase(PH_DEGRADED,
                dial_oauth_last_err_cert() ? DIAL_CERT_ERR_MSG : "Orion unreachable");
            backoff_wait(backoff_s);
            backoff_s = (backoff_s * 2 > BACKOFF_MAX_S) ? BACKOFF_MAX_S : backoff_s * 2;
            continue;
        }
        prep_fast_retries = 0;   // prep steps went through; re-arm fast retry for any later pass

        if (!dial_oauth_have_valid_access() && !dial_oauth_refresh(&disc, client_id)) {
            // Interactive consent: QR on screen; on timeout, a fresh QR — no dead end.
            char url[600];
            if (!dial_oauth_start_authorize(&disc, client_id, redirect_uri, url, sizeof(url))) {
                dial_state_set_phase(PH_DEGRADED,
                    dial_oauth_last_err_cert() ? DIAL_CERT_ERR_MSG : dial_oauth_last_error());
                backoff_wait(backoff_s);
                continue;
            }
            dial_state_commit(mut_oauth_url, url);
            dial_state_set_phase(PH_OAUTH_WAIT_CONSENT, NULL);
            // Wait out the consent window, servicing reboots every slice so a
            // Change network / Re-link / Factory reset tapped from the link
            // screen reboots now rather than in up to five minutes' time
            // (wait_servicing_reboots). The exchange itself still runs in one go,
            // once the code is in. Between slices the dial polls the relay
            // mailbox (outbound HTTPS GET) for the code the phone's redirect
            // deposited there — this replaced the old inbound LAN callback —
            // throttled to RELAY_POLL_INTERVAL_MS. The window is measured off the
            // monotonic clock, not a slice counter, so a blocking poll can't
            // stretch the nominal CONSENT_WINDOW_MS to hours on a slow relay.
            int64_t consent_start = esp_timer_get_time();
            int64_t next_poll_us = 0;   // poll immediately on the first pass
            while (!dial_oauth_have_code() &&
                   (esp_timer_get_time() - consent_start) < (int64_t)CONSENT_WINDOW_MS * 1000) {
                if (esp_timer_get_time() >= next_poll_us) {
                    dial_oauth_poll_relay_once(&disc);
                    next_poll_us = esp_timer_get_time() + (int64_t)RELAY_POLL_INTERVAL_MS * 1000;
                    if (dial_oauth_have_code()) break;
                }
                wait_servicing_reboots(CONSENT_SLICE_MS);
            }
            bool got = dial_oauth_have_code();
            // End the authorize session before the exchange. There is no
            // callback server to reclaim sockets from any more (the relay poll
            // replaced it — killing the old port-80 httpd/socket-exhaustion
            // failure outright); the call just retires the spent mailbox key.
            // The captured code and PKCE verifier are already in hand for the
            // finish below.
            dial_oauth_stop_authorize();
            bool ok = got && dial_oauth_finish_authorize(&disc, client_id, redirect_uri, 0);
            if (!ok) {
                if (!got) {
                    // A relay TLS/cert failure (e.g. a mis-set relay CA or an
                    // expired edge cert) would otherwise loop here forever behind
                    // a silent QR — the exact infinite-loader this whole change
                    // removes. Surface it honestly, the way the discovery leg
                    // does. Otherwise nobody scanned in time, and a fresh code is
                    // exactly the right answer — the QR screen already says so.
                    if (dial_oauth_last_err_cert()) {
                        ESP_LOGE(TAG, "relay poll TLS/cert failure — surfacing");
                        dial_state_set_phase(PH_DEGRADED, DIAL_CERT_ERR_MSG);
                        backoff_wait(backoff_s);
                        backoff_s = (backoff_s * 2 > BACKOFF_MAX_S) ? BACKOFF_MAX_S : backoff_s * 2;
                    } else {
                        ESP_LOGW(TAG, "consent window elapsed — new QR");
                    }
                    continue;
                }
                // The phone consented, the code came back, and the exchange
                // still failed. Painting a fresh QR at this point is the one
                // thing that tells the owner nothing: they did their part, saw
                // "Linking to Orion...", and got the code again with no reason
                // given (owner-reported). Say what the token endpoint actually
                // said and hold it long enough to read before re-arming.
                const char *why = dial_oauth_last_error();
                ESP_LOGE(TAG, "token exchange failed after consent: %s", why ? why : "(none)");
                dial_state_set_phase(PH_DEGRADED, (why && *why) ? why
                    : "Couldn't finish linking. The code will refresh — try again.");
                backoff_wait(backoff_s);
                backoff_s = (backoff_s * 2 > BACKOFF_MAX_S) ? BACKOFF_MAX_S : backoff_s * 2;
                continue;
            }
        }

        dial_state_set_phase(PH_MCP_CONNECTING, NULL);
        dial_oauth_release_connection();   // one connection to the host at a time
        bool linked = dial_mcp_connect(NULL) && orion_discover_device();
        // A bare, well-formed empty device list is an account problem, not a
        // token problem — skip the refresh/re-link dance below and go
        // straight to reporting it (still with the same retry/backoff, so a
        // device added later is picked up on a subsequent pass).
        if (!linked && !s_no_orion_devices) {
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
            } else if (dial_oauth_last_token_err_permanent()) {
                // The refresh token is dead too (RFC 6749 §5.2 invalid_grant).
                // Drop the stale access token so the next pass falls through to
                // interactive consent (the QR) rather than spinning on
                // credentials that can never work again.
                ESP_LOGW(TAG, "refresh permanently rejected — re-linking");
                dial_oauth_forget_access();
            } else {
                // Transient (network/5xx/etc): leave tokens alone and just fall
                // into the same backoff/retry as any other connect failure below.
                ESP_LOGW(TAG, "refresh failed (transient) — will retry");
            }
        }
        if (!linked) {
            dial_state_set_phase(PH_DEGRADED, s_no_orion_devices
                ? "No Orion device on this account. Add your topper in the Orion app, then retry."
                : (dial_mcp_last_err_cert() ? DIAL_CERT_ERR_MSG : dial_mcp_last_error()));
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
    // Schedules are wanted as soon as the worker is up, NOT SCHED_INTERVAL_US
    // after it. Seeding the timer with "now" (as this did) meant the first
    // fetch landed ~30 min after every boot, and until it does sched_valid is
    // false for both zones — so temp_write_phase() returns SLEEP_PHASE_NONE and
    // a knob turn in Follow-schedule mode silently falls back to a plain
    // set_zone hold instead of retargeting the phase that is actually running.
    // The schedule engine then overwrites that hold at the next boundary, which
    // is the reported "the dial goes back to the schedule temperature after a
    // reflash". The night window and the pill's hold/until grammar read the
    // same flag, so both also mis-report during that window.
    //
    // A deadline of 0 fetches on the first idle poll instead; a miss re-arms at
    // SCHED_RETRY_US because orion_refresh_schedules needs a synced clock to
    // know which weekday "tonight" is, and SNTP often hasn't landed by then.
    int64_t sched_due_us      = 0;
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
            // unlikely — each one is either a tap on its own settings row or
            // a trip through its own screen (SCR_BOOST, SCR_SETTINGS) — but
            // if one lands mid-drain, stop coalescing and handle it right
            // after rather than silently mis-treating it as a toggle.
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
                    // "Dial adjusts" (M8): orion_set_temp decides Follow-
                    // schedule-override vs. plain hold per zone/time; see its
                    // own comment. mut_zone_temp applies either way (the
                    // device now holds this target regardless of which write
                    // path got it there), so the UI reflects the new setpoint
                    // immediately exactly like it did pre-M8. On a successful
                    // override, also refresh schedules right away so the
                    // overridden phase's own temp field is correct on the
                    // very next read, not just after the next ~30min
                    // periodic refresh.
                    set_temp_args_t sa = { (zone_idx_t)z, up.temp_c, false };
                    if (with_auth_retry(orion_set_temp, &sa, &disc, client_id)) {
                        dial_state_commit(mut_zone_temp, &up);
                        if (sa.used_override)
                            with_auth_retry(sched_call, NULL, &disc, client_id);
                    }
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

        // Update-prompt wake edge (docs/SPEC-update-prompt.md rework):
        // sampled every idle tick regardless of clock validity, so a
        // STANDBY/DIMMED -> ACTIVE transition is never missed while waiting
        // on the entry gates below (which do need a valid clock -- see the
        // "Update prompt: entry" block inside the dial_time_now() branch).
        // A single dial_power_level() read can't tell "just woke" from
        // "has been ACTIVE for an hour"; comparing against the previous
        // tick's level is what makes it an edge instead of a level.
        dial_power_level_t ota_pwr_level = dial_power_level();
        bool ota_prompt_woke = (s_ota_prev_pwr_level == DPWR_STANDBY ||
                                 s_ota_prev_pwr_level == DPWR_DIMMED) &&
                                ota_pwr_level == DPWR_ACTIVE;
        s_ota_prev_pwr_level = ota_pwr_level;

        // Night mode: warm-dim + quiet haptics while the household sleeps.
        // Real window (M5): bedtime-30min -> wake+30min from ZONE_A's
        // schedule (the dial's own side: override_sleep_schedule_tonight has
        // no user_id in its confirmed schema, so it implicitly targets the
        // token owner's account and only that side's schedule can be trusted
        // to describe this dial); falls back to a fixed
        // 21:00-07:00 window until that schedule is known.
        struct tm lt;
        if (dial_time_now(&lt)) {
            int now_min = lt.tm_hour * 60 + lt.tm_min;
            app_state_t st;
            dial_state_get(&st);
            const zone_state_t *za = &st.zones[ZONE_A];
            // Hoisted out of the night-window `if` below (was local to it)
            // so the update-prompt/auto-update blocks further down can reuse
            // the same wakeup time instead of re-deriving it — see the spec's
            // explicit instruction to reuse this exact night-flag machinery.
            int bed_min, wake_min;
            bool have_sched = za->sched_valid &&
                dial_parse_hhmm(za->sched_bedtime, &bed_min) &&
                dial_parse_hhmm(za->sched_wakeup, &wake_min);

            bool night;
            if (have_sched) {
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

            // ---- Status pill hold/until (§3, scr_dial.c) ---------------------
            // Recomputed every idle tick, same cadence as the night-window calc
            // above and off the same (st, now_min) this tick already has — a
            // phase boundary crossing, a fresh schedule fetch (mut_schedules,
            // ~30min cadence), or an Adjustment-mode flip (scr_adjust_mode.c)
            // all show up within one tick this way, with no separate dirty flag
            // to wire up. Committed per zone, only on an actual change, same
            // edge-triggered shape as s_ui_night just above.
            for (int z = 0; z < ZONE_COUNT; z++) {
                int16_t hu = compute_hold_until_min(&st, (zone_idx_t)z, now_min);
                if (hu != s_ui_hold_until[z]) {
                    s_ui_hold_until[z] = hu;
                    hold_until_t up = { (zone_idx_t)z, hu };
                    dial_state_commit(mut_hold_until, &up);
                }
            }

            // ---- Update prompt: entry (docs/SPEC-update-prompt.md rework) --
            // Raised ONLY on the wake edge sampled above (STANDBY/DIMMED ->
            // ACTIVE), never on the initial boot transition (ota_prompt_woke
            // can't be true before an observed prior standby -- see
            // s_ota_prev_pwr_level's seed). The shipped v1.0-1.2 design
            // re-evaluated an idle-window gate (DPWR_ACTIVE + idle_us in
            // [10s,30s), the only slice between "settled" and "display about
            // to dim") continuously, every ~300ms tick: it could only ever
            // raise the prompt into an empty room (the user had already
            // walked away by the time idle_us cleared 10s), it consumed the
            // once-per-24h ota_shown stamp on that same empty-room raise, and
            // re-checking idle_us on every tick meant a touch (which resets
            // it) withdrew the sheet mid-reach. A wake edge is the one moment
            // a human is provably in front of the dial, so it needs no idle
            // window at all -- just the rest of the spec's gates, checked
            // once, right then.
            if (ota_prompt_woke) {
                time_t now_epoch = time(NULL);
                bool want_prompt =
                    st.ota.status == OTA_AVAILABLE &&
                    strcmp(st.ota_skip, st.ota.latest) != 0 &&
                    (uint32_t)now_epoch >= st.ota_defer &&
                    (st.ota_shown == 0 || (uint32_t)now_epoch - st.ota_shown >= 24 * 3600) &&
                    !night &&
                    st.phase == PH_READY && st.have_state &&
                    clock_valid &&
                    st.ota_auto == 0;   // Auto-update Off — if it's on, the dial handles it, don't ask
                if (want_prompt && !s_ota_prompt_live) {
                    s_ota_prompt_live = true;
                    dial_state_commit(mut_ota_prompt_due, &s_ota_prompt_live);
                    // Once-per-24h ceiling's own clock, independent of
                    // "Later"'s separate ota_defer — stamped the instant the
                    // sheet is actually raised, which is now honest: it only
                    // fires when a human just woke the device and is looking
                    // at it, not into an empty room.
                    dial_state_set_ota_shown((uint32_t)now_epoch);
                }
            }

            // ---- Update prompt: exit (deliberately separate from the entry
            // gates above) — once raised, the sheet is sticky (nav_policy
            // keeps routing to it, scoped to SCR_DIAL/SCR_UPDATE_PROMPT)
            // until the user acts (scr_update_prompt.c clears the flag
            // itself on every exit path: Update now / Later / Update
            // options / swipe-dismiss) or one of these three fires:
            //   - the display made it all the way back to STANDBY (the user
            //     walked away without acting)
            //   - night began
            //   - the update stopped being available (installed some other
            //     way, or superseded)
            // NOT a touch, and NOT a re-run of the entry gates: re-running
            // them was exactly what let a touch (idle_us resetting below the
            // old gate's floor) withdraw the sheet mid-reach before this
            // rework. These three are checked on their own so the sheet
            // survives being touched — that's the one thing "wake and reach
            // for the dial" is guaranteed to involve.
            if (s_ota_prompt_live &&
                (ota_pwr_level == DPWR_STANDBY || night || st.ota.status != OTA_AVAILABLE)) {
                s_ota_prompt_live = false;
                dial_state_commit(mut_ota_prompt_due, &s_ota_prompt_live);
            }

            // ---- Auto-update overnight install (docs/SPEC-update-prompt.md) -
            // Reuses dial_ota_download_and_apply() directly — the exact same
            // install path SCR_UPDATE's confirmed manual tap uses (CMD_OTA_APPLY
            // in handle_immediate_cmd below), including the takeover screen if
            // someone walks up mid-install (nav_policy's OTA check already
            // forces SCR_UPDATING off ota.status alone, regardless of who
            // started the download) and the v1.0.10 confirm-on-stable-boot
            // behavior (ota_confirm_once, unchanged by this).
            if (st.ota_auto == 1 && st.ota.status == OTA_AVAILABLE) {
                int auto_start, auto_end;
                if (have_sched) {
                    auto_start = (wake_min + 60) % 1440;
                    auto_end   = (wake_min + 180) % 1440;
                } else {
                    auto_start = 9 * 60;    // 09:00 fallback (spec)
                    auto_end   = 11 * 60;   // 11:00 fallback (spec)
                }
                bool in_window = (auto_start <= auto_end)
                    ? (now_min >= auto_start && now_min < auto_end)
                    : (now_min >= auto_start || now_min < auto_end);

                // NOT gated on the zones being off. The dial is a remote
                // control, not the bed's controller — heating/cooling is
                // driven by Orion's own hardware and the cloud, and the
                // dial's ~30s reboot to apply an install has zero effect on
                // the bed's operation. A zones-off requirement protects
                // against nothing and permanently starves auto-update for
                // anyone who runs their topper through the day or whose
                // schedule keeps it on — silently, forever. Do not
                // reintroduce it on the assumption it was protecting
                // something.
                //
                // What DOES need protecting is thermal relief ("boost"): a
                // timed session with a live countdown on screen, genuinely
                // disruptive to interrupt with the install takeover screen —
                // mirrors relief_any's own guard on the daily OTA
                // auto-CHECK below. Relief is temporary by construction, so
                // unlike zones_off this can never permanently disqualify
                // anyone.
                bool relief_any = st.zones[ZONE_A].relief_active ||
                                   st.zones[ZONE_B].relief_active;

                int64_t auto_idle_us = esp_timer_get_time() - dial_state_last_input_us();
                bool eligible = in_window && !night && st.phase == PH_READY &&
                                !relief_any && auto_idle_us >= 30LL * 60 * 1000000;

                if (!in_window) {
                    s_ota_auto_attempted = false;   // window closed; re-arm for tomorrow's occurrence
                } else if (eligible && !s_ota_auto_attempted) {
                    s_ota_auto_attempted = true;    // at most one attempt per window, success or fail
                    bool version_blocked = s_ota_auto_fail_ver[0] &&
                        s_ota_auto_fail_count >= 2 &&
                        strcmp(s_ota_auto_fail_ver, st.ota.latest) == 0;
                    if (version_blocked) {
                        ESP_LOGI(TAG, "auto-update: v%s blocked after 2 failed attempts -- skipping",
                                 st.ota.latest);
                    } else {
                        ESP_LOGI(TAG, "auto-update: attempting v%s in the overnight window",
                                 st.ota.latest);
                        s_ota_last_committed_pct = -100;   // guarantee the first progress commit fires
                        // Both clients hand their sockets back, same as the
                        // manual CMD_OTA_APPLY path below.
                        dial_mcp_release_connection();
                        dial_oauth_release_connection();
                        bool unattended = true;
                        dial_state_commit(mut_ota_unattended, &unattended);
                        bool ok = dial_ota_download_and_apply(ota_progress_cb);
                        commit_ota_snapshot();
                        if (ok) {
                            ESP_LOGI(TAG, "auto-update: image ready; rebooting into it");
                            esp_restart();
                        } else {
                            dial_ota_info_t info;
                            dial_ota_get(&info);
                            if (strcmp(s_ota_auto_fail_ver, info.latest) != 0) {
                                strlcpy(s_ota_auto_fail_ver, info.latest, sizeof(s_ota_auto_fail_ver));
                                s_ota_auto_fail_count = 0;
                            }
                            s_ota_auto_fail_count++;
                            ESP_LOGW(TAG, "auto-update: attempt %d failed for v%s",
                                     s_ota_auto_fail_count, s_ota_auto_fail_ver);
                        }
                    }
                }
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
            dial_state_set_phase(PH_DEGRADED,
                dial_mcp_last_err_cert() ? DIAL_CERT_ERR_MSG : dial_mcp_last_error());
        }
        last_poll_us = esp_timer_get_time();

        // Sleep schedules change far less often than device state — piggyback
        // on this same quiet-idle gate, just at a much longer interval. The
        // deadline only jumps a full interval once a fetch actually succeeds
        // (see sched_due_us above).
        if (esp_timer_get_time() >= sched_due_us) {
            bool got = with_auth_retry(sched_call, NULL, &disc, client_id);   // commits inside on success
            sched_due_us = esp_timer_get_time() + (got ? SCHED_INTERVAL_US : SCHED_RETRY_US);
        }

        // OTA_FAILED must not be terminal (field bug: it used to stay wedged
        // showing "Update failed" until a manual power cycle — see
        // dial_ota_clear_stale_failure()'s comment). Re-checked at this same
        // idle cadence; a no-op the vast majority of ticks (status isn't
        // FAILED, or it's not stale yet) so it's cheap to leave ungated.
        // This is the belt to CMD_OTA_CLEAR_FAILED's suspenders: it's what
        // un-wedges the row even if the user never leaves Settings at all.
        //
        // EXCEPT the two-failed-auto-installs case (docs/SPEC-update-prompt.md):
        // an unattended nightly retry that just hit its second strike must
        // leave a trace someone can actually find hours later on SCR_UPDATE,
        // not have this 25s timer erase it before anyone's looked. That
        // screen's own CMD_OTA_CLEAR_FAILED (posted on teardown) still clears
        // it the moment the user actually visits — this only skips the
        // silent, nobody-watching auto-clear.
        {
            dial_ota_info_t info;
            dial_ota_get(&info);
            bool blocked_and_failed = info.status == OTA_FAILED &&
                s_ota_auto_fail_ver[0] && s_ota_auto_fail_count >= 2 &&
                strcmp(s_ota_auto_fail_ver, info.latest) == 0;
            if (!blocked_and_failed && dial_ota_clear_stale_failure(OTA_FAILED_AUTOCLEAR_US))
                commit_ota_snapshot();
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
            // Per-device offset into the 10:00-16:00 window. Without it the
            // whole fleet checks at 10:00 sharp: the 24h timer expires
            // overnight for everybody, so the first tick past the window's
            // start releases every device at once. Installing a release makes
            // that worse rather than better -- an OTA reboots every device
            // that took it, aligning their timers from then on.
            //
            // Derived from the Wi-Fi MAC, so it is STABLE per device (a
            // random draw would re-roll each boot and could keep landing on
            // the same minute) and needs no storage. 0-299 minutes leaves at
            // least an hour of window after the latest offset.
            static int s_ota_check_offset_min = -1;
            if (s_ota_check_offset_min < 0) {
                uint8_t mac[6] = { 0 };
                esp_read_mac(mac, ESP_MAC_WIFI_STA);
                s_ota_check_offset_min = ((mac[4] << 8) | mac[5]) % 300;
                ESP_LOGI(TAG, "OTA auto-check window offset: +%dmin past 10:00",
                         s_ota_check_offset_min);
            }
            // Fill ota_lt BEFORE reading it: the old single-expression form
            // called dial_time_now() mid-condition, so anything computed from
            // ota_lt above it would have read stale/uninitialised fields.
            bool have_local = ota_st.clock_valid && dial_time_now(&ota_lt);
            // Offset is now minutes past the top of the hour rather than
            // past 10:00 — with a 6h interval the check can land in any
            // waking hour, and the jitter's job is unchanged: stop a fleet
            // that all rebooted together (an OTA does exactly that) from
            // hitting GitHub in the same minute forever after.
            int window_open_min = 10 * 60 + s_ota_check_offset_min;
            int now_min_local = have_local ? ota_lt.tm_hour * 60 + ota_lt.tm_min : -1;
            // "Not asleep" is the only real requirement: the daytime band
            // existed to keep checks away from the sleep window, and `night`
            // already answers that question directly (bedtime-30 to wake+30
            // from the actual schedule, not a fixed guess). Keeping the
            // per-device jitter as a minutes-into-the-hour stagger.
            (void)window_open_min;
            // No night gate at all (owner, 2026-08-04): the CHECK is silent
            // — one HTTPS request, no screen, no sound — so there is nothing
            // to protect a sleeping household from. What must never appear at
            // night is the PROMPT, and that has its own !night entry gate, as
            // does the ambient "Update available" line. Checking around the
            // clock just means a release published in the evening is known by
            // morning instead of waiting for the next daylight window.
            bool in_window = have_local &&
                              (now_min_local % 60) >= (s_ota_check_offset_min % 60);
            bool relief_any = ota_st.zones[ZONE_A].relief_active ||
                               ota_st.zones[ZONE_B].relief_active;
            if (in_window && !relief_any && ota_st.phase == PH_READY) {
                dial_mcp_release_connection();   // one TLS session at a time
                dial_ota_check(ota_st.beta);
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
    case DIAL_NET_EV_CONNECTING:
        // Credentials accepted (from the web form OR the dial's own keypad) and
        // a join is being attempted — move to the connecting phase so the
        // connecting screen shows through nav_policy rather than through screen
        // stickiness. Ignored once we're actually online, so a mid-session
        // reconnect blip doesn't flash "Connecting to Wi-Fi" over the dial.
        dial_state_get(&st);
        if (st.phase == PH_WIFI_PORTAL || st.phase == PH_BOOT)
            dial_state_set_phase(PH_WIFI_CONNECTING, NULL);
        break;
    case DIAL_NET_EV_SETUP_FAILED:
        // The credentials just submitted were rejected. nav_policy uses this to
        // put the user back on the password screen for the same network,
        // instead of at the start of setup with no idea what happened.
        dial_state_set_wifi_join_failed();
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

    // Brings up ADC1 ch0 (GPIO1 / BATT_ADC) and starts the sampler task that
    // feeds app_state_t.batt_*. Started after dial_state_init so the first
    // sample has somewhere to land. See dial_battery.h for the divider and
    // the measured plugged-vs-unplugged rail behaviour this relies on.
    dial_battery_start();

    // Capture the boot's pending-verify state (rollback armed?) and mirror
    // it into app_state_t BEFORE the first screen renders below -- the
    // user needs the "don't unplug" warning from frame one, not 30s+ from
    // now when ota_confirm_once() would otherwise first touch this state.
    dial_ota_init();
    commit_ota_snapshot();

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
    // dial_power_start() ran before prefs existed, so its first fade used the
    // 100% RAM default; without this nudge a saved dimmer preference wouldn't
    // apply until the next level change (~30s of full brightness after boot).
    dial_power_brightness_changed();

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

    // 30s stable-boot fallback confirm (see ota_confirm_once()'s comment for
    // the field incident this fixes): a crash-looping bad image never
    // survives anywhere near 30s, so this can't paper over a genuinely
    // broken update -- it only stops a cloud/network outage from holding a
    // GOOD image hostage to rollback. One-shot, never re-armed; left
    // allocated for the rest of the device's uptime rather than deleted
    // after firing, same as this codebase's other long-lived esp_timers
    // (e.g. dial_wifi.c's retry timer).
    const esp_timer_create_args_t ota_confirm_timer_args = {
        .callback = ota_confirm_timer_cb,
        .name = "ota_confirm",
    };
    esp_timer_handle_t ota_confirm_timer;
    ESP_ERROR_CHECK(esp_timer_create(&ota_confirm_timer_args, &ota_confirm_timer));
    ESP_ERROR_CHECK(esp_timer_start_once(ota_confirm_timer, 30ULL * 1000000ULL));

    // OAuth/TLS/MCP need a big stack; everything network runs on this task.
    // Priority 3 and pinned to core 0 — BELOW the LVGL task (5, core 1), which
    // the user is actually looking at. It used to be 4, outranking the UI, so a
    // TLS handshake froze the screen and swallowed taps. Core 0 is where Wi-Fi
    // and lwIP already live, so the network work stays on the network core and
    // leaves the UI a core of its own.
    xTaskCreatePinnedToCore(worker_task, "worker", 16384, NULL, 3, NULL, 0);
}
