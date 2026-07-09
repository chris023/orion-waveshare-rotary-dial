/*
 * SCR_TONIGHT — tonight's sleep-schedule face (M5). Reached by swiping left
 * from Dial(B) (scr_dial.c's on_gesture; face order Dial(A) <-left- Dial(B)
 * <-left- Tonight), dismissed by swiping right (back to Dial(B)) or the
 * standby idle timeout (main.c's nav_policy checks that before folding this
 * screen into the sticky set — see the comment there).
 *
 * Always shows ZONE_A's schedule regardless of which dial side you arrived
 * from: the Orion override call has no way to target a specific user, so
 * only the dial's own side supports the override control (see the long
 * comment beside CMD_TONIGHT_OVERRIDE in dial_state.h). zone_state_t's
 * sched_* fields are populated by main.c's orion_refresh_schedules() from
 * get_sleep_schedules, matched to a zone via a worker-side uuid map.
 *
 * The wake-time picker is a MODE within this screen (not a separate SCR_*),
 * per the same "no floating extra screen for a single control" reasoning
 * scr_boost.c/scr_quick.c already follow — it swaps the bedtime/wake blocks
 * out for a single big numeral in the same fixed anchor slot scr_dial and
 * scr_boost use, and reuses their detent/zoom-bump/range-stop vocabulary.
 */
#include "ui_screens_internal.h"
#include "dial_haptics.h"

#define CX 180
#define CY 180
#define ARC_R 165

#define PICKER_STEP_MIN    15
#define PICKER_RANGE_MIN  180   // +/- 3h from the schedule's current wakeup

static lv_obj_t *s_ring;
static lv_obj_t *s_title_lbl;
static lv_obj_t *s_bedtime_lbl, *s_bedtime_cap_lbl;
static lv_obj_t *s_wake_lbl, *s_wake_cap_lbl;
static lv_obj_t *s_no_sched_lbl;
static lv_obj_t *s_chip, *s_chip_lbl;
static lv_obj_t *s_pick_num_box, *s_pick_num_lbl;
static lv_obj_t *s_dot_a, *s_dot_b, *s_dot_tonight;

// Wake-time picker mode state. s_picker_base_min is ZONE_A's current wakeup
// (minutes-from-midnight) captured at entry; the knob adjusts an offset from
// it, never a running total, so the +-3h range is always relative to what
// the schedule actually says right now.
static bool s_picker_mode;
static int  s_picker_base_min;
static int  s_picker_offset_min;

// LVGL 8.4 sends LV_EVENT_CLICKED on release unconditionally (it does NOT
// suppress click after a long-press or a gesture the way one might expect —
// verified against lv_indev.c's indev_proc_release, which only guards
// LV_EVENT_SHORT_CLICKED on long_pr_sent, not LV_EVENT_CLICKED itself). Since
// confirm_picker_cb listens for CLICKED on the whole screen, both "the
// long-press that just opened the picker" and "a stray swipe while picking"
// would otherwise immediately fire a bogus confirm on release. This flag is
// set by whichever of those two fired during the current touch and consumed
// (without confirming) by the CLICKED that inevitably follows; it's cleared
// at the start of every fresh press so a genuine plain tap still confirms.
static bool s_suppress_click;

/* ---- motion helpers (same vocabulary as scr_dial.c/scr_boost.c's) ------- */

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

/* ---- formatting ---------------------------------------------------------*/

// 12h "H:MM AM/PM" — no leading zero on the hour (matches scr_standby's clock
// convention); wraps any out-of-range input into 0..1439 first.
static void format_time_12h(int total_min, char *out, size_t n)
{
    total_min = ((total_min % 1440) + 1440) % 1440;
    int hh = total_min / 60, mm = total_min % 60;
    bool pm = hh >= 12;
    int h12 = hh % 12;
    if (h12 == 0) h12 = 12;
    snprintf(out, n, "%d:%02d %s", h12, mm, pm ? "PM" : "AM");
}

// Display-only °C/°F conversion (M4 units toggle) — same convention as
// scr_dial's water caption.
static void format_temp(bool units_c, float c, char *out, size_t n)
{
    if (units_c) snprintf(out, n, "%.1f\xC2\xB0", c);
    else         snprintf(out, n, "%d\xC2\xB0", dial_c_to_f(c));
}

/* ---- picker mode ---------------------------------------------------------*/

static void render_picker_numeral(void)
{
    int wake_min = s_picker_base_min + s_picker_offset_min;
    char buf[12];
    format_time_12h(wake_min, buf, sizeof(buf));
    lv_label_set_text(s_pick_num_lbl, buf);
}

static void set_normal_blocks_hidden(bool hidden)
{
    lv_obj_t *objs[] = { s_bedtime_lbl, s_bedtime_cap_lbl, s_wake_lbl, s_wake_cap_lbl, s_chip };
    for (size_t i = 0; i < sizeof(objs) / sizeof(objs[0]); i++) {
        if (hidden) lv_obj_add_flag(objs[i], LV_OBJ_FLAG_HIDDEN);
        else        lv_obj_clear_flag(objs[i], LV_OBJ_FLAG_HIDDEN);
    }
}

// Re-renders the normal-mode blocks from a fresh snapshot — used on exit so
// they never sit stale (hidden with pre-picker text) until some unrelated
// state commit happens to land.
static void refresh_from_store(void);

static void exit_picker_mode(void)
{
    s_picker_mode = false;
    lv_obj_add_flag(s_pick_num_box, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_title_lbl, "TONIGHT");
    refresh_from_store();
}

// Long-press anywhere on the screen (labels/background aren't clickable
// targets, only the chip is — same occlusion reasoning as scr_dial's
// numeral/pill) opens the picker, but only when ZONE_A's schedule actually
// supports an override today.
static void enter_picker_mode_cb(lv_event_t *e)
{
    (void)e;
    if (s_picker_mode) return;
    app_state_t st;
    dial_state_get(&st);
    const zone_state_t *z = &st.zones[ZONE_A];
    int base_min;
    if (!z->sched_valid || !z->sched_override_available ||
        !dial_parse_hhmm(z->sched_wakeup, &base_min))
        return;

    s_picker_mode = true;
    s_picker_base_min = base_min;
    s_picker_offset_min = 0;
    s_suppress_click = true;   // swallow the CLICKED that follows this same long-press's release
    dial_haptics_play(HAPTIC_TICK);   // settle cue, matches scr_dial's mode-switch long-press
    lv_label_set_text(s_title_lbl, "SET WAKE");
    lv_obj_add_flag(s_no_sched_lbl, LV_OBJ_FLAG_HIDDEN);
    set_normal_blocks_hidden(true);
    lv_obj_clear_flag(s_pick_num_box, LV_OBJ_FLAG_HIDDEN);
    render_picker_numeral();
}

// A plain tap confirms the candidate wake time. Guarded by s_suppress_click
// (see its declaration) so the CLICKED that LVGL fires after the long-press
// which opened the picker, or after any swipe taken while picking, can't
// masquerade as a confirm.
static void confirm_picker_cb(lv_event_t *e)
{
    (void)e;
    if (!s_picker_mode) return;
    if (s_suppress_click) { s_suppress_click = false; return; }
    dial_haptics_play(HAPTIC_CONFIRM);
    int wake_min = ((s_picker_base_min + s_picker_offset_min) % 1440 + 1440) % 1440;
    app_cmd_t cmd = { .kind = CMD_TONIGHT_OVERRIDE, .zone = ZONE_A, .a = wake_min, .b = -1 };
    dial_cmd_post(&cmd);
    exit_picker_mode();
}

/* ---- palette + state -----------------------------------------------------*/

static void apply_palette_and_state(const app_state_t *st)
{
    const dial_palette_t *pal = PAL();
    bool night = dial_palette_is_night();

    lv_obj_t *scr = lv_obj_get_parent(s_ring);
    lv_obj_set_style_bg_color(scr, pal->bg, 0);
    lv_obj_set_style_arc_color(s_ring, pal->track, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_title_lbl, pal->ink_secondary, 0);
    lv_obj_set_style_text_color(s_pick_num_lbl, pal->ink_primary, 0);

    // Page dots — Tonight's own dot is always the filled one on this screen.
    lv_obj_set_style_bg_color(s_dot_a, pal->track, 0);
    lv_obj_set_style_bg_color(s_dot_b, pal->track, 0);
    lv_obj_set_style_bg_color(s_dot_tonight, pal->ink_secondary, 0);

    // While picking, leave the normal-mode blocks exactly as
    // enter_picker_mode_cb left them (hidden) — a schedule refresh or
    // day/night flip landing mid-edit must not fight the picker's own view.
    if (s_picker_mode) return;

    const zone_state_t *z = &st->zones[ZONE_A];
    lv_obj_set_style_text_color(s_bedtime_lbl, pal->ink_primary, 0);
    lv_obj_set_style_text_color(s_bedtime_cap_lbl, pal->ink_secondary, 0);
    lv_obj_set_style_text_color(s_wake_lbl, pal->ink_primary, 0);
    lv_obj_set_style_text_color(s_wake_cap_lbl, pal->ink_secondary, 0);
    lv_obj_set_style_text_color(s_no_sched_lbl, pal->ink_secondary, 0);

    // Overridden chip: warning color by day, stale (dim) at night — same
    // hue->value substitution the rest of the palette uses at night.
    lv_color_t chip_color = night ? pal->stale : pal->warning;
    lv_obj_set_style_bg_color(s_chip, pal->surface, 0);
    lv_obj_set_style_border_color(s_chip, chip_color, 0);
    lv_obj_set_style_text_color(s_chip_lbl, chip_color, 0);

    if (!z->sched_valid) {
        lv_obj_clear_flag(s_no_sched_lbl, LV_OBJ_FLAG_HIDDEN);
        set_normal_blocks_hidden(true);
        return;
    }
    lv_obj_add_flag(s_no_sched_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_bedtime_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_bedtime_cap_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_wake_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_wake_cap_lbl, LV_OBJ_FLAG_HIDDEN);

    char clock_buf[12], temp_buf[12], line[40];
    int mins;

    if (dial_parse_hhmm(z->sched_bedtime, &mins)) format_time_12h(mins, clock_buf, sizeof(clock_buf));
    else strlcpy(clock_buf, "--:--", sizeof(clock_buf));
    format_temp(st->units_c, z->sched_bedtime_temp_c, temp_buf, sizeof(temp_buf));
    snprintf(line, sizeof(line), "%s - %s", clock_buf, temp_buf);
    lv_label_set_text(s_bedtime_lbl, line);

    if (dial_parse_hhmm(z->sched_wakeup, &mins)) format_time_12h(mins, clock_buf, sizeof(clock_buf));
    else strlcpy(clock_buf, "--:--", sizeof(clock_buf));
    format_temp(st->units_c, z->sched_wakeup_temp_c, temp_buf, sizeof(temp_buf));
    snprintf(line, sizeof(line), "%s - %s", clock_buf, temp_buf);
    lv_label_set_text(s_wake_lbl, line);

    if (z->sched_override_applied) lv_obj_clear_flag(s_chip, LV_OBJ_FLAG_HIDDEN);
    else                           lv_obj_add_flag(s_chip, LV_OBJ_FLAG_HIDDEN);
}

static void refresh_from_store(void)
{
    if (!s_ring) return;
    app_state_t st;
    dial_state_get(&st);
    apply_palette_and_state(&st);
}

/* ---- events ----------------------------------------------------------------*/

// Start of a fresh touch — clears s_suppress_click so a genuine plain tap
// (a whole press/release cycle with no long-press or gesture in between)
// always reaches confirm_picker_cb able to confirm.
static void screen_pressed_cb(lv_event_t *e) { (void)e; s_suppress_click = false; }

static void chip_event_cb(lv_event_t *e)
{
    (void)e;
    dial_haptics_play(HAPTIC_CONFIRM);
    app_cmd_t cmd = { .kind = CMD_TONIGHT_REVERT, .zone = ZONE_A };
    dial_cmd_post(&cmd);
}

/* ---- vtable ----------------------------------------------------------------*/

static void create(lv_obj_t *scr, void *arg)
{
    (void)arg;
    s_picker_mode = false;
    s_suppress_click = false;
    const dial_palette_t *pal = PAL();
    lv_obj_set_style_bg_color(scr, pal->bg, 0);
    lv_obj_add_event_cb(scr, screen_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(scr, enter_picker_mode_cb, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(scr, confirm_picker_cb, LV_EVENT_CLICKED, NULL);

    // Chassis hairline ring — same persistent geometry as scr_standby's
    // (r=165, w=2, non-interactive): Tonight is just another face of the one
    // chassis object, per design-spec.md's "a chassis that persists".
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

    s_title_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_title_lbl, &lv_font_montserrat_16, 0);
    lv_label_set_text(s_title_lbl, "TONIGHT");
    lv_obj_align(s_title_lbl, LV_ALIGN_CENTER, 0, 64 - CY);

    s_bedtime_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_bedtime_lbl, &lv_font_montserrat_28, 0);
    lv_label_set_text(s_bedtime_lbl, "--:-- - --\xC2\xB0");
    lv_obj_align(s_bedtime_lbl, LV_ALIGN_CENTER, 0, 118 - CY);

    s_bedtime_cap_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_bedtime_cap_lbl, &lv_font_montserrat_12, 0);
    lv_label_set_text(s_bedtime_cap_lbl, "BEDTIME");
    lv_obj_align(s_bedtime_cap_lbl, LV_ALIGN_CENTER, 0, 146 - CY);

    s_wake_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_wake_lbl, &lv_font_montserrat_28, 0);
    lv_label_set_text(s_wake_lbl, "--:-- - --\xC2\xB0");
    lv_obj_align(s_wake_lbl, LV_ALIGN_CENTER, 0, 198 - CY);

    s_wake_cap_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_wake_cap_lbl, &lv_font_montserrat_12, 0);
    lv_label_set_text(s_wake_cap_lbl, "WAKE");
    lv_obj_align(s_wake_cap_lbl, LV_ALIGN_CENTER, 0, 226 - CY);

    s_no_sched_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_no_sched_lbl, &lv_font_montserrat_16, 0);
    lv_label_set_text(s_no_sched_lbl, "No schedule");
    lv_obj_align(s_no_sched_lbl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_no_sched_lbl, LV_OBJ_FLAG_HIDDEN);

    // Overridden chip (tap to revert) — same pill vocabulary as scr_dial's
    // state pill.
    s_chip = lv_obj_create(scr);
    lv_obj_set_height(s_chip, 24);
    lv_obj_set_width(s_chip, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(s_chip, 160, 0);
    lv_obj_set_style_radius(s_chip, 12, 0);
    lv_obj_set_style_border_width(s_chip, 1, 0);
    lv_obj_set_style_pad_left(s_chip, 12, 0);
    lv_obj_set_style_pad_right(s_chip, 12, 0);
    lv_obj_set_flex_flow(s_chip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(s_chip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_chip, LV_ALIGN_CENTER, 0, 260 - CY);
    lv_obj_add_event_cb(s_chip, chip_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_chip, LV_OBJ_FLAG_HIDDEN);

    s_chip_lbl = lv_label_create(s_chip);
    lv_obj_set_style_text_font(s_chip_lbl, &lv_font_montserrat_12, 0);
    lv_label_set_text(s_chip_lbl, "OVERRIDDEN");

    // Picker-mode numeral (candidate wake time, Mont 48) — same fixed-anchor
    // slot as scr_dial/scr_boost's numeral; hidden outside picker mode.
    s_pick_num_box = lv_obj_create(scr);
    lv_obj_set_size(s_pick_num_box, 280, 70);
    lv_obj_set_style_bg_opa(s_pick_num_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_pick_num_box, 0, 0);
    lv_obj_clear_flag(s_pick_num_box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_pick_num_box, LV_ALIGN_CENTER, 0, 150 - CY);
    lv_obj_add_flag(s_pick_num_box, LV_OBJ_FLAG_HIDDEN);

    s_pick_num_lbl = lv_label_create(s_pick_num_box);
    lv_obj_set_style_text_font(s_pick_num_lbl, &lv_font_montserrat_48, 0);
    lv_label_set_text(s_pick_num_lbl, "--:--");
    lv_obj_set_style_transform_pivot_x(s_pick_num_lbl, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(s_pick_num_lbl, LV_PCT(50), 0);
    lv_obj_center(s_pick_num_lbl);

    // Page dots (3): Dial(A) - Dial(B) - Tonight (design-spec.md §4 #13
    // extended by M5) — Tonight's own dot is always the filled one here.
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

    s_dot_tonight = lv_obj_create(scr);
    lv_obj_set_size(s_dot_tonight, 6, 6);
    lv_obj_set_style_radius(s_dot_tonight, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_dot_tonight, 0, 0);
    lv_obj_clear_flag(s_dot_tonight, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_dot_tonight, LV_ALIGN_CENTER, 196 - CX, 340 - CY);
}

static void destroy(void)
{
    if (s_pick_num_lbl) lv_anim_del(s_pick_num_lbl, NULL);
    if (s_pick_num_box) lv_anim_del(s_pick_num_box, NULL);

    s_ring = NULL;
    s_title_lbl = NULL;
    s_bedtime_lbl = s_bedtime_cap_lbl = NULL;
    s_wake_lbl = s_wake_cap_lbl = NULL;
    s_no_sched_lbl = NULL;
    s_chip = s_chip_lbl = NULL;
    s_pick_num_box = s_pick_num_lbl = NULL;
    s_dot_a = s_dot_b = s_dot_tonight = NULL;
    s_picker_mode = false;
    s_suppress_click = false;
}

static void on_state(const app_state_t *st)
{
    if (!s_ring) return;
    apply_palette_and_state(st);
}

// Outside picker mode the knob does nothing here (design-spec.md's brief for
// this face); inside it, rotate = candidate wake time +-15min, clamped to
// +-3h from the schedule's current wakeup.
static bool on_knob(int detents)
{
    if (!s_picker_mode) return false;

    int no = s_picker_offset_min + detents * PICKER_STEP_MIN;
    if (no < -PICKER_RANGE_MIN) no = -PICKER_RANGE_MIN;
    if (no > PICKER_RANGE_MIN)  no = PICKER_RANGE_MIN;
    if (no == s_picker_offset_min) {                 // at the range stop
        dial_haptics_play(HAPTIC_STOP);
        anim_nudge(s_pick_num_box, detents > 0 ? 1 : -1);
        return true;
    }

    s_picker_offset_min = no;
    render_picker_numeral();
    anim_zoom_bump(s_pick_num_lbl);
    return true;
}

static bool on_gesture(lv_dir_t dir)
{
    if (s_picker_mode) {
        // Any gesture reaching here means LVGL detected a swipe on this same
        // touch, so the CLICKED it fires at release must NOT be read as a
        // confirm tap (see s_suppress_click's declaration).
        s_suppress_click = true;
        // Swipe down cancels the edit (no command posted); every other
        // direction is swallowed so a stray swipe can't navigate away with
        // an edit in progress.
        if (dir == LV_DIR_BOTTOM) exit_picker_mode();
        return true;
    }
    if (dir != LV_DIR_RIGHT) return false;
    // Tonight always follows from Dial(B) in the face chain (scr_dial.c) —
    // ui_zone is already ZONE_B from that swipe, nothing to re-commit here.
    ui_router_go(SCR_DIAL, (void *)(uintptr_t)ZONE_B, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
    return true;
}

const ui_screen_t scr_tonight = {
    .create = create, .destroy = destroy, .on_state = on_state,
    .on_knob = on_knob, .on_gesture = on_gesture,
};
