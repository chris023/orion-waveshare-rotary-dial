/**
 * PORT: the control surface of a dual-zone liquid-cooled topper.
 *
 * Transport-agnostic and expressed in the app's units (°F, left/right).
 * `FakeDeviceClient` implements it in-memory for tests/dev; `OrionMcpClient`
 * implements it against Orion's real MCP server (translating to °C and
 * zone_a/zone_b). See docs/ORION_MCP.md.
 */

import type { Power, ReliefType, Zone, ZoneCapabilities, ZoneStatus } from '../domain/state.js';

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
  /** Start a temporary max heat/cool boost on a zone (auto-restores after). */
  startThermalRelief(
    deviceId: string,
    zone: Zone,
    type: ReliefType,
    minutes: number,
  ): Promise<void>;
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
