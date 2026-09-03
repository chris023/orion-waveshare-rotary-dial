/*
 * SCR_DIAG — the hidden diagnostics face (UI_DESIGN_SPEC.md §"Standby",
 * swipe down: "opens a minimal diagnostics face (Wi-Fi/battery/build) —
 * repurposed since Standby is the root and 'back' has nowhere else to go").
 *
 * Three read-only panels, nothing interactive. This is the one face the spec
 * lets use Montserrat 12 (§typography: "hidden diagnostics face only
 * (build/IP) — never in user-facing UI"), so the captions are 12 and the
 * values 16. No 24+ anywhere: nothing here is hero content.
 *
 * Entry/exit is a sheet pulled DOWN from the top, so it leaves by being
 * pushed back UP. That reads opposite to scr_boost/scr_brightness, which
 * rise from the bottom and dismiss downward, but it is the same rule — a
 * sheet goes back the way it came. LV_DIR_RIGHT also exits, matching the
 * back gesture every menu sub-screen already honours.
 *
 * Wi-Fi is polled rather than pushed: RSSI and IP live in the driver, not in
 * app_state_t, so this borrows scr_wifi.c's timer idiom. Battery IS in
 * app_state_t (dial_battery's sampler puts it there) and arrives via
 * on_state like everything else.
 *
 * NOTE for anyone diffing against the spec: the spec's motion table asks for
 * LV_SCR_LOAD_ANIM_MOVE_BOTTOM at 200ms on swipe-down, which is what this
 * uses. The two existing bottom sheets use LV_SCR_LOAD_ANIM_NONE instead.
 * The chassis ring is r=165 here to match every other hand-laid face
 * (scr_standby, scr_menu, scr_adjust_mode) rather than the spec's r=170.
 */
#include "ui_screens_internal.h"
#include "dial_battery.h"
#include "dial_haptics.h"
#include "dial_wifi.h"
#include "esp_app_desc.h"
#include "esp_wifi.h"

#define CX 180
#define CY 180
#define ARC_R 165

// Wi-Fi facts come from the driver, so they need polling. Slow: nothing here
// is worth a redraw budget.
#define POLL_MS 2000

// Warning breathe, matching the state chevron's ping-pong vocabulary in
// scr_dial.c. The spec is explicit that warning is never colour-only.
#define PULSE_MS 1000

static lv_obj_t *s_ring;
static lv_obj_t *s_title_lbl;
static lv_obj_t *s_cap_wifi, *s_cap_batt, *s_cap_build;
static lv_obj_t *s_val_ssid, *s_val_ip;
static lv_obj_t *s_val_batt, *s_val_mv;
static lv_obj_t *s_val_build;
static lv_timer_t *s_poll_timer;
static bool s_pulsing;

// Set from create()'s arg. Defaults are the standby case, which is also the
// safe fallback if a future caller forgets to pack an origin.
static screen_id_t s_origin = SCR_STANDBY;
static void *s_origin_arg;

/* ---- helpers ---------------------------------------------------------------*/

static lv_obj_t *caption(lv_obj_t *scr, const char *txt, int y)
{
    lv_obj_t *l = lv_label_create(scr);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_letter_space(l, 2, 0);
    lv_label_set_text(l, txt);
    lv_obj_align(l, LV_ALIGN_CENTER, 0, y - CY);
    return l;
}

static lv_obj_t *value(lv_obj_t *scr, const lv_font_t *font, int y)
{
    lv_obj_t *l = lv_label_create(scr);
    lv_obj_set_style_text_font(l, font, 0);
    lv_label_set_text(l, "--");
    lv_obj_align(l, LV_ALIGN_CENTER, 0, y - CY);
    return l;
}

static void set_opa_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

/* ---- low-battery breathe ---------------------------------------------------*/

static void pulse_start(void)
{
    if (s_pulsing || !s_val_batt) return;
    s_pulsing = true;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_val_batt);
    lv_anim_set_exec_cb(&a, set_opa_cb);
    lv_anim_set_values(&a, LV_OPA_40, LV_OPA_100);
    lv_anim_set_time(&a, PULSE_MS);
    lv_anim_set_playback_time(&a, PULSE_MS);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

static void pulse_stop(void)
{
    if (!s_pulsing) return;
    s_pulsing = false;
    if (!s_val_batt) return;
    lv_anim_del(s_val_batt, set_opa_cb);
    lv_obj_set_style_opa(s_val_batt, LV_OPA_100, 0);
}

/* ---- panels ----------------------------------------------------------------*/

static const char *signal_word(int8_t rssi)
{
    if (rssi >= -60) return "Strong";
    if (rssi >= -70) return "Good";
    return "Weak";
}

// Wi-Fi: SSID on the value line, "IP · signal" underneath. Both come from the
// driver, neither is in app_state_t.
static void refresh_wifi(void)
{
    if (!s_val_ssid) return;

    if (!dial_wifi_is_connected()) {
        lv_label_set_text(s_val_ssid, "Not connected");
        lv_label_set_text(s_val_ip, "--");
        return;
    }

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        lv_label_set_text(s_val_ssid, (const char *)ap.ssid);
        char ip[24];
        if (!dial_net_ip(ip, sizeof(ip))) snprintf(ip, sizeof(ip), "--");
        lv_label_set_text_fmt(s_val_ip, "%s  %s", ip, signal_word(ap.rssi));
    }
}

static void refresh_battery(const app_state_t *st)
{
    if (!s_val_batt) return;
    const dial_palette_t *pal = PAL();

    // No sample yet. Zero is the initialised value, not a real reading, and
    // showing "0%" on a healthy dial would be a lie for the first 30 seconds.
    if (st->batt_mv == 0) {
        lv_label_set_text(s_val_batt, "--");
        lv_label_set_text(s_val_mv, "no sample yet");
        lv_obj_set_style_text_color(s_val_batt, pal->ink_primary, 0);
        pulse_stop();
        return;
    }

    if (st->batt_charging) {
        // On USB the rail is the charger, so there is no level to show. Say
        // so rather than inventing one.
        //
        // "On USB" and not "Charging": all that was measured is that the rail
        // sits above the cell's range. Whether a cell is fitted at all, and
        // whether it is taking charge, is not visible from this divider. The
        // battery is optional on this board, and a unit built without one
        // reads exactly like a charging unit, so this is the only wording
        // that stays true on both.
        lv_label_set_text(s_val_batt, "On USB");
        lv_obj_set_style_text_color(s_val_batt, pal->ink_primary, 0);
        pulse_stop();
    } else {
        lv_label_set_text_fmt(s_val_batt, "%d%%", st->batt_pct);
        bool low = st->batt_pct <= DIAL_BATTERY_PCT_LOW;
        lv_obj_set_style_text_color(s_val_batt, low ? pal->warning : pal->ink_primary, 0);
        if (low) pulse_start();
        else     pulse_stop();
    }

    lv_label_set_text_fmt(s_val_mv, "%d.%02d V", st->batt_mv / 1000,
                          (st->batt_mv % 1000) / 10);
}

/* ---- palette ---------------------------------------------------------------*/

static void apply_palette(lv_obj_t *scr)
{
    const dial_palette_t *pal = PAL();
    lv_obj_set_style_bg_color(scr, pal->bg, 0);
    lv_obj_set_style_arc_color(s_ring, pal->track, LV_PART_MAIN);

    lv_obj_t *dim[] = { s_title_lbl, s_cap_wifi, s_cap_batt, s_cap_build,
                        s_val_ip, s_val_mv };
    for (size_t i = 0; i < sizeof(dim) / sizeof(dim[0]); i++)
        lv_obj_set_style_text_color(dim[i], pal->ink_secondary, 0);

    lv_obj_t *bright[] = { s_val_ssid, s_val_build };
    for (size_t i = 0; i < sizeof(bright) / sizeof(bright[0]); i++)
        lv_obj_set_style_text_color(bright[i], pal->ink_primary, 0);

    // s_val_batt is deliberately absent: refresh_battery owns its colour
    // because it alone knows whether the level is a warning.
}

/* ---- vtable ----------------------------------------------------------------*/

static void poll_cb(lv_timer_t *t) { (void)t; refresh_wifi(); }

/* ---- leaving --------------------------------------------------------------*/

// Back always returns to whoever opened us (see DIAG_ARG_FROM_DIAL). Dumping
// the user on the clock when they came from a dial page loses their place,
// and on a two-zone bed it silently loses which side they were looking at.
static void go_back(lv_scr_load_anim_t anim)
{
    dial_haptics_play(HAPTIC_TICK);
    ui_router_go(s_origin, s_origin_arg, anim);
}

// Tap anywhere dismisses. A tap has no direction, so it borrows the swipe-up
// animation: the sheet goes back the way it arrived either way.
static void tap_back_cb(lv_event_t *e)
{
    (void)e;
    go_back(LV_SCR_LOAD_ANIM_MOVE_TOP);
}

static void create(lv_obj_t *scr, void *arg)
{
    uintptr_t packed = (uintptr_t)arg;
    if (packed & DIAG_ARG_FROM_DIAL) {
        s_origin     = SCR_DIAL;
        s_origin_arg = (void *)(uintptr_t)(packed & 1u);   // zone_idx_t
    } else {
        s_origin     = SCR_STANDBY;
        s_origin_arg = NULL;
    }

    const dial_palette_t *pal = PAL();
    lv_obj_set_style_bg_color(scr, pal->bg, 0);
    // Nothing on this face is interactive, so a tap can only mean "done".
    lv_obj_add_event_cb(scr, tap_back_cb, LV_EVENT_CLICKED, NULL);

    // Chassis hairline ring — same geometry as scr_standby's, w=2, no lit
    // indicator, non-interactive.
    s_ring = lv_arc_create(scr);
    lv_obj_set_size(s_ring, 2 * ARC_R, 2 * ARC_R);
    lv_obj_center(s_ring);
    lv_arc_set_rotation(s_ring, 135);
    lv_arc_set_bg_angles(s_ring, 0, 270);
    lv_obj_set_style_arc_width(s_ring, 2, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_ring, 0, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(s_ring, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_ring, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_clear_flag(s_ring, LV_OBJ_FLAG_CLICKABLE);

    s_title_lbl = caption(scr, "DIAGNOSTICS", 72);

    s_cap_wifi  = caption(scr, "WI-FI", 116);
    s_val_ssid  = value(scr, &lv_font_montserrat_16, 136);
    s_val_ip    = value(scr, &lv_font_montserrat_12, 155);

    s_cap_batt  = caption(scr, "BATTERY", 184);
    s_val_batt  = value(scr, &lv_font_montserrat_16, 204);
    s_val_mv    = value(scr, &lv_font_montserrat_12, 223);

    s_cap_build = caption(scr, "BUILD", 252);
    s_val_build = value(scr, &lv_font_montserrat_16, 272);

    const esp_app_desc_t *desc = esp_app_get_description();
    lv_label_set_text_fmt(s_val_build, "v%s", desc->version);

    apply_palette(scr);
    refresh_wifi();
    s_poll_timer = lv_timer_create(poll_cb, POLL_MS, NULL);
}

static void destroy(void)
{
    pulse_stop();
    if (s_poll_timer) { lv_timer_del(s_poll_timer); s_poll_timer = NULL; }
    s_ring = NULL;
    s_title_lbl = NULL;
    s_cap_wifi = s_cap_batt = s_cap_build = NULL;
    s_val_ssid = s_val_ip = NULL;
    s_val_batt = s_val_mv = NULL;
    s_val_build = NULL;
    s_pulsing = false;
    s_origin = SCR_STANDBY;
    s_origin_arg = NULL;
}

static void on_state(const app_state_t *st)
{
    if (!s_ring) return;
    apply_palette(lv_obj_get_parent(s_ring));
    refresh_battery(st);
}

// Nothing here is adjustable. Swallow the detent rather than letting it fall
// through: this face is one gesture away from a bedside standby screen, and
// the whole point of the wake rule is that a blind reach never changes the
// bed. The soft stop says "nothing to turn" without doing anything.
static bool on_knob(int detents)
{
    if (detents == 0) return false;
    dial_haptics_play_soft(HAPTIC_STOP);
    return true;
}

static bool on_gesture(lv_dir_t dir)
{
    // Pushed back up the way it came, or the usual right-swipe back.
    if (dir != LV_DIR_TOP && dir != LV_DIR_RIGHT) return false;
    go_back(dir == LV_DIR_TOP ? LV_SCR_LOAD_ANIM_MOVE_TOP
                              : LV_SCR_LOAD_ANIM_MOVE_RIGHT);
    return true;
}

const ui_screen_t scr_diag = {
    .create = create, .destroy = destroy, .on_state = on_state,
    .on_knob = on_knob, .on_gesture = on_gesture,
};
