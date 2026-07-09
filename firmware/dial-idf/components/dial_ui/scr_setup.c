/*
 * Setup screens: SCR_WIFI_PORTAL (join the dial's AP) and SCR_OAUTH_QR
 * (scan the Orion authorize URL). Both are QR-centric white screens so a
 * phone camera locks on quickly.
 */
#include "ui_screens_internal.h"

/* ---- Wi-Fi portal ------------------------------------------------------ */

static lv_obj_t *s_wifi_qr, *s_wifi_lbl;

static void wifi_create(lv_obj_t *scr, void *arg)
{
    (void)arg;
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);

    s_wifi_lbl = lv_label_create(scr);
    lv_obj_set_width(s_wifi_lbl, 280);
    lv_label_set_long_mode(s_wifi_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_wifi_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_wifi_lbl, lv_color_hex(0x0b6a4a), 0);
    lv_obj_align(s_wifi_lbl, LV_ALIGN_TOP_MID, 0, 40);

    s_wifi_qr = lv_qrcode_create(scr, 190, lv_color_black(), lv_color_white());
    lv_obj_align(s_wifi_qr, LV_ALIGN_CENTER, 0, 24);
}

static void wifi_destroy(void) { s_wifi_qr = s_wifi_lbl = NULL; }

static void wifi_on_state(const app_state_t *st)
{
    if (!s_wifi_qr || !st->ap_ssid[0]) return;
    // WIFI: join-QR — phones join the open setup AP straight from camera.
    char qr[80];
    snprintf(qr, sizeof(qr), "WIFI:T:nopass;S:%s;;", st->ap_ssid);
    lv_qrcode_update(s_wifi_qr, qr, strlen(qr));
    char msg[96];
    snprintf(msg, sizeof(msg), "Scan to connect the dial\nto Wi-Fi \"%s\"", st->ap_ssid);
    lv_label_set_text(s_wifi_lbl, msg);
}

const ui_screen_t scr_wifi_portal = {
    .create = wifi_create, .destroy = wifi_destroy, .on_state = wifi_on_state,
};

/* ---- Orion OAuth QR ---------------------------------------------------- */

static lv_obj_t *s_oauth_qr, *s_oauth_lbl;

static void oauth_create(lv_obj_t *scr, void *arg)
{
    (void)arg;
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);

    s_oauth_lbl = lv_label_create(scr);
    lv_label_set_text(s_oauth_lbl, "Scan to link your dial");
    lv_obj_set_style_text_color(s_oauth_lbl, lv_color_hex(0x0b6a4a), 0);
    lv_obj_align(s_oauth_lbl, LV_ALIGN_TOP_MID, 0, 44);

    s_oauth_qr = lv_qrcode_create(scr, 220, lv_color_black(), lv_color_white());
    lv_obj_center(s_oauth_qr);
}

static void oauth_destroy(void) { s_oauth_qr = s_oauth_lbl = NULL; }

static void oauth_on_state(const app_state_t *st)
{
    if (!s_oauth_qr || !st->oauth_url[0]) return;
    lv_qrcode_update(s_oauth_qr, st->oauth_url, strlen(st->oauth_url));
}

const ui_screen_t scr_oauth_qr = {
    .create = oauth_create, .destroy = oauth_destroy, .on_state = oauth_on_state,
};
