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

#ifdef __cplusplus
}
#endif

#endif
