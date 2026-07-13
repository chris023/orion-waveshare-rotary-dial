/*
 * SCR_WIFI — Wi-Fi status + change-network sub-screen, reached from the new
 * menu face (SCR_MENU). Read-only network/IP/signal rows plus a single
 * destructive action ("Change network") that forgets the stored credentials
 * so the next boot re-runs the captive portal (dial_net_forget's contract,
 * driven here via CMD_WIFI_RESET so the worker task owns the NVS write —
 * this screen never touches NVS or the Wi-Fi driver itself).
 *
 * esp_wifi_sta_get_ap_info() is a documented thread-safe getter: it's a quick
 * IPC into the Wi-Fi driver task for the currently-associated AP's record,
 * not network I/O, so calling it straight from the LVGL task doesn't violate
 * the worker-owns-networking rule the rest of the UI follows.
 */
#include "ui_screens_internal.h"
#include "dial_haptics.h"
#include "dial_list.h"
#include "esp_wifi.h"
#include "dial_wifi.h"

#define CY 180   // title's fixed-anchor slot is expressed relative to center
#define ROW_H          76
#define CONFIRM_WINDOW_MS 3000

static lv_obj_t *s_title_lbl;
static lv_obj_t *s_list;
static lv_obj_t *s_val_network, *s_val_ip, *s_val_signal, *s_val_change;

// Single-row confirm state (scr_settings' confirm_id_t collapses to a bool
// here — this screen has exactly one confirmable row).
static bool     s_armed;
static uint32_t s_armed_at_ms;
static lv_timer_t *s_confirm_timer;
static lv_timer_t *s_poll_timer;

/* ---- row factory (same 76px/label-left/value-right idiom as scr_settings) */

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

/* ---- info rows: refreshed off a getter, never network I/O ---------------*/

static const char *signal_word(int8_t rssi)
{
    if (rssi >= -60) return "Strong";
    if (rssi >= -70) return "Good";
    return "Weak";
}

static void refresh_values(void)
{
    if (!dial_wifi_is_connected()) {
        lv_label_set_text(s_val_network, "Not connected");
        lv_label_set_text(s_val_ip, "--");
        lv_label_set_text(s_val_signal, "--");
        return;
    }

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        lv_label_set_text(s_val_network, (const char *)ap.ssid);
        char sig[24];
        snprintf(sig, sizeof(sig), "%s %d dBm", signal_word(ap.rssi), (int)ap.rssi);
        lv_label_set_text(s_val_signal, sig);
    }

    char ip[24];
    if (dial_net_ip(ip, sizeof(ip))) lv_label_set_text(s_val_ip, ip);
    else                             lv_label_set_text(s_val_ip, "--");
}

static void poll_timer_cb(lv_timer_t *t) { (void)t; refresh_values(); }

/* ---- change-network confirm (tap-twice-within-3s, scr_settings' pattern) */

static void confirm_disarm(void)
{
    if (s_armed) lv_label_set_text(s_val_change, "");
    s_armed = false;
}

// Ticks while armed so the "tap again" prompt reverts on its own if the
// window lapses without a second tap — same watchdog scr_settings runs.
static void confirm_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_armed && lv_tick_elaps(s_armed_at_ms) >= CONFIRM_WINDOW_MS) confirm_disarm();
}

static void row_change_network_cb(lv_event_t *e)
{
    (void)e;
    if (s_armed && lv_tick_elaps(s_armed_at_ms) < CONFIRM_WINDOW_MS) {
        confirm_disarm();
        dial_haptics_play(HAPTIC_CONFIRM);
        app_cmd_t cmd = { .kind = CMD_WIFI_RESET };
        dial_cmd_post(&cmd);
        return;
    }
    confirm_disarm();
    s_armed = true;
    s_armed_at_ms = lv_tick_get();
    lv_label_set_text(s_val_change, "Tap again to confirm");
}

/* ---- palette --------------------------------------------------------------*/

static void apply_palette(lv_obj_t *scr, lv_obj_t *title)
{
    const dial_palette_t *pal = PAL();
    lv_obj_set_style_bg_color(scr, pal->bg, 0);
    lv_obj_set_style_text_color(title, pal->ink_secondary, 0);

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
    s_armed = false;
    const dial_palette_t *pal = PAL();
    lv_obj_set_style_bg_color(scr, pal->bg, 0);

    s_list = dial_list_create(scr, ROW_H);

    make_row(s_list, "Network", NULL, &s_val_network);
    make_row(s_list, "IP",      NULL, &s_val_ip);
    make_row(s_list, "Signal",  NULL, &s_val_signal);
    make_row(s_list, "Change network", row_change_network_cb, &s_val_change);

    // Created AFTER the list so it draws over rows scrolling beneath it.
    s_title_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_title_lbl, &lv_font_montserrat_16, 0);
    lv_label_set_text(s_title_lbl, "WI-FI");
    lv_obj_align(s_title_lbl, LV_ALIGN_CENTER, 0, 64 - CY);

    apply_palette(scr, s_title_lbl);
    refresh_values();
    dial_list_settle(s_list);

    s_confirm_timer = lv_timer_create(confirm_timer_cb, 250, NULL);
    s_poll_timer = lv_timer_create(poll_timer_cb, 2000, NULL);
}

static void destroy(void)
{
    if (s_confirm_timer) { lv_timer_del(s_confirm_timer); s_confirm_timer = NULL; }
    if (s_poll_timer)    { lv_timer_del(s_poll_timer);    s_poll_timer = NULL; }
    s_list = NULL;
    s_title_lbl = NULL;
    s_val_network = s_val_ip = s_val_signal = s_val_change = NULL;
    s_armed = false;
}

// Nothing here is derived from app_state_t (Wi-Fi status lives in the driver,
// not the state store) — on_state just re-applies the palette on a day/night
// flip. The change-network value's "Tap again to confirm" text is never
// touched by apply_palette (it only sets style colors, not label content),
// so an armed confirm survives any state commit that lands mid-window.
static void on_state(const app_state_t *st)
{
    (void)st;
    if (!s_list) return;
    apply_palette(lv_obj_get_parent(s_list), s_title_lbl);
}

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

const ui_screen_t scr_wifi = {
    .create = create, .destroy = destroy, .on_state = on_state,
    .on_knob = on_knob, .on_gesture = on_gesture,
};
