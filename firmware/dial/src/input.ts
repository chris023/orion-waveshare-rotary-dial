/**
 * Rotary encoder via pins/pulsecount (ESP32 PCNT hardware quadrature decoder —
 * no manual edge decoding). The board has NO knob push-button, so "tap" comes
 * from the touchscreen (see cst816.ts + ui.ts), not from here.
 */

// @ts-expect-error - Moddable native module without bundled typings
import PulseCount from "pins/pulsecount";
import { ENCODER } from "config";

export class Encoder {
  private readonly pc: { read(): number; close(): void };
  private last = 0;

  constructor() {
    // signal = phase A, control = phase B. VERIFY sign/scale vs your wiring;
    // this board's encoder is slightly non-standard (see hub research notes).
    this.pc = new PulseCount({ signal: ENCODER.phaseA, control: ENCODER.phaseB });
  }

  /**
   * Net detents since last poll (positive = clockwise). PCNT counts quadrature
   * edges; divide by the counts-per-detent for your unit (often 2 or 4).
   * TODO(hw): confirm counts-per-detent and adjust DIVISOR.
   */
  readDetents(): number {
    const DIVISOR = 2;
    const now = this.pc.read();
    const rawDelta = now - this.last;
    const detents = (rawDelta / DIVISOR) | 0;
    if (detents !== 0) this.last += detents * DIVISOR;
    return detents;
  }

  close(): void {
    this.pc.close();
  }
}
