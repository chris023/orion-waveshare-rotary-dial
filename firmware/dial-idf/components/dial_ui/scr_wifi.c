/*
 * SCR_WIFI — Wi-Fi status + change-network sub-screen, reached from the menu
 * face (SCR_MENU). Read-only network/IP/signal rows plus "Change network".
 *
 * esp_wifi_sta_get_ap_info() is a documented thread-safe getter: it's a quick
 * IPC into the Wi-Fi driver task for the currently-associated AP's record,
 * not network I/O, so calling it straight from the LVGL task doesn't violate
 * the worker-owns-networking rule the rest of the UI follows.
 *
 * "Change network" opens a full-screen CONFIRM MODE rather than the value-
 * label tap-twice pattern the destructive Settings rows use: at Mont 16 the
 * "tap again" prompt collided with the Mont 24 row label (owner saw
 * overlapping text), and this action deserves a sentence of explanation
 * anyway — it reboots the dial into the SoftAP portal, where scr_setup's QR
 * gets the phone onto the setup network. The command that gets it there is
 * CMD_WIFI_RESET -> dial_net_request_setup(), which BOTH forgets the creds and
 * latches a flag: forgetting alone was not enough, because the dev seed would
 * re-inject the build's own network on the next boot and the dial would
 * silently rejoin it (the bug the owner hit — "it just resets the unit").
 */
#include "ui_screens_internal.h"
#include "dial_haptics.h"
#include "dial_list.h"
#include "esp_wifi.h"
#include "dial_wifi.h"

#define CX 180
#define CY 180
#define ROW_H 76

static lv_obj_t *s_title_lbl;
static lv_obj_t *s_list;
static lv_obj_t *s_val_network, *s_val_ip, *s_val_signal;
static lv_obj_t *s_confirm;                 // confirm-mode container (hidden by default)
static lv_obj_t *s_confirm_body, *s_confirm_btn, *s_confirm_btn_lbl;
static lv_obj_t *s_cancel_btn, *s_cancel_btn_lbl;
static lv_timer_t *s_poll_timer;
static bool s_confirm_mode;

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

/* ---- confirm mode --------------------------------------------------------*/

static void set_confirm_mode(bool on)
{
    s_confirm_mode = on;
    if (on) {
        lv_obj_add_flag(s_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_confirm, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_title_lbl, "CHANGE WI-FI");
    } else {
        lv_obj_clear_flag(s_list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_confirm, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_title_lbl, "WI-FI");
    }
}

static void row_change_network_cb(lv_event_t *e)
{
    (void)e;
    dial_haptics_play(HAPTIC_TICK);
    set_confirm_mode(true);
}

// The only irreversible tap on this screen: reboots into the setup portal.
static void confirm_btn_cb(lv_event_t *e)
{
    (void)e;
    dial_haptics_play(HAPTIC_CONFIRM);
    lv_label_set_text(s_confirm_btn_lbl, "Restarting...");
    app_cmd_t cmd = { .kind = CMD_WIFI_RESET };
    dial_cmd_post(&cmd);
}

// The way out. The right-swipe cancels too, but this screen's own reason for
// existing is that a swipe isn't discoverable — leaving the reboot button as
// the only visible control would push an unsure user straight into it. The
// tap-twice pattern this view replaced at least timed out on its own; a modal
// with no visible exit would not.
static void cancel_btn_cb(lv_event_t *e)
{
    (void)e;
    dial_haptics_play(HAPTIC_TICK);
    set_confirm_mode(false);
}

static void row_back_cb(lv_event_t *e)
{
    (void)e;
    dial_haptics_play(HAPTIC_TICK);
    ui_router_go(SCR_MENU, NULL, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
}

/* ---- palette --------------------------------------------------------------*/

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

    lv_obj_set_style_text_color(s_confirm_body, pal->ink_secondary, 0);
    lv_obj_set_style_bg_color(s_confirm_btn, pal->surface, 0);
    lv_obj_set_style_border_color(s_confirm_btn, pal->track, 0);
    lv_obj_set_style_text_color(s_confirm_btn_lbl, pal->ink_primary, 0);
    lv_obj_set_style_border_color(s_cancel_btn, pal->track, 0);
    lv_obj_set_style_text_color(s_cancel_btn_lbl, pal->ink_secondary, 0);
}

/* ---- vtable ----------------------------------------------------------------*/

static void create(lv_obj_t *scr, void *arg)
{
    (void)arg;
    s_confirm_mode = false;
    const dial_palette_t *pal = PAL();
    lv_obj_set_style_bg_color(scr, pal->bg, 0);

    s_list = dial_list_create(scr, ROW_H);

    make_row(s_list, LV_SYMBOL_LEFT "  Back", row_back_cb, NULL);
    make_row(s_list, "Network", NULL, &s_val_network);
    make_row(s_list, "IP",      NULL, &s_val_ip);
    make_row(s_list, "Signal",  NULL, &s_val_signal);
    make_row(s_list, "Change network", row_change_network_cb, NULL);

    // Confirm view: a sibling of the list, shown in its place (not over it).
    s_confirm = lv_obj_create(scr);
    lv_obj_set_size(s_confirm, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(s_confirm, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_confirm, 0, 0);
    lv_obj_clear_flag(s_confirm, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_confirm, LV_OBJ_FLAG_HIDDEN);

    // 240px wide keeps the wrapped text inside the round panel's chord at the
    // y-band it occupies; centered so no line ends near the bezel.
    s_confirm_body = lv_label_create(s_confirm);
    lv_obj_set_width(s_confirm_body, 240);
    lv_label_set_long_mode(s_confirm_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_confirm_body, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(s_confirm_body, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_confirm_body,
                      "The dial restarts and shows a QR code for joining its setup network.");
    lv_obj_align(s_confirm_body, LV_ALIGN_CENTER, 0, 126 - CY);

    s_confirm_btn = dial_btn_create(s_confirm);
    lv_obj_set_size(s_confirm_btn, 200, 88);   // primary action: >=88px (round-screen DLS)
    lv_obj_set_style_radius(s_confirm_btn, 44, 0);
    lv_obj_set_style_border_width(s_confirm_btn, 1, 0);
    lv_obj_align(s_confirm_btn, LV_ALIGN_CENTER, 0, 210 - CY);
    lv_obj_add_event_cb(s_confirm_btn, confirm_btn_cb, LV_EVENT_CLICKED, NULL);

    s_confirm_btn_lbl = lv_label_create(s_confirm_btn);
    lv_obj_set_style_text_font(s_confirm_btn_lbl, &lv_font_montserrat_20, 0);
    lv_label_set_text(s_confirm_btn_lbl, "Continue");
    lv_obj_center(s_confirm_btn_lbl);

    // Cancel sits below Continue as a quieter (transparent) pill: same 200px
    // width so the pair reads as one stack, and its rounded ends keep it clear
    // of the bezel at this y-band.
    s_cancel_btn = dial_btn_create(s_confirm);
    lv_obj_set_size(s_cancel_btn, 200, 72);
    lv_obj_set_style_radius(s_cancel_btn, 36, 0);
    lv_obj_set_style_bg_opa(s_cancel_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_cancel_btn, 1, 0);
    lv_obj_align(s_cancel_btn, LV_ALIGN_CENTER, 0, 302 - CY);
    lv_obj_add_event_cb(s_cancel_btn, cancel_btn_cb, LV_EVENT_CLICKED, NULL);

    s_cancel_btn_lbl = lv_label_create(s_cancel_btn);
    lv_obj_set_style_text_font(s_cancel_btn_lbl, &lv_font_montserrat_16, 0);
    lv_label_set_text(s_cancel_btn_lbl, "Cancel");
    lv_obj_center(s_cancel_btn_lbl);

    // Created AFTER the list so it draws over rows scrolling beneath it.
    s_title_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_title_lbl, &lv_font_montserrat_16, 0);
    lv_label_set_text(s_title_lbl, "WI-FI");
    lv_obj_align(s_title_lbl, LV_ALIGN_CENTER, 0, 64 - CY);

    apply_palette(scr);
    refresh_values();
    dial_list_settle(s_list, 1);   // open on "Network", not on Back

    s_poll_timer = lv_timer_create(poll_timer_cb, 2000, NULL);
}

static void destroy(void)
{
    if (s_poll_timer) { lv_timer_del(s_poll_timer); s_poll_timer = NULL; }
    s_list = NULL;
    s_title_lbl = NULL;
    s_val_network = s_val_ip = s_val_signal = NULL;
    s_confirm = s_confirm_body = s_confirm_btn = s_confirm_btn_lbl = NULL;
    s_cancel_btn = s_cancel_btn_lbl = NULL;
    s_confirm_mode = false;
}

// Nothing here is derived from app_state_t (Wi-Fi status lives in the driver,
// not the state store) — on_state just re-applies the palette on a day/night
// flip.
static void on_state(const app_state_t *st)
{
    (void)st;
    if (!s_list) return;
    apply_palette(lv_obj_get_parent(s_list));
}

static bool on_knob(int detents)
{
    if (!s_list || detents == 0 || s_confirm_mode) return false;
    int r = dial_list_knob(s_list, detents);
    if (r) dial_haptics_play(r > 0 ? HAPTIC_TICK : HAPTIC_STOP);
    return true;
}

static bool on_gesture(lv_dir_t dir)
{
    // While the confirm view is up, EVERY direction is consumed, not just the
    // one that means "cancel". An unconsumed gesture is not swallowed by the
    // router (ui_router.c only calls lv_indev_wait_release when on_gesture
    // returns true), and LVGL 8.4 then still delivers CLICKED on release to the
    // object the touch started on — so a thumb that drifts up/down/left while
    // pressing Continue would fire the Wi-Fi reset without a deliberate tap.
    // scr_tonight's picker mode swallows all four directions for exactly this
    // reason.
    if (s_confirm_mode) {
        if (dir == LV_DIR_RIGHT) set_confirm_mode(false);   // right-swipe = cancel
        return true;
    }
    if (dir != LV_DIR_RIGHT) return false;
    ui_router_go(SCR_MENU, NULL, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
    return true;
}

const ui_screen_t scr_wifi = {
    .create = create, .destroy = destroy, .on_state = on_state,
    .on_knob = on_knob, .on_gesture = on_gesture,
};
