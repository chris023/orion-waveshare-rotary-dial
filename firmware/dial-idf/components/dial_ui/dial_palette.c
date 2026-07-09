#include "dial_palette.h"

/*
 * The two token tables are built by value (not `static const` initializers)
 * because lv_color_hex() is a runtime function on this LVGL build — a static
 * initializer can't call it. Building the struct in a small function and
 * assigning it once on every set_night() is the cheap, obvious workaround.
 */

static dial_palette_t day_table(void)
{
    return (dial_palette_t){
        .bg               = lv_color_hex(0x101418),
        .surface          = lv_color_hex(0x181C20),
        .track            = lv_color_hex(0x202830),
        .ink_primary      = lv_color_hex(0xF0F0E8),
        .ink_secondary    = lv_color_hex(0x888C88),
        .accent_heat      = lv_color_hex(0xE86018),
        .accent_cool      = lv_color_hex(0x3888C8),
        .neutral_holding  = lv_color_hex(0x587868),
        .neutral_standby  = lv_color_hex(0x585858),
        .warning          = lv_color_hex(0xE82818),
        .stale            = lv_color_hex(0xC89838),
        .identity_home    = lv_color_hex(0xC8A050),
        .identity_partner = lv_color_hex(0x98A0A8),
    };
}

// Night: D0's warm/ember family, corrected so every swatch's blue channel is
// <=0x18 (24) — a checkable rule, not per-color judgment (design-spec.md §2).
static dial_palette_t night_table(void)
{
    return (dial_palette_t){
        .bg               = lv_color_hex(0x100C08),
        .surface          = lv_color_hex(0x201810),
        .track            = lv_color_hex(0x281C10),
        .ink_primary      = lv_color_hex(0xC87818),
        .ink_secondary    = lv_color_hex(0x785018),
        .accent_heat      = lv_color_hex(0xE88818),
        .accent_cool      = lv_color_hex(0x886018),
        .neutral_holding  = lv_color_hex(0x504018),
        .neutral_standby  = lv_color_hex(0x302818),
        .warning          = lv_color_hex(0xC83010),
        .stale            = lv_color_hex(0xA07818),
        // Same hue for both identities at night — solid-vs-dashed carries the
        // distinction instead of hue (design-spec.md §2, §8).
        .identity_home    = lv_color_hex(0x906818),
        .identity_partner = lv_color_hex(0x906818),
    };
}

static dial_palette_t s_active;
static bool           s_night;
static bool           s_init;

void dial_palette_set_night(bool night)
{
    s_night = night;
    s_active = night ? night_table() : day_table();
    s_init = true;
}

bool dial_palette_is_night(void) { return s_night; }

const dial_palette_t *PAL(void)
{
    if (!s_init) dial_palette_set_night(false);   // default to day until told otherwise
    return &s_active;
}
