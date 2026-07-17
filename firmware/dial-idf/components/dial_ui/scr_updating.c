/*
 * SCR_UPDATING — full-screen OTA install takeover (M6 UX hardening).
 *
 * Before this screen existed, confirming the install on SCR_ABOUT left the
 * user staring at the same About list while "Downloading NN%" quietly ticked
 * up inside one row — easy to miss, and nothing stopped them wandering off
 * to another screen mid-write. nav_policy (main.c) now forces this screen
 * the moment ota.status actually becomes OTA_DOWNLOADING (the same
 * first-run-phase forced-navigation trick SCR_WELCOME uses), the same way
 * it would force SCR_STANDBY or SCR_ERROR — see nav_policy's OTA block.
 *
 * LOCKED: no touch targets at all, and on_knob/on_gesture consume every
 * input without acting on it — an OTA write in progress must never be
 * interrupted by a stray swipe or knob turn (consuming still stamps input,
 * which is a plus: it keeps the display from dimming to standby mid-update).
 * The only ways off this screen are the reboot that follows a successful
 * install (this screen simply never gets torn down — esp_restart() doesn't
 * return) or nav_policy routing back to SCR_ABOUT once ota.status lands on
 * OTA_FAILED, where the stacked error line under the row explains why.
 */
#include "ui_screens_internal.h"
#include "dial_ota.h"

LV_FONT_DECLARE(dial_font_num_88)

#define CX 180
#define CY 180
#define ARC_R 165

static lv_obj_t *s_arc;
static lv_obj_t *s_title_lbl;
static lv_obj_t *s_pct_row, *s_pct_lbl, *s_pct_unit_lbl;
static lv_obj_t *s_caption_lbl;

/* ---- palette ---------------------------------------------------------- */
// Re-applied from on_state (not just create()) so a night palette swap
// mid-download recolors it — screens never cache PAL() past a render.
static void apply_palette(void)
{
    const dial_palette_t *pal = PAL();
    lv_obj_t *scr = lv_obj_get_parent(s_arc);
    lv_obj_set_style_bg_color(scr, pal->bg, 0);

    lv_obj_set_style_arc_color(s_arc, pal->track, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(s_arc, LV_OPA_70, LV_PART_MAIN);
    // Neutral, not heat/cool-accented — an install isn't a zone, and this
    // is the same "everything's fine, in progress" tone scr_dial reaches
    // for once a zone settles at its target.
    lv_obj_set_style_arc_color(s_arc, pal->neutral_holding, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(s_arc, LV_OPA_COVER, LV_PART_INDICATOR);

    lv_obj_set_style_text_color(s_title_lbl, pal->ink_secondary, 0);
    lv_obj_set_style_text_color(s_pct_lbl, pal->ink_primary, 0);
    lv_obj_set_style_text_color(s_pct_unit_lbl, pal->ink_secondary, 0);
    lv_obj_set_style_text_color(s_caption_lbl, pal->ink_secondary, 0);
}

/* ---- render ------------------------------------------------------------*/

static void render(const app_state_t *st)
{
    int pct = st->ota.progress_pct;
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    lv_arc_set_value(s_arc, pct);

    // dial_font_num_88 is a converted subset (see dial_font_num_88.c's header
    // comment: --range 0x2D,0x2E,0x30-0x3A, i.e. just "-.0123456789:") with no
    // '%' glyph -- scr_dial.c hits the same wall with "\xC2\xB0" and solves it
    // the same way, a separate label in a full font next to the bare number
    // rather than baking the unit into the numeral's own string.
    char buf[8];
    snprintf(buf, sizeof(buf), "%d", pct);
    lv_label_set_text(s_pct_lbl, buf);

    // OTA_READY_REBOOT: the image is written + verified and main.c is about
    // to esp_restart() — the caption is the last thing this screen will ever
    // show. Anything else (still OTA_DOWNLOADING, or a stray render before
    // nav_policy has whisked us away on failure) reads as still-downloading,
    // which is the safer default for a screen that shouldn't linger anyway.
    bool rebooting = (dial_ota_status_t)st->ota.status == OTA_READY_REBOOT;
    // "..." not the unicode ellipsis: the compiled Montserrat fonts here are
    // ASCII + the degree sign only (see scr_standby.c's clock/date note) —
    // matches scr_about's own "Restarting..." string verbatim.
    lv_label_set_text(s_caption_lbl, rebooting ? "Restarting..." : "Keep the dial plugged in");
}

/* ---- vtable --------------------------------------------------------------*/

static void create(lv_obj_t *scr, void *arg)
{
    (void)arg;
    const dial_palette_t *pal = PAL();
    lv_obj_set_style_bg_color(scr, pal->bg, 0);

    // Progress ring — same chassis geometry as scr_boost's duration arc.
    s_arc = lv_arc_create(scr);
    lv_obj_set_size(s_arc, 2 * ARC_R, 2 * ARC_R);
    lv_obj_center(s_arc);
    lv_arc_set_rotation(s_arc, 135);
    lv_arc_set_bg_angles(s_arc, 0, 270);
    lv_arc_set_range(s_arc, 0, 100);
    lv_arc_set_value(s_arc, 0);
    lv_obj_set_style_arc_width(s_arc, 16, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 16, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_arc, true, LV_PART_INDICATOR);
    // Kill the default theme's knob dot at the indicator's leading edge — a
    // knob is a drag handle, and nothing about a progress ring is draggable.
    // Same suppression scr_menu/scr_standby/scr_tonight apply to their rings.
    lv_obj_set_style_bg_opa(s_arc, LV_OPA_TRANSP, LV_PART_KNOB);
    // Locked screen: nothing here is draggable or tappable.
    lv_obj_clear_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);

    s_title_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_title_lbl, &lv_font_montserrat_16, 0);
    lv_label_set_text(s_title_lbl, "UPDATING");
    lv_obj_align(s_title_lbl, LV_ALIGN_CENTER, 0, 64 - CY);

    // Percent numeral + unit, same "big bare number + small unit label"
    // vocabulary as scr_dial's setpoint/"°F" pairing — a flex row (rather
    // than scr_dial's fixed-offset placement) so the "%" stays glued to the
    // right of the digits as they go from "5" to "62" to "100" over the
    // download instead of the gap growing/shrinking.
    s_pct_row = lv_obj_create(scr);
    lv_obj_set_size(s_pct_row, LV_SIZE_CONTENT, 92);
    lv_obj_set_style_bg_opa(s_pct_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_pct_row, 0, 0);
    lv_obj_set_style_pad_all(s_pct_row, 0, 0);
    lv_obj_set_style_pad_column(s_pct_row, 4, 0);
    lv_obj_clear_flag(s_pct_row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s_pct_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_pct_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(s_pct_row, LV_ALIGN_CENTER, 0, 150 - CY);

    s_pct_lbl = lv_label_create(s_pct_row);
    lv_obj_set_style_text_font(s_pct_lbl, &dial_font_num_88, 0);
    lv_label_set_text(s_pct_lbl, "0");

    s_pct_unit_lbl = lv_label_create(s_pct_row);
    lv_obj_set_style_text_font(s_pct_unit_lbl, &lv_font_montserrat_28, 0);
    lv_label_set_text(s_pct_unit_lbl, "%");

    s_caption_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_caption_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_width(s_caption_lbl, 260);
    lv_label_set_long_mode(s_caption_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_caption_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_caption_lbl, "Keep the dial plugged in");
    lv_obj_align(s_caption_lbl, LV_ALIGN_CENTER, 0, 220 - CY);

    apply_palette();
}

static void destroy(void)
{
    s_arc = s_title_lbl = s_caption_lbl = NULL;
    s_pct_row = s_pct_lbl = s_pct_unit_lbl = NULL;
}

static void on_state(const app_state_t *st)
{
    if (!s_arc) return;
    apply_palette();
    render(st);
}

// Locked: every knob turn is swallowed rather than left unconsumed — an OTA
// write in progress must never respond to input, and consuming it still
// stamps activity (dial_state_stamp_input(), via the router/main.c callers),
// which is a plus: it keeps the idle timer from dimming the takeover mid-way
// through a slow download.
static bool on_knob(int detents) { (void)detents; return true; }

// Locked: every swipe is swallowed too, so there is no way to navigate off
// this screen except the reboot (success) or nav_policy's own failure route.
static bool on_gesture(lv_dir_t dir) { (void)dir; return true; }

const ui_screen_t scr_updating = {
    .create = create, .destroy = destroy, .on_state = on_state,
    .on_knob = on_knob, .on_gesture = on_gesture,
};
