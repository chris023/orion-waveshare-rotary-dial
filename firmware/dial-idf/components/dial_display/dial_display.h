#ifndef DIAL_DISPLAY_H
#define DIAL_DISPLAY_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Bring up backlight, QSPI panel, touch, LVGL, tick timer, and the LVGL task.
// Must be called once from app_main before any UI call.
void dial_display_start(void);

// Non-recursive LVGL lock. timeout_ms<0 = wait forever. Returns true if taken.
bool dial_display_lock(int timeout_ms);
void dial_display_unlock(void);

// Raw touch filter, called on every indev sample from the LVGL task.
// `pressed` is the panel's state; return true to swallow the sample (LVGL
// then sees "released"). Lets the app stamp activity and consume the
// gesture that wakes the display from standby.
typedef bool (*dial_display_touch_filter_t)(bool pressed);
void dial_display_set_touch_filter(dial_display_touch_filter_t filter);

/*
 * Screen rotation, in quarter turns clockwise (0..3). The panel itself cannot
 * rotate (its driver rejects mirror_y and swap_xy), so this is applied in the
 * flush path, and the touch coordinates are un-rotated to match. Call from the
 * LVGL task (or before it starts). Returns false only if a 90/270 rotation
 * couldn't get its DMA scratch buffer, in which case the rotation is unchanged.
 */
bool    dial_display_set_rotation(uint8_t quarters);
uint8_t dial_display_rotation(void);

#ifdef __cplusplus
}
#endif

#endif
