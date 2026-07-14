/*
 * SCR_SETTINGS — full-screen scrollable settings list. Reached from the menu
 * face (scr_menu.c's "Settings" row); swipe right returns there. Wi-Fi and
 * software-update controls live on their own menu sub-screens (scr_wifi.c /
 * scr_about.c), so this list is only the preference + account rows.
 *
 * Rows >=72px tall: label (Mont 24) left, value (Mont 16) right-aligned,
 * living in a rotor list (dial_list.h — snap-centered focus row, edge rows
 * zoomed/faded for the round panel; knob walks one row per detent, a finger
 * drag free-scrolls then snaps). Tap activates a row. The two destructive
 * rows (re-link/factory reset) use a tap-twice-within-3s confirm pattern
 * instead of firing immediately.
 */
#include "ui_screens_internal.h"
#include "dial_haptics.h"
#include "dial_list.h"
#include "dial_display.h"

#define CY 180
#define ROW_H          76
#define CONFIRM_WINDOW_MS 3000

static lv_obj_t *s_title_lbl;
static lv_obj_t *s_list;
static lv_obj_t *s_val_units, *s_val_haptics, *s_val_rotation;

typedef enum { CONFIRM_RELINK = 0, CONFIRM_FACTORY, CONFIRM_COUNT } confirm_id_t;
static lv_obj_t   *s_val_confirm[CONFIRM_COUNT];
static confirm_id_t s_armed = CONFIRM_COUNT;   // CONFIRM_COUNT = "none armed"
static uint32_t     s_armed_at_ms;
static lv_timer_t   *s_confirm_timer;

/* ---- row factory --------------------------------------------------------*/

static lv_obj_t *make_row(lv_obj_t *parent, const char *label_txt, lv_event_cb_t cb, lv_obj_t **value_out)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), ROW_H);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    // 36px side insets, not 20: rows near the top/bottom of the list sit
    // where the round panel's chord is narrower, and 20 put label/value
    // ends outside the visible circle (owner-reported clipping).
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

/* ---- confirm-row helper (re-link / Wi-Fi reset / factory reset) --------- */

static void confirm_set_label(confirm_id_t id, const char *txt)
{
    if (s_val_confirm[id]) lv_label_set_text(s_val_confirm[id], txt);
}

static void confirm_disarm(void)
{
    if (s_armed != CONFIRM_COUNT) confirm_set_label(s_armed, "");
    s_armed = CONFIRM_COUNT;
}

// Ticks while a confirm row is armed, so the "tap again" prompt reverts if
// the 3s window lapses without a second tap.
static void confirm_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_armed != CONFIRM_COUNT && lv_tick_elaps(s_armed_at_ms) >= CONFIRM_WINDOW_MS)
        confirm_disarm();
}

// Returns true if this tap landed within the window of a matching prior tap
// (i.e. the action should fire now).
static bool confirm_tap(confirm_id_t id)
{
    if (s_armed == id && lv_tick_elaps(s_armed_at_ms) < CONFIRM_WINDOW_MS) {
        confirm_disarm();
        return true;
    }
    confirm_disarm();
    s_armed = id;
    s_armed_at_ms = lv_tick_get();
    confirm_set_label(id, "Tap again to confirm");
    return false;
}

/* ---- row actions ---------------------------------------------------------*/

// Row 0 on every menu sub-screen: swiping right still works, but the gesture
// isn't discoverable on its own (owner feedback), and a row is the one back
// affordance that can't occlude the list it sits in.
static void row_back_cb(lv_event_t *e)
{
    (void)e;
    dial_haptics_play(HAPTIC_TICK);
    ui_router_go(SCR_MENU, NULL, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
}

// Which way is "up" is a property of the room, not the device: the dial's cable
// exits one edge, and on a nightstand that edge is as likely to point at the
// bed as away from it. Cycles 0 -> 90 -> 180 -> 270 and applies immediately, so
// the effect of the tap is the thing you're looking at.
static void row_rotation_cb(lv_event_t *e)
{
    (void)e;
    app_state_t st;
    dial_state_get(&st);
    uint8_t next = (st.rotation + 1) & 3;
    if (!dial_display_set_rotation(next)) {
        dial_haptics_play(HAPTIC_ERROR);   // no memory for the 90/270 scratch buffer
        return;
    }
    dial_state_set_rotation(next);
    dial_haptics_play(HAPTIC_TICK);
}

static void row_units_cb(lv_event_t *e)
{
    (void)e;
    app_state_t st;
    dial_state_get(&st);
    dial_haptics_play(HAPTIC_TICK);
    dial_state_set_units_c(!st.units_c);
}

static void row_haptics_cb(lv_event_t *e)
{
    (void)e;
    app_state_t st;
    dial_state_get(&st);
    bool enabled = !st.haptics_enabled;
    dial_haptics_set_enabled(enabled);
    dial_state_set_haptics_enabled(enabled);
    dial_haptics_play(HAPTIC_CONFIRM);   // audible only if now enabled
}

static void row_relink_cb(lv_event_t *e)
{
    (void)e;
    if (!confirm_tap(CONFIRM_RELINK)) return;
    dial_haptics_play(HAPTIC_CONFIRM);
    app_cmd_t cmd = { .kind = CMD_RELINK };
    dial_cmd_post(&cmd);
}

static void row_factory_reset_cb(lv_event_t *e)
{
    (void)e;
    if (!confirm_tap(CONFIRM_FACTORY)) return;
    dial_haptics_play(HAPTIC_CONFIRM);
    app_cmd_t cmd = { .kind = CMD_FACTORY_RESET };
    dial_cmd_post(&cmd);
}

/* ---- palette --------------------------------------------------------------*/

static void apply_palette(lv_obj_t *scr)
{
    const dial_palette_t *pal = PAL();
    lv_obj_set_style_bg_color(scr, pal->bg, 0);
    if (s_title_lbl) lv_obj_set_style_text_color(s_title_lbl, pal->ink_secondary, 0);

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
    s_armed = CONFIRM_COUNT;
    const dial_palette_t *pal = PAL();
    lv_obj_set_style_bg_color(scr, pal->bg, 0);

    s_list = dial_list_create(scr, ROW_H);

    // No "My side" row: it only re-ran SCR_SIDEPICK, which sets the very same
    // ui_zone that one swipe on the dial already sets (and persists) — the row
    // changed nothing you couldn't change faster by swiping.
    make_row(s_list, LV_SYMBOL_LEFT "  Back", row_back_cb, NULL);
    make_row(s_list, "Units",         row_units_cb,         &s_val_units);
    make_row(s_list, "Rotation",      row_rotation_cb,      &s_val_rotation);
    make_row(s_list, "Haptics",       row_haptics_cb,       &s_val_haptics);
    make_row(s_list, "Re-link Orion", row_relink_cb,        &s_val_confirm[CONFIRM_RELINK]);
    make_row(s_list, "Factory reset", row_factory_reset_cb, &s_val_confirm[CONFIRM_FACTORY]);

    // Created AFTER the list so it draws over rows scrolling beneath it —
    // same fixed title slot the other menu sub-screens use.
    s_title_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_title_lbl, &lv_font_montserrat_16, 0);
    lv_label_set_text(s_title_lbl, "SETTINGS");
    lv_obj_align(s_title_lbl, LV_ALIGN_CENTER, 0, 64 - CY);

    apply_palette(scr);
    dial_list_settle(s_list, 1);   // open on "My side", not on Back
    s_confirm_timer = lv_timer_create(confirm_timer_cb, 250, NULL);
}

static void destroy(void)
{
    if (s_confirm_timer) { lv_timer_del(s_confirm_timer); s_confirm_timer = NULL; }
    s_list = NULL;
    s_title_lbl = NULL;
    s_val_units = s_val_haptics = s_val_rotation = NULL;
    for (int i = 0; i < CONFIRM_COUNT; i++) s_val_confirm[i] = NULL;
    s_armed = CONFIRM_COUNT;
}

static void on_state(const app_state_t *st)
{
    if (!s_list) return;
    apply_palette(lv_obj_get_parent(s_list));

    static const char *ROT[] = { "0\xC2\xB0", "90\xC2\xB0", "180\xC2\xB0", "270\xC2\xB0" };
    lv_label_set_text(s_val_rotation, ROT[st->rotation & 3]);

    lv_label_set_text(s_val_units, st->units_c ? "\xC2\xB0" "C" : "\xC2\xB0" "F");
    lv_label_set_text(s_val_haptics, st->haptics_enabled ? "On" : "Off");
}

// The knob walks the focused row (one per detent, dial_list's rotor snap).
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
    ui_router_go(SCR_MENU, NULL, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
    return true;
}

const ui_screen_t scr_settings = {
    .create = create, .destroy = destroy, .on_state = on_state,
    .on_knob = on_knob, .on_gesture = on_gesture,
};
