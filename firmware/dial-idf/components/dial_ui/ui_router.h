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
    SCR_WIFI_PORTAL,      // how to hand the dial your Wi-Fi (SoftAP name + the on-device way in)
    SCR_NETPICK,          // pick the home network on the dial itself (arg: unused)
    SCR_PASSKEY,          // type its password with the knob (arg: network index)
    SCR_OAUTH_QR,         // Orion link QR
    SCR_DIAL,             // the temperature dial (arg: zone_idx_t)
    SCR_MENU,             // vertical menu, third face of the swipe chain (arg: unused/NULL)
    SCR_STANDBY,          // always-on clock face (arg: zone_idx_t to wake to)
    SCR_BOOST,            // boost duration picker (arg: (zone_idx_t<<1)|heat)
    SCR_ERROR,            // offline / degraded, with retry countdown
    SCR_WELCOME,          // fresh-device onboarding splash (M4)
    SCR_SIDEPICK,         // "which side of the bed?" (M4, reused from Settings)
    SCR_SETTINGS,         // settings list (M4, arg: unused/NULL — returns to SCR_MENU)
    SCR_ADJUST_MODE,      // Schedule vs. Hold choice screen (arg: packed origin — 0 = Settings, 1+zone = the dial face; see scr_adjust_mode.c's header comment)
    SCR_BRIGHTNESS_MENU,  // Day/Night brightness submenu (arg: unused/NULL — returns to SCR_SETTINGS)
    SCR_BRIGHTNESS,       // day/night backlight percent picker (arg: 0=day, 1=night — returns to SCR_BRIGHTNESS_MENU)
    SCR_WIFI,             // Wi-Fi status + change-network (arg: unused/NULL)
    SCR_ABOUT,            // versions, serial (read-only; arg: unused/NULL)
    SCR_UPDATE,           // update submenu: check for updates + beta builds toggle (arg: unused/NULL — returns to SCR_MENU)
    SCR_UPDATING,         // full-screen OTA install takeover (M6, arg: unused/NULL)
    SCR_UPDATE_PROMPT,    // dismissible "update available" sheet over the dial (docs/SPEC-update-prompt.md, arg: zone_idx_t to return to)
    SCR_DIAG,             // hidden diagnostics face: Wi-Fi/battery/build (UI_DESIGN_SPEC.md, swipe down; arg: see DIAG_ARG_FROM_DIAL)
    SCR_COUNT,
} screen_id_t;

/*
 * SCR_DIAG's arg encodes where the user came from, so backing out of the face
 * returns them there instead of always dumping them on the clock.
 *   NULL / 0                      -> opened from SCR_STANDBY, back goes there
 *   DIAG_ARG_FROM_DIAL | zone_idx -> opened from SCR_DIAL, back returns to
 *                                    that page (bit 0 carries the zone, the
 *                                    same low-bit packing scr_boost uses)
 */
#define DIAG_ARG_FROM_DIAL 2u

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

// Leave a screen the user deliberately opened (the menu face) for wherever the
// app actually belongs right now, as decided by the nav policy: the dial, the
// standby clock, the QR code mid-link, the portal mid-setup, or a status
// screen. Screens must NOT hardcode SCR_DIAL for this — during setup, or
// before device state has arrived, there is no dial to go back to and
// navigating to one renders an empty face. LVGL task only.
void ui_router_go_home(lv_scr_load_anim_t anim);

// The currently shown screen.
screen_id_t ui_router_current(void);

// Knob input from any task: accumulate detents; the dispatcher drains them
// into the active screen's on_knob within one dispatch period (~50ms).
void ui_router_knob_input(int detents);
