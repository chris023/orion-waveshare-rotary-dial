#pragma once
#include <stdbool.h>

/*
 * DRV2605 LRA haptics. HAPTIC_EN is hardwired to 3V3 and TRIG to GND on this
 * board, so the GO-bit over I2C is the only trigger path.
 *
 * Effects fire from a small dedicated task via a queue: callers never block
 * on the I2C bus (the knob decoder and LVGL task must stay fast), and a new
 * effect simply replaces one still playing (never queue haptics — lagging
 * the knob feels broken).
 */

typedef enum {
    HAPTIC_TICK,      // one knob detent — soft, short
    HAPTIC_STOP,      // range end — firmer double
    HAPTIC_CONFIRM,   // action committed
    HAPTIC_ERROR,     // rejected / failed
} haptic_effect_t;

// Probe + configure the chip (LRA mode, library 6, auto-calibration persisted
// in NVS). Requires i2c_master_Init() to have run. Safe to call when the chip
// is absent — haptics just stay disabled.
void dial_haptics_init(void);

// Fire an effect (non-blocking; drops if haptics disabled/absent).
void dial_haptics_play(haptic_effect_t fx);

// Night attenuation: true = quieter effect variants (owner asleep nearby).
void dial_haptics_set_night(bool night);

// Master enable (settings toggle; persisted by the caller).
void dial_haptics_set_enabled(bool enabled);
