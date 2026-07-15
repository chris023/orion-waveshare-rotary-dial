/*
 * Touch test — register recipe (Fable-5 agent Fix 2) + INT diagnostic (Fix 3).
 *
 * The driver now does the proper reset dance and writes the touch-report-enable
 * registers we'd been missing: 0xFE=0xFF (disable auto-sleep), 0xFA=0x41
 * (enable touch IRQ), 0xEC=0x00, 0xFB=0x00, then 0x00=0x00 (normal mode).
 * We POLL sample() (no INT-wiring dependency) AND log the INT line (GPIO9) so we
 * learn whether the chip now asserts IRQ on touch. NOTE: we do NOT read the chip
 * id first — the working stock code never touches reg 0xA7 before polling.
 */

import CST816 from "cst816";
import Digital from "embedded:io/digital";
import Timer from "timer";

const touch = new CST816({ sda: 11, scl: 12, rst: 10, hz: 300000 });
const intPin = new Digital({ pin: 9, mode: Digital.Input }); // TP INT, idles high

trace("touch (0xFA recipe) — tap & drag; logging touches + INT\n");

let lastInt = intPin.read();
Timer.repeat(() => {
  const p = touch.sample();
  if (p) trace(`touch x=${p.x} y=${p.y}\n`);
  const iv = intPin.read();
  if (iv !== lastInt) {
    trace(`INT ${lastInt}->${iv}\n`);
    lastInt = iv;
  }
}, 25);
