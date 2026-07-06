/**
 * Commands the controller emits toward the device layer. These are the pure,
 * transport-agnostic intents produced by mapping dial events onto zones.
 */

import type { Power, Zone } from './state.js';

export interface SetTemperatureCommand {
  readonly kind: 'setTemperature';
  readonly deviceId: string;
  readonly zone: Zone;
  readonly target: number;
}

export interface SetPowerCommand {
  readonly kind: 'setPower';
  readonly deviceId: string;
  readonly zone: Zone;
  readonly power: Power;
}

export type DeviceCommand = SetTemperatureCommand | SetPowerCommand;
