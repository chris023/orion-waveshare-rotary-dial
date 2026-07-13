/*
 * SCR_MENU — the third face of the swipe chain, replacing the old spot
 * Tonight used to occupy directly off Dial(B) (chain is now
 * Dial(A) <-left- Dial(B) <-left- Menu). Tonight/Settings/Wi-Fi/About are
 * no longer faces of the chain themselves: they're sub-screens one tap
 * off this list, each dismissed by swiping RIGHT back to here rather than
 * all the way to Dial(B). That indirection exists so the chain stays a
 * fixed 3 faces regardless of how many settings-ish screens accumulate.
 *
 * No title label: the focused row IS the heading, and the page dots are
 * what tell you you're on face 3 of 3 — a heading would either crowd a row
 * or get cropped by the physical round bezel up top.
 *
 * The rows live in a rotor list (dial_list.h): one row snaps focused to the
 * vertical center, neighbors shrink/fade toward the bezel, and the knob
 * walks focus one row per detent.
 */
#include "ui_screens_internal.h"
#include "dial_haptics.h"
#include "dial_list.h"

#define CX 180
#define CY 180
#define ARC_R 165
#define ROW_H 72

static lv_obj_t *s_ring;
static lv_obj_t *s_list;
static lv_obj_t *s_dot_a, *s_dot_b, *s_dot_menu;

/* ---- row factory -----------------------------------------------------*/

// One callback for all four rows: the destination screen rides in
// user_data (bound at create(), same idiom scr_dial.c uses for the zone a
// widget was built for), so adding a fifth row never means adding a fifth
// near-identical callback.
static void row_event_cb(lv_event_t *e)
{
    dial_haptics_play(HAPTIC_TICK);
    screen_id_t dest = (screen_id_t)(uintptr_t)lv_event_get_user_data(e);
    // The Back row is the one destination that moves BACKWARD along the chain
    // (to the dial the menu was swiped in from), so it gets the reverse
    // transition and the zone arg the dial needs; every other row descends
    // into a sub-screen. Keeping it a row (rather than floating chrome) means
    // it never occludes the list and the knob can reach it like anything else.
    if (dest == SCR_DIAL) {
        ui_router_go(SCR_DIAL, (void *)(uintptr_t)ZONE_B, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
        return;
    }
    ui_router_go(dest, NULL, LV_SCR_LOAD_ANIM_MOVE_LEFT);
}

static lv_obj_t *make_row(lv_obj_t *parent, const char *label_txt, screen_id_t dest)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), ROW_H);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    // Pressed-state color itself is set in the palette pass (day/night can
    // flip while this screen is sitting idle underneath another face) —
    // only the opacity/selector is fixed at create time.
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(row, row_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)dest);

    lv_obj_t *lbl = lv_label_create(row);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
    lv_label_set_text(lbl, label_txt);
    lv_obj_center(lbl);

    return row;
}

/* ---- palette -----------------------------------------------------------*/

// Walks s_list generically (row -> its one label) instead of naming four
// pairs of statics — same "palette walk" scr_settings.c's apply_palette
// uses for its longer row list.
static void apply_palette(void)
{
    const dial_palette_t *pal = PAL();
    lv_obj_t *scr = lv_obj_get_parent(s_ring);
    lv_obj_set_style_bg_color(scr, pal->bg, 0);
    lv_obj_set_style_arc_color(s_ring, pal->track, LV_PART_MAIN);

    uint32_t n = lv_obj_get_child_cnt(s_list);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *row = lv_obj_get_child(s_list, i);
        lv_obj_set_style_bg_color(row, pal->surface, LV_STATE_PRESSED);
        lv_obj_t *lbl = lv_obj_get_child(row, 0);
        lv_obj_set_style_text_color(lbl, pal->ink_primary, 0);
    }

    // Page dots (3): Dial(A) - Dial(B) - Menu — Menu's own dot is always
    // the filled one on this face, same convention scr_tonight used for
    // its own dot before this screen took its slot in the chain.
    lv_obj_set_style_bg_color(s_dot_a, pal->track, 0);
    lv_obj_set_style_bg_color(s_dot_b, pal->track, 0);
    lv_obj_set_style_bg_color(s_dot_menu, pal->ink_secondary, 0);
}

/* ---- vtable ------------------------------------------------------------*/

static void create(lv_obj_t *scr, void *arg)
{
    (void)arg;
    const dial_palette_t *pal = PAL();
    lv_obj_set_style_bg_color(scr, pal->bg, 0);

    // Chassis hairline ring — identical geometry to scr_tonight's/
    // scr_standby's: every face is the same chassis object, just a
    // different face of it (design-spec.md's "a chassis that persists").
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

    s_list = dial_list_create(scr, ROW_H);

    make_row(s_list, LV_SYMBOL_LEFT "  Back", SCR_DIAL);
    make_row(s_list, "Tonight",  SCR_TONIGHT);
    make_row(s_list, "Settings", SCR_SETTINGS);
    make_row(s_list, "Wi-Fi",    SCR_WIFI);
    make_row(s_list, "About",    SCR_ABOUT);

    s_dot_a = lv_obj_create(scr);
    lv_obj_set_size(s_dot_a, 6, 6);
    lv_obj_set_style_radius(s_dot_a, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_dot_a, 0, 0);
    lv_obj_clear_flag(s_dot_a, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_dot_a, LV_ALIGN_CENTER, 164 - CX, 340 - CY);

    s_dot_b = lv_obj_create(scr);
    lv_obj_set_size(s_dot_b, 6, 6);
    lv_obj_set_style_radius(s_dot_b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_dot_b, 0, 0);
    lv_obj_clear_flag(s_dot_b, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_dot_b, LV_ALIGN_CENTER, 180 - CX, 340 - CY);

    s_dot_menu = lv_obj_create(scr);
    lv_obj_set_size(s_dot_menu, 6, 6);
    lv_obj_set_style_radius(s_dot_menu, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_dot_menu, 0, 0);
    lv_obj_clear_flag(s_dot_menu, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_dot_menu, LV_ALIGN_CENTER, 196 - CX, 340 - CY);

    apply_palette();
    dial_list_settle(s_list, 1);   // open on "Tonight", not on Back
}

static void destroy(void)
{
    s_ring = NULL;
    s_list = NULL;
    s_dot_a = s_dot_b = s_dot_menu = NULL;
}

static void on_state(const app_state_t *st)
{
    (void)st;   // nothing on this face is state-driven besides the palette
    if (!s_ring) return;
    apply_palette();
}

// The knob walks the focused row (one per detent); ends of the list voice
// the same range-stop haptic the dial's temperature stops use.
static bool on_knob(int detents)
{
    if (!s_list) return false;
    int r = dial_list_knob(s_list, detents);
    if (r) dial_haptics_play(r > 0 ? HAPTIC_TICK : HAPTIC_STOP);
    return true;
}

static bool on_gesture(lv_dir_t dir)
{
    if (dir != LV_DIR_RIGHT) return false;
    // Menu always follows from Dial(B) in the face chain (scr_dial.c) —
    // ui_zone is already ZONE_B from that swipe, nothing to re-commit here
    // (same reasoning scr_tonight's old exit used before this face took
    // its slot in the chain).
    ui_router_go(SCR_DIAL, (void *)(uintptr_t)ZONE_B, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
    return true;
}

const ui_screen_t scr_menu = {
    .create = create, .destroy = destroy, .on_state = on_state,
    .on_knob = on_knob, .on_gesture = on_gesture,
};
