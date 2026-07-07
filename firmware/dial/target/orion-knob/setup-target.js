/*
 * orion-knob board setup. The ST77916 is a backlit IPS panel, so turn the
 * backlight (GPIO47) on. Uses the legacy one-shot pins/digital (modGPIO) — the
 * same API the driver uses for RST — rather than embedded:io/digital, which
 * takes exclusive ownership of the pin and threw "in use" during bring-up.
 */

import Digital from "pins/digital";

export default function (done) {
  Digital.write(47, 1); // backlight full on
  done?.();
}
