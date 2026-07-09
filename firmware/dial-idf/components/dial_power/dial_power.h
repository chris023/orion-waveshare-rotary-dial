#pragma once
#include <stdbool.h>

/*
 * Display power / standby manager: idle-driven backlight dimming with LEDC
 * hardware fades, plus the wake-consumes-first-input rule.
 *
 * Levels (day / night duty targets):
 *   ACTIVE  — full brightness, user interacting or recently active
 *   DIMMED  — legible-but-quiet after DIM_AFTER of no input
 *   STANDBY — near-dark after STANDBY_AFTER; the router should be showing
 *             the standby face by then (it follows the same idle clock)
 *
 * Wake rule: when the display is in STANDBY, the first touch or detent must
 * wake the screen and DO NOTHING ELSE (a 3am reach must not change the
 * temperature). Input handlers call dial_power_wake_consumes() first and
 * drop the event if it returns true.
 */

typedef enum { DPWR_ACTIVE, DPWR_DIMMED, DPWR_STANDBY } dial_power_level_t;

// Start the idle timer task. Requires dial_display + dial_state up.
void dial_power_start(void);

// Current level (for the router's standby-face decision).
dial_power_level_t dial_power_level(void);

// True exactly once when an input arrives during STANDBY: the input woke the
// screen and must be consumed (not acted on). Thread-safe, call from input
// handlers before processing.
bool dial_power_wake_consumes(void);

// Night mode (bedtime window from the sleep schedule): lower duty targets
// and a warm-dim standby. Also forwards to dial_haptics_set_night().
void dial_power_set_night(bool night);
