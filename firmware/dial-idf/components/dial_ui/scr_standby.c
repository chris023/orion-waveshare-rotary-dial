/*
 * SCR_STANDBY — always-on clock face (design-spec.md §5). "A clock, not a
 * dashboard" — no temperatures rest on screen; the same r=165 chassis ring
 * that lights up as the Home arc persists here as a bare hairline.
 *
 * Wake rule (design-spec.md §5, D1's roadmap rule): a dark-screen touch or
 * detent must never change the bed. main.c's touch_filter and knob_step
 * already swallow the FIRST standby input end-to-end via
 * dial_power_wake_consumes() — that alone flips dial_power_level() to
 * ACTIVE, and ui_router.c's dispatcher notices the level change and
 * re-runs nav_policy within one ~50ms tick, landing here -> SCR_DIAL on its
 * own. The tap/on_knob handlers below are the belt-and-suspenders path for
 * the rare case a second input lands on this screen before that tick fires.
 *
 * Peek (spec: setpoints sliding in on tap before waking) is deferred — see
 * the note on tap_cb.
 */
#include "ui_screens_internal.h"
#include "dial_ota.h"
#include <time.h>

LV_FONT_DECLARE(dial_font_num_88)

#define CX 180
#define CY 180
#define ARC_R 165

static lv_obj_t *s_ring;
static lv_obj_t *s_clock_lbl, *s_date_lbl;
static lv_obj_t *s_dot_left, *s_dot_right;   // left=ZONE_B (partner), right=ZONE_A (home)
static lv_obj_t *s_ota_lbl;   // ambient "Update available" — see create()
static lv_timer_t *s_clock_timer;

static zone_idx_t s_zone = ZONE_A;   // last-shown side; wake target

static const char *WD[7]  = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };
static const char *MO[12] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                               "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };

// H:MM, 12-hour (no AM/PM — the date line + a 24h room clock elsewhere is
// enough context; a bare "0:00" 24h midnight reads worse at a glance).
// "WED · JUL 9" per the mock, but the compiled Montserrat fonts here only
// carry ASCII + the degree sign (see scr_dial.c's water caption) — no
// middle-dot glyph — so the separator is an ASCII hyphen instead.
static void render_clock(void)
{
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);

    int h12 = lt.tm_hour % 12;
    if (h12 == 0) h12 = 12;
    char buf[8];
    snprintf(buf, sizeof(buf), "%d:%02d", h12, lt.tm_min);
    lv_label_set_text(s_clock_lbl, buf);

    char date[24];
    snprintf(date, sizeof(date), "%s - %s %d", WD[lt.tm_wday], MO[lt.tm_mon], lt.tm_mday);
    lv_label_set_text(s_date_lbl, date);
}

static void clock_timer_cb(lv_timer_t *t) { (void)t; render_clock(); }

static void set_presence_dot(lv_obj_t *dot, zone_kind_t k, const dial_palette_t *pal)
{
    lv_color_t c = dial_zone_accent(k, pal);
    lv_obj_set_style_border_color(dot, c, 0);
    lv_obj_set_style_bg_color(dot, c, 0);
    bool filled = (k == ZK_HEATING || k == ZK_COOLING || k == ZK_HOLDING);
    lv_obj_set_style_bg_opa(dot, filled ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}

// Re-applied every on_state: covers both fresh telemetry and a palette swap
// (screens read PAL() at render time so a night transition needs no rebuild).
static void apply_palette_and_state(const app_state_t *st)
{
    const dial_palette_t *pal = PAL();
    bool night = dial_palette_is_night();

    lv_obj_t *scr = lv_obj_get_parent(s_ring);
    lv_obj_set_style_bg_color(scr, pal->bg, 0);
    lv_obj_set_style_arc_color(s_ring, pal->track, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_date_lbl, pal->ink_secondary, 0);
    // Deliberately dimmer than the ember ink at night so the room stays dark.
    lv_obj_set_style_text_color(s_clock_lbl, night ? pal->neutral_holding : pal->ink_primary, 0);

    // Ambient "Update available" (owner reassessment,
    // docs/SPEC-update-prompt.md) — unconditional on status + night, same
    // rule scr_dial.c's own copy of this notice uses: no idle window, no
    // daily ceiling, no skip/defer interaction (those govern the
    // SCR_UPDATE_PROMPT sheet, not this). Hidden entirely at night rather
    // than dimmed — a bedside device must not advertise anything at 3am.
    // Deliberately NOT tappable here (unlike the dial face): this screen's
    // whole surface is already a single wake target (tap_cb below), and the
    // wake-consumes-first-input rule owns that touch, not this label.
    lv_obj_set_style_text_color(s_ota_lbl, pal->ink_secondary, 0);
    if ((dial_ota_status_t)st->ota.status == OTA_AVAILABLE && !night)
        lv_obj_clear_flag(s_ota_lbl, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(s_ota_lbl, LV_OBJ_FLAG_HIDDEN);

    if (!st->have_state) return;
    // One presence dot per side the bed actually has: a single-zone topper
    // shows the one dot, centered, instead of a permanently-dead second one.
    if (dial_state_is_dual(st)) {
        lv_obj_clear_flag(s_dot_left, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_dot_right, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(s_dot_left,  LV_ALIGN_CENTER, 164 - CX, 104 - CY);
        lv_obj_align(s_dot_right, LV_ALIGN_CENTER, 196 - CX, 104 - CY);
        set_presence_dot(s_dot_left,  dial_zone_kind(&st->zones[ZONE_B], st->device_online), pal);
        set_presence_dot(s_dot_right, dial_zone_kind(&st->zones[ZONE_A], st->device_online), pal);
    } else {
        zone_idx_t p = dial_state_primary_zone(st);
        lv_obj_add_flag(s_dot_left, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_dot_right, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(s_dot_right, LV_ALIGN_CENTER, 0, 104 - CY);
        set_presence_dot(s_dot_right, dial_zone_kind(&st->zones[p], st->device_online), pal);
    }
}

static void wake(void)
{
    ui_router_go(SCR_DIAL, (void *)(uintptr_t)s_zone, LV_SCR_LOAD_ANIM_FADE_ON);
}

// Tap-anywhere wake. Spec also calls for a peek (both setpoints sliding in
// before the wake commits) — deferred: it needs its own in/hold/out timeline
// and this pass is scoped to get wake + the clock face solid first.
static void tap_cb(lv_event_t *e) { (void)e; wake(); }

static void create(lv_obj_t *scr, void *arg)
{
    s_zone = (zone_idx_t)(uintptr_t)arg;
    const dial_palette_t *pal = PAL();
    lv_obj_set_style_bg_color(scr, pal->bg, 0);
    lv_obj_add_event_cb(scr, tap_cb, LV_EVENT_CLICKED, NULL);

    // Chassis hairline ring — same object geometry as the Home arc, w=2, no
    // lit indicator, non-interactive (taps fall through to `scr` above).
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

    s_clock_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_clock_lbl, &dial_font_num_88, 0);
    lv_label_set_text(s_clock_lbl, "--:--");
    lv_obj_align(s_clock_lbl, LV_ALIGN_CENTER, 0, 168 - CY);

    s_date_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_date_lbl, &lv_font_montserrat_16, 0);
    lv_label_set_text(s_date_lbl, "");
    lv_obj_align(s_date_lbl, LV_ALIGN_CENTER, 0, 232 - CY);

    s_dot_left = lv_obj_create(scr);
    lv_obj_set_size(s_dot_left, 8, 8);
    lv_obj_set_style_radius(s_dot_left, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_dot_left, 2, 0);
    lv_obj_clear_flag(s_dot_left, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_dot_left, LV_ALIGN_CENTER, 164 - CX, 104 - CY);

    s_dot_right = lv_obj_create(scr);
    lv_obj_set_size(s_dot_right, 8, 8);
    lv_obj_set_style_radius(s_dot_right, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_dot_right, 2, 0);
    lv_obj_clear_flag(s_dot_right, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_dot_right, LV_ALIGN_CENTER, 196 - CX, 104 - CY);

    // Ambient "Update available" — same slot scr_dial.c parks its own copy
    // of this notice in (below the ring's own content, inside its bottom
    // gap): here that gap is entirely empty (no page dots on this face — it
    // isn't part of the swipe chain), so there's nothing to collide with.
    // Plain label, default (non-clickable) flags: a tap anywhere on this
    // screen — including squarely on this text — must fall through to
    // tap_cb above and wake, never navigate on its own (see the header
    // comment's wake rule / apply_palette_and_state's note on why this one
    // stays passive unlike scr_dial.c's).
    s_ota_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_ota_lbl, &lv_font_montserrat_12, 0);
    lv_label_set_text(s_ota_lbl, "Update available");
    lv_obj_align(s_ota_lbl, LV_ALIGN_CENTER, 0, 330 - CY);
    lv_obj_add_flag(s_ota_lbl, LV_OBJ_FLAG_HIDDEN);

    render_clock();
    s_clock_timer = lv_timer_create(clock_timer_cb, 1000, NULL);
}

static void destroy(void)
{
    if (s_clock_timer) { lv_timer_del(s_clock_timer); s_clock_timer = NULL; }
    s_ring = s_clock_lbl = s_date_lbl = s_dot_left = s_dot_right = s_ota_lbl = NULL;
}

static void on_state(const app_state_t *st)
{
    if (!s_ring) return;
    s_zone = st->ui_zone;
    apply_palette_and_state(st);
}

// A detent while on standby wakes and is consumed, never applied (D1's
// resolved rule). The dispatcher's power-level check (ui_router.c) is the
// primary path; this covers a detent landing here before that tick catches
// up (dial_power_wake_consumes() already flipped the level to ACTIVE).
static bool on_knob(int detents)
{
    (void)detents;
    wake();
    return true;
}

// Swipe down opens the hidden diagnostics face (UI_DESIGN_SPEC.md §"Standby":
// "repurposed since Standby is the root and 'back' has nowhere else to go").
//
// In practice this path is nearly unreachable and that is by design, not by
// accident: nav_policy only parks on this face while dial_power_level() is
// STANDBY, and main.c's touch_filter swallows a standby touch from press to
// release to honour the wake-consumes-first-input rule. So the usual outcome
// of swiping down on a dark clock is "the screen wakes", and the router has
// moved on to SCR_DIAL before a second gesture can land. It is wired anyway
// because the face IS the spec'd root for this gesture, it costs four lines,
// and a wake that lands here without consuming (the dispatcher's tick racing
// the filter) should do the spec'd thing rather than nothing. SCR_DIAL carries
// the same gesture as the entry a person can actually hit while awake.
static bool on_gesture(lv_dir_t dir)
{
    if (dir != LV_DIR_BOTTOM) return false;
    ui_router_go(SCR_DIAG, NULL, LV_SCR_LOAD_ANIM_MOVE_BOTTOM);
    return true;
}

const ui_screen_t scr_standby = {
    .create = create, .destroy = destroy, .on_state = on_state,
    .on_knob = on_knob, .on_gesture = on_gesture,
};
