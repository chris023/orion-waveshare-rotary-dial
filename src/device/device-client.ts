/**
 * PORT: the control surface of a dual-zone liquid-cooled topper.
 *
 * The real Orion Sleep API is not yet documented or captured, so this contract
 * is deliberately transport-agnostic. A `FakeDeviceClient` implements it for
 * tests/dev; `OrionDeviceClient` will implement it against the real cloud once
 * the API is captured (see docs/ORION_API.md).
 */

import type { Power, Zone, ZoneCapabilities, ZoneStatus } from '../domain/state.js';

export interface DeviceStatus {
  readonly deviceId: string;
  readonly online: boolean;
  readonly zones: Record<Zone, ZoneStatus>;
  /** Epoch ms when this snapshot was taken (hub clock). */
  readonly fetchedAt: number;
}

export interface DeviceClient {
  /** Authenticate / establish any session needed before other calls. */
  connect(): Promise<void>;
  /** List device ids available on the account. */
  listDevices(): Promise<string[]>;
  /** Temperature unit + min/max/step per zone. Callers must not hardcode ranges. */
  getCapabilities(deviceId: string): Promise<Record<Zone, ZoneCapabilities>>;
  /** Read current state of a device (used to refresh dial displays). */
  getStatus(deviceId: string): Promise<DeviceStatus>;
  /** Set a zone's target temperature (in the unit reported by capabilities). */
  setTemperature(deviceId: string, zone: Zone, target: number): Promise<void>;
  /** Set a zone's power state. */
  setPower(deviceId: string, zone: Zone, power: Power): Promise<void>;
  /** Release any resources / sessions. */
  close(): Promise<void>;
}

/** Thrown by adapters whose behavior isn't implemented yet (e.g. real Orion). */
export class NotImplementedError extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'NotImplementedError';
  }
}
