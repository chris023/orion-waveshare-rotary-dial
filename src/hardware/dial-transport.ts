/**
 * PORT: how physical dials communicate with the hub.
 *
 * Real dials (Waveshare ESP32-S3 knobs) talk MQTT over Wi-Fi; tests and local
 * dev use an in-memory mock. Both implement this interface so nothing above the
 * hardware layer knows which is in use.
 *
 * It is bidirectional: the hub receives dial events AND pushes display state
 * back so each knob's round screen can show its zone's current temperature.
 */

import type { DialMessage } from '../domain/events.js';
import type { Power } from '../domain/state.js';

/** What a dial should render on its screen. */
export interface DialDisplay {
  readonly label: string;
  readonly power: Power;
  /** Target setpoint to show, or null if unknown. */
  readonly target: number | null;
  /** Measured temperature to show, or null if unknown/unsupported. */
  readonly current: number | null;
  /** Whether the zone is actively heating/cooling (drives an indicator). */
  readonly active: boolean;
  /** True when the hub has lost contact with the device. */
  readonly offline: boolean;
}

export type DialEventHandler = (message: DialMessage) => void;

export interface DialTransport {
  /** Connect/begin listening. Resolves once ready to receive events. */
  start(): Promise<void>;
  /** Stop listening and release resources. */
  stop(): Promise<void>;
  /** Register a handler for inbound dial events. */
  onEvent(handler: DialEventHandler): void;
  /** Push a display update to a specific dial's screen. */
  publishDisplay(dialId: string, display: DialDisplay): Promise<void>;
}
