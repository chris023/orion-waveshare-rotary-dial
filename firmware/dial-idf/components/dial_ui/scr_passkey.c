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

// Four charsets the SET button cycles through; the trailing space in the
// symbol set is deliberate — SSID passwords are free-form and can contain
// spaces, so leaving it out would make some real passwords untypeable here.
static const char *SETS[4] = {
    "abcdefghijklmnopqrstuvwxyz",
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ",
    "0123456789",
    "!@#$%^&*()-_=+[]{};:,.<>/?~ ",
};
static const char *SET_NAMES[4] = { "abc", "ABC", "123", "#$%" };

static lv_obj_t *s_title_lbl;
static lv_obj_t *s_pw_lbl;
static lv_obj_t *s_prev_lbl, *s_cand_lbl, *s_next_lbl;
static lv_obj_t *s_cand_btn;       // transparent >=72px hit box wrapping s_cand_lbl — the commit action
static lv_obj_t *s_slot_hair;
static lv_obj_t *s_del_btn, *s_del_glyph;
static lv_obj_t *s_set_btn, *s_set_lbl;
static lv_obj_t *s_done_btn, *s_done_glyph;

static int  s_idx;                // network index this screen was opened for (arg)
static int  s_set;                // which row of SETS[] the wheel is on
static int  s_pos;                // index within SETS[s_set]
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
    const char *set = SETS[s_set];
    int n = (int)strlen(set);

    if (s_pos > 0) set_wheel_glyph(s_prev_lbl, set[s_pos - 1]);
    else           lv_label_set_text(s_prev_lbl, "");

    set_wheel_glyph(s_cand_lbl, set[s_pos]);

    if (s_pos < n - 1) set_wheel_glyph(s_next_lbl, set[s_pos + 1]);
    else                lv_label_set_text(s_next_lbl, "");

    lv_label_set_text(s_set_lbl, SET_NAMES[s_set]);
}

// Cleartext readout with a trailing "|" caret standing in for the wheel's
// current position (there is no blinking cursor animation here — the caret
// just marks "typing continues from here", same idea, cheaper to render).
static void render_readout(void)
{
    if (s_len == 0) {
        lv_obj_set_style_text_opa(s_pw_lbl, LV_OPA_50, 0);
        lv_label_set_text(s_pw_lbl, "tap the letter below to start");
        return;
    }
    lv_obj_set_style_text_opa(s_pw_lbl, LV_OPA_COVER, 0);
    char buf[sizeof(s_pw) + 1];
    snprintf(buf, sizeof(buf), "%s|", s_pw);
    lv_label_set_text(s_pw_lbl, buf);
}

/* ---- events ----------------------------------------------------------------*/

// THE commit action: the knob only ever stages a candidate, this is what
// writes it into the password. Guards s_len so a full buffer just ignores
// further taps rather than truncating silently mid-character.
static void cand_event_cb(lv_event_t *e)
{
    (void)e;
    if (s_len >= (int)sizeof(s_pw) - 1) return;
    s_pw[s_len++] = SETS[s_set][s_pos];
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

static void set_event_cb(lv_event_t *e)
{
    (void)e;
    s_set = (s_set + 1) % 4;
    s_pos = 0;   // a charset switch with the old offset kept would land on an
                 // arbitrary, unrelated character in the new set
    render_wheel();
    dial_haptics_play(HAPTIC_TICK);
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

    lv_obj_set_style_bg_color(s_set_btn, pal->surface, 0);
    lv_obj_set_style_border_color(s_set_btn, pal->track, 0);
    lv_obj_set_style_text_color(s_set_lbl, pal->ink_primary, 0);

    lv_obj_set_style_bg_color(s_done_btn, pal->surface, 0);
    lv_obj_set_style_border_color(s_done_btn, pal->track, 0);
    lv_obj_set_style_text_color(s_done_glyph, pal->ink_primary, 0);
}

/* ---- vtable ------------------------------------------------------------*/

static void create(lv_obj_t *scr, void *arg)
{
    s_idx = (int)(uintptr_t)arg;
    s_set = 0;
    s_pos = 0;
    s_len = 0;
    memset(s_pw, 0, sizeof(s_pw));

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

    // Bottom row of three 88px discs (radius 44 = a true circle, not a
    // rounded square, so only the disc's own reach from center matters).
    // At y=270 (dy=90 from CY) a disc centered farther out than x=80/280
    // would poke past the panel's r=180 cutoff once its own 44px radius is
    // added back in (dist-to-center + radius <= 180) — 76/284 as drafted
    // computes to ~181.5 and just clips; 80/280 clears it with ~1.5px left.
    s_del_btn = dial_btn_create(scr);
    lv_obj_set_size(s_del_btn, 88, 88);
    lv_obj_set_style_radius(s_del_btn, 44, 0);
    lv_obj_set_style_border_width(s_del_btn, 1, 0);
    lv_obj_align(s_del_btn, LV_ALIGN_CENTER, 80 - CX, 270 - CY);
    lv_obj_add_event_cb(s_del_btn, del_event_cb, LV_EVENT_CLICKED, NULL);
    s_del_glyph = lv_label_create(s_del_btn);
    lv_obj_set_style_text_font(s_del_glyph, &lv_font_montserrat_28, 0);
    lv_label_set_text(s_del_glyph, LV_SYMBOL_BACKSPACE);
    lv_obj_center(s_del_glyph);

    s_set_btn = dial_btn_create(scr);
    lv_obj_set_size(s_set_btn, 88, 88);
    lv_obj_set_style_radius(s_set_btn, 44, 0);
    lv_obj_set_style_border_width(s_set_btn, 1, 0);
    lv_obj_align(s_set_btn, LV_ALIGN_CENTER, 0, 270 - CY);
    lv_obj_add_event_cb(s_set_btn, set_event_cb, LV_EVENT_CLICKED, NULL);
    s_set_lbl = lv_label_create(s_set_btn);
    lv_obj_set_style_text_font(s_set_lbl, &lv_font_montserrat_16, 0);
    lv_obj_center(s_set_lbl);

    s_done_btn = dial_btn_create(scr);
    lv_obj_set_size(s_done_btn, 88, 88);
    lv_obj_set_style_radius(s_done_btn, 44, 0);
    lv_obj_set_style_border_width(s_done_btn, 1, 0);
    lv_obj_align(s_done_btn, LV_ALIGN_CENTER, 280 - CX, 270 - CY);
    lv_obj_add_event_cb(s_done_btn, done_event_cb, LV_EVENT_CLICKED, NULL);
    s_done_glyph = lv_label_create(s_done_btn);
    lv_obj_set_style_text_font(s_done_glyph, &lv_font_montserrat_28, 0);
    lv_label_set_text(s_done_glyph, LV_SYMBOL_OK);
    lv_obj_center(s_done_glyph);

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
    s_set_btn = s_set_lbl = NULL;
    s_done_btn = s_done_glyph = NULL;
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

    int n = (int)strlen(SETS[s_set]);
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
