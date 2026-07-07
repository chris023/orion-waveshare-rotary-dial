/*
 * Standalone ST77916 display test — the de-risk step 2 before the full firmware.
 * Fills the whole round screen with RED, GREEN, BLUE, WHITE in a 1s loop.
 *
 * Build/flash:
 *   cd firmware/dial/test/display-test
 *   UPLOAD_PORT=/dev/cu.usbmodem2101 mcconfig -d -m -p esp32/orion-knob
 *
 * What it tells you:
 *   - screen stays black  -> init/pins/QSPI wrong (check driver + defines)
 *   - colors show but RED looks BLUE (swapped) -> flip MADCTL 0x36 to 0x08 in
 *     drivers/st77916/st77916_init.h, or change target config.format BE<->LE
 *   - image is torn/offset -> tweak column_offset/row_offset in the target
 */

import {} from "piu/MC";
// @ts-ignore
import Timer from "timer";

const colors = ["#ff0000", "#00ff00", "#0000ff", "#ffffff"];
const names = ["RED", "GREEN", "BLUE", "WHITE"];

const application = new Application(null, {
  skin: new Skin({ fill: colors }),
});
trace(`display ${application.width} x ${application.height}\n`);

let i = 0;
Timer.repeat(() => {
  i = (i + 1) % colors.length;
  application.state = i; // indexes the skin fill array
  trace(`fill ${names[i]}\n`);
}, 1000);
