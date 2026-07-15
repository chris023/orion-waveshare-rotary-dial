/**
 * Events produced by a physical dial (Waveshare ESP32-S3 knob).
 *
 * The board has an incremental rotary encoder plus a capacitive touchscreen but
 * NO dedicated knob push-button, so the interaction vocabulary is "rotate" and
 * "tap"/"long-press" (via touch). See docs/ARCHITECTURE.md and the firmware/.
 */

export type RotateDirection = 'cw' | 'ccw';

export interface RotateEvent {
  readonly kind: 'rotate';
  readonly direction: RotateDirection;
  /** Number of detents moved this event; always >= 1. */
  readonly steps: number;
}

/** A short touch on the screen — the primary "click" affordance. */
export interface TapEvent {
  readonly kind: 'tap';
}

/** A long touch — a secondary affordance (e.g. cycle a mode). */
export interface LongPressEvent {
  readonly kind: 'longPress';
}

export type DialEvent = RotateEvent | TapEvent | LongPressEvent;

/** A dial event tagged with the originating dial and hub-clock timestamp. */
export interface DialMessage {
  readonly dialId: string;
  readonly event: DialEvent;
  /** Epoch milliseconds, stamped by the hub on receipt. */
  readonly at: number;
}

export function isRotate(event: DialEvent): event is RotateEvent {
  return event.kind === 'rotate';
}
