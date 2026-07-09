/*
 * SCR_WELCOME — onboarding splash for a genuinely fresh device (no stored
 * Wi-Fi credentials at boot; dial_state's fresh_device flag, set once in
 * app_main). main.c's nav_policy pins this screen through the earliest
 * connection phases (BOOT/WIFI_CONNECTING/WIFI_PORTAL).
 *
 * Any tap or knob detent dismisses it: the worker is already running
 * underneath (Wi-Fi bring-up, OAuth, ...), so "dismiss" just means flipping
 * `welcomed` — nav_policy stops pinning this screen on the next dispatch
 * tick and falls through to whatever the connection phase naturally shows
 * next (Wi-Fi portal QR, Orion link QR, ...). This screen never navigates
 * anywhere itself.
 *
 * Note: the knob decoder isn't initialized until deep in the worker's
 * steady-state handoff (main.c's knob_init(), after PH_READY — see that
 * comment for why it isn't started earlier), so in practice only a tap
 * reliably dismisses this screen during the early phases it's shown in. The
 * on_knob hook below is still wired per spec and is harmless either way.
 */
#include "ui_screens_internal.h"

static lv_obj_t *s_title, *s_sub;

static void dismiss_cb(lv_event_t *e) { (void)e; dial_state_set_welcomed(); }

static void create(lv_obj_t *scr, void *arg)
{
    (void)arg;
    const dial_palette_t *pal = PAL();
    lv_obj_set_style_bg_color(scr, pal->bg, 0);
    lv_obj_add_event_cb(scr, dismiss_cb, LV_EVENT_CLICKED, NULL);

    s_title = lv_label_create(scr);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_title, pal->ink_primary, 0);
    lv_label_set_text(s_title, "ORION DIAL");
    lv_obj_align(s_title, LV_ALIGN_CENTER, 0, -16);

    s_sub = lv_label_create(scr);
    lv_obj_set_width(s_sub, 260);
    lv_label_set_long_mode(s_sub, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_sub, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_sub, pal->ink_secondary, 0);
    lv_label_set_text(s_sub, "Turn the knob or tap to begin");
    lv_obj_align(s_sub, LV_ALIGN_CENTER, 0, 20);
}

static void destroy(void) { s_title = s_sub = NULL; }

static void on_state(const app_state_t *st)
{
    (void)st;
    if (!s_title) return;
    const dial_palette_t *pal = PAL();
    lv_obj_t *scr = lv_obj_get_parent(s_title);
    lv_obj_set_style_bg_color(scr, pal->bg, 0);
    lv_obj_set_style_text_color(s_title, pal->ink_primary, 0);
    lv_obj_set_style_text_color(s_sub, pal->ink_secondary, 0);
}

static bool on_knob(int detents)
{
    (void)detents;
    dial_state_set_welcomed();
    return true;
}

const ui_screen_t scr_welcome = {
    .create = create, .destroy = destroy, .on_state = on_state, .on_knob = on_knob,
};
