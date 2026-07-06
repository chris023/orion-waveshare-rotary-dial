/**
 * In-memory dual-zone topper simulator implementing DeviceClient.
 *
 * Lets the entire hub run end-to-end with no Orion account and no hardware. The
 * simulated "current" temperature drifts toward the target each time it's read,
 * so dial displays show believable movement. Also records calls for assertions.
 */

import type { Power, Zone, ZoneCapabilities, ZoneStatus } from '../domain/state.js';
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
}

export interface RecordedCall {
  method: 'setTemperature' | 'setPower';
  deviceId: string;
  zone: Zone;
  value: number | Power;
}

const DEFAULT_CAPS: ZoneCapabilities = { unit: 'fahrenheit', min: 55, max: 115, step: 1 };

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
      left: { power: 'off', target: mid, current: mid },
      right: { power: 'off', target: mid, current: mid },
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
        const dir = Math.sign(model.target - model.current);
        model.current += dir;
      }
      zones[zone] = {
        zone,
        power: model.power,
        target: model.target,
        current: model.current,
        active: model.power === 'on' && model.current !== model.target,
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
    return Promise.resolve();
  }

  setPower(deviceId: string, zone: Zone, power: Power): Promise<void> {
    this.calls.push({ method: 'setPower', deviceId, zone, value: power });
    this.zones[zone].power = power;
    return Promise.resolve();
  }

  close(): Promise<void> {
    return Promise.resolve();
  }
}
