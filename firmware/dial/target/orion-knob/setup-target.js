/*
 * orion-knob board setup. The ST77916 is a backlit IPS panel (unlike the AMOLED
 * CO5300 this driver derives from), so we turn the backlight (GPIO47) on here.
 * Swap to a PWM for dimming later if desired.
 */

import Digital from "embedded:io/digital";

globalThis.Host = {
  Backlight: class {
    constructor(brightness = 100) {
      this.pin = new Digital({ pin: 47, mode: Digital.Output, initialValue: brightness ? 1 : 0 });
    }
    write(value) {
      this.pin.write(value ? 1 : 0);
    }
    close() {
      this.pin.close();
    }
  },
};

export default function (done) {
  // Keep a reference so it isn't collected — backlight stays on.
  globalThis.backlight = new globalThis.Host.Backlight(100);
  done?.();
}
