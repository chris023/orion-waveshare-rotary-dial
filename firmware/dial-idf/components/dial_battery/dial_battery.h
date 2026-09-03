#pragma once
#include <stdbool.h>

/*
 * Battery / rail voltage over ADC1 channel 0 (GPIO1).
 *
 * Hardware, read off Waveshare's schematic rather than guessed:
 *
 *   sheet 2 (ESP32S3-R8) netlist:  GPIO1 ──── BATT_ADC
 *   sheet 4 (OTHER):               5V ──┬── R62 10K ──┬── BATT_ADC
 *                                                     R63 10K
 *                                                     GND
 *
 * A 10K/10K leg is a 1:1 divide, so the pin sees half the rail and the rail
 * is the reading doubled. That matches Waveshare's own demo exactly
 * (01_ADC_Test/components/adc_bsp/adc_bsp.c: `*value = 0.001 * vol * 2`),
 * which is where the atten/bitwidth/calibration choices below come from too.
 *
 * The divider taps the board's `5V` net rather than a dedicated VBAT net, and
 * Waveshare's demo calls the result "system voltage". Their schematic has no
 * battery connector, charge IC or boost converter on any of its five sheets,
 * so whether this pin can see the cell at all had to be measured rather than
 * reasoned about. Measured on a battery-fitted unit:
 *
 *   plugged in   4700mV+
 *   unplugged    4000-4100mV, and falling with discharge
 *
 * So the rail DOES sag to the cell once USB goes away. That gives us both a
 * state of charge and a free "on USB" flag, with ~600mV of clean separation
 * to put a threshold in.
 *
 * Two consequences worth knowing before reading the numbers below:
 *
 *   - While USB is attached the rail is the charger, not the cell, so there
 *     is no state of charge to report. dial_battery reports charging=true and
 *     pct=DIAL_BATTERY_PCT_UNKNOWN rather than inventing a level.
 *   - The board's 3V3 rail comes off a TLV62569 buck, which needs headroom
 *     above its output. The dial browns out well before a LiPo's nominal
 *     3000mV floor, so "empty" here is DIAL_BATTERY_MV_EMPTY, not 3000.
 *
 * The cell is optional on this board, and a unit built without one is
 * indistinguishable from a charging unit: no cell means no way to run
 * unplugged, so the rail is above DIAL_BATTERY_MV_USB_ON whenever there is
 * anything alive to read it. charging stays true for that dial's whole life
 * and pct stays UNKNOWN, which is why every consumer has to treat charging as
 * "no level available" rather than as a transient state to wait out. The dial
 * face's badge hides on it, so a battery-less unit looks exactly like stock.
 *
 * If the ADC fails to come up, the sampler never starts and batt_mv stays 0.
 * Consumers should read 0 as "no reading" and show nothing rather than 0%.
 */

// Reported in app_state_t.batt_pct when the rail is the charger and the cell's
// level is therefore unknowable from this pin.
#define DIAL_BATTERY_PCT_UNKNOWN (-1)

// Usable window. Full is a LiPo at rest off the charger. Empty is where this
// board stops running, not where the cell is flat.
#define DIAL_BATTERY_MV_FULL  4200
#define DIAL_BATTERY_MV_EMPTY 3500

// On-USB detection, with hysteresis so a rail hovering near the line does not
// flap the icon. Measured separation is ~600mV, so both edges sit in clear air.
#define DIAL_BATTERY_MV_USB_ON  4400
#define DIAL_BATTERY_MV_USB_OFF 4300

// At or below this, the diagnostics face pulses its warning.
#define DIAL_BATTERY_PCT_LOW 15

// Set up ADC1 oneshot + curve-fitting calibration. Safe to call once, early.
void dial_battery_init(void);

/*
 * init() plus a sampler task that pushes readings into app_state_t via
 * dial_state_set_battery(). Takes one sample immediately so the UI is never
 * blank waiting on the first tick, then every DIAL_BATTERY_PERIOD_MS.
 * Requires dial_state_init() to have run.
 */
void dial_battery_start(void);

/*
 * One reading, median of DIAL_BATTERY_SAMPLES raw conversions (a single S3
 * conversion wanders by tens of counts; the median throws out the spikes
 * without the lag of an average).
 *
 *   rail_mv — millivolts at the `5V` net, i.e. calibrated pin mV doubled
 *   raw     — median raw count, 0..4095, for sanity-checking the cali path
 *
 * Either out-param may be NULL. Returns false if the conversion failed, in
 * which case the out-params are untouched.
 */
bool dial_battery_read(int *rail_mv, int *raw);

/*
 * Map a rail reading to 0..100 across the usable window above, or
 * DIAL_BATTERY_PCT_UNKNOWN if `charging`.
 *
 * The curve is piecewise rather than linear on purpose. A LiPo spends most of
 * its capacity between roughly 3.9V and 3.7V, so a straight line makes the
 * top of the range crawl and the bottom fall off a cliff. Exposed for the
 * simulator, which fabricates rail values without an ADC.
 */
int dial_battery_pct(int rail_mv, bool charging);
