/*
 * SCR_ABOUT — device/about sub-screen, reached from SCR_MENU. A single
 * scrollable list: the Software update control (M6's GitHub-Releases OTA)
 * on top, then read-only firmware/IDF/serial identifiers below it. There is
 * no other entry point (no schedule, no zone) so on_state has nothing to
 * gate on besides its own root pointer.
 */
#include "ui_screens_internal.h"
#include "dial_haptics.h"
#include "dial_list.h"
#include "dial_ota.h"
#include "esp_app_desc.h"

#define CY 180
#define ROW_H             76
#define CONFIRM_WINDOW_MS 3000

static lv_obj_t *s_title_lbl;
static lv_obj_t *s_list;
static lv_obj_t *s_val_ota;         // Software update row's value label, also the confirm target
static lv_obj_t *s_ota_err_lbl;     // second line under that row, FAILED only
static lv_obj_t *s_val_serial;

// Only one row on this screen can ever need a confirm tap, so the
// CONFIRM_RELINK/WIFI/FACTORY id table scr_settings.c needs collapses to a
// single flag here — same 3s window, same 250ms sweep timer.
static bool      s_ota_armed;
static uint32_t  s_armed_at_ms;
static lv_timer_t *s_confirm_timer;

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

/* ---- confirm-tap helper (Software update / AVAILABLE only) --------------*/

static void confirm_disarm(void)
{
    if (s_ota_armed && s_val_ota) lv_label_set_text(s_val_ota, "");
    s_ota_armed = false;
}

// Ticks while the row is armed, so "Tap again to confirm" reverts on its own
// if the second tap never comes — same sweep scr_settings.c runs for its
// three confirm rows, just watching one flag instead of an id table.
static void confirm_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_ota_armed && lv_tick_elaps(s_armed_at_ms) >= CONFIRM_WINDOW_MS)
        confirm_disarm();
}

// True if this tap lands within a prior tap's window (i.e. fire the install now).
static bool confirm_tap(void)
{
    if (s_ota_armed && lv_tick_elaps(s_armed_at_ms) < CONFIRM_WINDOW_MS) {
        confirm_disarm();
        return true;
    }
    confirm_disarm();
    s_ota_armed = true;
    s_armed_at_ms = lv_tick_get();
    if (s_val_ota) lv_label_set_text(s_val_ota, "Tap again to confirm");
    return false;
}

/* ---- row actions ----------------------------------------------------------*/

// Row 0 on every menu sub-screen (see scr_settings.c): the right-swipe still
// works, but it isn't discoverable on its own.
static void row_back_cb(lv_event_t *e)
{
    (void)e;
    dial_haptics_play(HAPTIC_TICK);
    ui_router_go(SCR_MENU, NULL, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
}

// Software update (M6). Behavior depends on the worker's last-known status:
//  - CHECKING/DOWNLOADING: tap is a no-op (screen stays navigable, just not
//    this row) — nothing to confirm or retry mid-flight.
//  - AVAILABLE: tap-twice-to-confirm, since this reboots the device.
//  - IDLE/FAILED/READY_REBOOT: a single tap (re)runs the check — cheap and
//    non-destructive, so no confirm needed (also how a FAILED row retries).
static void row_ota_cb(lv_event_t *e)
{
    (void)e;
    app_state_t st;
    dial_state_get(&st);
    switch ((dial_ota_status_t)st.ota.status) {
    case OTA_CHECKING:
    case OTA_DOWNLOADING:
        return;
    case OTA_AVAILABLE: {
        if (!confirm_tap()) return;
        dial_haptics_play(HAPTIC_CONFIRM);
        app_cmd_t cmd = { .kind = CMD_OTA_APPLY };
        dial_cmd_post(&cmd);
        return;
    }
    default:   // OTA_IDLE, OTA_FAILED, OTA_READY_REBOOT
        dial_haptics_play(HAPTIC_TICK);
        app_cmd_t cmd = { .kind = CMD_OTA_CHECK };
        dial_cmd_post(&cmd);
        return;
    }
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

// Renders the Software update row's value (+ the FAILED-only error line) from
// the worker-owned app_state_t.ota mirror. Skipped entirely while a confirm
// tap is armed: confirm_tap() already wrote "Tap again to confirm" into this
// label, and a routine state commit landing mid-window (a background poll,
// say) must not blow that away before the second tap arrives.
static void render_ota_row(const app_state_t *st)
{
    if (!s_val_ota || s_ota_armed) return;

    const dial_palette_t *pal = PAL();
    lv_obj_set_style_text_color(s_val_ota, pal->ink_secondary, 0);
    if (s_ota_err_lbl) lv_label_set_text(s_ota_err_lbl, "");

    char buf[48];
    switch ((dial_ota_status_t)st->ota.status) {
    case OTA_CHECKING:
        lv_label_set_text(s_val_ota, "Checking...");
        break;
    case OTA_AVAILABLE:
        snprintf(buf, sizeof(buf), "v%s available - tap to install", st->ota.latest);
        lv_label_set_text(s_val_ota, buf);
        break;
    case OTA_DOWNLOADING:
        snprintf(buf, sizeof(buf), "Downloading %d%%", st->ota.progress_pct);
        lv_label_set_text(s_val_ota, buf);
        break;
    case OTA_READY_REBOOT:
        lv_label_set_text(s_val_ota, "Restarting...");
        break;
    case OTA_FAILED:
        lv_label_set_text(s_val_ota, "Update failed");
        lv_obj_set_style_text_color(s_val_ota, pal->warning, 0);
        if (s_ota_err_lbl) {
            lv_label_set_text(s_ota_err_lbl, st->ota.err);
            lv_obj_set_style_text_color(s_ota_err_lbl, pal->warning, 0);
        }
        break;
    case OTA_IDLE:
    default: {
        const esp_app_desc_t *desc = esp_app_get_description();
        snprintf(buf, sizeof(buf), "v%s", desc->version);
        lv_label_set_text(s_val_ota, buf);
        break;
    }
    }
}

/* ---- vtable ----------------------------------------------------------------*/

static void create(lv_obj_t *scr, void *arg)
{
    (void)arg;
    s_ota_armed = false;
    const dial_palette_t *pal = PAL();
    lv_obj_set_style_bg_color(scr, pal->bg, 0);

    s_list = dial_list_create(scr, ROW_H);

    make_row(s_list, LV_SYMBOL_LEFT "  Back", row_back_cb, NULL);

    // Unlike every other row's value ("v1.0.1", "--", ...), this one carries
    // worker-driven prose that can run long ("Tap again to confirm", "v1.2.3
    // available - tap to install", the ~40-char dial_ota_set_blocked reasons)
    // — too long to sit beside "Software update" on one line without running
    // into it (owner-reported overlap). So this row alone drops make_row's
    // label-left/value-right split and stacks three left-aligned lines
    // instead, each capped to the row's own content width with LONG_DOT so a
    // pathological string ellipsizes rather than overlapping anything.
    lv_obj_t *ota_row = make_row(s_list, "Software update", row_ota_cb, &s_val_ota);
    lv_obj_t *ota_lbl = lv_obj_get_child(ota_row, 0);
    lv_obj_align(ota_lbl, LV_ALIGN_LEFT_MID, 0, -20);

    lv_obj_set_width(s_val_ota, LV_PCT(100));
    lv_label_set_long_mode(s_val_ota, LV_LABEL_LONG_DOT);
    lv_obj_align(s_val_ota, LV_ALIGN_LEFT_MID, 0, 6);

    s_ota_err_lbl = lv_label_create(ota_row);
    lv_obj_set_style_text_font(s_ota_err_lbl, &lv_font_montserrat_12, 0);
    lv_label_set_text(s_ota_err_lbl, "");
    lv_obj_set_width(s_ota_err_lbl, LV_PCT(100));
    lv_label_set_long_mode(s_ota_err_lbl, LV_LABEL_LONG_DOT);
    lv_obj_align(s_ota_err_lbl, LV_ALIGN_LEFT_MID, 0, 26);

    lv_obj_t *fw_val, *idf_val;
    make_row(s_list, "Firmware", NULL, &fw_val);
    make_row(s_list, "IDF",      NULL, &idf_val);
    make_row(s_list, "Serial",   NULL, &s_val_serial);

    const esp_app_desc_t *desc = esp_app_get_description();
    char fw[36];
    snprintf(fw, sizeof(fw), "v%s", desc->version);
    lv_label_set_text(fw_val, fw);
    lv_label_set_text(idf_val, desc->idf_ver);
    lv_label_set_text(s_val_serial, "--");   // filled from the state snapshot on first on_state

    // Created AFTER the list so it draws over rows scrolling beneath it.
    s_title_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_title_lbl, &lv_font_montserrat_16, 0);
    lv_label_set_text(s_title_lbl, "ABOUT");
    lv_obj_align(s_title_lbl, LV_ALIGN_CENTER, 0, 64 - CY);

    apply_palette(scr);
    dial_list_settle(s_list, 1);   // open on "Software update", not on Back
    s_confirm_timer = lv_timer_create(confirm_timer_cb, 250, NULL);
}

static void destroy(void)
{
    if (s_confirm_timer) { lv_timer_del(s_confirm_timer); s_confirm_timer = NULL; }
    s_title_lbl = NULL;
    s_list = NULL;
    s_val_ota = NULL;
    s_ota_err_lbl = NULL;
    s_val_serial = NULL;
    s_ota_armed = false;
}

static void on_state(const app_state_t *st)
{
    if (!s_list) return;
    apply_palette(lv_obj_get_parent(s_list));
    render_ota_row(st);
    lv_label_set_text(s_val_serial, st->serial[0] ? st->serial : "--");
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
    ui_router_go(SCR_MENU, NULL, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
    return true;
}

const ui_screen_t scr_about = {
    .create = create, .destroy = destroy, .on_state = on_state,
    .on_knob = on_knob, .on_gesture = on_gesture,
};
