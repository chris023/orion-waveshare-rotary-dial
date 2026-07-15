/*
 * Knob native-driver test — validates the vendored iot_knob C decoder.
 *
 * The C decoder polls GPIO8/7 on its own 3ms esp_timer and accumulates a signed
 * count. We just read() that count and report per-poll deltas. If this shows
 * counts while the JS-level GPIO reads didn't, it confirms the native-C path is
 * the right approach for this board's inputs.
 */

import Knob from "knob";
import Timer from "timer";

const knob = new Knob({ signal: 8, control: 7 });
knob.write(0);

let last = 0;
trace("native knob test — turn the knob (CW and CCW)\n");

Timer.repeat(() => {
  const total = knob.read();
  if (total !== last) {
    trace(`knob total ${total} (delta ${total - last})\n`);
    last = total;
  }
}, 50);

// Diagnostic: raw GPIO8/7 levels straight from gpio_get_level. If these never
// change from 3 (both high) while turning, the pins truly aren't toggling under
// our build; if they DO change, the decode/timer is the issue.
let lastRaw = -1;
Timer.repeat(() => {
  const raw = knob.readRaw();
  if (raw !== lastRaw) {
    trace(`raw AB=${raw} (A=${raw >> 1} B=${raw & 1})\n`);
    lastRaw = raw;
  }
}, 5);

Timer.repeat(() => {
  trace(`hb total=${knob.read()} raw=${knob.readRaw()}\n`);
}, 2000);
