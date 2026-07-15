/*
 * Combined input bring-up v2.
 *
 * Both inputs work in Waveshare's stock demo but neither worked via Moddable
 * ECMA-419. Changes here to isolate why:
 *   KNOB: use LEGACY pins/digital (calls gpio_get_level, exactly like the
 *         stock iot_knob) instead of embedded:io/digital.
 *   TOUCH: report the chip id, whether the 0x00=0x00 "normal mode" write lands,
 *          and its read-back, so we can see if the fix actually took.
 */

import Timer from "timer";
import LegacyDigital from "pins/digital"; // gpio_get_level, like the stock demo

// ===== Touch (ECMA-419 SMBus) =====
const trst = new device.io.Digital({
  pin: device.pin.touchReset,
  mode: device.io.Digital.Output,
});
trst.write(0);
Timer.delay(20);
trst.write(1);
Timer.delay(160);

const tp = new device.io.SMBus({
  ...device.I2C.default,
  address: 0x15,
  hz: 300_000,
});
let tid, twr, trb;
try {
  tid = "0x" + tp.readUint8(0xa7).toString(16);
} catch (e) {
  tid = "ERR";
}
try {
  tp.writeUint8(0x00, 0x00);
  twr = "ok";
} catch (e) {
  twr = "ERR";
}
try {
  trb = "0x" + tp.readUint8(0x00).toString(16);
} catch (e) {
  trb = "ERR";
}
const tbuf = new Uint8Array(7);

// ===== Knob (LEGACY pins/digital on GPIO8/7) =====
const encA = new LegacyDigital(8, LegacyDigital.InputPullUp);
const encB = new LegacyDigital(7, LegacyDigital.InputPullUp);
let lastA = encA.read();
let lastB = encB.read();
let count = 0;

trace(
  `INPUT v2 — touch id=${tid} wr0x00=${twr} rb=${trb} | knob(legacy) A=${lastA} B=${lastB} — turn & tap\n`,
);

Timer.repeat(() => {
  const a = encA.read();
  const b = encB.read();
  if (a && !lastA) {
    count += 1;
    trace(`knob RIGHT ${count}\n`);
  }
  if (b && !lastB) {
    count -= 1;
    trace(`knob LEFT ${count}\n`);
  }
  lastA = a;
  lastB = b;
}, 3);

Timer.repeat(() => {
  try {
    tp.readBuffer(0x00, tbuf);
  } catch (e) {
    return;
  }
  if (tbuf[2]) {
    const x = ((tbuf[3] & 0x0f) << 8) | tbuf[4];
    const y = ((tbuf[5] & 0x0f) << 8) | tbuf[6];
    trace(`touch x=${x} y=${y}\n`);
  }
}, 40);

Timer.repeat(() => {
  trace(`hb A=${encA.read()} B=${encB.read()} n=${count}\n`);
}, 1000);
