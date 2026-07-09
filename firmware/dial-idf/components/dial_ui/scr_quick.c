/*
 * SCR_QUICK — quick-actions bottom sheet (design-spec.md §6's "Quick-actions"
 * motion row + M3's extension). Reached by a long-press anywhere on the dial
 * (scr_dial.c's screen_long_press_cb); the arg is the zone_idx_t the sheet
 * was opened from, which flows through to "Bed off"/"Away" (zone-agnostic)
 * and to SCR_BOOST (packed with the chosen heat/cool bit).
 *
 * The screen behind the sheet is just a flat bg fill — no dimmed home face,
 * per the brief ("keep it simple"). The sheet itself is an opaque `surface`
 * panel that slides up over it; entry/exit therefore live entirely in this
 * screen's own y-position anim rather than a full-screen lv_scr_load_anim
 * (ui_router_go uses LV_SCR_LOAD_ANIM_NONE both in and out, see main.c /
 * scr_dial.c / go_boost() below).
 */
#include "ui_screens_internal.h"
#include "dial_haptics.h"

#define SCREEN_W       360
#define SHEET_H        198   // bottom 55% of a 360px screen
#define SHEET_Y_OPEN   162   // 360 - SHEET_H
#define SHEET_Y_CLOSED 360   // fully below the visible screen

#define ROW_H 72

static lv_obj_t *s_sheet;
static lv_obj_t *s_grab;
static lv_obj_t *s_list;
static lv_obj_t *s_row_boost_heat, *s_row_boost_cool, *s_row_bed_off, *s_row_away, *s_row_settings;
static lv_obj_t *s_lbl_boost_heat, *s_lbl_boost_cool, *s_lbl_bed_off, *s_lbl_away, *s_lbl_settings;

static zone_idx_t s_zone = ZONE_A;   // the dial side this sheet was opened from
static bool s_away;                  // last-known away flag, for the toggle

/* ---- sheet slide anim (design-spec.md §6: 180ms up ease-out / 150ms down) */

static void set_y_cb(void *obj, int32_t v) { lv_obj_set_y((lv_obj_t *)obj, (lv_coord_t)v); }

static void anim_sheet(int32_t from, int32_t to, uint32_t time_ms,
                        lv_anim_path_cb_t path, lv_anim_ready_cb_t ready_cb)
{
    lv_anim_del(s_sheet, set_y_cb);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_sheet);
    lv_anim_set_exec_cb(&a, set_y_cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, time_ms);
    lv_anim_set_path_cb(&a, path);
    if (ready_cb) lv_anim_set_ready_cb(&a, ready_cb);
    lv_anim_start(&a);
}

// Fires once the close animation finishes; navigates back to the dial on the
// same side the sheet was opened from.
static void close_ready_cb(lv_anim_t *a)
{
    (void)a;
    ui_router_go(SCR_DIAL, (void *)(uintptr_t)s_zone, LV_SCR_LOAD_ANIM_NONE);
}

static void dismiss(void)
{
    anim_sheet(lv_obj_get_y(s_sheet), SHEET_Y_CLOSED, 150, lv_anim_path_ease_in, close_ready_cb);
}

/* ---- row actions -------------------------------------------------------- */

static void go_boost(bool heat)
{
    uintptr_t packed = ((uintptr_t)s_zone << 1) | (heat ? 1u : 0u);
    ui_router_go(SCR_BOOST, (void *)packed, LV_SCR_LOAD_ANIM_NONE);
}

static void row_boost_heat_cb(lv_event_t *e) { (void)e; go_boost(true); }
static void row_boost_cool_cb(lv_event_t *e) { (void)e; go_boost(false); }

static void row_bed_off_cb(lv_event_t *e)
{
    (void)e;
    dial_haptics_play(HAPTIC_CONFIRM);
    app_cmd_t cmd = { .kind = CMD_BED_OFF };
    dial_cmd_post(&cmd);
    dismiss();
}

static void row_away_cb(lv_event_t *e)
{
    (void)e;
    dial_haptics_play(HAPTIC_CONFIRM);
    app_cmd_t cmd = { .kind = CMD_AWAY, .a = s_away ? 0 : 1 };
    dial_cmd_post(&cmd);
    dismiss();
}

static void row_settings_cb(lv_event_t *e)
{
    (void)e;
    ui_router_go(SCR_SETTINGS, (void *)(uintptr_t)s_zone, LV_SCR_LOAD_ANIM_NONE);
}

/* ---- palette -------------------------------------------------------------*/

static void apply_palette(void)
{
    const dial_palette_t *pal = PAL();
    lv_obj_t *scr = lv_obj_get_parent(s_sheet);
    lv_obj_set_style_bg_color(scr, pal->bg, 0);
    lv_obj_set_style_bg_color(s_sheet, pal->surface, 0);
    lv_obj_set_style_bg_color(s_grab, pal->ink_secondary, 0);

    lv_obj_t *labels[] = { s_lbl_boost_heat, s_lbl_boost_cool, s_lbl_bed_off, s_lbl_away, s_lbl_settings };
    for (size_t i = 0; i < sizeof(labels) / sizeof(labels[0]); i++)
        lv_obj_set_style_text_color(labels[i], pal->ink_primary, 0);
}

/* ---- row factory ---------------------------------------------------------*/

static lv_obj_t *make_row(lv_obj_t *parent, lv_event_cb_t cb, lv_obj_t **lbl_out)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), ROW_H);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(row);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(lbl);
    *lbl_out = lbl;
    return row;
}

/* ---- vtable ---------------------------------------------------------------*/

static void create(lv_obj_t *scr, void *arg)
{
    s_zone = (zone_idx_t)(uintptr_t)arg;
    const dial_palette_t *pal = PAL();
    lv_obj_set_style_bg_color(scr, pal->bg, 0);

    s_sheet = lv_obj_create(scr);
    lv_obj_set_size(s_sheet, SCREEN_W, SHEET_H);
    lv_obj_set_pos(s_sheet, 0, SHEET_Y_CLOSED);
    lv_obj_set_style_radius(s_sheet, 0, 0);
    lv_obj_set_style_border_width(s_sheet, 0, 0);
    lv_obj_set_style_bg_opa(s_sheet, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_sheet, LV_OBJ_FLAG_SCROLLABLE);

    // Grab bar (40x4, radius 2) at the sheet's top edge.
    s_grab = lv_obj_create(s_sheet);
    lv_obj_set_size(s_grab, 40, 4);
    lv_obj_set_style_radius(s_grab, 2, 0);
    lv_obj_set_style_border_width(s_grab, 0, 0);
    lv_obj_clear_flag(s_grab, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_grab, LV_ALIGN_TOP_MID, 0, 10);

    // Row list: scrollable (four >=72px rows can exceed the 198px sheet), the
    // knob does not drive this scroll (on_knob below always returns false) —
    // only a finger drag does, which is ordinary LVGL flex behavior.
    s_list = lv_obj_create(s_sheet);
    lv_obj_set_pos(s_list, 0, 24);
    lv_obj_set_size(s_list, SCREEN_W, SHEET_H - 24);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_style_pad_row(s_list, 0, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);

    s_row_boost_heat = make_row(s_list, row_boost_heat_cb, &s_lbl_boost_heat);
    lv_label_set_text(s_lbl_boost_heat, "Boost heat");
    s_row_boost_cool = make_row(s_list, row_boost_cool_cb, &s_lbl_boost_cool);
    lv_label_set_text(s_lbl_boost_cool, "Boost cool");
    s_row_bed_off    = make_row(s_list, row_bed_off_cb, &s_lbl_bed_off);
    lv_label_set_text(s_lbl_bed_off, "Bed off");
    s_row_away       = make_row(s_list, row_away_cb, &s_lbl_away);
    lv_label_set_text(s_lbl_away, "Away mode on");   // on_state corrects this immediately
    s_row_settings   = make_row(s_list, row_settings_cb, &s_lbl_settings);
    lv_label_set_text(s_lbl_settings, "Settings");

    apply_palette();
    anim_sheet(SHEET_Y_CLOSED, SHEET_Y_OPEN, 180, lv_anim_path_ease_out, NULL);
}

static void destroy(void)
{
    if (s_sheet) lv_anim_del(s_sheet, NULL);
    s_sheet = s_grab = s_list = NULL;
    s_row_boost_heat = s_row_boost_cool = s_row_bed_off = s_row_away = s_row_settings = NULL;
    s_lbl_boost_heat = s_lbl_boost_cool = s_lbl_bed_off = s_lbl_away = s_lbl_settings = NULL;
}

static void on_state(const app_state_t *st)
{
    if (!s_sheet) return;
    s_away = st->away;
    lv_label_set_text(s_lbl_away, s_away ? "Away mode off" : "Away mode on");
    apply_palette();
}

// The knob scrolls/adjusts nothing on this screen.
static bool on_knob(int detents) { (void)detents; return false; }

static bool on_gesture(lv_dir_t dir)
{
    if (dir != LV_DIR_BOTTOM) return false;
    dismiss();
    return true;
}

const ui_screen_t scr_quick = {
    .create = create, .destroy = destroy, .on_state = on_state,
    .on_knob = on_knob, .on_gesture = on_gesture,
};
