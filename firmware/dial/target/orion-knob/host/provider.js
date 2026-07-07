/*
 * `device` provider for the orion-knob target (ECMA-419 style). Exposes the I2C
 * bus (CST816 touch @ SDA=11/SCL=12), the quadrature encoder (PulseCount), and
 * the IO classes the firmware uses. Not needed by the display-only test.
 */

import Analog from "embedded:io/analog";
import Digital from "embedded:io/digital";
import DigitalBank from "embedded:io/digitalbank";
import I2C from "embedded:io/i2c";
import PulseCount from "embedded:io/pulsecount";
import PWM from "embedded:io/pwm";
import SMBus from "embedded:io/smbus";
import SPI from "embedded:io/spi";

const device = {
  I2C: {
    default: { io: I2C, data: 11, clock: 12 },
  },
  Encoder: {
    default: { io: PulseCount, signal: 8, control: 7 },
  },
  io: { Analog, Digital, DigitalBank, I2C, PulseCount, PWM, SMBus, SPI },
  pin: {
    button: 0, // BOOT
    touchInterrupt: 9,
    touchReset: 10,
    backlight: 47,
  },
};

export default device;
