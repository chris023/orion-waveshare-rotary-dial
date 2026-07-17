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

#include "lvgl.h"
#include "ui_router.h"
#include "ui_screens.h"
#include "dial_state.h"
#include "dial_palette.h"
#include "sim_state.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define SCREEN_W 360
#define SCREEN_H 360

#ifndef DIAL_SIM_OUTPUT_DIR
#define DIAL_SIM_OUTPUT_DIR "docs/screens"
#endif

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
    st->rotation = 0;
    st->haptics_enabled = true;
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
    ui_router_go(SCR_OAUTH_QR, NULL, LV_SCR_LOAD_ANIM_NONE);
    pump_ms(300);
    snapshot("oauth-qr");
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

static void scenario_quick(void)
{
    apply_baseline();
    ui_router_go(SCR_QUICK, (void *)(uintptr_t)ZONE_A, LV_SCR_LOAD_ANIM_NONE);
    // Capture the sheet exactly as it looks the moment it finishes opening:
    // grab bar at the top edge, row list at scroll position 0 showing the
    // first rows. The 180ms open-slide is an lv_anim, so gate on LVGL's own
    // animation bookkeeping (plus a generous fixed pump first) rather than
    // guessing — a too-short guess is what once produced a mid-motion frame.
    pump_ms(400);
    pump_until_idle(1000);
    snapshot("quick");
}

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

static void scenario_tonight(void)
{
    apply_baseline();
    app_state_t *st = sim_state_ptr();
    zone_state_t *a = &st->zones[ZONE_A];   // primary zone; Tonight always shows it
    a->sched_valid = true;
    snprintf(a->sched_bedtime, sizeof(a->sched_bedtime), "22:30");
    a->sched_bedtime_temp_c = 19.0f;   // -> 66F
    snprintf(a->sched_wakeup, sizeof(a->sched_wakeup), "06:30");
    a->sched_wakeup_temp_c = 21.0f;    // -> 70F
    a->sched_override_applied = false;
    a->sched_override_available = true;
    ui_router_go(SCR_TONIGHT, NULL, LV_SCR_LOAD_ANIM_NONE);
    pump_ms(300);
    snapshot("tonight");
}

static void scenario_menu(void)
{
    apply_baseline();
    ui_router_go(SCR_MENU, NULL, LV_SCR_LOAD_ANIM_NONE);
    pump_ms(300);
    snapshot("menu");
}

static void scenario_settings(void)
{
    apply_baseline();
    ui_router_go(SCR_SETTINGS, NULL, LV_SCR_LOAD_ANIM_NONE);
    pump_ms(300);
    snapshot("settings");
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

static void scenario_standby(void)
{
    apply_baseline();
    ui_router_go(SCR_STANDBY, (void *)(uintptr_t)ZONE_A, LV_SCR_LOAD_ANIM_NONE);
    pump_ms(300);
    snapshot("standby");
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
    scenario_sidepick();
    scenario_connecting();
    scenario_dial();
    scenario_quick();
    scenario_boost();
    scenario_tonight();
    scenario_menu();
    scenario_settings();
    scenario_wifi_info();
    scenario_about();
    scenario_standby();

    printf("done: 16 screens rendered to %s\n", DIAL_SIM_OUTPUT_DIR);
    return 0;
}
