#pragma once
#include <stdio.h>
#include <string.h>
#include "lvgl.h"
#include "ui_router.h"
#include "dial_state.h"
#include "dial_palette.h"

// Screen vtables defined one per file, gathered by ui_screens_register_all().
extern const ui_screen_t scr_connecting;
extern const ui_screen_t scr_wifi_portal;
extern const ui_screen_t scr_netpick;
extern const ui_screen_t scr_passkey;
extern const ui_screen_t scr_oauth_qr;
extern const ui_screen_t scr_dial;
extern const ui_screen_t scr_menu;
extern const ui_screen_t scr_tonight;
extern const ui_screen_t scr_standby;
extern const ui_screen_t scr_quick;
extern const ui_screen_t scr_boost;
extern const ui_screen_t scr_welcome;
extern const ui_screen_t scr_sidepick;
extern const ui_screen_t scr_settings;
extern const ui_screen_t scr_wifi;
extern const ui_screen_t scr_about;

/*
 * Shared state->visual classification (design-spec.md §2's grammar table).
 * Both scr_dial (full glyph+word+accent) and scr_standby (accent only, for
 * the presence dots) need "which token does this zone's state map to", so it
 * lives here once instead of drifting between two copies.
 */
typedef enum { ZK_OFFLINE, ZK_STANDBY, ZK_HEATING, ZK_COOLING, ZK_HOLDING } zone_kind_t;

static inline zone_kind_t dial_zone_kind(const zone_state_t *z, bool device_online)
{
    if (!device_online)               return ZK_OFFLINE;
    if (!z->on)                       return ZK_STANDBY;   // off IS standby, regardless of stale telemetry text
    if (!strcmp(z->thermal_state, "heating")) return ZK_HEATING;
    if (!strcmp(z->thermal_state, "cooling")) return ZK_COOLING;
    return ZK_HOLDING;               // "holding", empty, or anything else while on
}

/*
 * Every button on the dial is built here.
 *
 * LVGL's default theme hangs a grey drop shadow off every lv_btn, offset 4px
 * DOWNWARD (lv_theme_default.c styles->btn: shadow_width 3, opa 50%, ofs_y 4).
 * It's a material-style elevation cue that belongs to nothing else in this
 * design language — on the dial's near-black ground it reads as a smudge under
 * the power disc rather than as depth. Nothing here is meant to float above the
 * face; the chassis is flat and state is carried by ring color, not elevation.
 *
 * Zeroing it per-button is how it got missed twice, so the constructor owns it.
 */
static inline lv_obj_t *dial_btn_create(lv_obj_t *parent)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    return btn;
}

/*
 * Page-dot row, shared by the dial faces and the menu face so the two can't
 * drift out of agreement about the chain.
 *
 * Face order is Dial(B) - Dial(A) - Menu: zone_b is the LEFT side of the bed
 * and zone_a the RIGHT, so walking the chain left-to-right walks the bed
 * left-to-right. A single-zone topper has no partner face at all — its absent
 * side's dot is dropped and the remaining pair re-centered, rather than
 * leaving a dot for a face the swipe can never reach.
 *
 * Callers own the fill colors (which dot is "current" differs per screen).
 */
static inline void dial_dots_layout(const app_state_t *st, lv_obj_t *dot_b,
                                    lv_obj_t *dot_a, lv_obj_t *dot_menu)
{
    const lv_coord_t y = 340 - 180;   // same rim band on every face
    if (dial_state_is_dual(st)) {
        lv_obj_clear_flag(dot_b, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(dot_a, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(dot_b,    LV_ALIGN_CENTER, 164 - 180, y);
        lv_obj_align(dot_a,    LV_ALIGN_CENTER, 180 - 180, y);
        lv_obj_align(dot_menu, LV_ALIGN_CENTER, 196 - 180, y);
    } else {
        zone_idx_t p = dial_state_primary_zone(st);
        lv_obj_t *keep = (p == ZONE_A) ? dot_a : dot_b;
        lv_obj_t *drop = (p == ZONE_A) ? dot_b : dot_a;
        lv_obj_add_flag(drop, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(keep, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align(keep,     LV_ALIGN_CENTER, 172 - 180, y);
        lv_obj_align(dot_menu, LV_ALIGN_CENTER, 188 - 180, y);
    }
}

static inline lv_color_t dial_zone_accent(zone_kind_t k, const dial_palette_t *pal)
{
    switch (k) {
    case ZK_OFFLINE: return pal->warning;
    case ZK_STANDBY: return pal->neutral_standby;
    case ZK_HEATING: return pal->accent_heat;
    case ZK_COOLING: return pal->accent_cool;
    default:         return pal->neutral_holding;
    }
}
