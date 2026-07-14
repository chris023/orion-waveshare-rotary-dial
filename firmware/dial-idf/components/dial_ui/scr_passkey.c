/*
 * SCR_PASSKEY — type the chosen network's Wi-Fi password ON THE DIAL, reached
 * from SCR_NETPICK's row tap (arg: the network's index into the scan table).
 *
 * WHY A CHARACTER WHEEL AND NOT A KEYBOARD: a real QWERTY cannot exist here.
 * This design system requires >=72px touch targets on a ~200dpi round panel,
 * and a 360px circle only affords ~35px per key across a 10-column row — less
 * than half the minimum. So the knob does the typing, exactly as the Nest
 * thermostat does with its ring: spin to a character, tap to commit. The knob
 * is this device's precision input; the touchscreen is not. What the
 * touchscreen IS good at — a big, unmissable "commit this" target — is where
 * it does the actual work: the candidate glyph's hit box is the single
 * largest tap target on the screen, because tapping it is the only thing that
 * writes a character into the password.
 *
 * The wheel does not wrap. A ring that wraps has no "you are at the start/
 * end" tell short of counting detents in your head; clamping and voicing
 * HAPTIC_STOP at either edge is the same range-stop vocabulary scr_boost's
 * duration arc and scr_tonight's wake-time picker already use.
 *
 * The password is shown in clear text, caret and all. This is the user's own
 * bedroom network, typed on a device sitting in it — hiding the characters
 * would turn a fiddly knob-typing task into a blind one for no security this
 * screen could actually provide (anyone close enough to read the panel is
 * close enough to have watched the knob spin). It lives in a bare static
 * buffer only as long as this screen is on screen: zeroed in both create()
 * and destroy() so a finished or abandoned attempt never lingers in RAM.
 */
#include <ctype.h>
#include "ui_screens_internal.h"
#include "dial_haptics.h"
#include "dial_wifi.h"

#define CX 180
#define CY 180

/*
 * ONE alphabet, not four with a mode button. The knob simply keeps turning:
 * lowercase, then uppercase, then digits, then symbols. A mode toggle is a
 * second thing to understand and a second place to be lost — "which set am I
 * in?" — for a screen someone uses once. Turning further is not a mode.
 *
 * The trailing space is deliberate: Wi-Fi passwords are free-form and can
 * contain spaces, and leaving it out would make some real passwords untypeable.
 */
static const char WHEEL[] =
    "abcdefghijklmnopqrstuvwxyz"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "0123456789"
    "!@#$%^&*()-_=+[]{};:,.<>/?~ ";

static lv_obj_t *s_title_lbl;
static lv_obj_t *s_pw_lbl;
static lv_obj_t *s_prev_lbl, *s_cand_lbl, *s_next_lbl;
static lv_obj_t *s_cand_btn;       // transparent >=72px hit box wrapping s_cand_lbl — the commit action
static lv_obj_t *s_slot_hair;
static lv_obj_t *s_del_btn, *s_del_glyph;
static lv_obj_t *s_add_btn, *s_add_glyph;
static lv_obj_t *s_done_btn, *s_done_glyph;
static lv_obj_t *s_del_cap, *s_add_cap, *s_done_cap;
static bool      s_last_failed;   // the previous attempt on this network was rejected

static int  s_idx;                // network index this screen was opened for (arg)
static int  s_pos;                // index into WHEEL
static char s_pw[65];
static int  s_len;

/* ---- motion helpers (same vocabulary as scr_boost.c/scr_dial.c) ----------*/

static void set_zoom_cb(void *obj, int32_t v) { lv_obj_set_style_transform_zoom((lv_obj_t *)obj, (int16_t)v, 0); }
static void set_x_cb(void *obj, int32_t v)    { lv_obj_set_x((lv_obj_t *)obj, (lv_coord_t)v); }

static void anim_zoom_bump(lv_obj_t *obj)
{
    lv_anim_del(obj, set_zoom_cb);
    lv_obj_set_style_transform_zoom(obj, 256, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, set_zoom_cb);
    lv_anim_set_values(&a, 256, 266);
    lv_anim_set_time(&a, 45);
    lv_anim_set_playback_time(&a, 45);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void anim_nudge(lv_obj_t *obj, int dir)
{
    lv_anim_del(obj, set_x_cb);
    lv_obj_set_x(obj, 4 * dir);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, set_x_cb);
    lv_anim_set_values(&a, 4 * dir, 0);
    lv_anim_set_time(&a, 140);
    lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
    lv_anim_start(&a);
}

/* ---- rendering -------------------------------------------------------------*/

// The symbol set's real space character and "no neighbor at this end stop"
// would otherwise both render as an empty label — indistinguishable, so a
// user spinning onto the space couldn't tell it from having hit the edge.
// Stand in an underscore for a selectable space; true blank stays reserved
// for the wheel's actual edges (set below, not here).
static void set_wheel_glyph(lv_obj_t *lbl, char c)
{
    char buf[2] = { (c == ' ') ? '_' : c, '\0' };
    lv_label_set_text(lbl, buf);
}

static void render_wheel(void)
{
    int n = (int)strlen(WHEEL);

    if (s_pos > 0) set_wheel_glyph(s_prev_lbl, WHEEL[s_pos - 1]);
    else           lv_label_set_text(s_prev_lbl, "");

    set_wheel_glyph(s_cand_lbl, WHEEL[s_pos]);

    if (s_pos < n - 1) set_wheel_glyph(s_next_lbl, WHEEL[s_pos + 1]);
    else               lv_label_set_text(s_next_lbl, "");
}

// Cleartext readout with a trailing "|" caret standing in for the wheel's
// current position (there is no blinking cursor animation here — the caret
// just marks "typing continues from here", same idea, cheaper to render).
static void render_readout(void)
{
    if (s_len == 0) {
        // An empty field is also where the last attempt's verdict goes: the
        // router rejecting a password is the one thing the user most needs told,
        // and this is the line their eye is already on.
        lv_obj_set_style_text_color(s_pw_lbl, s_last_failed ? PAL()->warning : PAL()->ink_primary, 0);
        lv_obj_set_style_text_opa(s_pw_lbl, s_last_failed ? LV_OPA_COVER : LV_OPA_50, 0);
        lv_label_set_text(s_pw_lbl, s_last_failed ? "That password didn't work. Try again."
                                                  : "Spin the knob, then tap " LV_SYMBOL_OK);
        return;
    }
    lv_obj_set_style_text_color(s_pw_lbl, PAL()->ink_primary, 0);
    lv_obj_set_style_text_opa(s_pw_lbl, LV_OPA_COVER, 0);
    char buf[sizeof(s_pw) + 1];
    snprintf(buf, sizeof(buf), "%s|", s_pw);
    lv_label_set_text(s_pw_lbl, buf);
}

/* ---- events ----------------------------------------------------------------*/

// THE commit action: the knob only ever stages a candidate, this is what
// writes it into the password. Bound to the checkmark disc AND to the candidate
// glyph itself, so whichever one the user reaches for does the same thing.
// Guards s_len so a full buffer just ignores further taps rather than
// truncating silently mid-character.
static void cand_event_cb(lv_event_t *e)
{
    (void)e;
    if (s_len >= (int)sizeof(s_pw) - 1) return;
    s_last_failed = false;   // they're typing again; stop showing the old verdict
    s_pw[s_len++] = WHEEL[s_pos];
    s_pw[s_len] = '\0';
    dial_haptics_play(HAPTIC_CONFIRM);
    render_readout();
}

static void del_event_cb(lv_event_t *e)
{
    (void)e;
    if (s_len == 0) {
        dial_haptics_play(HAPTIC_STOP);
        return;
    }
    s_pw[--s_len] = '\0';
    dial_haptics_play(HAPTIC_TICK);
    render_readout();
}

static void done_event_cb(lv_event_t *e)
{
    (void)e;
    if (s_len == 0) {
        // An open network is real but rare; refusing a blank submit here
        // instead of guessing "did they mean to leave it empty" keeps this
        // screen's one irreversible action a deliberate one.
        dial_haptics_play(HAPTIC_STOP);
        return;
    }
    dial_haptics_play(HAPTIC_CONFIRM);
    // Tell the store what we're attempting BEFORE handing the credentials over:
    // the connecting screen names the network from this, and if the join is
    // rejected, nav_policy uses it to bring the user back here rather than to
    // the start of setup.
    dial_state_set_wifi_join(s_idx, dial_net_scan_ssid(s_idx));
    dial_net_submit_creds(dial_net_scan_ssid(s_idx), s_pw);
    ui_router_go(SCR_CONNECTING, NULL, LV_SCR_LOAD_ANIM_FADE_ON);
}

/* ---- palette -----------------------------------------------------------*/
// Re-applied from on_state, not just create(): a night palette swap while
// mid-password must not force the screen to rebuild (that would drop the
// wheel position and everything typed so far).
static void apply_palette(void)
{
    const dial_palette_t *pal = PAL();
    lv_obj_t *scr = lv_obj_get_parent(s_title_lbl);
    lv_obj_set_style_bg_color(scr, pal->bg, 0);

    lv_obj_set_style_text_color(s_title_lbl, pal->ink_secondary, 0);
    lv_obj_set_style_text_color(s_pw_lbl, pal->ink_primary, 0);

    lv_obj_set_style_text_color(s_prev_lbl, pal->ink_secondary, 0);
    lv_obj_set_style_text_opa(s_prev_lbl, LV_OPA_40, 0);
    lv_obj_set_style_text_color(s_next_lbl, pal->ink_secondary, 0);
    lv_obj_set_style_text_opa(s_next_lbl, LV_OPA_40, 0);
    lv_obj_set_style_text_color(s_cand_lbl, pal->ink_primary, 0);

    lv_obj_set_style_bg_color(s_slot_hair, pal->track, 0);

    lv_obj_set_style_bg_color(s_del_btn, pal->surface, 0);
    lv_obj_set_style_border_color(s_del_btn, pal->track, 0);
    lv_obj_set_style_text_color(s_del_glyph, pal->ink_primary, 0);


    lv_obj_set_style_bg_color(s_add_btn, pal->surface, 0);
    lv_obj_set_style_border_color(s_add_btn, pal->ink_secondary, 0);   // the typing key carries the emphasis
    lv_obj_set_style_text_color(s_add_glyph, pal->ink_primary, 0);
    lv_obj_set_style_text_color(s_del_cap, pal->ink_secondary, 0);
    lv_obj_set_style_text_color(s_add_cap, pal->ink_secondary, 0);
    lv_obj_set_style_text_color(s_done_cap, pal->ink_secondary, 0);
    lv_obj_set_style_bg_color(s_done_btn, pal->surface, 0);
    lv_obj_set_style_border_color(s_done_btn, pal->track, 0);
    lv_obj_set_style_text_color(s_done_glyph, pal->ink_primary, 0);
}

/* ---- vtable ------------------------------------------------------------*/

static void create(lv_obj_t *scr, void *arg)
{
    s_idx = (int)(uintptr_t)arg;
    s_pos = 0;
    s_len = 0;
    memset(s_pw, 0, sizeof(s_pw));

    // Arriving here after a rejected password: say so, once, and clear the flag
    // so nav_policy stops routing us here and lets the screen be sticky again.
    {
        app_state_t st;
        dial_state_get(&st);
        s_last_failed = st.wifi_join_failed;
        if (s_last_failed) dial_state_clear_wifi_join_failed();
    }

    const dial_palette_t *pal = PAL();
    lv_obj_set_style_bg_color(scr, pal->bg, 0);

    char upper[33];   // SSIDs are <=32 bytes; uppercased into a local, one-shot buffer
    const char *ssid = dial_net_scan_ssid(s_idx);
    size_t i = 0;
    for (; ssid[i] != '\0' && i < sizeof(upper) - 1; i++)
        upper[i] = (char)toupper((unsigned char)ssid[i]);
    upper[i] = '\0';

    s_title_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_title_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_width(s_title_lbl, 260);
    lv_label_set_long_mode(s_title_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_title_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_title_lbl, upper);
    lv_obj_align(s_title_lbl, LV_ALIGN_CENTER, 0, 56 - CY);

    s_pw_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_pw_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_width(s_pw_lbl, 280);
    lv_label_set_long_mode(s_pw_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_pw_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_pw_lbl, LV_ALIGN_CENTER, 0, 96 - CY);

    s_prev_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_prev_lbl, &lv_font_montserrat_24, 0);
    lv_obj_align(s_prev_lbl, LV_ALIGN_CENTER, 100 - CX, 170 - CY);

    s_next_lbl = lv_label_create(scr);
    lv_obj_set_style_text_font(s_next_lbl, &lv_font_montserrat_24, 0);
    lv_obj_align(s_next_lbl, LV_ALIGN_CENTER, 260 - CX, 170 - CY);

    // The "slot" hairline reads as a landing mark under the candidate — pure
    // decoration, so it stays unclickable and doesn't compete with s_cand_btn.
    s_slot_hair = lv_obj_create(scr);
    lv_obj_set_size(s_slot_hair, 56, 2);
    lv_obj_set_style_border_width(s_slot_hair, 0, 0);
    lv_obj_clear_flag(s_slot_hair, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_slot_hair, LV_ALIGN_CENTER, 0, 200 - CY);

    // The candidate's hit box is 100px, well past the glyph's own ~50px
    // footprint: this is the one tap that writes a character, so it gets
    // the most forgiving target on the screen, not just the visible glyph.
    s_cand_btn = dial_btn_create(scr);
    lv_obj_set_size(s_cand_btn, 100, 100);
    lv_obj_set_style_radius(s_cand_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_cand_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_cand_btn, 0, 0);
    lv_obj_align(s_cand_btn, LV_ALIGN_CENTER, 0, 170 - CY);
    lv_obj_add_event_cb(s_cand_btn, cand_event_cb, LV_EVENT_CLICKED, NULL);

    s_cand_lbl = lv_label_create(s_cand_btn);
    lv_obj_set_style_text_font(s_cand_lbl, &lv_font_montserrat_48, 0);
    lv_obj_set_style_transform_pivot_x(s_cand_lbl, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(s_cand_lbl, LV_PCT(50), 0);
    lv_obj_center(s_cand_lbl);

    /*
     * Three discs, and the CHECKMARK IS THE ONE THAT TYPES.
     *
     * It used to be the finish button, with the letter itself as the commit
     * target — and a tick sitting next to a letter reads as "take this letter",
     * because that is what a tick means. So it is: the middle disc adds the
     * candidate, and finishing gets its own disc with its own word under it.
     * Every disc is captioned, because two glyph-only circles side by side are
     * a guessing game on a screen someone uses once.
     *
     * Discs are true circles (radius 44), so only reach-from-centre matters:
     * at y=240, a disc at x=84 sits 113px out, +44 = 157 < 180. The captions at
     * y=292 stay inside the panel too (worst point ~166 from centre).
     */
    s_del_btn = dial_btn_create(scr);
    lv_obj_set_size(s_del_btn, 88, 88);
    lv_obj_set_style_radius(s_del_btn, 44, 0);
    lv_obj_set_style_border_width(s_del_btn, 1, 0);
    lv_obj_align(s_del_btn, LV_ALIGN_CENTER, 84 - CX, 240 - CY);
    lv_obj_add_event_cb(s_del_btn, del_event_cb, LV_EVENT_CLICKED, NULL);
    s_del_glyph = lv_label_create(s_del_btn);
    lv_obj_set_style_text_font(s_del_glyph, &lv_font_montserrat_28, 0);
    lv_label_set_text(s_del_glyph, LV_SYMBOL_BACKSPACE);
    lv_obj_center(s_del_glyph);

    // The typing key. Biggest visual weight of the three: it is pressed once per
    // character, the others once per password.
    s_add_btn = dial_btn_create(scr);
    lv_obj_set_size(s_add_btn, 88, 88);
    lv_obj_set_style_radius(s_add_btn, 44, 0);
    lv_obj_set_style_border_width(s_add_btn, 2, 0);
    lv_obj_align(s_add_btn, LV_ALIGN_CENTER, 0, 240 - CY);
    lv_obj_add_event_cb(s_add_btn, cand_event_cb, LV_EVENT_CLICKED, NULL);
    s_add_glyph = lv_label_create(s_add_btn);
    lv_obj_set_style_text_font(s_add_glyph, &lv_font_montserrat_28, 0);
    lv_label_set_text(s_add_glyph, LV_SYMBOL_OK);
    lv_obj_center(s_add_glyph);

    s_done_btn = dial_btn_create(scr);
    lv_obj_set_size(s_done_btn, 88, 88);
    lv_obj_set_style_radius(s_done_btn, 44, 0);
    lv_obj_set_style_border_width(s_done_btn, 1, 0);
    lv_obj_align(s_done_btn, LV_ALIGN_CENTER, 276 - CX, 240 - CY);
    lv_obj_add_event_cb(s_done_btn, done_event_cb, LV_EVENT_CLICKED, NULL);
    s_done_glyph = lv_label_create(s_done_btn);
    lv_obj_set_style_text_font(s_done_glyph, &lv_font_montserrat_28, 0);
    lv_label_set_text(s_done_glyph, LV_SYMBOL_WIFI);
    lv_obj_center(s_done_glyph);

    struct { lv_obj_t **lbl; const char *txt; lv_coord_t x; } caps[] = {
        { &s_del_cap,  "Delete",  84  },
        { &s_add_cap,  "Add",     180 },
        { &s_done_cap, "Connect", 276 },
    };
    for (size_t i = 0; i < sizeof(caps) / sizeof(caps[0]); i++) {
        lv_obj_t *c = lv_label_create(scr);
        lv_obj_set_style_text_font(c, &lv_font_montserrat_12, 0);
        lv_label_set_text(c, caps[i].txt);
        lv_obj_align(c, LV_ALIGN_CENTER, caps[i].x - CX, 292 - CY);
        lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE);
        *caps[i].lbl = c;
    }

    render_wheel();
    render_readout();
    apply_palette();
}

static void destroy(void)
{
    if (s_cand_lbl) lv_anim_del(s_cand_lbl, NULL);

    // The one thing on this screen worth being paranoid about: don't let a
    // typed password outlive the screen that collected it.
    memset(s_pw, 0, sizeof(s_pw));
    s_len = 0;

    s_title_lbl = NULL;
    s_pw_lbl = NULL;
    s_prev_lbl = s_cand_lbl = s_next_lbl = NULL;
    s_cand_btn = NULL;
    s_slot_hair = NULL;
    s_del_btn = s_del_glyph = NULL;
    s_done_btn = s_done_glyph = NULL;
    s_add_btn = s_add_glyph = NULL;
    s_del_cap = s_add_cap = s_done_cap = NULL;
    s_last_failed = false;
}

static void on_state(const app_state_t *st)
{
    (void)st;
    if (!s_title_lbl) return;
    apply_palette();
}

// Clamped, not wrapping: a ring that wraps has no felt "start"/"end", so
// either edge just voices the range-stop haptic and holds position instead.
static bool on_knob(int detents)
{
    if (!s_cand_lbl || detents == 0) return false;

    int n = (int)strlen(WHEEL);
    int np = s_pos + detents;
    if (np < 0)     np = 0;
    if (np > n - 1) np = n - 1;

    if (np == s_pos) {
        dial_haptics_play(HAPTIC_STOP);
        anim_nudge(s_cand_btn, detents > 0 ? 1 : -1);
        return true;
    }

    s_pos = np;
    render_wheel();
    dial_haptics_play(HAPTIC_TICK);
    anim_zoom_bump(s_cand_lbl);
    return true;
}

static bool on_gesture(lv_dir_t dir)
{
    if (dir != LV_DIR_RIGHT) return false;
    ui_router_go(SCR_NETPICK, NULL, LV_SCR_LOAD_ANIM_MOVE_RIGHT);
    return true;
}

const ui_screen_t scr_passkey = {
    .create = create, .destroy = destroy, .on_state = on_state,
    .on_knob = on_knob, .on_gesture = on_gesture,
};
