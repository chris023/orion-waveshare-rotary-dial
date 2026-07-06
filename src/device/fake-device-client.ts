/**
 * In-memory dual-zone topper simulator implementing DeviceClient.
 *
 * Lets the entire hub + virtual dial run end-to-end with no Orion account and no
 * hardware. The simulated "current" temperature drifts toward the target each
 * time it's read, so dial displays show believable movement. Works in °F (the
 * app unit), mirroring the real device's 50–113°F scale. Records calls for tests.
 */

import type { Power, ReliefType, Zone, ZoneCapabilities, ZoneStatus } from '../domain/state.js';
import { ZONES } from '../domain/state.js';
import type { DeviceClient, DeviceStatus } from './device-client.js';

export interface FakeDeviceOptions {
  deviceId?: string;
  caps?: ZoneCapabilities;
  now?: () => number;
}

interface ZoneModel {
  power: Power;
  target: number;
  current: number;
  relief: ReliefType | null;
}

export interface RecordedCall {
  method: 'setTemperature' | 'setPower' | 'startThermalRelief';
  deviceId: string;
  zone: Zone;
  value: number | Power | ReliefType;
}

const DEFAULT_CAPS: ZoneCapabilities = { unit: 'fahrenheit', min: 50, max: 113, step: 1 };

export class FakeDeviceClient implements DeviceClient {
  readonly calls: RecordedCall[] = [];
  private readonly deviceId: string;
  private readonly caps: ZoneCapabilities;
  private readonly now: () => number;
  private readonly zones: Record<Zone, ZoneModel>;

  constructor(options: FakeDeviceOptions = {}) {
    this.deviceId = options.deviceId ?? 'orion-1';
    this.caps = options.caps ?? DEFAULT_CAPS;
    this.now = options.now ?? Date.now;
    const mid = Math.round((this.caps.min + this.caps.max) / 2);
    this.zones = {
      left: { power: 'off', target: mid, current: mid, relief: null },
      right: { power: 'off', target: mid, current: mid, relief: null },
    };
  }

  connect(): Promise<void> {
    return Promise.resolve();
  }

  listDevices(): Promise<string[]> {
    return Promise.resolve([this.deviceId]);
  }

  getCapabilities(_deviceId: string): Promise<Record<Zone, ZoneCapabilities>> {
    return Promise.resolve({ left: this.caps, right: this.caps });
  }

  getStatus(_deviceId: string): Promise<DeviceStatus> {
    const zones = {} as Record<Zone, ZoneStatus>;
    for (const zone of ZONES) {
      const model = this.zones[zone];
      // Drift the simulated current temperature toward the target.
      if (model.power === 'on' && model.current !== model.target) {
        model.current += Math.sign(model.target - model.current);
      }
      zones[zone] = {
        zone,
        power: model.power,
        target: model.target,
        current: model.current,
        active: model.power === 'on' && model.current !== model.target,
        relief: model.relief,
      };
    }
    return Promise.resolve({
      deviceId: this.deviceId,
      online: true,
      zones,
      fetchedAt: this.now(),
    });
  }

  setTemperature(deviceId: string, zone: Zone, target: number): Promise<void> {
    this.calls.push({ method: 'setTemperature', deviceId, zone, value: target });
    this.zones[zone].target = target;
    this.zones[zone].relief = null;
    return Promise.resolve();
  }

  setPower(deviceId: string, zone: Zone, power: Power): Promise<void> {
    this.calls.push({ method: 'setPower', deviceId, zone, value: power });
    this.zones[zone].power = power;
    if (power === 'off') this.zones[zone].relief = null;
    return Promise.resolve();
  }

  startThermalRelief(
    deviceId: string,
    zone: Zone,
    type: ReliefType,
    _minutes: number,
  ): Promise<void> {
    this.calls.push({ method: 'startThermalRelief', deviceId, zone, value: type });
    const model = this.zones[zone];
    model.power = 'on';
    model.relief = type;
    model.target = type === 'heat' ? this.caps.max : this.caps.min;
    return Promise.resolve();
  }

  close(): Promise<void> {
    return Promise.resolve();
  }
}
