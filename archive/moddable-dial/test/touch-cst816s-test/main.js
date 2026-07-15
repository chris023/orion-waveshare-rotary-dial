/*
 * Agent Fix 1 — Moddable's PROVEN Waveshare driver `CST816S` (the 's' variant,
 * used by the shipping ws_esp32s3_touch_1_69 target), interrupt-driven.
 *
 * Unlike everything tried so far, this driver:
 *   - writes 0xFA=0x41 (enable touch IRQ) AND 0xED=0x01 (IRQ pulse width 0.1ms),
 *   - reads on a HARDWARE falling-edge ISR on INT (GPIO9) — so a brief touch
 *     pulse is latched and caught (my 25ms polling missed short pulses).
 * The chip is put in wake-on-touch mode; touching -> INT falling -> onSample.
 */

import Touch from "embedded:sensor/Touch/CST816S";
import SMBus from "embedded:io/smbus";
import Digital from "embedded:io/digital";
import Timer from "timer";

let fires = 0;

const touch = new Touch({
  sensor: { io: SMBus, data: 11, clock: 12, address: 0x15, hz: 400_000 },
  reset: { io: Digital, pin: 10, mode: Digital.Output },
  interrupt: { io: Digital, pin: 9, mode: Digital.Input },
  onSample() {
    fires++;
    const points = this.sample();
    if (points?.length)
      trace(`TOUCH x=${points[0].x} y=${points[0].y}\n`);
    else
      trace(`INT fired (${fires}) but no point\n`);
  },
});

trace("CST816S INT-driven (0xFA=0x41 + 0xED=0x01 + hw ISR) — tap the screen\n");
Timer.repeat(() => trace(`hb int-fires=${fires}\n`), 2000);
