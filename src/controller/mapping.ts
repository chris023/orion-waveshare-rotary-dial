/**
 * Pure mapping from a dial event to the next desired zone state. No I/O — this
 * is the heart of the UX and is fully unit-tested.
 *
 * Interaction model for the Waveshare knob (rotate + touch, no physical click):
 *   - rotate cw/ccw : adjust target temperature by step*steps (clamped to caps);
 *                     also wakes the zone to "on" if it was off.
 *   - tap           : toggle power on <-> off.
 *   - longPress     : handled by the Controller as a thermal-relief boost (not here).
 */

import type { RotateEvent, TapEvent } from '../domain/events.js';
import {
  clampToCapabilities,
  type DesiredZoneState,
  type ZoneCapabilities,
} from '../domain/state.js';

export interface MappingResult {
  readonly next: DesiredZoneState;
  readonly temperatureChanged: boolean;
  readonly powerChanged: boolean;
}

export function applyEvent(
  event: RotateEvent | TapEvent,
  current: DesiredZoneState,
  caps: ZoneCapabilities,
): MappingResult {
  switch (event.kind) {
    case 'rotate': {
      const sign = event.direction === 'cw' ? 1 : -1;
      const target = clampToCapabilities(current.target + sign * caps.step * event.steps, caps);
      const next: DesiredZoneState = { power: 'on', target };
      return {
        next,
        temperatureChanged: target !== current.target,
        powerChanged: next.power !== current.power,
      };
    }
    case 'tap': {
      const power = current.power === 'off' ? 'on' : 'off';
      return { next: { ...current, power }, temperatureChanged: false, powerChanged: true };
    }
  }
}
