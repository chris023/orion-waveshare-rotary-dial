/*
 * GPIO scanner — find the encoder pins empirically.
 *
 * The documented encoder pins (GPIO8/7) don't respond on this unit, but the
 * knob physically turns. So configure every FREE ESP32-S3 GPIO as an input
 * with pull-up and log which ones change while the knob is turned — the two
 * that toggle in sync with rotation ARE the encoder. If none toggle, the
 * encoder is wired to the companion ESP32 (read over UART GPIO40/41), not the S3.
 *
 * Excluded (in use / unsafe to reconfigure): 9-21 + 47 (display/touch),
 * 11/12 (I2C), 40/41 (companion UART), 43/44 (console UART), 19/20 (USB),
 * 26-37 (octal PSRAM/flash — reconfiguring would crash), 0/3/45/46 (strapping).
 */

import Digital from "embedded:io/digital";
import Timer from "timer";

// Probe the excluded suspects: companion UART (40 RX / 41 TX) + touch INT/RST
// (9/10). Activity on 40 that tracks rotation = the companion relays the encoder.
const candidates = [9, 10, 40, 41];

const ios = [];
const last = [];
for (const pin of candidates) {
  try {
    const io = new Digital({ pin, mode: Digital.InputPullUp });
    ios.push({ pin, io });
    last.push(io.read());
  } catch (e) {
    trace(`pin ${pin}: open failed\n`);
  }
}

trace(`scan pins: ${ios.map((x) => x.pin).join(",")} — turn the knob\n`);
trace(`start: ${ios.map((x, i) => x.pin + "=" + last[i]).join(" ")}\n`);

Timer.repeat(() => {
  for (let i = 0; i < ios.length; i++) {
    const v = ios[i].io.read();
    if (v !== last[i]) {
      trace(`pin ${ios[i].pin}: ${last[i]}->${v}\n`);
      last[i] = v;
    }
  }
}, 5);
