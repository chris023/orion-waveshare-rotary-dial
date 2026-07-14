/*
 * SCR_BOOST — thermal-relief ("boost") duration picker. Reached from
 * SCR_QUICK's "Boost heat"/"Boost cool" rows; arg packs the zone the sheet
 * was opened from together with the heat/cool choice: `(zone<<1)|heat`.
 *
 * The knob steps the duration (+-5min, clamped 5..120, default 30) using the
 * same detent/zoom-bump/range-stop vocabulary as scr_dial's temperature arc;
 * the arc here is display-only (duration/120), not draggable, so a swipe
 * down (cancel) isn't fighting an arc drag for the touch.
 */
#include "ui_screens_internal.h"
#include "dial_haptics.h"

LV_FONT_DECLARE(dial_font_num_88)

#define CX 180
#define CY 180
#define ARC_R 165

#define BOOST_MIN_MIN     5
#define BOOST_MAX_MIN   120
#define BOOST_STEP_MIN    5
#define BOOST_DEFAULT_MIN 30

static lv_obj_t *s_arc;
static lv_obj_t *s_title_lbl;
static lv_obj_t *s_num_box, *s_num_lbl;
static lv_obj_t *s_unit_lbl;
static lv_obj_t *s_start_btn, *s_start_glyph;

static zone_idx_t s_zone = ZONE_A;
static bool       s_heat;
static int        s_minutes = BOOST_DEFAULT_MIN;

/* ---- motion helpers (same vocabulary as scr_dial.c's §6 helpers) -------- */

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

static void render_numeral(int minutes)
{
    char t[8];
    snprintf(t, sizeof(t), "%d", minutes);
    lv_label_set_text(s_num_lbl, t);
}

/* ---- palette -------------------------------------------------------------*/
// Re-applied from on_state (not just create()) so a night palette swap while
// the picker is open recolors it — screens never cache PAL() past a render.
static void apply_palette(void)
{
    const dial_palette_t *pal = PAL();
    lv_color_t accent = s_heat ? pal->accent_heat : pal->accent_cool;

    lv_obj_t *scr = lv_obj_get_parent(s_arc);
    lv_obj_set_style_bg_color(scr, pal->bg, 0);

    lv_obj_set_style_arc_color(s_arc, pal->track, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(s_arc, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, accent, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(s_arc, LV_OPA_COVER, LV_PART_INDICATOR);

    lv_obj_set_style_text_color(s_title_lbl, pal->ink_secondary, 0);
    lv_obj_set_style_text_color(s_num_lbl, pal->ink_primary, 0);
    lv_obj_set_style_text_color(s_unit_lbl, pal->ink_secondary, 0);

    lv_obj_set_style_bg_color(s_start_btn, pal->surface, 0);
    lv_obj_set_style_border_color(s_start_btn, accent, 0);
    lv_obj_set_style_text_color(s_start_glyph, pal->ink_primary, 0);
}

/* ---- events ----------------------------------------------------------------*/

static void start_event_cb(lv_event_t *e)
{
    (void)e;
    dial_haptics_play(HAPTIC_CONFIRM);
    app_cmd_t cmd = { .kind = CMD_BOOST_START, .zone = s_zone,
                      .a = s_heat ? 1 : 0, .b = s_minutes };
    dial_cmd_post(&cmd);
    ui_router_go(SCR_DIAL, (void *)(uintptr_t)s_zone, LV_SCR_LOAD_ANIM_NONE);
}

/* ---- vtable ----------------------------------------------------------------*/

static void create(lv_obj_t *scr, void *arg)
{
    uintptr_t packed = (uintptr_t)arg;
    s_heat    = (packed & 1u) != 0;
    s_zone    = (zone_idx_t)(packed >> 1);
    s_minutes = BOOST_DEFAULT_MIN;

    const dial_palette_t *pal = PAL();
    lv_obj_set_style_bg_color(scr, pal->bg, 0);

    // Chassis ring — same geometry as scr_dial's, but display-only here (no
    // drag): CLICKABLE stays cleared so a swipe-down (cancel) passes through
    // to the screen instead of starting an arc drag.
    s_arc = lv_arc_create(scr);
    lv_obj_set_size(s_arc, 2 * ARC_R, 2 * ARC_R);
    lv_obj_center(s_arc);
    lv_arc_set_rotation(s_arc, 135);
    lv_arc_set_bg_angles(s_arc, 0, 270);
    lv_arc_set_range(s_arc, BOOST_MIN_MIN, BOOST_MAX_MIN);
    lv_arc_set_value(s_arc, s_minutes);
    lv_obj_set_style_arc_width(s_arc, 16, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 16, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_arc, true, LV_PART_INDICATOR);
    lv_obj_clear_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);

    // Title.
    s_title_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_title_lbl, &lv_font_montserrat_16, 0);
    lv_label_set_text(s_title_lbl, s_heat ? "BOOST HEAT" : "BOOST COOL");
    lv_obj_align(s_title_lbl, LV_ALIGN_CENTER, 0, 64 - CY);

    // Duration numeral — fixed anchor box, same slot as scr_dial's setpoint.
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
    render_numeral(s_minutes);

    // Unit, below the numeral.
    s_unit_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_unit_lbl, &lv_font_montserrat_20, 0);
    lv_label_set_text(s_unit_lbl, "MIN");
    lv_obj_align(s_unit_lbl, LV_ALIGN_CENTER, 0, 214 - CY);

    // Start disc.
    s_start_btn = dial_btn_create(scr);
    lv_obj_set_size(s_start_btn, 88, 88);
    lv_obj_set_style_radius(s_start_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_start_btn, 2, 0);
    lv_obj_align(s_start_btn, LV_ALIGN_CENTER, 0, 280 - CY);
    lv_obj_add_event_cb(s_start_btn, start_event_cb, LV_EVENT_CLICKED, NULL);
    s_start_glyph = lv_label_create(s_start_btn);
    lv_obj_set_style_text_font(s_start_glyph, &lv_font_montserrat_28, 0);
    lv_label_set_text(s_start_glyph, LV_SYMBOL_PLAY);
    lv_obj_center(s_start_glyph);

    apply_palette();
}

static void destroy(void)
{
    if (s_num_lbl) lv_anim_del(s_num_lbl, NULL);
    if (s_num_box) lv_anim_del(s_num_box, NULL);
    if (s_arc)     lv_anim_del(s_arc, NULL);

    s_arc = s_title_lbl = s_num_box = s_num_lbl = s_unit_lbl = NULL;
    s_start_btn = s_start_glyph = NULL;
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
    int nm = s_minutes + detents * BOOST_STEP_MIN;
    if (nm < BOOST_MIN_MIN) nm = BOOST_MIN_MIN;
    if (nm > BOOST_MAX_MIN) nm = BOOST_MAX_MIN;
    if (nm == s_minutes) {                           // at the range stop
        dial_haptics_play(HAPTIC_STOP);
        int dir = detents > 0 ? 1 : -1;
        anim_nudge(s_num_box, dir);
        anim_nudge(s_arc, dir);
        return true;
    }

    s_minutes = nm;
    lv_arc_set_value(s_arc, nm);
    render_numeral(nm);
    anim_zoom_bump(s_num_lbl);
    return true;
}

static bool on_gesture(lv_dir_t dir)
{
    if (dir != LV_DIR_BOTTOM) return false;
    ui_router_go(SCR_DIAL, (void *)(uintptr_t)s_zone, LV_SCR_LOAD_ANIM_NONE);
    return true;
}

const ui_screen_t scr_boost = {
    .create = create, .destroy = destroy, .on_state = on_state,
    .on_knob = on_knob, .on_gesture = on_gesture,
};
