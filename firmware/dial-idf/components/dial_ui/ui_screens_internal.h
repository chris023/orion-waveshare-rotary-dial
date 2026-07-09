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
extern const ui_screen_t scr_oauth_qr;
extern const ui_screen_t scr_dial;
extern const ui_screen_t scr_standby;

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
