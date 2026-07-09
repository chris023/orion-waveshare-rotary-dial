#pragma once
#include <stdbool.h>
#include "lvgl.h"

/*
 * Day/night color tokens (design-spec.md §2). Two flat tables, swapped
 * wholesale on night transitions — no per-color interpolation. Every night
 * value satisfies the checkable rule "blue channel <= 0x18" (visible glow
 * without driving the panel's blue subpixels); every value in both tables
 * quantizes exactly to RGB565.
 *
 * Screens never cache these colors past a single render: they call PAL() at
 * the top of on_state (or a small apply-palette helper called from on_state)
 * so a night swap takes effect on the next re-render without recreating any
 * widget.
 */

typedef struct {
    lv_color_t bg;                // screen background
    lv_color_t surface;           // pill, power disc, sheet
    lv_color_t track;             // chassis ring / arc track
    lv_color_t ink_primary;       // numeral, clock — never state-tinted
    lv_color_t ink_secondary;     // name, unit, captions, ghost ring
    lv_color_t accent_heat;       // heating
    lv_color_t accent_cool;       // cooling (day only — dim khaki at night)
    lv_color_t neutral_holding;   // at target
    lv_color_t neutral_standby;   // zone off
    lv_color_t warning;           // faults only — never thermal
    lv_color_t stale;             // freshness dot only
    lv_color_t identity_home;     // brass underline (yours); == identity_partner at night
    lv_color_t identity_partner;  // pewter underline (partner); pattern (dashed) carries the cue at night
} dial_palette_t;

// Switch the active table. Cheap (a handful of lv_color_hex calls); call
// whenever the night window (or override) flips, then force a re-render.
void dial_palette_set_night(bool night);

// The table dial_palette_set_night last selected (day until first call).
bool dial_palette_is_night(void);

// The active token table. Never NULL.
const dial_palette_t *PAL(void);
