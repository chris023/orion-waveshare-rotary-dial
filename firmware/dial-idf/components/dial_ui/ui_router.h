#pragma once
#include <stdbool.h>
#include "lvgl.h"
#include "dial_state.h"

/*
 * Screen router. One lv_obj_t screen per view, created on enter and destroyed
 * on exit (auto_del via lv_scr_load_anim), driven by a vtable per screen.
 *
 * Threading contract: EVERY router entry point runs in the LVGL task —
 * ui_router_go/back are only legal from LVGL callbacks or the dispatcher
 * timer, so screens never take the LVGL lock themselves. The knob decoder
 * (esp_timer task) and the worker never call the router: the knob feeds an
 * atomic detent accumulator that the dispatcher drains, and the worker just
 * commits state (the dispatcher notices the generation change).
 */

typedef enum {
    SCR_CONNECTING = 0,   // boot/progress/status text
    SCR_WIFI_PORTAL,      // SoftAP join instructions + QR
    SCR_OAUTH_QR,         // Orion link QR
    SCR_DIAL,             // the temperature dial (arg: zone_idx_t)
    SCR_STANDBY,          // always-on clock face (arg: zone_idx_t to wake to)
    SCR_QUICK,            // quick-actions bottom sheet (arg: zone_idx_t it was opened from)
    SCR_BOOST,            // boost duration picker (arg: (zone_idx_t<<1)|heat)
    SCR_ERROR,            // offline / degraded, with retry countdown
    SCR_WELCOME,          // fresh-device onboarding splash (M4)
    SCR_SIDEPICK,         // "which side of the bed?" (M4, reused from Settings)
    SCR_SETTINGS,         // settings list (M4, arg: zone_idx_t to return to)
    SCR_COUNT,
} screen_id_t;

typedef struct {
    // Build the widget tree onto `scr` (an empty lv_obj screen). `arg` is the
    // value passed to ui_router_go. Then render the first state via on_state.
    void (*create)(lv_obj_t *scr, void *arg);
    // Widgets are being destroyed (screen unloaded): null your pointers.
    void (*destroy)(void);
    // Re-render from a fresh snapshot (state generation changed).
    void (*on_state)(const app_state_t *st);
    // Knob turned by `detents` (+CW/-CCW). Return true if consumed.
    bool (*on_knob)(int detents);
    // Horizontal swipe. dir = LV_DIR_LEFT/RIGHT. Return true if consumed.
    bool (*on_gesture)(lv_dir_t dir);
} ui_screen_t;

// Register a screen implementation (call for each screen before ui_router_start).
void ui_router_register(screen_id_t id, const ui_screen_t *scr);

// App-level navigation policy: given a fresh snapshot, which screen should be
// showing? Return the screen id (and set *arg). Runs in the dispatcher after
// every state change; returning the current screen/arg is a no-op.
typedef screen_id_t (*ui_nav_policy_t)(const app_state_t *st, void **arg);
void ui_router_set_nav_policy(ui_nav_policy_t policy);

// Create the dispatcher timer and show the first screen. LVGL must be up.
// Call from a context holding the LVGL lock (or before the LVGL task runs).
void ui_router_start(screen_id_t first, void *arg);

// Navigate. anim: LV_SCR_LOAD_ANIM_NONE/FADE_ON/MOVE_LEFT/... LVGL task only.
void ui_router_go(screen_id_t id, void *arg, lv_scr_load_anim_t anim);

// The currently shown screen.
screen_id_t ui_router_current(void);

// Knob input from any task: accumulate detents; the dispatcher drains them
// into the active screen's on_knob within one dispatch period (~50ms).
void ui_router_knob_input(int detents);
