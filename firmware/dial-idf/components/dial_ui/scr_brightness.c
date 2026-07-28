/*
 * SCR_BRIGHTNESS — full-screen day/night backlight percent picker, reached
 * from SCR_SETTINGS's Day/Night brightness rows. Replaces the old tap-to-
 * edit-in-place on those rows (owner field feedback: it was an unintuitive,
 * one-off micro-pattern) — every other "adjust a value with the knob"
 * control in this UI is a full-screen face with a big number (the
 * temperature dial, scr_boost's duration picker), so brightness now matches
 * that vocabulary: same caption styling, same big numeral font, same
 * display-only rim arc, same detent/zoom-bump/range-stop feel. `arg` packs
 * which row opened it: 0 = day, 1 = night.
 *
 * There is deliberately NO timeout and NO second-tap-to-confirm — leaving
 * the screen IS the commit, from every exit path (tap-anywhere, swipe-down
 * back, or any future caller of ui_router_go while this is current). See
 * destroy()'s comment for the single choke point that guarantees this.
 */
#include "ui_screens_internal.h"
#include "dial_haptics.h"
#include "dial_power.h"

LV_FONT_DECLARE(dial_font_num_88)

#define CX 180
#define CY 180
#define ARC_R 165

#define BRI_MIN_PCT   10
#define BRI_MAX_PCT  100
#define BRI_STEP_PCT  10

static lv_obj_t *s_arc;
static lv_obj_t *s_title_lbl;
static lv_obj_t *s_num_box, *s_num_lbl;
static lv_obj_t *s_unit_lbl;

static bool s_night;
static int  s_pct;

/* ---- motion helpers (scr_boost.c's §6 vocabulary, verbatim) -------------*/

static void set_zoom_cb(void *obj, int32_t v) { lv_obj_set_style_transform_zoom((lv_obj_t *)obj, (int16_t)v, 0); }
static void set_x_cb(void *obj, int32_t v)    { lv_obj_set_x((lv_obj_t *)obj, (lv_coord_t)v); }

static void anim_zoom_bump(lv_obj_t *obj)
{
    lv_anim_del(obj, set_zoom_cb);
    lv_obj_set_style_transform_zoom(obj, 256, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, set_zoom_cb);
    lv_anim_set_values(&a, 256, 266);
    lv_anim_set_time(&a, 45);
    lv_anim_set_playback_time(&a, 45);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void anim_nudge(lv_obj_t *obj, int dir)
{
    lv_anim_del(obj, set_x_cb);
    lv_obj_set_x(obj, 4 * dir);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, set_x_cb);
    lv_anim_set_values(&a, 4 * dir, 0);
    lv_anim_set_time(&a, 140);
    lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
    lv_anim_start(&a);
}

static void render_numeral(int pct)
{
    char t[8];
    snprintf(t, sizeof(t), "%d", pct);
    lv_label_set_text(s_num_lbl, t);
}

/* ---- palette -------------------------------------------------------------*/
// Re-applied from on_state (not just create()) so a night palette swap while
// the picker is open recolors it — screens never cache PAL() past a render.
static void apply_palette(void)
{
    const dial_palette_t *pal = PAL();
    // Brightness has no heat/cool meaning of its own, so the live value uses
    // ink_primary as its accent — the exact token the inline row edit this
    // screen replaces used for the same purpose (scr_settings.c's old
    // apply_palette comment: "without reaching for a thermal/warning/
    // identity token that means something else everywhere else").
    lv_color_t accent = pal->ink_primary;

    lv_obj_t *scr = lv_obj_get_parent(s_arc);
    lv_obj_set_style_bg_color(scr, pal->bg, 0);

    lv_obj_set_style_arc_color(s_arc, pal->track, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(s_arc, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, accent, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(s_arc, LV_OPA_COVER, LV_PART_INDICATOR);

    lv_obj_set_style_text_color(s_title_lbl, pal->ink_secondary, 0);
    lv_obj_set_style_text_color(s_num_lbl, pal->ink_primary, 0);
    lv_obj_set_style_text_color(s_unit_lbl, pal->ink_secondary, 0);
}

/* ---- events ----------------------------------------------------------------*/

// Tap anywhere on the face exits — the arc and numeral box below both clear
// CLICKABLE so the tap reaches this handler on `scr` itself regardless of
// where on the dial it lands (same idiom scr_standby.c's tap-anywhere-wake
// uses). Commit happens in destroy(), not here — see its comment.
static void tap_cb(lv_event_t *e)
{
    (void)e;
    dial_haptics_play(HAPTIC_CONFIRM);
    ui_router_go(SCR_BRIGHTNESS_MENU, NULL, LV_SCR_LOAD_ANIM_NONE);
}

/* ---- vtable ----------------------------------------------------------------*/

static void create(lv_obj_t *scr, void *arg)
{
    uintptr_t packed = (uintptr_t)arg;
    s_night = (packed & 1u) != 0;
    s_pct = s_night ? dial_state_get_bri_night_pct() : dial_state_get_bri_day_pct();

    const dial_palette_t *pal = PAL();
    lv_obj_set_style_bg_color(scr, pal->bg, 0);
    lv_obj_add_event_cb(scr, tap_cb, LV_EVENT_CLICKED, NULL);

    // Chassis ring — same geometry as scr_boost's, display-only (no drag):
    // CLICKABLE stays cleared so both a tap (exit) and a swipe-down
    // (on_gesture, also exit) pass through to `scr` instead of this ring.
    s_arc = lv_arc_create(scr);
    lv_obj_set_size(s_arc, 2 * ARC_R, 2 * ARC_R);
    lv_obj_center(s_arc);
    lv_arc_set_rotation(s_arc, 135);
    lv_arc_set_bg_angles(s_arc, 0, 270);
    lv_arc_set_range(s_arc, BRI_MIN_PCT, BRI_MAX_PCT);
    lv_arc_set_value(s_arc, s_pct);
    lv_obj_set_style_arc_width(s_arc, 16, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 16, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_arc, true, LV_PART_INDICATOR);
    // Kill the default theme's knob dot — a knob is a drag handle, and this
    // arc is display-only (percent rides the encoder, per on_knob). Same
    // suppression every other non-draggable ring in this UI applies.
    lv_obj_set_style_bg_opa(s_arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_clear_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);

    // Caption.
    s_title_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_title_lbl, &lv_font_montserrat_16, 0);
    lv_label_set_text(s_title_lbl, s_night ? "NIGHT BRIGHTNESS" : "DAY BRIGHTNESS");
    // 84, not scr_boost's 64: this caption is nearly twice as wide as
    // "BOOST HEAT", and the arc's inner edge (r=149) leaves only ~159px of
    // chord at y=54 (a 16px-font line's top edge there) — "NIGHT BRIGHTNESS"
    // measures ~155px, so its corners nearly touched the stroke. 20px lower
    // the chord opens to ~209px, restoring a comfortable margin on both
    // sides without crowding the numeral below.
    lv_obj_align(s_title_lbl, LV_ALIGN_CENTER, 0, 84 - CY);

    // Percent numeral — fixed anchor box, same slot as scr_boost's duration.
    s_num_box = lv_obj_create(scr);
    lv_obj_set_size(s_num_box, 210, 92);
    lv_obj_set_style_bg_opa(s_num_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_num_box, 0, 0);
    lv_obj_clear_flag(s_num_box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_num_box, LV_ALIGN_CENTER, 0, 150 - CY);

    s_num_lbl = lv_label_create(s_num_box);
    lv_obj_set_style_text_font(s_num_lbl, &dial_font_num_88, 0);
    lv_obj_set_style_transform_pivot_x(s_num_lbl, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(s_num_lbl, LV_PCT(50), 0);
    lv_obj_center(s_num_lbl);
    render_numeral(s_pct);

    // Unit, below the numeral.
    s_unit_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_unit_lbl, &lv_font_montserrat_20, 0);
    lv_label_set_text(s_unit_lbl, "%");
    lv_obj_align(s_unit_lbl, LV_ALIGN_CENTER, 0, 214 - CY);

    apply_palette();

    // Live preview from the moment the screen opens, not just on the first
    // knob turn — the backlight should already be showing the row's current
    // value the instant this face is on screen (task: "on entry AND on
    // every change... that's the point").
    dial_power_preview(s_night, (uint8_t)s_pct);
}

// The ONE commit path for every exit: tap_cb, on_gesture, or any other
// future caller of ui_router_go while this screen is current all funnel
// through here, unconditionally — ui_router.c:82 calls the OUTGOING
// screen's destroy() before the incoming screen's create() runs, so this
// fires exactly once per visit no matter which exit fired. That's why there
// is no separate "confirm" step anywhere above: leaving IS committing.
static void destroy(void)
{
    if (s_num_lbl) lv_anim_del(s_num_lbl, NULL);
    if (s_num_box) lv_anim_del(s_num_box, NULL);
    if (s_arc)     lv_anim_del(s_arc, NULL);

    dial_power_preview_end();
    if (s_night) dial_state_set_bri_night_pct((uint8_t)s_pct);
    else         dial_state_set_bri_day_pct((uint8_t)s_pct);
    dial_power_brightness_changed();

    s_arc = s_title_lbl = s_num_box = s_num_lbl = s_unit_lbl = NULL;
}

static void on_state(const app_state_t *st)
{
    (void)st;
    if (!s_arc) return;
    apply_palette();
}

static bool on_knob(int detents)
{
    if (!s_arc) return false;
    int np = s_pct + detents * BRI_STEP_PCT;
    if (np < BRI_MIN_PCT) np = BRI_MIN_PCT;
    if (np > BRI_MAX_PCT) np = BRI_MAX_PCT;
    if (np == s_pct) {                                // at the range stop
        dial_haptics_play(HAPTIC_STOP);
        int dir = detents > 0 ? 1 : -1;
        anim_nudge(s_num_box, dir);
        anim_nudge(s_arc, dir);
        return true;
    }

    s_pct = np;
    lv_arc_set_value(s_arc, np);
    render_numeral(np);
    anim_zoom_bump(s_num_lbl);
    dial_power_preview(s_night, (uint8_t)s_pct);   // live: the backlight follows the knob
    dial_haptics_play(HAPTIC_TICK);
    return true;
}

static bool on_gesture(lv_dir_t dir)
{
    if (dir != LV_DIR_BOTTOM) return false;
    ui_router_go(SCR_BRIGHTNESS_MENU, NULL, LV_SCR_LOAD_ANIM_NONE);
    return true;
}

const ui_screen_t scr_brightness = {
    .create = create, .destroy = destroy, .on_state = on_state,
    .on_knob = on_knob, .on_gesture = on_gesture,
};
