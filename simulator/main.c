/*
 * main.c — the simulator harness. Brings up LVGL against a 360x360 host
 * framebuffer, registers the real firmware's screens (ui_screens_register_all,
 * the actual firmware/dial-idf/components/dial_ui sources), then for each
 * named scenario: builds a demo app_state_t, navigates the real router to
 * the real screen, pumps simulated time so creation/animation settle, and
 * writes a circularly-masked PNG to docs/screens/.
 *
 * No touchscreen or knob hardware exists here, but the INPUTS still go
 * through the real code paths: ui_router_knob_input() is the same entry
 * point the knob decoder's esp_timer task calls on real hardware, and the
 * one screen that needs a simulated tap (scr_passkey, to show a few
 * characters already typed) gets it through a real LV_INDEV_TYPE_POINTER
 * indev — the same press/release/CLICKED pipeline a finger on the panel
 * would drive. Nothing here reaches into a screen's file-static state.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <time.h>   // scenario_dial_off_bedtime needs the real wall clock

#include "lvgl.h"
#include "ui_router.h"
#include "ui_screens.h"
#include "dial_state.h"
#include "dial_palette.h"
#include "dial_ota.h"
#include "dial_battery.h"   // DIAL_BATTERY_PCT_UNKNOWN for the charging scenario
#include "sim_state.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define SCREEN_W 360
#define SCREEN_H 360

#ifndef DIAL_SIM_OUTPUT_DIR
#define DIAL_SIM_OUTPUT_DIR "docs/screens"
#endif

// The "available" version every OTA scenario advertises. One constant rather
// than a literal per scenario, because it has to stay AHEAD of the version the
// simulator reports as installed (stubs.c's esp_app_desc_t, which tracks
// PROJECT_VER) — otherwise the screenshots show a dial offering to update
// itself to something it already runs. Bump it with each release.
#define SIM_OTA_LATEST "1.5.1"

/* ---- host framebuffer + LVGL display driver ----------------------------- */

static uint16_t s_host_fb[SCREEN_W * SCREEN_H];
static lv_disp_draw_buf_t s_draw_buf;
static lv_color_t s_lv_buf[SCREEN_W * SCREEN_H];   /* full-frame, single buffer */

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    for (lv_coord_t y = area->y1; y <= area->y2; y++) {
        for (lv_coord_t x = area->x1; x <= area->x2; x++) {
            if (x >= 0 && x < SCREEN_W && y >= 0 && y < SCREEN_H)
                s_host_fb[y * SCREEN_W + x] = color_p->full;
            color_p++;
        }
    }
    lv_disp_flush_ready(drv);
}

/* ---- simulated pointer indev (for scr_passkey's pre-fill tap) ----------- */

static bool      s_ptr_pressed;
static lv_coord_t s_ptr_x, s_ptr_y;

static void indev_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
    data->point.x = s_ptr_x;
    data->point.y = s_ptr_y;
    data->state = s_ptr_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
}

/* ---- time pump / input helpers ------------------------------------------ */

static void pump_ms(int ms)
{
    for (int t = 0; t < ms; t += 5) {
        lv_tick_inc(5);
        lv_timer_handler();
    }
}

// Pumps simulated time until LVGL reports no animations left running (a
// sheet slide, scroll momentum, an elastic-overscroll bounce-back — anything
// driven by lv_anim), or max_ms elapses as a backstop. A fixed pump_ms(N)
// is a guess against however long those animations turn out to take; this
// instead gates on LVGL's own bookkeeping so a capture never lands mid-anim.
static void pump_until_idle(int max_ms)
{
    int waited = 0;
    while (lv_anim_count_running() > 0 && waited < max_ms) {
        lv_tick_inc(5);
        lv_timer_handler();
        waited += 5;
    }
}

// Turns the knob `detents` and lets one dispatcher tick (50ms) drain it into
// the active screen's on_knob — the same accumulate-then-drain path the real
// decoder's esp_timer task and ui_router.c's dispatch_tick implement.
static void sim_knob(int detents)
{
    ui_router_knob_input(detents);
    pump_ms(100);
}

// A real press-then-release at an absolute screen coordinate, through the
// pointer indev registered below — drives LV_EVENT_PRESSED/RELEASED/CLICKED
// on whatever widget actually sits there, exactly as a finger would.
static void sim_tap(lv_coord_t x, lv_coord_t y)
{
    s_ptr_x = x; s_ptr_y = y;
    s_ptr_pressed = true;
    pump_ms(60);
    s_ptr_pressed = false;
    pump_ms(60);
}

/* ---- PNG output ----------------------------------------------------------*/

static void ensure_dir(const char *path)
{
    mkdir(path, 0755);   // ignores EEXIST-equivalent; good enough for our own tree
}

// Converts the host RGB565 framebuffer to RGBA8888 and stamps a 180px-radius
// anti-aliased circular alpha mask over it (the round panel), then writes it
// as docs/screens/<name>.png.
static int s_snapshots_written;   // counted for the "done:" summary — a
                                  // hardcoded total drifted the first time a
                                  // scenario was added

static void snapshot(const char *name)
{
    static uint8_t rgba[SCREEN_W * SCREEN_H * 4];
    const float cx = SCREEN_W / 2.0f, cy = SCREEN_H / 2.0f, r = 180.0f;

    for (int y = 0; y < SCREEN_H; y++) {
        for (int x = 0; x < SCREEN_W; x++) {
            uint16_t px = s_host_fb[y * SCREEN_W + x];
            uint8_t r5 = (px >> 11) & 0x1F;
            uint8_t g6 = (px >> 5) & 0x3F;
            uint8_t b5 = px & 0x1F;
            uint8_t rr = (uint8_t)((r5 * 255 + 15) / 31);
            uint8_t gg = (uint8_t)((g6 * 255 + 31) / 63);
            uint8_t bb = (uint8_t)((b5 * 255 + 15) / 31);

            float dx = (x + 0.5f) - cx, dy = (y + 0.5f) - cy;
            float dist = sqrtf(dx * dx + dy * dy);
            float alpha = (r - dist) / 1.5f;      // ~1.5px anti-aliased edge
            if (alpha > 1.0f) alpha = 1.0f;
            if (alpha < 0.0f) alpha = 0.0f;

            int idx = (y * SCREEN_W + x) * 4;
            rgba[idx + 0] = rr;
            rgba[idx + 1] = gg;
            rgba[idx + 2] = bb;
            rgba[idx + 3] = (uint8_t)(alpha * 255.0f + 0.5f);
        }
    }

    char path[512];
    snprintf(path, sizeof(path), "%s/%s.png", DIAL_SIM_OUTPUT_DIR, name);
    if (!stbi_write_png(path, SCREEN_W, SCREEN_H, 4, rgba, SCREEN_W * 4)) {
        fprintf(stderr, "FAILED to write %s\n", path);
        return;
    }

    // Cheap sanity check: sample a coarse grid over the whole circle and
    // count distinct raw pixel values seen, to flag a render that came out
    // suspiciously uniform (almost certainly a blank/broken screen).
    uint16_t seen[64];
    int n_seen = 0;
    for (int gy = 20; gy < SCREEN_H - 20; gy += 40) {
        for (int gx = 20; gx < SCREEN_W - 20; gx += 40) {
            uint16_t v = s_host_fb[gy * SCREEN_W + gx];
            bool known = false;
            for (int i = 0; i < n_seen; i++) if (seen[i] == v) { known = true; break; }
            if (!known && n_seen < (int)(sizeof(seen) / sizeof(seen[0]))) seen[n_seen++] = v;
        }
    }
    printf("wrote %-16s %s (%d distinct colors sampled)\n", name, path, n_seen);
    s_snapshots_written++;
}

/* ---- scenario baseline --------------------------------------------------- */

// Shared, realistic dual-zone state every scenario starts from; individual
// scenarios below only touch the fields their screen actually cares about.
static void apply_baseline(void)
{
    app_state_t *st = sim_state_ptr();

    st->phase = PH_READY;
    st->have_state = true;
    st->device_online = true;
    st->clock_valid = true;
    snprintf(st->serial, sizeof(st->serial), "ORION-7F3A1");

    st->zone_present[ZONE_A] = true;
    st->zone_present[ZONE_B] = true;
    st->units_c = false;
    st->rel_mode = false;   // absolute by default; relative scenarios opt in
    st->rotation = 0;
    st->haptics_level = 1;   // HAPTIC_LEVEL_AUTO
    st->welcomed = true;
    st->side_picked = true;
    st->ui_zone = ZONE_A;
    st->away = false;

    zone_state_t *a = &st->zones[ZONE_A];
    snprintf(a->user_name, sizeof(a->user_name), "Alex");
    a->on = true;
    snprintf(a->thermal_state, sizeof(a->thermal_state), "holding");
    a->temp_c = 21.1f;    // -> 70F
    a->actual_c = 21.1f;  // at target

    zone_state_t *b = &st->zones[ZONE_B];
    snprintf(b->user_name, sizeof(b->user_name), "Sam");
    b->on = true;
    snprintf(b->thermal_state, sizeof(b->thermal_state), "heating");
    b->temp_c = 22.2f;    // -> 72F target
    b->actual_c = 20.0f;  // -> 68F current, still warming

    st->ota.status = 0;   // OTA_IDLE
}

/* ---- scenarios ------------------------------------------------------------*/

static void scenario_welcome(void)
{
    apply_baseline();
    ui_router_go(SCR_WELCOME, NULL, LV_SCR_LOAD_ANIM_NONE);
    pump_ms(300);
    snapshot("welcome");
}

static void scenario_wifi_portal(void)
{
    apply_baseline();
    app_state_t *st = sim_state_ptr();
    st->phase = PH_WIFI_PORTAL;
    snprintf(st->ap_ssid, sizeof(st->ap_ssid), "OrionDial-A1B2");
    ui_router_go(SCR_WIFI_PORTAL, NULL, LV_SCR_LOAD_ANIM_NONE);
    pump_ms(300);
    snapshot("wifi-portal");
}

static void scenario_netpick(void)
{
    apply_baseline();
    ui_router_go(SCR_NETPICK, NULL, LV_SCR_LOAD_ANIM_NONE);
    pump_ms(600);
    snapshot("netpick");
}

// Types "Sky4" onto the wheel (knob turns + a tap on the "Add" disc per
// character, the same commit path a finger on the real disc drives) so the
// screenshot shows a password mid-entry instead of the blank first-open state.
static void scenario_passkey(void)
{
    apply_baseline();
    ui_router_go(SCR_PASSKEY, (void *)(uintptr_t)0 /* "Home" */, LV_SCR_LOAD_ANIM_NONE);
    pump_ms(300);

    static const int WHEEL_INDEX[] = { 44, 10, 24, 56 };  // 'S','k','y','4'
    int pos = 0;
    for (size_t i = 0; i < sizeof(WHEEL_INDEX) / sizeof(WHEEL_INDEX[0]); i++) {
        int target = WHEEL_INDEX[i];
        sim_knob(target - pos);
        pos = target;
        sim_tap(180, 240);   // the "Add" disc — commits the candidate glyph
    }
    pump_ms(200);
    snapshot("passkey");
}

static void scenario_oauth_qr(void)
{
    apply_baseline();
    app_state_t *st = sim_state_ptr();
    st->phase = PH_OAUTH_WAIT_CONSENT;
    snprintf(st->oauth_url, sizeof(st->oauth_url),
             "https://github.com/chris023/orion-waveshare-rotary-dial");
    // sta_ssid is still set (other setup screens use it), but scr_setup.c's QR
    // hint no longer names the network: relay linking is outbound-only, so the
    // phone can be on ANY network and the copy is network-agnostic now.
    snprintf(st->sta_ssid, sizeof(st->sta_ssid), "Kestrel-5G");
    ui_router_go(SCR_OAUTH_QR, NULL, LV_SCR_LOAD_ANIM_NONE);
    pump_ms(300);
    snapshot("oauth-qr");
}

// Same setup as scenario_oauth_qr, but pumps past the 45s-quiet threshold so
// scr_setup.c's dismissible "still waiting" explainer (Part 2) is on screen
// -- the case where a fresh code has been up a while and nobody has scanned it
// yet (a nudge to scan and approve, no longer a same-network warning).
static void scenario_oauth_waiting(void)
{
    apply_baseline();
    app_state_t *st = sim_state_ptr();
    st->phase = PH_OAUTH_WAIT_CONSENT;
    snprintf(st->oauth_url, sizeof(st->oauth_url),
             "https://github.com/chris023/orion-waveshare-rotary-dial");
    snprintf(st->sta_ssid, sizeof(st->sta_ssid), "Kestrel-5G");
    ui_router_go(SCR_OAUTH_QR, NULL, LV_SCR_LOAD_ANIM_NONE);
    pump_ms(300);
    pump_ms(45500);   // clears scr_setup.c's OAUTH_WAIT_FIRST_MS (45000ms)
    snapshot("oauth-waiting");
}

static void scenario_sidepick(void)
{
    apply_baseline();
    ui_router_go(SCR_SIDEPICK, NULL, LV_SCR_LOAD_ANIM_NONE);
    pump_ms(300);
    snapshot("sidepick");
}

static void scenario_connecting(void)
{
    apply_baseline();
    app_state_t *st = sim_state_ptr();
    st->phase = PH_WIFI_CONNECTING;
    snprintf(st->wifi_join_ssid, sizeof(st->wifi_join_ssid), "Home");
    ui_router_go(SCR_CONNECTING, NULL, LV_SCR_LOAD_ANIM_NONE);
    pump_ms(300);
    snapshot("connecting");
}

static void scenario_dial(void)
{
    apply_baseline();   // zone B (left) is already ON/heating, 68 -> 72, by baseline
    ui_router_go(SCR_DIAL, (void *)(uintptr_t)ZONE_B, LV_SCR_LOAD_ANIM_NONE);
    pump_ms(600);
    snapshot("dial");
}

// The Home face with the ambient "Update available" indicator (owner
// reassessment, docs/SPEC-update-prompt.md) — unconditional on
// ota.status==OTA_AVAILABLE and not-night, no idle window/daily ceiling
// unlike the SCR_UPDATE_PROMPT sheet (see scenario_update_prompt). Also
// proves it doesn't collide with the M6 "Finalizing update..."
// pending_verify caption that shares this same slot: apply_baseline()
// leaves pending_verify false, so this is the OTA_AVAILABLE side of that
// shared label. Not exercising the tap-to-SCR_UPDATE affordance here — the
// label's own presence/position is what needs a permanent screenshot; the
// simulator's sim_tap harness is reserved for scr_passkey's pre-fill need.
static void scenario_dial_update(void)
{
    apply_baseline();
    app_state_t *st = sim_state_ptr();
    st->ota.status = OTA_AVAILABLE;
    snprintf(st->ota.latest, sizeof(st->ota.latest), SIM_OTA_LATEST);
    st->generation++;
    ui_router_go(SCR_DIAL, (void *)(uintptr_t)ZONE_B, LV_SCR_LOAD_ANIM_NONE);
    pump_ms(600);
    snapshot("dial-update");
}

// The Home face in RELATIVE scale. Deliberately a POSITIVE, OFF-GRID setpoint:
// 30.0C -> 86F, which is level +2 (its anchor is 87F/30.5C) — so the render
// proves the spliced '+' glyph draws AND that an off-grid device value shows as
// the nearest level. Water below the setpoint keeps the heating overlay + pill
// on screen, and the neutral notch/"LEVEL" suffix are visible.
static void scenario_dial_relative(void)
{
    apply_baseline();
    app_state_t *st = sim_state_ptr();
    st->rel_mode = true;
    st->ui_zone = ZONE_A;
    zone_state_t *a = &st->zones[ZONE_A];
    a->on = true;
    snprintf(a->thermal_state, sizeof(a->thermal_state), "heating");
    a->temp_c = 30.0f;    // off-grid -> 86F -> level +2
    a->actual_c = 26.0f;  // -> 79F, below setpoint: still warming
    st->generation++;     // direct field-sets don't bump it; make on_state re-run
    ui_router_go(SCR_DIAL, (void *)(uintptr_t)ZONE_A, LV_SCR_LOAD_ANIM_NONE);
    pump_ms(600);
    snapshot("dial-relative");
}

// The status pill's "Until H:MM" state (§3 rework) — the ecobee-style
// counterpart to the default baseline's "Holding" (apply_baseline() leaves
// hold_until_min at sim_state_reset()'s -1 default, which every other dial
// scenario above renders as-is). Seeds a real sleep schedule on ZONE_A —
// bedtime 22:00, phase 1 starting an hour later (23:00), phase 2 two hours
// after THAT (01:00), wakeup 07:00 — so the state this screenshot bakes in
// is a coherent one: "currently in phase 1" hands off to phase 2 at 01:00,
// which is exactly the clock-minutes value poked into hold_until_min below.
// There is no worker task in this simulator to derive that value FROM the
// schedule fields (compute_hold_until_min lives in main.c, never linked
// here — see sim_state.c's own header comment), so both are set by hand:
// the schedule fields document why, hold_until_min is what actually renders.
static void scenario_dial_until(void)
{
    apply_baseline();
    app_state_t *st = sim_state_ptr();
    st->sched_follow = true;
    zone_state_t *a = &st->zones[ZONE_A];
    a->sched_valid = true;
    a->sched_smart_temp_active = true;
    snprintf(a->sched_bedtime, sizeof(a->sched_bedtime), "22:00");
    a->sched_bedtime_temp_c = 19.4f;
    snprintf(a->sched_wakeup, sizeof(a->sched_wakeup), "07:00");
    a->sched_wakeup_temp_c = 21.1f;
    a->sched_phase1_offset_min = 60;    // 22:00 + 60min -> phase 1 starts 23:00
    a->sched_phase1_temp_c = 18.3f;
    a->sched_phase2_offset_min = 180;   // 22:00 + 180min -> phase 2 starts 01:00
    a->sched_phase2_temp_c = 20.0f;
    a->hold_until_min = 60;             // 01:00 -> "Until 1:00"
    st->generation++;                   // direct field-sets don't bump it; make on_state re-run
    ui_router_go(SCR_DIAL, (void *)(uintptr_t)ZONE_A, LV_SCR_LOAD_ANIM_NONE);
    pump_ms(600);
    snapshot("dial-until");
}

// The status pill's newest state (owner refinement): a zone that's OFF but
// still has a bedtime ahead TODAY isn't "holding" — nothing is being held,
// the schedule is going to switch it on — so the pill shows the pause icon
// (not the loop icon scenario_dial_until's phase-boundary case uses) with
// "Until <bedtime>". Unlike scenario_dial_until, scr_dial.c computes this
// state itself from the REAL wall clock (time(NULL), gated on clock_valid —
// see that file's own comment for why it doesn't need a worker-computed
// field the way hold_until_min does), so this scenario seeds a bedtime
// relative to whenever the simulator actually runs (+4h from now, wrapped
// past midnight if needed) rather than a fixed clock string — a hardcoded
// "22:00" would already be in the past for a chunk of the day and silently
// fall through to a different pill state instead of exercising this one.
static void scenario_dial_off_bedtime(void)
{
    apply_baseline();
    app_state_t *st = sim_state_ptr();
    st->clock_valid = true;
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    int bed_min = ((lt.tm_hour * 60 + lt.tm_min) + 4 * 60) % 1440;   // "bedtime" +4h from now
    zone_state_t *a = &st->zones[ZONE_A];
    a->on = false;
    a->sched_valid = true;
    snprintf(a->sched_bedtime, sizeof(a->sched_bedtime), "%02d:%02d", bed_min / 60, bed_min % 60);
    a->sched_bedtime_temp_c = 19.4f;
    st->generation++;   // direct field-sets don't bump it; make on_state re-run
    ui_router_go(SCR_DIAL, (void *)(uintptr_t)ZONE_A, LV_SCR_LOAD_ANIM_NONE);
    pump_ms(600);
    snapshot("dial-off-bedtime");
}

// TEMP verification scenario (owner task: "off + night is the dimmest
// combination on the device ... say what it looks like"). Off zone under the
// night palette — reuses scenario_dial_off_bedtime's off-zone setup. Not
// part of the permanent doc set — removed again once inspected.

// Boost-heat duration picker, knob-adjusted off the 30min default to 45 so
// the render shows a deliberately chosen duration, not just the opening value.
static void scenario_boost(void)
{
    apply_baseline();
    uintptr_t packed = ((uintptr_t)ZONE_A << 1) | 1u;   // heat
    ui_router_go(SCR_BOOST, (void *)packed, LV_SCR_LOAD_ANIM_NONE);
    pump_ms(200);
    sim_knob(3);   // +5min * 3 = 30 -> 45
    pump_ms(200);
    snapshot("boost");
}

// Also documents the M7 permanent "Update" row (replaces the M6 conditional
// "Install X.Y.Z" row — confirmation moved into SCR_UPDATE itself, this row
// is now pure navigation): sets the OTA status to available with a pending
// version before navigating, then knob-walks focus down onto the row itself
// (Back/Settings/Update/Wi-Fi/About — 1 detent past the "Settings" the list
// opens on) so menu.png actually shows the version badge it's meant to
// document, not just proves it doesn't crash off-frame.
static void scenario_menu(void)
{
    apply_baseline();
    app_state_t *st = sim_state_ptr();
    st->ota.status = OTA_AVAILABLE;
    snprintf(st->ota.latest, sizeof(st->ota.latest), SIM_OTA_LATEST);
    st->generation++;
    ui_router_go(SCR_MENU, NULL, LV_SCR_LOAD_ANIM_NONE);
    pump_ms(300);
    sim_knob(1);            // Settings -> Update (badge: SIM_OTA_LATEST)
    pump_ms(300);
    pump_until_idle(800);  // rotor snap is an lv_anim; land before the capture
    snapshot("menu");
}

// The Update submenu (M7), opened with an update pending — shows "Check for
// updates"' AVAILABLE state (tap-to-confirm prompt not yet armed) and the
// Beta builds toggle in its default Off state beneath it.
static void scenario_update(void)
{
    apply_baseline();
    app_state_t *st = sim_state_ptr();
    st->ota.status = OTA_AVAILABLE;
    snprintf(st->ota.latest, sizeof(st->ota.latest), SIM_OTA_LATEST);
    st->generation++;
    ui_router_go(SCR_UPDATE, NULL, LV_SCR_LOAD_ANIM_NONE);
    pump_ms(300);

    // Knob-walked one row past the rotor's opening focus, onto "Check for
    // updates" (Back(0) / Installed(1) / Check for updates(2) / Auto-update(3)
    // / Skip this version(4) / Beta builds(5)) — brings docs/SPEC-update-prompt.md's
    // two new rows into the neighbor band below it, so this one shot still
    // documents the OTA-available status AND proves the new rows actually
    // render (rather than adding a second persisted screenshot just for
    // them). Auto-update set to Overnight and the version left unskipped so
    // both show a real, non-blank value rather than their empty defaults.
    st->ota_auto = 1;
    st->generation++;
    sim_knob(1);
    pump_ms(300);
    pump_until_idle(800);
    snapshot("update");
}

// The update-prompt sheet (docs/SPEC-update-prompt.md): the real firmware
// only ever reaches this screen via nav_policy (the worker's idle-loop gate
// raising ota_prompt_due while showing SCR_DIAL) — the simulator doesn't run
// a nav policy at all, so this navigates there directly, same as every other
// scenario, with an update pending so the sheet's copy has a real version to
// show.
static void scenario_update_prompt(void)
{
    apply_baseline();
    app_state_t *st = sim_state_ptr();
    st->ota.status = OTA_AVAILABLE;
    snprintf(st->ota.latest, sizeof(st->ota.latest), SIM_OTA_LATEST);
    st->generation++;
    ui_router_go(SCR_UPDATE_PROMPT, (void *)(uintptr_t)ZONE_A, LV_SCR_LOAD_ANIM_NONE);
    // Same idle-out-the-slide-anim gate scenario_quick uses for its sheet.
    pump_ms(400);
    pump_until_idle(1000);
    snapshot("update-prompt");
}

static void scenario_settings(void)
{
    apply_baseline();
    ui_router_go(SCR_SETTINGS, NULL, LV_SCR_LOAD_ANIM_NONE);
    pump_ms(300);
    snapshot("settings");
}

// The Adjustment mode choice screen, reached from Settings' "Adjustment
// mode" row: apply_baseline() doesn't touch sched_follow, and
// sim_state_reset() left it at its fresh-device default (true = Schedule),
// so this renders with no extra state poking — Schedule selected, its
// description visible below the two options.
static void scenario_adjust_mode(void)
{
    apply_baseline();
    ui_router_go(SCR_ADJUST_MODE, NULL, LV_SCR_LOAD_ANIM_NONE);
    pump_ms(300);
    snapshot("adjust-mode");
}

// The Brightness submenu (M7, collapses Settings' old separate Day/Night
// rows): shows the last-committed percents read straight off app_state_t
// (and "Off" for a Night (clock) at 0), same contract the rows had before
// the collapse.
static void scenario_brightness_menu(void)
{
    apply_baseline();
    ui_router_go(SCR_BRIGHTNESS_MENU, NULL, LV_SCR_LOAD_ANIM_NONE);
    pump_ms(300);
    snapshot("brightness-menu");
}

// The full-screen SCR_BRIGHTNESS picker (replaces the old inline settings-row
// edit — see scr_brightness.c) opened on the Night (in use) row, then knob-adjusted
// so the render shows a deliberately chosen value (and the drag handle parked
// at it) rather than the untouched opening state — same idiom scenario_boost
// uses for its own picker. Turned UP, not down: the picker opens on the stored
// pref, and Night's shipped default is 0%, so any downward turn just clamps at
// the floor and documents nothing. sim_knob(+3) drains as one on_knob(+3)
// batch: |batch|>=3 accelerates to BRI_ACCEL_3_MULT (6%/detent), so
// 0% -> 0 + 3*6 = 18%.
static void scenario_settings_brightness(void)
{
    apply_baseline();
    ui_router_go(SCR_BRIGHTNESS, (void *)(uintptr_t)1 /* night */, LV_SCR_LOAD_ANIM_NONE);
    pump_ms(300);
    sim_knob(3);   // 0% -> 18% (accelerated: 3 detents * 6%/detent)
    pump_ms(300);
    snapshot("brightness");
}

// The same picker opened on the Night (clock) row (packed arg 2) — mainly
// to verify the "NIGHT (CLOCK)" caption clears the arc's chord at the
// same y-offset tuned for the old "NIGHT BRIGHTNESS" (see scr_brightness.c's
// create() comment), and that the live preview visibly differs from the Night row
// above (it previews the NIGHT table's STANDBY duty, deliberately very dim).
// sim_knob(+3) drains as one on_knob(+3) batch: |batch|>=3 accelerates to
// BRI_ACCEL_3_MULT (6%/detent), so the 20% default -> 20 + 3*6 = 38%.
static void scenario_settings_brightness_clock(void)
{
    apply_baseline();
    ui_router_go(SCR_BRIGHTNESS, (void *)(uintptr_t)2 /* night clock */, LV_SCR_LOAD_ANIM_NONE);
    pump_ms(300);
    sim_knob(3);   // 20% -> 38% (accelerated: 3 detents * 6%/detent)
    pump_ms(300);
    snapshot("brightness-clock");
}

// The clock picker driven to 0 — the one value on this row that is a state,
// not a level: the standby clock goes genuinely dark
// (dial_power_night_clock_duty(0) == 0), and the unit slot names it, "Off"
// replacing "%" — the 88px numeral font is digits-only, so the word rides
// the small label (scr_brightness.c's render_numeral).
static void scenario_settings_brightness_clock_off(void)
{
    apply_baseline();
    // The previous scenario ends with this same picker open on this same row,
    // and ui_router_go no-ops on an identical id+arg pair (ui_router.c) — so
    // step out to the menu first to force a real rebuild, then re-seed the
    // pref that stepping out just committed (destroy() persisted its 38%).
    ui_router_go(SCR_BRIGHTNESS_MENU, NULL, LV_SCR_LOAD_ANIM_NONE);
    pump_ms(100);
    sim_state_ptr()->bri_night_clock_pct = 20;
    ui_router_go(SCR_BRIGHTNESS, (void *)(uintptr_t)2 /* night clock */, LV_SCR_LOAD_ANIM_NONE);
    pump_ms(300);
    sim_knob(-6);   // one -6 batch: 6%/detent -> -36, clamping at the 0 rail -> "Off"
    pump_ms(300);
    snapshot("brightness-clock-off");
}

static void scenario_wifi_info(void)
{
    apply_baseline();
    ui_router_go(SCR_WIFI, NULL, LV_SCR_LOAD_ANIM_NONE);
    pump_ms(300);
    snapshot("wifi-info");
}

static void scenario_about(void)
{
    apply_baseline();
    ui_router_go(SCR_ABOUT, NULL, LV_SCR_LOAD_ANIM_NONE);
    pump_ms(300);
    snapshot("about");
}

// The hidden diagnostics face (UI_DESIGN_SPEC.md "Wi-Fi/battery/build"),
// reached by swiping down. Wi-Fi comes from stubs.c (connected, "HomeNet",
// 192.168.1.23) rather than app_state_t, so only the battery fields need
// setting here. 4056mV is a real reading off the bench dial on battery.
// The dial face's own battery badge. Only shows on battery, so this scenario
// is unplugged and low: red plus a 1s breathe, caught mid-cycle by the pump
// below so the capture shows the reduced opacity rather than an endpoint.
static void scenario_dial_batt_low(void)
{
    apply_baseline();
    app_state_t *st = sim_state_ptr();
    st->batt_mv = 3598;
    st->batt_pct = 9;
    st->batt_charging = false;
    ui_router_go(SCR_DIAL, (void *)(uintptr_t)ZONE_A, LV_SCR_LOAD_ANIM_NONE);
    pump_ms(1100);
    snapshot("dial-batt-low");
}

// Both row-44 badges up at once. Alone each sits dead centre; together they
// split evenly around it. Also the only capture of the badge in its ordinary
// (not-low) colour.
static void scenario_dial_batt_away(void)
{
    apply_baseline();
    app_state_t *st = sim_state_ptr();
    st->away = true;
    st->batt_mv = 3961;
    st->batt_pct = 64;
    st->batt_charging = false;
    // Bounce off another screen first: ui_router_go early-returns when the
    // target id and arg both match what is already loaded, so going straight
    // back to SCR_DIAL/ZONE_A after the previous scenario would be a no-op.
    ui_router_go(SCR_STANDBY, NULL, LV_SCR_LOAD_ANIM_NONE);
    ui_router_go(SCR_DIAL, (void *)(uintptr_t)ZONE_A, LV_SCR_LOAD_ANIM_NONE);
    // snapshot() reads the framebuffer as-is, it does not render. Without a
    // pump the capture is whatever the previous scenario left behind.
    pump_ms(100);
    snapshot("dial-batt-away");
}

static void scenario_diag(void)
{
    apply_baseline();
    app_state_t *st = sim_state_ptr();
    st->batt_mv = 4056;
    st->batt_pct = 82;
    st->batt_charging = false;
    // Bounce through the dial first. ui_router_go returns early when the
    // target is already current, so back-to-back SCR_DIAG scenarios would
    // silently render the previous one's numbers three times. Going via the
    // dial is also how a person actually reaches this face (swipe down).
    ui_router_go(SCR_DIAL, (void *)(uintptr_t)ZONE_A, LV_SCR_LOAD_ANIM_NONE);
    ui_router_go(SCR_DIAG, NULL, LV_SCR_LOAD_ANIM_NONE);
    pump_ms(300);
    snapshot("diag");
}

// Same face on USB. The percentage is deliberately not shown while charging:
// the rail is the charger's output, not the cell's, so any number would be a
// guess dressed up as a measurement.
static void scenario_diag_charging(void)
{
    apply_baseline();
    app_state_t *st = sim_state_ptr();
    st->batt_mv = 4732;
    st->batt_pct = DIAL_BATTERY_PCT_UNKNOWN;
    st->batt_charging = true;
    ui_router_go(SCR_DIAL, (void *)(uintptr_t)ZONE_A, LV_SCR_LOAD_ANIM_NONE);
    ui_router_go(SCR_DIAG, NULL, LV_SCR_LOAD_ANIM_NONE);
    pump_ms(300);
    snapshot("diag-charging");
}

// Low battery. Exercises the warning treatment, which per UI_DESIGN_SPEC.md
// ("Warning is never colour-only") is a colour change AND a 1s opacity
// breathe. The still frame can only show the colour; pump past a full cycle
// so the capture lands mid-breathe rather than at an endpoint.
static void scenario_diag_low(void)
{
    apply_baseline();
    app_state_t *st = sim_state_ptr();
    st->batt_mv = 3612;
    st->batt_pct = 11;
    st->batt_charging = false;
    ui_router_go(SCR_DIAL, (void *)(uintptr_t)ZONE_A, LV_SCR_LOAD_ANIM_NONE);
    ui_router_go(SCR_DIAG, NULL, LV_SCR_LOAD_ANIM_NONE);
    pump_ms(1100);
    snapshot("diag-low");
}

// Full-screen OTA install takeover (M6 UX hardening) — nav_policy forces this
// in the real firmware the moment ota.status becomes OTA_DOWNLOADING, but the
// simulator doesn't run a nav policy at all (every scenario navigates
// directly), so this just sets the status/progress a real download-in-
// -progress commit would carry and goes straight there. ~62% so the ring
// reads as mid-download, not just-started or about-to-finish.
static void scenario_updating(void)
{
    apply_baseline();
    app_state_t *st = sim_state_ptr();
    st->ota.status = 3;   // OTA_DOWNLOADING (dial_ota.h's dial_ota_status_t)
    st->ota.progress_pct = 62;
    ui_router_go(SCR_UPDATING, NULL, LV_SCR_LOAD_ANIM_NONE);
    pump_ms(300);
    snapshot("updating");
}

static void scenario_standby(void)
{
    apply_baseline();
    ui_router_go(SCR_STANDBY, (void *)(uintptr_t)ZONE_A, LV_SCR_LOAD_ANIM_NONE);
    pump_ms(300);
    snapshot("standby");
}

// The clock face with the ambient "Update available" indicator (see
// scenario_dial_update's own comment — same owner reassessment, same
// unconditional status+night gate, no idle window/daily ceiling). Confirms
// the notice's slot on this face (otherwise-empty gap below the clock/date,
// no page dots here to collide with) renders correctly. Like scenario_standby
// itself this bakes in the live wall clock — not meant to be diffed against
// a checked-in reference; see this file's own header / the caller's note on
// why standby*.png never stay checked out after a run.
static void scenario_standby_update(void)
{
    apply_baseline();
    app_state_t *st = sim_state_ptr();
    st->ota.status = OTA_AVAILABLE;
    snprintf(st->ota.latest, sizeof(st->ota.latest), SIM_OTA_LATEST);
    st->generation++;
    ui_router_go(SCR_STANDBY, (void *)(uintptr_t)ZONE_A, LV_SCR_LOAD_ANIM_NONE);
    pump_ms(300);
    snapshot("standby-update");
}

/* ---- entry point -----------------------------------------------------------*/

int main(void)
{
    ensure_dir(DIAL_SIM_OUTPUT_DIR);

    lv_init();

    lv_disp_draw_buf_init(&s_draw_buf, s_lv_buf, NULL, SCREEN_W * SCREEN_H);
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.draw_buf = &s_draw_buf;
    disp_drv.flush_cb = flush_cb;
    disp_drv.hor_res = SCREEN_W;
    disp_drv.ver_res = SCREEN_H;
    lv_disp_drv_register(&disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = indev_read_cb;
    lv_indev_drv_register(&indev_drv);

    sim_state_reset();
    ui_screens_register_all();
    // Sets up the 50ms dispatcher (drains ui_router_knob_input); the initial
    // screen doesn't matter since every scenario below navigates explicitly.
    ui_router_start(SCR_CONNECTING, NULL);

    scenario_welcome();
    scenario_wifi_portal();
    scenario_netpick();
    scenario_passkey();
    scenario_oauth_qr();
    scenario_oauth_waiting();
    scenario_sidepick();
    scenario_connecting();
    scenario_dial();
    scenario_dial_update();
    scenario_dial_relative();
    scenario_dial_until();
    scenario_dial_off_bedtime();
    scenario_boost();
    scenario_menu();
    scenario_update();
    scenario_update_prompt();
    scenario_settings();
    scenario_adjust_mode();
    scenario_brightness_menu();
    scenario_settings_brightness();
    scenario_settings_brightness_clock();
    scenario_settings_brightness_clock_off();
    scenario_wifi_info();
    scenario_about();
    scenario_dial_batt_low();
    scenario_dial_batt_away();
    scenario_diag();
    scenario_diag_charging();
    scenario_diag_low();
    scenario_updating();
    scenario_standby();
    scenario_standby_update();

    printf("done: %d screens rendered to %s\n", s_snapshots_written, DIAL_SIM_OUTPUT_DIR);
    return 0;
}
