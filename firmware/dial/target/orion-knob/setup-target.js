/*
 * orion-knob board setup. The backlight (GPIO47) is now driven by the app via
 * LEDC PWM (see the display-test main.js), so this no longer touches GPIO47 —
 * having two owners (digital-high here + PWM there) would fight over the pin.
 */

export default function (done) {
  done?.();
}
