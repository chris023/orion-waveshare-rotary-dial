/*
 * Touch bring-up — match Espressif's esp_lcd_touch_cst816s exactly.
 *
 * That driver (the one modi12jin uses for this panel family) does NO config
 * writes — just: assert reset 200ms, release, wait 200ms, then read a block
 * from register 0x02. Our earlier attempts used a 20ms reset assert + extra
 * config writes; a too-short reset can skip the CST816's re-calibration. This
 * mirrors the reference precisely.
 *
 * Block from 0x02: [0]=count, [1]=evt|Xhi, [2]=Xlo, [3]=Yhi, [4]=Ylo.
 */

import Timer from "timer";

const rst = new device.io.Digital({
  pin: device.pin.touchReset,
  mode: device.io.Digital.Output,
});
rst.write(0); // assert reset (active-low)
Timer.delay(200);
rst.write(1); // release
Timer.delay(200);

const intp = new device.io.Digital({
  pin: device.pin.touchInterrupt,
  mode: device.io.Digital.InputPullUp, // INT is open-drain; idle high
});

const tp = new device.io.SMBus({
  ...device.I2C.default,
  address: 0x15,
  hz: 100_000, // match the Moddable CST816 driver (was 400k)
});

let id;
try {
  id = tp.readUint8(0xa7);
} catch (e) {
  id = -1;
}
trace(`CST816 id=0x${(id & 0xff).toString(16)} — tap the screen (reading reg 0x02 block)\n`);

const buf = new Uint8Array(6); // regs 0x02..0x07
let hb = 0;
Timer.repeat(() => {
  let ok = true;
  try {
    tp.readBuffer(0x02, buf);
  } catch (e) {
    ok = false;
  }
  const intL = intp.read();
  const n = ok ? buf[0] & 0x0f : -1;
  hb++;
  if (n > 0 || intL === 0 || hb % 40 === 0) {
    const x = ((buf[1] & 0x0f) << 8) | buf[2];
    const y = ((buf[3] & 0x0f) << 8) | buf[4];
    const raw = Array.from(buf)
      .map((v) => v.toString(16).padStart(2, "0"))
      .join("");
    trace(`INT=${intL} n=${n} x=${x} y=${y} raw=${raw}\n`);
  }
}, 25);
