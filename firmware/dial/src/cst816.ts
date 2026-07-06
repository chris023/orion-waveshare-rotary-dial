/**
 * CST816 capacitive touch driver in pure JS (pins/smbus), modeled on Moddable's
 * ft6206 driver so Piu can drive it via the standard read(points) contract:
 * each point gets .state (0 none, 1 began, 2 moved, 3 ended) + .x/.y.
 *
 * TODO(hw): verify the I2C address (0xB4/0xB5 variants report via reg 0xA7) and
 * the X/Y register layout on your unit; handle the RST pin if wired.
 */

// @ts-expect-error - Moddable native module without bundled typings
import SMBus from "pins/smbus";
import { TOUCH } from "config";

const CST816_ADDR = 0x15; // VERIFY: common CST816 address

interface TouchPoint {
  state: number;
  x: number;
  y: number;
}

export default class CST816 {
  private readonly io: { readBlock(register: number, count: number): ArrayBuffer | Uint8Array };

  constructor() {
    this.io = new SMBus({
      address: CST816_ADDR,
      hz: TOUCH.hz,
      sda: TOUCH.sda,
      scl: TOUCH.scl,
    });
    // TODO(hw): pulse the RST pin (TOUCH.rst) high->low->high on boot if required.
  }

  read(points: TouchPoint[]): void {
    // Registers: 0x02 = finger count; 0x03..0x06 = XH,XL,YH,YL (12-bit, high
    // nibble in the H byte). VERIFY against the CST816 datasheet for your board.
    const raw = this.io.readBlock(0x02, 5);
    const data = raw instanceof Uint8Array ? raw : new Uint8Array(raw);
    const count = data[0] & 0x0f;
    const p = points[0];
    if (!p) return;

    if (count > 0) {
      const x = ((data[1] & 0x0f) << 8) | data[2];
      const y = ((data[3] & 0x0f) << 8) | data[4];
      p.x = x;
      p.y = y;
      p.state = p.state === 1 || p.state === 2 ? 2 : 1; // began -> moved
    } else {
      p.state = p.state === 1 || p.state === 2 ? 3 : 0; // -> ended -> none
    }
  }
}
