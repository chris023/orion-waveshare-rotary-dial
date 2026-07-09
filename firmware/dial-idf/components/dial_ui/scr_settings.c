/*
 * SCR_SETTINGS — full-screen scrollable settings list. Reached from
 * scr_quick.c's "Settings" row (arg: the zone_idx_t to return to on swipe
 * down, same convention as SCR_BOOST/SCR_QUICK).
 *
 * Rows >=72px tall: label (Mont 24) left, value (Mont 16) right-aligned.
 * The knob scrolls the list (lv_obj_scroll_by); a finger drag scrolls it too
 * (ordinary LVGL flex/scroll behavior — nothing special needed for that).
 * Tap activates a row. The three destructive rows (re-link/Wi-Fi reset/
 * factory reset) use a tap-twice-within-3s confirm pattern instead of firing
 * immediately.
 */
#include "ui_screens_internal.h"
#include "dial_haptics.h"
#include "dial_ota.h"
#include "esp_app_desc.h"

#define ROW_H          76
#define CONFIRM_WINDOW_MS 3000

static zone_idx_t s_zone = ZONE_A;   // side to return to on swipe-down

static lv_obj_t *s_list;
static lv_obj_t *s_val_my_side, *s_val_units, *s_val_haptics;
static lv_obj_t *s_ota_err_lbl;   // second line under the Software update row, FAILED only

// CONFIRM_OTA reuses the tap-twice pattern for "install now" (design-spec:
// destructive-ish enough to want a confirm, since it reboots) — its value
// label IS the Software update row's value (s_val_confirm[CONFIRM_OTA]),
// exactly like the three rows below share their row's value label with
// their confirm state.
typedef enum { CONFIRM_RELINK = 0, CONFIRM_WIFI, CONFIRM_FACTORY, CONFIRM_OTA, CONFIRM_COUNT } confirm_id_t;
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
    lv_obj_set_style_pad_hor(row, 20, 0);
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

static void row_my_side_cb(lv_event_t *e)
{
    (void)e;
    ui_router_go(SCR_SIDEPICK, NULL, LV_SCR_LOAD_ANIM_NONE);
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

static void row_wifi_reset_cb(lv_event_t *e)
{
    (void)e;
    if (!confirm_tap(CONFIRM_WIFI)) return;
    dial_haptics_play(HAPTIC_CONFIRM);
    app_cmd_t cmd = { .kind = CMD_WIFI_RESET };
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

// Software update (M6). Behavior depends on the worker's last-known status:
//  - CHECKING/DOWNLOADING: disabled (row stays visible, tap is a no-op) —
//    the design spec calls for the screen to stay navigable, just not this
//    row's tap during those two states.
//  - AVAILABLE: tap-twice-to-confirm (a real install + reboot), same pattern
//    as the destructive rows below.
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
        if (!confirm_tap(CONFIRM_OTA)) return;
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

/* ---- palette --------------------------------------------------------------*/

static void apply_palette(lv_obj_t *scr)
{
    const dial_palette_t *pal = PAL();
    lv_obj_set_style_bg_color(scr, pal->bg, 0);

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

// Renders the Software update row's value (+ the FAILED-only error line)
// from the worker-owned app_state_t.ota mirror. Skipped entirely while a
// CONFIRM_OTA tap is armed: confirm_tap() already wrote "Tap again to
// confirm" into this same label, and a routine state commit landing mid-
// window (a background poll, say) must not blow that away before the
// second tap — exactly the reasoning nav_policy uses to keep this whole
// screen sticky in main.c.
static void render_ota_row(const app_state_t *st)
{
    lv_obj_t *val = s_val_confirm[CONFIRM_OTA];
    if (!val || s_armed == CONFIRM_OTA) return;

    const dial_palette_t *pal = PAL();
    lv_obj_set_style_text_color(val, pal->ink_secondary, 0);
    if (s_ota_err_lbl) lv_label_set_text(s_ota_err_lbl, "");

    char buf[48];
    switch ((dial_ota_status_t)st->ota.status) {
    case OTA_CHECKING:
        lv_label_set_text(val, "Checking...");
        break;
    case OTA_AVAILABLE:
        snprintf(buf, sizeof(buf), "v%s available - tap to install", st->ota.latest);
        lv_label_set_text(val, buf);
        break;
    case OTA_DOWNLOADING:
        snprintf(buf, sizeof(buf), "Downloading %d%%", st->ota.progress_pct);
        lv_label_set_text(val, buf);
        break;
    case OTA_READY_REBOOT:
        lv_label_set_text(val, "Restarting...");
        break;
    case OTA_FAILED:
        lv_label_set_text(val, "Update failed");
        lv_obj_set_style_text_color(val, pal->warning, 0);
        if (s_ota_err_lbl) {
            lv_label_set_text(s_ota_err_lbl, st->ota.err);
            lv_obj_set_style_text_color(s_ota_err_lbl, pal->warning, 0);
        }
        break;
    case OTA_IDLE:
    default: {
        const esp_app_desc_t *desc = esp_app_get_description();
        snprintf(buf, sizeof(buf), "v%s", desc->version);
        lv_label_set_text(val, buf);
        break;
    }
    }
}

/* ---- vtable ----------------------------------------------------------------*/

static void create(lv_obj_t *scr, void *arg)
{
    s_zone = (zone_idx_t)(uintptr_t)arg;
    s_armed = CONFIRM_COUNT;
    const dial_palette_t *pal = PAL();
    lv_obj_set_style_bg_color(scr, pal->bg, 0);

    s_list = lv_obj_create(scr);
    lv_obj_set_size(s_list, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_style_pad_row(s_list, 0, 0);
    lv_obj_set_style_pad_top(s_list, 28, 0);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);

    make_row(s_list, "My side",       row_my_side_cb,      &s_val_my_side);
    make_row(s_list, "Units",         row_units_cb,         &s_val_units);
    make_row(s_list, "Haptics",       row_haptics_cb,       &s_val_haptics);
    make_row(s_list, "Re-link Orion", row_relink_cb,        &s_val_confirm[CONFIRM_RELINK]);
    make_row(s_list, "Wi-Fi reset",   row_wifi_reset_cb,    &s_val_confirm[CONFIRM_WIFI]);
    make_row(s_list, "Factory reset", row_factory_reset_cb, &s_val_confirm[CONFIRM_FACTORY]);

    lv_obj_t *ota_row = make_row(s_list, "Software update", row_ota_cb, &s_val_confirm[CONFIRM_OTA]);
    s_ota_err_lbl = lv_label_create(ota_row);
    lv_obj_set_style_text_font(s_ota_err_lbl, &lv_font_montserrat_12, 0);
    lv_label_set_text(s_ota_err_lbl, "");
    lv_obj_align(s_ota_err_lbl, LV_ALIGN_RIGHT_MID, 0, 22);

    lv_obj_t *fw_val, *idf_val;
    make_row(s_list, "Firmware", NULL, &fw_val);
    make_row(s_list, "IDF",      NULL, &idf_val);
    const esp_app_desc_t *desc = esp_app_get_description();
    char fw[36];
    snprintf(fw, sizeof(fw), "v%s", desc->version);
    lv_label_set_text(fw_val, fw);
    lv_label_set_text(idf_val, desc->idf_ver);

    apply_palette(scr);
    s_confirm_timer = lv_timer_create(confirm_timer_cb, 250, NULL);
}

static void destroy(void)
{
    if (s_confirm_timer) { lv_timer_del(s_confirm_timer); s_confirm_timer = NULL; }
    s_list = NULL;
    s_val_my_side = s_val_units = s_val_haptics = NULL;
    s_ota_err_lbl = NULL;
    for (int i = 0; i < CONFIRM_COUNT; i++) s_val_confirm[i] = NULL;
    s_armed = CONFIRM_COUNT;
}

static void on_state(const app_state_t *st)
{
    if (!s_list) return;
    apply_palette(lv_obj_get_parent(s_list));
    render_ota_row(st);

    const char *name = st->zones[st->ui_zone].user_name;
    char side_buf[24];
    if (name[0]) strlcpy(side_buf, name, sizeof(side_buf));
    else         strlcpy(side_buf, st->ui_zone == ZONE_A ? "Right" : "Left", sizeof(side_buf));
    lv_label_set_text(s_val_my_side, side_buf);

    lv_label_set_text(s_val_units, st->units_c ? "\xC2\xB0" "C" : "\xC2\xB0" "F");
    lv_label_set_text(s_val_haptics, st->haptics_enabled ? "On" : "Off");
}

// The knob scrolls the list instead of adjusting a value.
static bool on_knob(int detents)
{
    if (!s_list || detents == 0) return false;
    lv_obj_scroll_by(s_list, 0, -detents * 24, LV_ANIM_ON);
    return true;
}

static bool on_gesture(lv_dir_t dir)
{
    if (dir != LV_DIR_BOTTOM) return false;
    ui_router_go(SCR_DIAL, (void *)(uintptr_t)s_zone, LV_SCR_LOAD_ANIM_NONE);
    return true;
}

const ui_screen_t scr_settings = {
    .create = create, .destroy = destroy, .on_state = on_state,
    .on_knob = on_knob, .on_gesture = on_gesture,
};
