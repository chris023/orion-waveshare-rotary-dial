/*
 * SCR_SIDEPICK — "Which side of the bed?" Shown once, right after first
 * link on a fresh device (main.c's nav_policy), and reused from Settings'
 * "My side" row on any device thereafter.
 *
 * Two full-height halves (>=88px tall — they're the full 340px content
 * height): left = ZONE_B, right = ZONE_A, matching design-spec.md §8's
 * "swipe mirrors the bed" mapping used everywhere else. A tap on either half
 * commits immediately — there's no push-to-confirm here, so the knob only
 * moves a cosmetic highlight between the two halves (a nicety); tapping a
 * half always selects THAT half regardless of which one is highlighted.
 */
#include "ui_screens_internal.h"
#include "dial_haptics.h"

#define SCREEN_H 360
#define HALF_W   180

static lv_obj_t *s_title;
static lv_obj_t *s_divider;
static lv_obj_t *s_half[ZONE_COUNT];       // indexed by zone_idx_t
static lv_obj_t *s_half_lbl[ZONE_COUNT];
static zone_idx_t s_highlight = ZONE_A;    // knob-driven, cosmetic only

static void apply_highlight(void)
{
    const dial_palette_t *pal = PAL();
    for (int z = 0; z < ZONE_COUNT; z++) {
        bool hi = ((zone_idx_t)z == s_highlight);
        lv_obj_set_style_bg_color(s_half[z], hi ? pal->surface : pal->bg, 0);
        lv_obj_set_style_text_color(s_half_lbl[z], hi ? pal->ink_primary : pal->ink_secondary, 0);
    }
}

static void pick(zone_idx_t z)
{
    dial_haptics_play(HAPTIC_CONFIRM);
    // Commit BEFORE navigating (same rule as scr_dial's side-swap): the nav
    // policy follows ui_zone/side_picked, so an uncommitted pick would be
    // undone by the next state commit.
    dial_state_set_ui_zone(z);        // persists "ui"/"zone" to NVS
    dial_state_set_side_picked();     // stop nav_policy pinning this screen
    ui_router_go(SCR_DIAL, (void *)(uintptr_t)z, LV_SCR_LOAD_ANIM_FADE_ON);
}

static void half_a_cb(lv_event_t *e) { (void)e; pick(ZONE_A); }
static void half_b_cb(lv_event_t *e) { (void)e; pick(ZONE_B); }

static void create(lv_obj_t *scr, void *arg)
{
    (void)arg;
    s_highlight = ZONE_A;
    const dial_palette_t *pal = PAL();
    lv_obj_set_style_bg_color(scr, pal->bg, 0);

    s_title = lv_label_create(scr);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_title, pal->ink_primary, 0);
    lv_label_set_text(s_title, "Which side of the bed?");
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 36);

    static const struct { zone_idx_t zone; lv_event_cb_t cb; lv_coord_t x; const char *fallback; } halves[ZONE_COUNT] = {
        { ZONE_B, half_b_cb, 0,       "LEFT"  },
        { ZONE_A, half_a_cb, HALF_W,  "RIGHT" },
    };
    for (int i = 0; i < ZONE_COUNT; i++) {
        zone_idx_t z = halves[i].zone;
        lv_obj_t *h = lv_obj_create(scr);
        lv_obj_set_size(h, HALF_W, SCREEN_H);
        lv_obj_set_pos(h, halves[i].x, 0);
        lv_obj_set_style_radius(h, 0, 0);
        lv_obj_set_style_border_width(h, 0, 0);
        lv_obj_clear_flag(h, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(h, halves[i].cb, LV_EVENT_CLICKED, NULL);
        s_half[z] = h;

        lv_obj_t *lbl = lv_label_create(h);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
        lv_label_set_text(lbl, halves[i].fallback);   // on_state fills in the real name
        lv_obj_center(lbl);
        s_half_lbl[z] = lbl;
    }

    // Thin center divider (non-interactive, drawn over both halves' edge).
    s_divider = lv_obj_create(scr);
    lv_obj_set_size(s_divider, 2, SCREEN_H);
    lv_obj_set_pos(s_divider, HALF_W - 1, 0);
    lv_obj_set_style_border_width(s_divider, 0, 0);
    lv_obj_set_style_radius(s_divider, 0, 0);
    lv_obj_clear_flag(s_divider, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_divider, pal->track, 0);

    apply_highlight();
}

static void destroy(void)
{
    s_title = s_divider = NULL;
    s_half[ZONE_A] = s_half[ZONE_B] = NULL;
    s_half_lbl[ZONE_A] = s_half_lbl[ZONE_B] = NULL;
}

static void on_state(const app_state_t *st)
{
    if (!s_title) return;
    const dial_palette_t *pal = PAL();
    lv_obj_t *scr = lv_obj_get_parent(s_title);
    lv_obj_set_style_bg_color(scr, pal->bg, 0);
    lv_obj_set_style_text_color(s_title, pal->ink_primary, 0);
    lv_obj_set_style_bg_color(s_divider, pal->track, 0);

    for (int z = 0; z < ZONE_COUNT; z++) {
        const char *name = st->zones[z].user_name;
        char buf[24];
        if (name[0]) {
            strlcpy(buf, name, sizeof(buf));
            for (char *p = buf; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
        } else {
            strlcpy(buf, z == ZONE_A ? "RIGHT" : "LEFT", sizeof(buf));
        }
        lv_label_set_text(s_half_lbl[z], buf);
    }
    apply_highlight();
}

// Cosmetic only (design-spec.md: "knob highlight is a nicety") — flips which
// half looks highlighted. A tap on either half always picks THAT half, so
// there's no risk of a wrong pick from a stale highlight.
static bool on_knob(int detents)
{
    if (detents == 0) return false;
    s_highlight = (detents > 0) ? ZONE_A : ZONE_B;
    apply_highlight();
    return true;
}

const ui_screen_t scr_sidepick = {
    .create = create, .destroy = destroy, .on_state = on_state, .on_knob = on_knob,
};
