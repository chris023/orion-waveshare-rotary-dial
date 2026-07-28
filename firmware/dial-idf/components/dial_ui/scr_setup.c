/*
 * Setup screens: SCR_WIFI_PORTAL (how to give the dial your Wi-Fi) and
 * SCR_OAUTH_QR (scan the Orion authorize URL — a normal https link, which
 * phone cameras handle perfectly).
 */
#include "ui_screens_internal.h"
#include "dial_haptics.h"

/* ---- Wi-Fi setup ------------------------------------------------------- */
/*
 * This screen used to show a WIFI: QR code, so the camera could join the dial's
 * SoftAP in one tap. It is gone, deliberately.
 *
 * iOS fails that join outright the first time on a network it hasn't recently
 * scanned — reported for years, never explained by Apple, and no AP-side
 * setting fixes it (a manual tap on the SAME AP seconds later works). Espressif
 * say as much themselves: with SoftAP provisioning on iOS, "discovery as well as
 * connection APIs are not available", so the trip to Settings isn't a fallback,
 * it IS the supported path. The QR was trying to skip a step the platform
 * doesn't let you skip, and turned a reliable ten-second task into a minute of
 * failure. No shipping consumer product does it this way.
 *
 * So: name the network, say where to tap, and offer the way out for anyone who
 * would rather not involve a phone at all.
 */

static lv_obj_t *s_wifi_ssid_lbl, *s_wifi_step_lbl, *s_wifi_hint_lbl, *s_wifi_btn, *s_wifi_btn_lbl;

static void wifi_on_dial_cb(lv_event_t *e)
{
    (void)e;
    dial_haptics_play(HAPTIC_TICK);
    ui_router_go(SCR_NETPICK, NULL, LV_SCR_LOAD_ANIM_MOVE_LEFT);
}

static void wifi_create(lv_obj_t *scr, void *arg)
{
    (void)arg;
    const dial_palette_t *pal = PAL();
    lv_obj_set_style_bg_color(scr, pal->bg, 0);

    s_wifi_step_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_wifi_step_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_width(s_wifi_step_lbl, 280);
    lv_label_set_long_mode(s_wifi_step_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_wifi_step_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_wifi_step_lbl, "On your phone, open\nSettings " LV_SYMBOL_RIGHT " Wi-Fi\nand tap");
    lv_obj_align(s_wifi_step_lbl, LV_ALIGN_CENTER, 0, 96 - 180);

    // The one thing they have to find. Big enough to read from across the room,
    // and it is the whole instruction.
    s_wifi_ssid_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_wifi_ssid_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_width(s_wifi_ssid_lbl, 300);
    lv_label_set_long_mode(s_wifi_ssid_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_wifi_ssid_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_wifi_ssid_lbl, "...");
    lv_obj_align(s_wifi_ssid_lbl, LV_ALIGN_CENTER, 0, 166 - 180);

    s_wifi_hint_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_wifi_hint_lbl, &lv_font_montserrat_12, 0);
    lv_label_set_text(s_wifi_hint_lbl, "A setup page opens by itself");
    lv_obj_align(s_wifi_hint_lbl, LV_ALIGN_CENTER, 0, 200 - 180);

    // The no-phone path.
    s_wifi_btn = dial_btn_create(scr);
    lv_obj_set_size(s_wifi_btn, 240, 72);
    lv_obj_set_style_radius(s_wifi_btn, 36, 0);
    lv_obj_set_style_border_width(s_wifi_btn, 1, 0);
    lv_obj_align(s_wifi_btn, LV_ALIGN_CENTER, 0, 272 - 180);
    lv_obj_add_event_cb(s_wifi_btn, wifi_on_dial_cb, LV_EVENT_CLICKED, NULL);

    s_wifi_btn_lbl = lv_label_create(s_wifi_btn);
    lv_obj_set_style_text_font(s_wifi_btn_lbl, &lv_font_montserrat_16, 0);
    lv_label_set_text(s_wifi_btn_lbl, "Set up on the dial");
    lv_obj_center(s_wifi_btn_lbl);
}

static void wifi_destroy(void)
{
    s_wifi_ssid_lbl = s_wifi_step_lbl = s_wifi_hint_lbl = NULL;
    s_wifi_btn = s_wifi_btn_lbl = NULL;
}

static void wifi_on_state(const app_state_t *st)
{
    if (!s_wifi_ssid_lbl) return;
    const dial_palette_t *pal = PAL();
    lv_obj_set_style_bg_color(lv_obj_get_parent(s_wifi_ssid_lbl), pal->bg, 0);
    lv_obj_set_style_text_color(s_wifi_step_lbl, pal->ink_secondary, 0);
    lv_obj_set_style_text_color(s_wifi_ssid_lbl, pal->ink_primary, 0);
    lv_obj_set_style_text_color(s_wifi_hint_lbl, pal->ink_secondary, 0);
    lv_obj_set_style_bg_color(s_wifi_btn, pal->surface, 0);
    lv_obj_set_style_border_color(s_wifi_btn, pal->track, 0);
    lv_obj_set_style_text_color(s_wifi_btn_lbl, pal->ink_primary, 0);

    if (st->ap_ssid[0]) lv_label_set_text(s_wifi_ssid_lbl, st->ap_ssid);
}

const ui_screen_t scr_wifi_portal = {
    .create = wifi_create, .destroy = wifi_destroy, .on_state = wifi_on_state,
};

/* ---- Orion OAuth QR ---------------------------------------------------- */

// The code itself has to stay pure black-on-white no matter the hour — a
// phone camera needs real contrast and an unbroken quiet zone, and the
// dial's near-black night palette can't give it either. So unlike every
// other screen, the card under the QR is hardcoded white; only the screen
// behind it and the caption follow PAL() like everywhere else.
//
// Card padding is 24px on every side — comfortably more than four modules
// at this QR's ~5px module scale (design-spec's own quiet-zone minimum) —
// and the whole card sits just far enough below the caption, and well
// inside the round panel's edge, to render without clipping either side.
#define OAUTH_QR_SIZE    180
#define OAUTH_CARD_PAD    24
#define OAUTH_CARD_SIZE  (OAUTH_QR_SIZE + 2 * OAUTH_CARD_PAD)

/*
 * Linking's last step is an OAuth loopback: the phone approves on Orion's
 * site, and Orion's browser redirect lands back on the DIAL's own LAN
 * address (http://orion-dial-xxxxxx.local/callback). That redirect can only
 * ever arrive over the SAME network the dial is on — cellular data, a guest
 * network, or client-isolated Wi-Fi all silently strand it, leaving the
 * phone spinning forever with nothing on the dial explaining why (a real
 * field incident: an owner lost a long stretch to exactly this). Orion's
 * auth server doesn't support the device-authorization grant, so this LAN
 * dependency can't be engineered away — the fix is naming it, twice:
 *   1) a persistent one-line hint under the QR, always visible;
 *   2) if nothing has come back for a while, a specific, dismissible
 *      explainer restating it — for the very likely case that a user has
 *      simply not scanned yet, this must have a clear close state and must
 *      never permanently sit over the code.
 */
static lv_obj_t *s_oauth_card, *s_oauth_qr, *s_oauth_lbl, *s_oauth_hint_lbl;

// The explainer is a dismissible sheet drawn OVER the card (not squeezed in
// below it): below the card there is only the ~50px strip between its
// bottom edge (y=308) and the round panel's rim, nowhere near enough for
// three lines of body text plus a >=72px close target without either
// shrinking the QR (risking the one thing here that must stay scannable) or
// crowding text out past the visible circle. Covering the card is safe
// specifically because dismissal is trivial and immediate — tap anywhere on
// the sheet, or the "Got it" button — and it never re-arms itself to pop
// back up right away (see OAUTH_WAIT_REARM_MS below), so a user who just
// hasn't scanned yet gets the code back with one tap and isn't fought over
// it while they're in the middle of scanning.
#define OAUTH_WAIT_W  240
#define OAUTH_WAIT_H  264
static lv_obj_t *s_wait_sheet, *s_wait_title_lbl, *s_wait_body_lbl, *s_wait_close_btn, *s_wait_close_lbl;

// One-shot timer (LVGL auto-deletes it after it fires — see
// oauth_wait_timer_cb) that decides when the explainer is next allowed to
// show. Re-armed at OAUTH_WAIT_FIRST_MS on every genuinely fresh QR (first
// render, or main.c restarting the authorize flow after the 5-minute
// consent window elapses) and at the longer OAUTH_WAIT_REARM_MS after every
// dismissal, so a user who wandered off after dismissing still gets
// reminded, but nobody actively scanning gets interrupted twice.
static lv_timer_t *s_wait_timer;
static char s_oauth_last_url[600];   // last URL the timer logic has seen; detects a fresh QR
#define OAUTH_WAIT_FIRST_MS  45000
#define OAUTH_WAIT_REARM_MS  90000

// The sheet's top edge sits just below the "Scan to link your dial" caption
// (y=44..~63) rather than over it — there wasn't quite enough room to clear
// it AND keep the sheet's width inside the round edge, so the caption is
// hidden/shown alongside the sheet instead of leaving a sliver of it peeking
// out above the card while the explainer is up.
static void oauth_wait_show(void)
{
    lv_obj_clear_flag(s_wait_sheet, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_oauth_lbl, LV_OBJ_FLAG_HIDDEN);
}

static void oauth_wait_hide(void)
{
    lv_obj_add_flag(s_wait_sheet, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_oauth_lbl, LV_OBJ_FLAG_HIDDEN);
}

static void oauth_wait_timer_cb(lv_timer_t *t)
{
    (void)t;
    s_wait_timer = NULL;   // one-shot: LVGL deletes the timer itself right after this call
    if (!s_wait_sheet) return;   // screen was torn down before this fired
    oauth_wait_show();
}

static void oauth_arm_wait_timer(uint32_t delay_ms)
{
    if (s_wait_timer) { lv_timer_del(s_wait_timer); s_wait_timer = NULL; }
    s_wait_timer = lv_timer_create(oauth_wait_timer_cb, delay_ms, NULL);
    lv_timer_set_repeat_count(s_wait_timer, 1);
}

static void oauth_wait_dismiss_cb(lv_event_t *e)
{
    (void)e;
    dial_haptics_play(HAPTIC_TICK);
    oauth_wait_hide();
    oauth_arm_wait_timer(OAUTH_WAIT_REARM_MS);   // may reappear once, later — never right away
}

static void oauth_create(lv_obj_t *scr, void *arg)
{
    (void)arg;
    const dial_palette_t *pal = PAL();
    lv_obj_set_style_bg_color(scr, pal->bg, 0);

    s_oauth_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_oauth_lbl, &lv_font_montserrat_16, 0);
    lv_label_set_text(s_oauth_lbl, "Scan to link your dial");
    lv_obj_align(s_oauth_lbl, LV_ALIGN_TOP_MID, 0, 44);

    s_oauth_card = lv_obj_create(scr);
    lv_obj_set_size(s_oauth_card, OAUTH_CARD_SIZE, OAUTH_CARD_SIZE);
    lv_obj_set_style_radius(s_oauth_card, 20, 0);
    lv_obj_set_style_border_width(s_oauth_card, 0, 0);
    lv_obj_set_style_bg_opa(s_oauth_card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_oauth_card, lv_color_white(), 0);
    lv_obj_clear_flag(s_oauth_card, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_oauth_card, LV_ALIGN_CENTER, 0, 14);

    s_oauth_qr = lv_qrcode_create(s_oauth_card, OAUTH_QR_SIZE, lv_color_black(), lv_color_white());
    lv_obj_center(s_oauth_qr);

    // Part 1: persistent, subordinate to the QR (small/secondary-ink) — sits
    // in the strip between the card's bottom edge (y=308) and the panel's
    // rim, centered and narrow enough (220px) to stay clear of the round
    // edge at this height. Real text filled in by on_state (SSID isn't known
    // at create time); placeholder here only so the screen never flashes
    // blank before the first render.
    s_oauth_hint_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_oauth_hint_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_width(s_oauth_hint_lbl, 220);
    lv_label_set_long_mode(s_oauth_hint_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_oauth_hint_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_oauth_hint_lbl, "Scan with a phone on this Wi-Fi network");
    lv_obj_align(s_oauth_hint_lbl, LV_ALIGN_CENTER, 0, 138);

    // Part 2: the dismissible explainer, hidden until its timer fires.
    s_wait_sheet = lv_obj_create(scr);
    lv_obj_set_size(s_wait_sheet, OAUTH_WAIT_W, OAUTH_WAIT_H);
    lv_obj_set_style_radius(s_wait_sheet, 20, 0);
    lv_obj_set_style_border_width(s_wait_sheet, 0, 0);
    lv_obj_set_style_bg_opa(s_wait_sheet, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_wait_sheet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_wait_sheet, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_wait_sheet, LV_OBJ_FLAG_HIDDEN);
    // Tap ANYWHERE on the sheet dismisses it, not just the button below — the
    // whole point is that nobody who just hasn't scanned yet can get stuck.
    lv_obj_add_event_cb(s_wait_sheet, oauth_wait_dismiss_cb, LV_EVENT_CLICKED, NULL);

    s_wait_title_lbl = lv_label_create(s_wait_sheet);
    lv_obj_set_style_text_font(s_wait_title_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_width(s_wait_title_lbl, 210);
    lv_label_set_long_mode(s_wait_title_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_wait_title_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_wait_title_lbl, "Still waiting for approval");
    lv_obj_align(s_wait_title_lbl, LV_ALIGN_TOP_MID, 0, 18);

    s_wait_body_lbl = lv_label_create(s_wait_sheet);
    lv_obj_set_style_text_font(s_wait_body_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_width(s_wait_body_lbl, 200);
    lv_label_set_long_mode(s_wait_body_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_wait_body_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_wait_body_lbl, "Check your phone is on this\nWi-Fi network, not cellular.\nThen approve again.");
    lv_obj_align(s_wait_body_lbl, LV_ALIGN_TOP_MID, 0, 62);

    // Explicit close affordance, sized to this project's >=72px touch rule.
    s_wait_close_btn = dial_btn_create(s_wait_sheet);
    lv_obj_set_size(s_wait_close_btn, 200, 72);
    lv_obj_set_style_radius(s_wait_close_btn, 36, 0);
    lv_obj_set_style_border_width(s_wait_close_btn, 1, 0);
    lv_obj_align(s_wait_close_btn, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_add_event_cb(s_wait_close_btn, oauth_wait_dismiss_cb, LV_EVENT_CLICKED, NULL);

    s_wait_close_lbl = lv_label_create(s_wait_close_btn);
    lv_obj_set_style_text_font(s_wait_close_lbl, &lv_font_montserrat_16, 0);
    lv_label_set_text(s_wait_close_lbl, "Got it");
    lv_obj_center(s_wait_close_lbl);

    // No timer armed here: on_state (called synchronously right after this,
    // by ui_router_go) sees oauth_url go from "" to the real URL on its very
    // first call, and that "fresh QR" branch is what arms the timer — the
    // exact same path a later mid-session refresh takes. One state machine,
    // not two.
    s_oauth_last_url[0] = 0;
}

static void oauth_destroy(void)
{
    if (s_wait_timer) { lv_timer_del(s_wait_timer); s_wait_timer = NULL; }
    s_oauth_card = s_oauth_qr = s_oauth_lbl = s_oauth_hint_lbl = NULL;
    s_wait_sheet = s_wait_title_lbl = s_wait_body_lbl = s_wait_close_btn = s_wait_close_lbl = NULL;
}

static void oauth_on_state(const app_state_t *st)
{
    if (!s_oauth_qr) return;
    const dial_palette_t *pal = PAL();
    lv_obj_set_style_bg_color(lv_obj_get_parent(s_oauth_card), pal->bg, 0);
    lv_obj_set_style_text_color(s_oauth_lbl, pal->ink_secondary, 0);
    lv_obj_set_style_text_color(s_oauth_hint_lbl, pal->ink_secondary, 0);
    lv_obj_set_style_bg_color(s_wait_sheet, pal->surface, 0);
    lv_obj_set_style_text_color(s_wait_title_lbl, pal->ink_primary, 0);
    lv_obj_set_style_text_color(s_wait_body_lbl, pal->ink_secondary, 0);
    lv_obj_set_style_bg_color(s_wait_close_btn, pal->bg, 0);
    lv_obj_set_style_border_color(s_wait_close_btn, pal->track, 0);
    lv_obj_set_style_text_color(s_wait_close_lbl, pal->ink_primary, 0);

    if (st->oauth_url[0])
        lv_qrcode_update(s_oauth_qr, st->oauth_url, strlen(st->oauth_url));

    // Part 1's copy, and the explainer's — both name the real network when
    // it's known, and fall back to generic wording rather than show nothing.
    if (st->sta_ssid[0]) {
        char buf[64];
        snprintf(buf, sizeof(buf), "Scan with a phone on \"%s\"", st->sta_ssid);
        lv_label_set_text(s_oauth_hint_lbl, buf);
    } else {
        lv_label_set_text(s_oauth_hint_lbl, "Scan with a phone on this Wi-Fi network");
    }

    char wait_body[160];
    if (st->sta_ssid[0]) {
        snprintf(wait_body, sizeof(wait_body),
                 "Check your phone is on\n\"%s\"\nWi-Fi, not cellular. Then approve again.",
                 st->sta_ssid);
    } else {
        snprintf(wait_body, sizeof(wait_body),
                 "Check your phone is on this\nWi-Fi network, not cellular.\nThen approve again.");
    }
    lv_label_set_text(s_wait_body_lbl, wait_body);

    // Fresh QR detection. This screen is NOT rebuilt when main.c's 5-minute
    // consent window elapses and restarts the authorize flow with a new URL
    // — nav_policy returns the same SCR_OAUTH_QR with the same (NULL) arg,
    // which ui_router_go treats as a no-op (create/destroy never re-run).
    // Only this callback ever sees the new URL land, so it's the one place
    // that can notice "this is effectively a brand new wait" and restart the
    // quiet-time clock — including hiding an explainer that was talking
    // about a code which no longer exists.
    if (st->oauth_url[0] && strcmp(st->oauth_url, s_oauth_last_url) != 0) {
        strlcpy(s_oauth_last_url, st->oauth_url, sizeof(s_oauth_last_url));
        oauth_wait_hide();
        oauth_arm_wait_timer(OAUTH_WAIT_FIRST_MS);
    }
}

const ui_screen_t scr_oauth_qr = {
    .create = oauth_create, .destroy = oauth_destroy, .on_state = oauth_on_state,
};
