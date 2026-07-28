/*
 * SCR_BRIGHTNESS_MENU — the Day/Night brightness submenu, reached from
 * scr_settings.c's single "Brightness" row (owner requirement: collapse the
 * two brightness rows that used to live directly in Settings into the same
 * submenu shape as the Update screen). A single scrollable list:
 *
 *   < Back   -> SCR_SETTINGS
 *   Day      value = the current day percent (st->bri_day_pct); tap opens
 *            the full-screen SCR_BRIGHTNESS picker with packed arg 0.
 *   Night    value = the current night percent (st->bri_night_pct); tap
 *            opens SCR_BRIGHTNESS with packed arg 1.
 *
 * The picker (scr_brightness.c) owns the live preview and the actual
 * commit, same as when these two rows lived in Settings directly — this
 * screen only ever shows the last-committed percent and, on both of the
 * picker's exit paths, is where the user lands back.
 *
 * No other entry point (no schedule, no zone), so on_state has nothing to
 * gate on besides its own root pointer — same shape as every other menu
 * sub-screen (scr_settings.c, scr_about.c, scr_update.c).
 */
#include "ui_screens_internal.h"
#include "dial_haptics.h"
#include "dial_list.h"

#define CY 180
#define ROW_H 76

static lv_obj_t *s_title_lbl;
static lv_obj_t *s_list;
static lv_obj_t *s_val_day;
static lv_obj_t *s_val_night;

/* ---- row factory (scr_settings.c's, ported verbatim) --------------------*/

static lv_obj_t *make_row(lv_obj_t *parent, const char *label_txt, lv_event_cb_t cb, lv_obj_t **value_out)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), ROW_H);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    // 36px side insets, not 20: neighbor rows in the rotor rest where the
    // round panel's chord is narrower, and 20 left their ends cropped.
    lv_obj_set_style_pad_hor(row, 36, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    if (cb) lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(row);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
    lv_label_set_text(lbl, label_txt);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *val = lv_label_create(row);
    lv_obj_set_style_text_font(val, &lv_font_montserrat_16, 0);
    lv_label_set_text(val, "");
    lv_obj_align(val, LV_ALIGN_RIGHT_MID, 0, 0);
    if (value_out) *value_out = val;

    return row;
}

/* ---- row actions ----------------------------------------------------------*/

// Row 0 on every menu sub-screen: the right-swipe still works, but it isn't
// discoverable on its own.
static void row_back_cb(lv_event_t *e)
{
    (void)e;
    dial_haptics_play(HAPTIC_TICK);
    ui_router_go(SCR_SETTINGS, NULL, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
}

// Both open the full-screen SCR_BRIGHTNESS picker, packing which row it was
// opened from (0 = day, 1 = night) — plain navigation, same as every other
// row on this screen. The picker owns the live preview and commits on its
// own exit, then returns here (not to Settings — see scr_brightness.c).
static void row_day_cb(lv_event_t *e)
{
    (void)e;
    dial_haptics_play(HAPTIC_TICK);
    ui_router_go(SCR_BRIGHTNESS, (void *)(uintptr_t)0, LV_SCR_LOAD_ANIM_NONE);
}

static void row_night_cb(lv_event_t *e)
{
    (void)e;
    dial_haptics_play(HAPTIC_TICK);
    ui_router_go(SCR_BRIGHTNESS, (void *)(uintptr_t)1, LV_SCR_LOAD_ANIM_NONE);
}

/* ---- palette ---------------------------------------------------------------*/

static void apply_palette(lv_obj_t *scr)
{
    const dial_palette_t *pal = PAL();
    lv_obj_set_style_bg_color(scr, pal->bg, 0);
    lv_obj_set_style_text_color(s_title_lbl, pal->ink_secondary, 0);

    uint32_t n = lv_obj_get_child_cnt(s_list);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *row = lv_obj_get_child(s_list, i);
        lv_obj_set_style_border_color(row, pal->track, 0);
        uint32_t rc = lv_obj_get_child_cnt(row);
        for (uint32_t j = 0; j < rc; j++) {
            lv_obj_t *lbl = lv_obj_get_child(row, j);
            lv_obj_set_style_text_color(lbl, j == 0 ? pal->ink_primary : pal->ink_secondary, 0);
        }
    }
}

/* ---- vtable ----------------------------------------------------------------*/

static void create(lv_obj_t *scr, void *arg)
{
    (void)arg;
    const dial_palette_t *pal = PAL();
    lv_obj_set_style_bg_color(scr, pal->bg, 0);

    s_list = dial_list_create(scr, ROW_H);

    make_row(s_list, LV_SYMBOL_LEFT "  Back", row_back_cb, NULL);
    make_row(s_list, "Day",   row_day_cb,   &s_val_day);
    make_row(s_list, "Night", row_night_cb, &s_val_night);

    // Created AFTER the list so it draws over rows scrolling beneath it —
    // same fixed title slot the other menu sub-screens use.
    s_title_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_title_lbl, &lv_font_montserrat_16, 0);
    lv_label_set_text(s_title_lbl, "BRIGHTNESS");
    lv_obj_align(s_title_lbl, LV_ALIGN_CENTER, 0, 64 - CY);

    apply_palette(scr);
    dial_list_settle(s_list, 1);   // open on "Day", not on Back
}

static void destroy(void)
{
    s_list = NULL;
    s_title_lbl = NULL;
    s_val_day = NULL;
    s_val_night = NULL;
}

static void on_state(const app_state_t *st)
{
    if (!s_list) return;
    apply_palette(lv_obj_get_parent(s_list));

    // Plain read of the last-committed values — SCR_BRIGHTNESS owns the live
    // preview and the actual commit; this screen just mirrors app_state_t
    // (same contract these two rows had when they lived directly in
    // scr_settings.c).
    char buf[8];
    snprintf(buf, sizeof buf, "%u%%", (unsigned)st->bri_day_pct);
    lv_label_set_text(s_val_day, buf);
    snprintf(buf, sizeof buf, "%u%%", (unsigned)st->bri_night_pct);
    lv_label_set_text(s_val_night, buf);
}

// The knob walks the focused row (one per detent, dial_list's rotor snap) —
// nothing on this screen is itself an adjustable control.
static bool on_knob(int detents)
{
    if (!s_list || detents == 0) return false;
    int r = dial_list_knob(s_list, detents);
    if (r) dial_haptics_play(r > 0 ? HAPTIC_TICK : HAPTIC_STOP);
    return true;
}

static bool on_gesture(lv_dir_t dir)
{
    if (dir != LV_DIR_RIGHT) return false;
    ui_router_go(SCR_SETTINGS, NULL, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
    return true;
}

const ui_screen_t scr_brightness_menu = {
    .create = create, .destroy = destroy, .on_state = on_state,
    .on_knob = on_knob, .on_gesture = on_gesture,
};
