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

/*
 * The worker flips night mode while the LVGL task reads tokens field-by-field,
 * so the active table must never be rewritten in place (a swap mid-render
 * would paint a torn day/night mix). Both tables are built once and the
 * switch is a single aligned pointer store — atomic on Xtensa.
 */
static dial_palette_t s_day, s_nightt;
static const dial_palette_t *volatile s_active;
static volatile bool s_night;

static void ensure_init(void)
{
    if (s_active) return;
    s_day    = day_table();
    s_nightt = night_table();
    s_active = &s_day;
}

void dial_palette_set_night(bool night)
{
    ensure_init();
    s_night  = night;
    s_active = night ? &s_nightt : &s_day;
}

bool dial_palette_is_night(void) { return s_night; }

const dial_palette_t *PAL(void)
{
    ensure_init();   // first caller is the LVGL task at boot, pre-worker
    return s_active;
}
