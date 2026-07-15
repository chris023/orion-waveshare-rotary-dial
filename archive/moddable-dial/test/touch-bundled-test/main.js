/*
 * "One more Moddable try" — use Moddable's OWN bundled CST816 driver.
 *
 * It uses Moddable's SMBus (new i2c driver, so NO driver_ng conflict) and is
 * INTERRUPT-DRIVEN: INT (GPIO9, falling) wakes onSample(), which reads the
 * point. That's fundamentally different from our polling attempts — if the
 * CST816D only latches valid data momentarily on touch (asserting INT), an
 * INT-triggered read may catch what polling misses.
 *
 * We ALSO poll sample() on a timer as a cross-check. Watch which (if either)
 * reports coordinates when you tap.
 */

import CST816 from "embedded:sensor/Touch/CST816";
import SMBus from "embedded:io/smbus";
import Digital from "embedded:io/digital";
import Timer from "timer";

let intFired = 0;

const touch = new CST816({
  sensor: { io: SMBus, data: 11, clock: 12, address: 0x15, hz: 300000 },
  reset: { io: Digital, pin: 10, mode: Digital.Output },
  interrupt: { io: Digital, pin: 9, mode: Digital.Input },
  onSample() {
    intFired++;
    const points = this.sample();
    if (points && points.length)
      trace(`INT touch: ${JSON.stringify(points)}\n`);
    else
      trace(`INT fired but sample empty (${intFired})\n`);
  },
});

trace("bundled CST816 (INT-driven + poll cross-check) — tap the screen\n");

// Cross-check: also poll.
Timer.repeat(() => {
  const points = touch.sample();
  if (points && points.length)
    trace(`POLL touch: ${JSON.stringify(points)}\n`);
}, 60);

Timer.repeat(() => {
  trace(`hb int-fired=${intFired}\n`);
}, 2000);
