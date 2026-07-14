/*
 * SCR_DIAL — the temperature dial (design-spec.md §4, "HOME FACE").
 * Arc drag + knob detents set the target °F; tap the power disc to toggle;
 * swipe left/right to show the other side. All input posts commands to the
 * worker queue and renders optimistically; the poll reconciles later.
 *
 * The chassis ring (arc), fixed numeral anchor, and state-token colors are
 * shared vocabulary with scr_standby (same r=165 ring, same palette) — see
 * dial_palette.h and ui_screens_internal.h's dial_zone_kind/dial_zone_accent.
 */
#include "ui_screens_internal.h"
#include "dial_haptics.h"
#include <math.h>
#include <time.h>

LV_FONT_DECLARE(dial_font_num_88)

#define CX 180   // screen center — every position in design-spec.md §4 is
#define CY 180   // given as an absolute (x,y); we align everything to the
                 // screen's center and offset by (x-CX, y-CY).
#define ARC_R 165

static lv_obj_t *s_arc;
static lv_obj_t *s_stale_dot;

/*
 * Water level, drawn as a VALUE STEP rather than a texture: no stripes, no
 * second material — the arc says where the water is purely by changing the
 * value of what's already there. One overlay arc, two configurations, and the
 * boundary between them IS the water level:
 *
 *   HEATING  water below the setpoint, so the water sits UNDER the accent fill.
 *            That stretch of fill is deepened (a `bg` wash over it), leaving the
 *            rest of the fill bright: dark = water that's really there, bright =
 *            the heat still to add.
 *   COOLING  water above the setpoint, so it overshoots the fill. The overshoot
 *            is the accent at low opacity over the bare track: a ghost of the
 *            fill, which is exactly what it is — heat that has to leave.
 *
 * There is nothing to alias and nothing to animate, and the water never becomes
 * a third color to decode: it's the same accent, one step down in value.
 *
 * The under-fill stretch is not deepened when cooling — the water level there is
 * already above the setpoint by definition, so the only thing worth marking is
 * where it overshoots to.
 *
 * Recomputed only when a poll moves the water or the user moves the target.
 */
#define LEVEL_OPA_UNDER  77     // ~30%: `bg` wash deepening the fill  (heating)
#define LEVEL_OPA_OVER   82     // ~32%: accent ghost over the track   (cooling)

static lv_obj_t  *s_level;              // the value-step overlay arc
static lv_color_t s_level_accent;       // cached: the drag path has no state snapshot
static float      s_actual_f = -1.0f;   // measured water temp, °F; <0 = unknown

static lv_obj_t *s_name_lbl;
static lv_obj_t *s_underline_solid, *s_underline_dash;
static lv_obj_t *s_water_lbl;
static lv_obj_t *s_num_box, *s_temp_lbl;
static lv_obj_t *s_unit_lbl;
static lv_obj_t *s_pill, *s_pill_glyph, *s_pill_word;
static lv_obj_t *s_power_btn, *s_power_glyph;
static lv_obj_t *s_dot_a, *s_dot_b, *s_dot_menu;
static lv_obj_t *s_away_lbl;
static lv_point_t s_dash_pts[2];

static zone_idx_t s_zone = ZONE_A;

// The setpoint currently shown (optimistic); -1 until first state arrives.
static int s_shown_f = -1;

// Display-units cache (design-spec.md's units toggle, M4): updated from
// apply_palette_and_state on every on_state, read by render_numeral — which
// is also called mid-drag/mid-detent, where only the raw °F value is at
// hand. The store, arc range, and knob detent size all stay °F regardless.
static bool s_units_c;

// Chevron pulse (design-spec.md §6): only running while heating/cooling, and
// only restarted when that changes or the day/night duration changes.
static bool s_chevron_active;
static bool s_chevron_night;

// Only fade the staleness dot on an actual transition, not every on_state.
static bool s_stale_shown;

// Boost ("thermal relief") countdown: a 1s ticker that only exists while the
// shown zone's relief is active — created lazily on the transition into
// relief, deleted on the transition out (and on destroy()).
static lv_timer_t *s_boost_timer;

/* ---- motion helpers (design-spec.md §6) -------------------------------- */

static void set_zoom_cb(void *obj, int32_t v) { lv_obj_set_style_transform_zoom((lv_obj_t *)obj, (int16_t)v, 0); }
static void set_x_cb(void *obj, int32_t v)    { lv_obj_set_x((lv_obj_t *)obj, (lv_coord_t)v); }
static void set_opa_cb(void *obj, int32_t v)  { lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0); }

// Detent zoom bump: 256->266->256 over 90ms on the numeral only.
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

// Range-stop nudge: instant 4px kick in the blocked direction, one-overshoot
// spring back over 140ms (numeral + indicator, design-spec.md §6).
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

// Generic opacity fade to an arbitrary target (the staleness dot's
// night-quiet level, M4) — like lv_obj_fade_in/out but not hardcoded to
// LV_OPA_COVER, reusing set_opa_cb above.
static void anim_opa(lv_obj_t *obj, lv_opa_t from, lv_opa_t to, uint32_t time_ms)
{
    lv_anim_del(obj, set_opa_cb);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, set_opa_cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, time_ms);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_start(&a);
}

// State chevron pulse: pill glyph opa 60%<->100%, 1.2s day / 2.4s night,
// ping-pong, infinite — only while heating/cooling (design-spec.md §6).
static void chevron_start(bool night)
{
    lv_anim_del(s_pill_glyph, set_opa_cb);
    uint32_t half = night ? 2400 : 1200;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_pill_glyph);
    lv_anim_set_exec_cb(&a, set_opa_cb);
    lv_anim_set_values(&a, LV_OPA_60, LV_OPA_100);
    lv_anim_set_time(&a, half);
    lv_anim_set_playback_time(&a, half);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

static void chevron_stop(void)
{
    lv_anim_del(s_pill_glyph, set_opa_cb);
    lv_obj_set_style_opa(s_pill_glyph, LV_OPA_100, 0);
}

/* ---- water level: value step over the arc ------------------------------ */

// Temperature -> degrees into the arc's own 0..270 sweep (rotation adds 135).
// Same mapping the setpoint indicator uses, so the level and the fill land on
// one scale.
static uint16_t level_angle(float f)
{
    if (f < DIAL_TEMP_MIN_F) f = DIAL_TEMP_MIN_F;
    if (f > DIAL_TEMP_MAX_F) f = DIAL_TEMP_MAX_F;
    float d = 270.0f * (f - DIAL_TEMP_MIN_F) / (float)(DIAL_TEMP_MAX_F - DIAL_TEMP_MIN_F);
    return (uint16_t)lroundf(d);
}

static void level_render(int target_f)
{
    if (!s_level) return;
    if (s_actual_f < 0) {                       // no measurement yet
        lv_obj_add_flag(s_level, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    uint16_t a_water = level_angle(s_actual_f);
    uint16_t a_set   = level_angle((float)target_f);
    bool cooling = a_water > a_set;

    uint16_t a0 = cooling ? a_set : 0;
    uint16_t a1 = a_water;
    if (a1 <= a0 + 1) {                         // at target: no step to draw
        lv_obj_add_flag(s_level, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_obj_set_style_arc_color(s_level, cooling ? s_level_accent : PAL()->bg, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(s_level, cooling ? LEVEL_OPA_OVER : LEVEL_OPA_UNDER, LV_PART_INDICATOR);
    lv_arc_set_angles(s_level, a0, a1);
    lv_obj_clear_flag(s_level, LV_OBJ_FLAG_HIDDEN);
}

/* ---- identity underline (design-spec.md §4 #6, §8) --------------------- */
// ZONE_A is "yours" (always solid); ZONE_B is the partner (solid pewter by
// day, dashed warm-hue by night — the hue->pattern substitution rule).
static void apply_identity(const dial_palette_t *pal, bool night)
{
    if (s_zone == ZONE_A) {
        lv_obj_set_style_bg_color(s_underline_solid, pal->identity_home, 0);
        lv_obj_clear_flag(s_underline_solid, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_underline_dash, LV_OBJ_FLAG_HIDDEN);
    } else if (night) {
        lv_obj_set_style_line_color(s_underline_dash, pal->identity_partner, 0);
        lv_obj_clear_flag(s_underline_dash, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_underline_solid, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_set_style_bg_color(s_underline_solid, pal->identity_partner, 0);
        lv_obj_clear_flag(s_underline_solid, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_underline_dash, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---- palette + state application --------------------------------------- */
// Re-applied from on_state every render (cheap): both device telemetry and a
// palette swap (day/night) land here, so neither needs its own code path.
static void apply_palette_and_state(const app_state_t *st)
{
    const dial_palette_t *pal = PAL();
    bool night = dial_palette_is_night();
    s_units_c = st->units_c;
    const zone_state_t *z = &st->zones[s_zone];
    zone_kind_t kind = dial_zone_kind(z, st->device_online);
    lv_color_t accent = dial_zone_accent(kind, pal);

    // Thermal relief ("boost") in progress: the arc + pill switch to the
    // relief's own heat/cool accent and go full-strength, regardless of what
    // kind/opacity the ordinary state grammar would otherwise pick — the
    // power disc below stays kind-based (boost doesn't change on/off meaning).
    bool relief = z->relief_active;
    lv_color_t relief_accent = z->relief_heat ? pal->accent_heat : pal->accent_cool;
    lv_color_t arc_accent = relief ? relief_accent : accent;

    lv_obj_t *scr = lv_obj_get_parent(s_arc);   // s_arc's parent is the screen
    lv_obj_set_style_bg_color(scr, pal->bg, 0);

    // Arc track + indicator (design-spec.md §4 #2-3).
    lv_obj_set_style_arc_color(s_arc, pal->track, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(s_arc, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, arc_accent, LV_PART_INDICATOR);
    lv_opa_t indic_opa = relief ? LV_OPA_COVER
                        : (kind == ZK_OFFLINE) ? LV_OPA_TRANSP
                        : (kind == ZK_STANDBY) ? LV_OPA_30 : LV_OPA_COVER;
    lv_obj_set_style_arc_opa(s_arc, indic_opa, LV_PART_INDICATOR);

    // Water level. Cached in °F (and the accent with it) so a drag or a detent
    // can re-draw the step without waiting for the next poll — the water doesn't
    // move while you turn the knob, but the setpoint does, and that's what
    // decides which side of it the water is on.
    s_actual_f = (z->actual_c >= 0) ? (float)dial_c_to_f(z->actual_c) : -1.0f;
    s_level_accent = arc_accent;
    level_render(s_shown_f);

    // Side name + identity underline.
    lv_obj_set_style_text_color(s_name_lbl, pal->ink_secondary, 0);
    apply_identity(pal, night);

    // Water caption — display-only °C conversion when units_c (M4); the
    // store keeps actual_c as-is either way.
    lv_obj_set_style_text_color(s_water_lbl, pal->ink_secondary, 0);
    lv_obj_set_style_text_opa(s_water_lbl, LV_OPA_80, 0);
    char water[16];
    if (z->actual_c < 0) snprintf(water, sizeof(water), "WATER --");
    else if (st->units_c) snprintf(water, sizeof(water), "WATER %.1f\xC2\xB0", z->actual_c);
    else snprintf(water, sizeof(water), "WATER %d\xC2\xB0", dial_c_to_f(z->actual_c));
    lv_label_set_text(s_water_lbl, water);

    // Setpoint numeral — ink_primary always (never state-tinted); dimmed only
    // for OFFLINE (design-spec.md's "numeral whose color never lies").
    lv_obj_set_style_text_color(s_temp_lbl, pal->ink_primary, 0);
    lv_obj_set_style_text_opa(s_temp_lbl, (kind == ZK_OFFLINE) ? 115 : LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_unit_lbl, pal->ink_secondary, 0);
    lv_label_set_text(s_unit_lbl, st->units_c ? "\xC2\xB0" "C" : "\xC2\xB0" "F");

    // State pill: surface fill, state-accent border/text/glyph. While relief
    // is active it takes over the pill entirely (glyph + countdown word),
    // pre-empting the normal glyph/word grammar below.
    lv_obj_set_style_bg_color(s_pill, pal->surface, 0);
    lv_obj_set_style_border_color(s_pill, relief ? relief_accent : accent, 0);
    lv_obj_set_style_text_color(s_pill_glyph, relief ? relief_accent : accent, 0);
    lv_obj_set_style_text_color(s_pill_word, relief ? relief_accent : accent, 0);
    const char *glyph;
    char word[24];
    if (relief) {
        glyph = LV_SYMBOL_CHARGE;
        if (st->clock_valid) {
            int64_t now_ms = (int64_t)time(NULL) * 1000;
            int64_t remain_ms = z->relief_end_ms - now_ms;
            if (remain_ms < 0) remain_ms = 0;
            int total_s = (int)(remain_ms / 1000);
            snprintf(word, sizeof(word), "BOOST %d:%02d", total_s / 60, total_s % 60);
        } else {
            snprintf(word, sizeof(word), "BOOST");
        }
    } else {
        const char *w;
        switch (kind) {
        case ZK_OFFLINE: glyph = LV_SYMBOL_CLOSE; w = "OFFLINE"; break;
        case ZK_STANDBY: glyph = LV_SYMBOL_STOP;  w = "STANDBY"; break;
        case ZK_HEATING: glyph = LV_SYMBOL_UP;    w = "HEATING"; break;
        case ZK_COOLING: glyph = LV_SYMBOL_DOWN;  w = "COOLING"; break;
        default:         glyph = LV_SYMBOL_MINUS; w = "HOLDING"; break;
        }
        strlcpy(word, w, sizeof(word));
    }
    lv_label_set_text(s_pill_glyph, glyph);
    lv_label_set_text(s_pill_word, word);

    // The pill is only a tap target while relief is active (tap-to-cancel).
    if (relief) lv_obj_add_flag(s_pill, LV_OBJ_FLAG_CLICKABLE);
    else        lv_obj_clear_flag(s_pill, LV_OBJ_FLAG_CLICKABLE);

    // Chevron pulse tracks the underlying thermal kind, not relief — a boost
    // still pulses (on the charge glyph) while the zone is actively driving
    // toward the relief extreme, and goes static once it's holding there.
    bool pulsing = (kind == ZK_HEATING || kind == ZK_COOLING);
    if (pulsing && (!s_chevron_active || night != s_chevron_night)) chevron_start(night);
    else if (!pulsing && s_chevron_active) chevron_stop();
    s_chevron_active = pulsing;
    s_chevron_night = night;

    // Power disc.
    lv_obj_set_style_bg_color(s_power_btn, pal->surface, 0);
    lv_obj_set_style_border_color(s_power_btn, z->on ? accent : pal->track, 0);
    lv_obj_set_style_text_color(s_power_glyph, z->on ? pal->ink_primary : pal->ink_secondary, 0);

    // Staleness dot. Night-quiet errors (design-spec.md's "silent staleness
    // at night"): shown at 40% instead of full opacity after dark, so a
    // routine offline blip doesn't glow at 3am.
    lv_obj_set_style_bg_color(s_stale_dot, pal->stale, 0);
    lv_opa_t stale_target = night ? LV_OPA_40 : LV_OPA_COVER;
    bool stale = (st->phase != PH_READY) || !st->device_online;
    if (stale != s_stale_shown) {
        s_stale_shown = stale;
        if (stale) anim_opa(s_stale_dot, LV_OPA_TRANSP, stale_target, 300);
        else       anim_opa(s_stale_dot, lv_obj_get_style_opa(s_stale_dot, 0), LV_OPA_TRANSP, 300);
    } else if (stale) {
        // Still stale, no transition this render — keep the level in sync
        // with a day/night flip (e.g. the worker's dusk palette swap) even
        // without a fresh fade.
        lv_anim_del(s_stale_dot, set_opa_cb);
        lv_obj_set_style_opa(s_stale_dot, stale_target, 0);
    }

    // Page dots — one per face, in the order the chain walks them (see
    // on_gesture): Dial(B) - Dial(A) - Menu on a dual topper, and just
    // Dial - Menu on a single-zone one, re-centered so the pair sits
    // symmetric. Filled = the face this screen instance is showing; scr_dial
    // is never the menu face, so that dot always tracks.
    dial_dots_layout(st, s_dot_b, s_dot_a, s_dot_menu);
    lv_obj_set_style_bg_color(s_dot_b, s_zone == ZONE_B ? pal->ink_secondary : pal->track, 0);
    lv_obj_set_style_bg_color(s_dot_a, s_zone == ZONE_A ? pal->ink_secondary : pal->track, 0);
    lv_obj_set_style_bg_color(s_dot_menu, pal->track, 0);

    // Away badge (design-spec.md §7 extension) — session-optimistic, tiny.
    lv_obj_set_style_text_color(s_away_lbl, pal->ink_secondary, 0);
    if (st->away) lv_obj_clear_flag(s_away_lbl, LV_OBJ_FLAG_HIDDEN);
    else          lv_obj_add_flag(s_away_lbl, LV_OBJ_FLAG_HIDDEN);
}

// Display-only °C conversion when s_units_c (M4): one decimal, dial_f_to_c's
// natural granularity. The value passed in is always °F — the internal
// representation, arc range, and knob detent never change.
static void render_numeral(int temp_f)
{
    char t[8];
    if (s_units_c) snprintf(t, sizeof(t), "%.1f", dial_f_to_c(temp_f));
    else           snprintf(t, sizeof(t), "%d", temp_f);
    lv_label_set_text(s_temp_lbl, t);
}

static void post_temp_for(zone_idx_t zone, int temp_f)
{
    if (zone == s_zone) s_shown_f = temp_f;
    dial_state_set_ui_temp(zone, temp_f);
    app_cmd_t cmd = { .kind = CMD_SET_TEMP, .zone = zone, .temp_f = temp_f };
    dial_cmd_post(&cmd);
}

static void post_temp(int temp_f) { post_temp_for(s_zone, temp_f); }

// LVGL event callbacks already hold the LVGL mutex — no locking here.
// The zone rides in user_data, bound at create(): a release event delivered
// after a swipe already flipped s_zone must command the zone the widget was
// built for, not whichever side the screen shows now.
static void arc_event_cb(lv_event_t *e)
{
    dial_state_stamp_input();
    lv_obj_t *arc = lv_event_get_target(e);
    int f = lv_arc_get_value(arc);
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        // Live feedback while dragging. The stripes move too: dragging the
        // target past the water level flips heating<->cooling, which changes
        // both where they start and what color they are.
        s_shown_f = f;
        if (s_temp_lbl) render_numeral(f);
        level_render(f);
    } else {                                          // LV_EVENT_RELEASED
        post_temp_for((zone_idx_t)(uintptr_t)lv_event_get_user_data(e), f);
    }
}

static void power_event_cb(lv_event_t *e)
{
    dial_haptics_play(HAPTIC_CONFIRM);
    app_cmd_t cmd = { .kind = CMD_TOGGLE_ON,
                      .zone = (zone_idx_t)(uintptr_t)lv_event_get_user_data(e) };
    dial_cmd_post(&cmd);
}

// Tap the state pill while relief is active (only then is it clickable —
// see apply_palette_and_state) to cancel the boost early.
static void pill_event_cb(lv_event_t *e)
{
    (void)e;
    dial_haptics_play(HAPTIC_CONFIRM);
    app_cmd_t cmd = { .kind = CMD_BOOST_CANCEL };
    dial_cmd_post(&cmd);
}

// Long-press anywhere the screen itself receives the press (numeral, labels,
// background — the arc and power disc are clickable and keep their own press
// handling, design-spec.md §4's occlusion note) opens quick-actions.
static void screen_long_press_cb(lv_event_t *e)
{
    (void)e;
    dial_haptics_play(HAPTIC_TICK);
    // The finger is still down and the quick sheet is about to slide up under
    // it; without this, LVGL re-hit-tests the held touch onto whichever row
    // lands beneath it and fires a phantom CLICKED on release (bed off, away,
    // ...). wait_release makes LVGL ignore this touch until the finger lifts.
    lv_indev_wait_release(lv_indev_get_act());
    ui_router_go(SCR_QUICK, (void *)(uintptr_t)s_zone, LV_SCR_LOAD_ANIM_NONE);
}

static void create(lv_obj_t *scr, void *arg)
{
    s_zone = (zone_idx_t)(uintptr_t)arg;
    s_shown_f = -1;
    s_chevron_active = false;
    s_stale_shown = false;
    s_units_c = false;   // on_state (called right after create) sets the real value
    const dial_palette_t *pal = PAL();
    lv_obj_set_style_bg_color(scr, pal->bg, 0);
    lv_obj_add_event_cb(scr, screen_long_press_cb, LV_EVENT_LONG_PRESSED, NULL);

    // #2-3 Chassis ring / arc (r=165, w=16, 90deg gap at 6 o'clock).
    s_arc = lv_arc_create(scr);
    lv_obj_set_size(s_arc, 2 * ARC_R, 2 * ARC_R);
    lv_obj_center(s_arc);
    lv_arc_set_rotation(s_arc, 135);
    lv_arc_set_bg_angles(s_arc, 0, 270);
    lv_arc_set_range(s_arc, DIAL_TEMP_MIN_F, DIAL_TEMP_MAX_F);
    lv_arc_set_value(s_arc, (DIAL_TEMP_MIN_F + DIAL_TEMP_MAX_F) / 2);
    lv_obj_set_style_arc_width(s_arc, 16, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 16, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_arc, LV_OPA_TRANSP, LV_PART_KNOB);   // draggable, invisible
    lv_obj_add_event_cb(s_arc, arc_event_cb, LV_EVENT_VALUE_CHANGED, (void *)(uintptr_t)s_zone);
    lv_obj_add_event_cb(s_arc, arc_event_cb, LV_EVENT_RELEASED, (void *)(uintptr_t)s_zone);

    // #4 Water level: a value-step overlay sharing the arc's exact geometry,
    // created after it so it lands on top of the fill. Rounded caps to match
    // every other arc on the face — a square end read as a different material
    // sitting on the ring rather than as part of it.
    s_level = lv_arc_create(scr);
    lv_obj_set_size(s_level, 2 * ARC_R, 2 * ARC_R);
    lv_obj_center(s_level);
    lv_arc_set_rotation(s_level, 135);
    lv_arc_set_bg_angles(s_level, 0, 270);
    lv_arc_set_angles(s_level, 0, 0);
    lv_obj_set_style_arc_opa(s_level, LV_OPA_TRANSP, LV_PART_MAIN);   // no second track
    lv_obj_set_style_arc_width(s_level, 16, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_level, true, LV_PART_INDICATOR);
    lv_obj_remove_style(s_level, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(s_level, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_level, LV_OBJ_FLAG_HIDDEN);   // until the first measurement

    // #5 Side name.
    s_name_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_name_lbl, &lv_font_montserrat_16, 0);
    lv_label_set_text(s_name_lbl, s_zone == ZONE_A ? "RIGHT SIDE" : "LEFT SIDE");
    lv_obj_align(s_name_lbl, LV_ALIGN_CENTER, 0, 64 - CY);

    // #6 Identity underline (solid bar + dashed line variant, one shown).
    s_underline_solid = lv_obj_create(scr);
    lv_obj_set_size(s_underline_solid, 60, 3);
    lv_obj_set_style_radius(s_underline_solid, 0, 0);
    lv_obj_set_style_border_width(s_underline_solid, 0, 0);
    lv_obj_clear_flag(s_underline_solid, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_underline_solid, LV_ALIGN_CENTER, 0, 82 - CY);

    s_dash_pts[0] = (lv_point_t){ 0, 0 };
    s_dash_pts[1] = (lv_point_t){ 60, 0 };
    s_underline_dash = lv_line_create(scr);
    lv_line_set_points(s_underline_dash, s_dash_pts, 2);
    lv_obj_set_style_line_width(s_underline_dash, 3, 0);
    lv_obj_set_style_line_dash_width(s_underline_dash, 6, 0);
    lv_obj_set_style_line_dash_gap(s_underline_dash, 4, 0);
    lv_obj_set_style_line_rounded(s_underline_dash, false, 0);
    lv_obj_clear_flag(s_underline_dash, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(s_underline_dash, LV_ALIGN_CENTER, 0, 82 - CY);
    lv_obj_add_flag(s_underline_dash, LV_OBJ_FLAG_HIDDEN);

    // #7 Water caption.
    s_water_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_water_lbl, &lv_font_montserrat_16, 0);
    lv_label_set_text(s_water_lbl, "WATER --");
    lv_obj_align(s_water_lbl, LV_ALIGN_CENTER, 0, 98 - CY);

    // #8 Setpoint numeral — fixed anchor box so digits never reflow.
    s_num_box = lv_obj_create(scr);
    lv_obj_set_size(s_num_box, 210, 92);
    lv_obj_set_style_bg_opa(s_num_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_num_box, 0, 0);
    lv_obj_clear_flag(s_num_box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_num_box, LV_ALIGN_CENTER, 0, 150 - CY);

    s_temp_lbl = lv_label_create(s_num_box);
    lv_obj_set_style_text_font(s_temp_lbl, &dial_font_num_88, 0);
    lv_label_set_text(s_temp_lbl, "--");
    lv_obj_set_style_transform_pivot_x(s_temp_lbl, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(s_temp_lbl, LV_PCT(50), 0);
    lv_obj_center(s_temp_lbl);

    // #9 Unit.
    s_unit_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_unit_lbl, &lv_font_montserrat_20, 0);
    lv_label_set_text(s_unit_lbl, "\xC2\xB0" "F");
    lv_obj_align(s_unit_lbl, LV_ALIGN_CENTER, 266 - CX, 122 - CY);

    // #10 State pill.
    s_pill = lv_obj_create(scr);
    lv_obj_set_height(s_pill, 26);
    lv_obj_set_width(s_pill, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(s_pill, 160, 0);
    lv_obj_set_style_radius(s_pill, 13, 0);
    lv_obj_set_style_border_width(s_pill, 1, 0);
    lv_obj_set_style_pad_left(s_pill, 12, 0);
    lv_obj_set_style_pad_right(s_pill, 12, 0);
    lv_obj_set_style_pad_column(s_pill, 6, 0);
    lv_obj_set_flex_flow(s_pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(s_pill, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_pill, LV_ALIGN_CENTER, 0, 214 - CY);
    // CLICKABLE is added/removed per-render (apply_palette_and_state): the
    // pill is only a tap target while relief is active (tap to cancel boost).
    lv_obj_add_event_cb(s_pill, pill_event_cb, LV_EVENT_CLICKED, NULL);

    s_pill_glyph = lv_label_create(s_pill);
    lv_obj_set_style_text_font(s_pill_glyph, &lv_font_montserrat_16, 0);
    lv_label_set_text(s_pill_glyph, LV_SYMBOL_MINUS);

    s_pill_word = lv_label_create(s_pill);
    lv_obj_set_style_text_font(s_pill_word, &lv_font_montserrat_16, 0);
    lv_label_set_text(s_pill_word, "--");

    // #11 Power disc.
    s_power_btn = dial_btn_create(scr);
    lv_obj_set_size(s_power_btn, 88, 88);
    lv_obj_set_style_radius(s_power_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_power_btn, 2, 0);
    lv_obj_align(s_power_btn, LV_ALIGN_CENTER, 0, 280 - CY);
    lv_obj_add_event_cb(s_power_btn, power_event_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)s_zone);
    s_power_glyph = lv_label_create(s_power_btn);
    lv_obj_set_style_text_font(s_power_glyph, &lv_font_montserrat_28, 0);
    lv_label_set_text(s_power_glyph, LV_SYMBOL_POWER);
    lv_obj_center(s_power_glyph);

    // #12 Staleness dot.
    s_stale_dot = lv_obj_create(scr);
    lv_obj_set_size(s_stale_dot, 10, 10);
    lv_obj_set_style_radius(s_stale_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_stale_dot, 0, 0);
    lv_obj_clear_flag(s_stale_dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_stale_dot, LV_ALIGN_CENTER, 0, 26 - CY);
    // Never LV_OBJ_FLAG_HIDDEN: lv_obj_fade_in/out (used from apply_palette_
    // and_state) only animate opacity, so visibility must be opacity-driven
    // the whole time or fade_in would animate an object LVGL still skips.
    lv_obj_set_style_opa(s_stale_dot, LV_OPA_TRANSP, 0);

    // #13 Page dots — 3 (Dial(A) - Dial(B) - Menu), evenly spaced
    // 16px apart around the same centered slot the original 2-dot pair used.
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

    // Away badge — tiny, hidden unless st->away (design-spec.md §7 extension).
    s_away_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_away_lbl, &lv_font_montserrat_12, 0);
    lv_label_set_text(s_away_lbl, "AWAY");
    lv_obj_clear_flag(s_away_lbl, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_away_lbl, LV_ALIGN_CENTER, 180 - CX, 44 - CY);
    lv_obj_add_flag(s_away_lbl, LV_OBJ_FLAG_HIDDEN);
}

static void destroy(void)
{
    // Kill anims targeting our objects before the router frees them.
    if (s_temp_lbl)   lv_anim_del(s_temp_lbl, NULL);
    if (s_num_box)    lv_anim_del(s_num_box, NULL);
    if (s_arc)        lv_anim_del(s_arc, NULL);
    if (s_pill_glyph) lv_anim_del(s_pill_glyph, NULL);
    if (s_stale_dot)  lv_anim_del(s_stale_dot, NULL);
    if (s_boost_timer) { lv_timer_del(s_boost_timer); s_boost_timer = NULL; }

    s_actual_f = -1.0f;

    s_level = NULL;
    s_arc = s_stale_dot = s_name_lbl = NULL;
    s_underline_solid = s_underline_dash = s_water_lbl = NULL;
    s_num_box = s_temp_lbl = s_unit_lbl = NULL;
    s_pill = s_pill_glyph = s_pill_word = NULL;
    s_power_btn = s_power_glyph = NULL;
    s_dot_a = s_dot_b = s_dot_menu = NULL;
    s_away_lbl = NULL;
    s_chevron_active = false;
    s_stale_shown = false;
}

// Ticks once a second while the shown zone's relief is active, just to
// recompute the pill's mm:ss text — everything else it touches is idempotent.
static void boost_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_arc) return;
    app_state_t st;
    dial_state_get(&st);
    apply_palette_and_state(&st);
}

static void on_state(const app_state_t *st)
{
    if (!s_arc || !st->have_state) return;
    const zone_state_t *z = &st->zones[s_zone];

    // Optimistic intent wins while set; otherwise follow the device. Resolved
    // BEFORE apply_palette_and_state, which lays the water stripes out against
    // s_shown_f — reading a stale target there would leave them a poll behind.
    int f = (st->ui_temp_f[s_zone] >= 0) ? st->ui_temp_f[s_zone] : dial_c_to_f(z->temp_c);
    s_shown_f = f;

    // Sets s_units_c (among other things) before render_numeral below reads it
    // — otherwise a units toggle would render one call stale.
    apply_palette_and_state(st);

    lv_arc_set_value(s_arc, f);
    render_numeral(f);

    if (z->user_name[0]) {
        char name[36];
        snprintf(name, sizeof(name), "%s'S SIDE", z->user_name);
        // Upper-case for the label style (ASCII only — names come from Orion).
        for (char *p = name; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
        lv_label_set_text(s_name_lbl, name);
    }

    // Countdown ticker: created on the transition into relief, deleted on the
    // transition out (or in destroy() if the screen is torn down first).
    if (z->relief_active && !s_boost_timer) {
        s_boost_timer = lv_timer_create(boost_timer_cb, 1000, NULL);
    } else if (!z->relief_active && s_boost_timer) {
        lv_timer_del(s_boost_timer);
        s_boost_timer = NULL;
    }
}

static bool on_knob(int detents)
{
    if (!s_arc || s_shown_f < 0) return false;
    int nf = s_shown_f + detents;
    if (nf < DIAL_TEMP_MIN_F) nf = DIAL_TEMP_MIN_F;
    if (nf > DIAL_TEMP_MAX_F) nf = DIAL_TEMP_MAX_F;
    if (nf == s_shown_f) {                          // at the range stop
        dial_haptics_play(HAPTIC_STOP);
        int dir = detents > 0 ? 1 : -1;
        anim_nudge(s_num_box, dir);
        anim_nudge(s_arc, dir);
        return true;
    }

    lv_arc_set_value(s_arc, nf);
    s_shown_f = nf;
    render_numeral(nf);
    level_render(nf);
    anim_zoom_bump(s_temp_lbl);
    post_temp(nf);
    return true;
}

// Face order: Dial(B) <-left- Dial(A) <-left- Menu. zone_b is the LEFT side of
// the bed and zone_a the RIGHT, so the chain now reads left-to-right the way
// the bed does. A single-zone topper has no partner face: its one dial sits
// where the pair would, and a left swipe goes straight to the menu.
//
// Quick-actions opens via long-press, not a swipe on the dial itself (see
// screen_long_press_cb) — up/down are simply unhandled here now that the
// router forwards all four directions.
static bool on_gesture(lv_dir_t dir)
{
    if (dir != LV_DIR_LEFT && dir != LV_DIR_RIGHT) return false;

    app_state_t st;
    dial_state_get(&st);
    bool dual = dial_state_is_dual(&st);

    if (dir == LV_DIR_LEFT) {
        if (dual && s_zone == ZONE_B) {
            // Commit the side choice BEFORE navigating: the nav policy follows
            // ui_zone, so an uncommitted swipe would be undone by the next
            // state commit (the poll would yank the view back).
            dial_state_set_ui_zone(ZONE_A);
            ui_router_go(SCR_DIAL, (void *)(uintptr_t)ZONE_A, LV_SCR_LOAD_ANIM_MOVE_LEFT);
        } else {
            // End of the dial chain (or the only dial there is) — the menu face
            // is a step further left.
            ui_router_go(SCR_MENU, NULL, LV_SCR_LOAD_ANIM_MOVE_LEFT);
        }
        return true;
    }

    // RIGHT walks back toward Dial(B). From the leftmost face there's nothing
    // further right, so leave the gesture unconsumed rather than fake a move.
    if (dual && s_zone == ZONE_A) {
        dial_state_set_ui_zone(ZONE_B);
        ui_router_go(SCR_DIAL, (void *)(uintptr_t)ZONE_B, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
        return true;
    }
    return false;
}

const ui_screen_t scr_dial = {
    .create = create, .destroy = destroy, .on_state = on_state,
    .on_knob = on_knob, .on_gesture = on_gesture,
};
