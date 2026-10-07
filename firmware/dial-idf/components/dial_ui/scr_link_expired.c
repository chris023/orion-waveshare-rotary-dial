/*
 * SCR_LINK_EXPIRED — the calm "Sign-in expired" screen.
 *
 * Shown when this dial WAS linked and its Orion session has since run out (the
 * refresh token was permanently rejected — app_state_t.link_lapsed), as opposed
 * to a dial that was never linked, which still goes straight to the QR. The two
 * used to look identical: a dial that had quietly worked for a month would wake
 * up one morning showing a link QR it re-minted every five minutes, polling the
 * relay the whole time, which reads as "something broke" when nothing did — the
 * bed keeps following its schedule without the dial. So this screen says
 * exactly that, offers one deliberate action, and asks for nothing else.
 *
 * While it is up the worker is PARKED (main.c's park_until_renew): no
 * discovery, no authorize session, no relay polling. Renew posts
 * CMD_LINK_START; the worker then mints a code and nav_policy moves to
 * SCR_OAUTH_QR (in its "renew" copy). Navigation stays phase-driven — this
 * screen never routes to the QR itself, it only shows that the tap landed.
 *
 * Deliberately absent: the warning colour, any spinner, any temperatures or
 * schedule times. Stale values would mislead, and nothing here is an error.
 *
 * Touch only. The knob has nothing to do here (on_knob is NULL, so the router
 * drops detents); swipe left reaches the menu (Re-link, Wi-Fi, About, Update),
 * and the menu's go-home lands back here through nav_policy.
 */
#include "ui_screens_internal.h"
#include "dial_haptics.h"

// Fallback for a tap the worker never answers (it should leave the park on its
// very next 1s receive; this only exists so the button can never stay stuck on
// "Getting a code..." if something upstream wedges). Long enough to cover the
// discovery + registration round trips that legitimately follow a tap.
#define RENEW_PENDING_MS 20000

static lv_obj_t *s_ring, *s_title, *s_body, *s_btn, *s_btn_lbl;
static lv_timer_t *s_pending_timer;
// True from the tap until the worker visibly moves (or the fallback fires) —
// covers the beat between the tap and PH_OAUTH_DISCOVER landing, so the button
// reacts on the same frame as the finger.
static bool s_tap_pending;

static void paint(const app_state_t *st)
{
    const dial_palette_t *pal = PAL();
    bool night = dial_palette_is_night();
    // "Getting a code..." from the tap until the QR takes over.
    bool fetching = s_tap_pending || st->phase == PH_OAUTH_DISCOVER;
    // Renew is only live once the worker is actually PARKED. This screen also
    // shows in PH_OAUTH_WAIT_CONSENT, the moment the owner swipes back off the
    // renewal QR (link_qr_hidden): the worker can still be inside a blocking
    // relay poll and not yet have taken the CMD_LINK_CANCEL. A Renew tap in
    // that beat would clear link_qr_hidden and put the OLD code back up — a
    // session the worker is about to retire, so a scan of it is silently
    // lost — then flick calm -> new code once the cancel lands. Holding the
    // button inert until PH_OAUTH_LAPSED (a few seconds at most) costs nothing.
    bool settling = !fetching && st->phase != PH_OAUTH_LAPSED;

    lv_obj_set_style_bg_color(lv_obj_get_parent(s_title), pal->bg, 0);
    lv_obj_set_style_arc_color(s_ring, pal->track, LV_PART_MAIN);
    // Night keeps the title off full-strength ink — this is a bedside screen
    // that may sit here all night, and nothing on it is urgent.
    lv_obj_set_style_text_color(s_title, night ? pal->ink_secondary : pal->ink_primary, 0);
    lv_obj_set_style_text_color(s_body, pal->ink_secondary, 0);

    lv_obj_set_style_bg_color(s_btn, pal->surface, 0);
    if (fetching || settling) {
        // Not clickable while a code is on its way: a second tap would only
        // queue a second CMD_LINK_START the worker has to drop.
        // Three ASCII dots, not U+2026: LVGL's built-in Montserrat carries
        // no ellipsis glyph (same convention as scr_connecting's copy).
        // While settling the label keeps its normal copy, just dimmed:
        // nothing is being fetched, the button simply isn't ready yet.
        lv_label_set_text(s_btn_lbl, fetching ? "Getting a code..." : "Renew sign-in");
        lv_obj_clear_flag(s_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_border_color(s_btn, pal->ink_secondary, 0);
        lv_obj_set_style_text_color(s_btn_lbl, pal->ink_secondary, 0);
    } else {
        lv_label_set_text(s_btn_lbl, "Renew sign-in");
        lv_obj_add_flag(s_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_border_color(s_btn, pal->track, 0);
        lv_obj_set_style_text_color(s_btn_lbl, night ? pal->ink_secondary : pal->ink_primary, 0);
    }
}

static void repaint_now(void)
{
    app_state_t st;
    dial_state_get(&st);
    paint(&st);
}

static void pending_timer_cb(lv_timer_t *t)
{
    (void)t;
    s_pending_timer = NULL;   // one-shot: LVGL deletes the timer itself right after this call
    s_tap_pending = false;
    if (s_btn) repaint_now();
}

static void renew_cb(lv_event_t *e)
{
    (void)e;
    dial_haptics_play(HAPTIC_TICK);
    s_tap_pending = true;
    dial_state_link_renew();
    repaint_now();
    if (s_pending_timer) lv_timer_del(s_pending_timer);
    s_pending_timer = lv_timer_create(pending_timer_cb, RENEW_PENDING_MS, NULL);
    lv_timer_set_repeat_count(s_pending_timer, 1);
}

static void create(lv_obj_t *scr, void *arg)
{
    (void)arg;
    const dial_palette_t *pal = PAL();
    lv_obj_set_style_bg_color(scr, pal->bg, 0);
    s_tap_pending = false;

    // Chassis hairline ring, same geometry as scr_standby's: purely decorative,
    // in the rim band, non-interactive. It keeps this screen looking like part
    // of the dial rather than a status page.
    s_ring = lv_arc_create(scr);
    lv_obj_set_size(s_ring, 330, 330);
    lv_obj_center(s_ring);
    lv_arc_set_rotation(s_ring, 135);
    lv_arc_set_bg_angles(s_ring, 0, 270);
    lv_obj_set_style_arc_width(s_ring, 2, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_ring, 0, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(s_ring, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_ring, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_clear_flag(s_ring, LV_OBJ_FLAG_CLICKABLE);

    s_title = lv_label_create(scr);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_20, 0);
    lv_obj_set_width(s_title, 240);
    lv_obj_set_style_text_align(s_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_title, "Sign-in expired");
    lv_obj_align(s_title, LV_ALIGN_CENTER, 0, -68);

    // The reassurance is the point of the screen: the bed is fine, nothing is
    // urgent, renewing is the owner's call.
    s_body = lv_label_create(scr);
    lv_obj_set_style_text_font(s_body, &lv_font_montserrat_16, 0);
    lv_obj_set_width(s_body, 250);
    lv_label_set_long_mode(s_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_body, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_body, "Your bed is still running its schedule. Renew whenever you're ready.");
    lv_obj_align(s_body, LV_ALIGN_CENTER, 0, -6);

    // 200x72 at y 222..294: the end-cap centres sit ~101px from the panel
    // centre, so the far edge is ~137px out — inside the r<=150 touch-safe
    // zone, clear of the rim band.
    s_btn = dial_btn_create(scr);
    lv_obj_set_size(s_btn, 200, 72);
    lv_obj_set_style_radius(s_btn, 36, 0);
    lv_obj_set_style_border_width(s_btn, 1, 0);
    lv_obj_align(s_btn, LV_ALIGN_CENTER, 0, 78);
    lv_obj_add_event_cb(s_btn, renew_cb, LV_EVENT_CLICKED, NULL);

    s_btn_lbl = lv_label_create(s_btn);
    lv_obj_set_style_text_font(s_btn_lbl, &lv_font_montserrat_16, 0);
    lv_label_set_text(s_btn_lbl, "Renew sign-in");
    lv_obj_center(s_btn_lbl);
}

static void destroy(void)
{
    if (s_pending_timer) { lv_timer_del(s_pending_timer); s_pending_timer = NULL; }
    s_ring = s_title = s_body = s_btn = s_btn_lbl = NULL;
}

static void on_state(const app_state_t *st)
{
    if (!s_title) return;
    paint(st);
}

// Swipe left to the menu, the same gesture as every other face. Nothing lives
// to the right of this screen, so RIGHT is left unconsumed.
static bool on_gesture(lv_dir_t dir)
{
    if (dir != LV_DIR_LEFT) return false;
    ui_router_go(SCR_MENU, NULL, LV_SCR_LOAD_ANIM_MOVE_LEFT);
    return true;
}

const ui_screen_t scr_link_expired = {
    .create = create, .destroy = destroy, .on_state = on_state, .on_gesture = on_gesture,
};
