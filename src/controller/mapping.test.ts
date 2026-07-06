import { describe, expect, it } from 'vitest';
import type { DesiredZoneState, ZoneCapabilities } from '../domain/state.js';
import { applyEvent } from './mapping.js';

const caps: ZoneCapabilities = { unit: 'fahrenheit', min: 55, max: 115, step: 1 };
const off = (target: number): DesiredZoneState => ({ power: 'off', target });
const on = (target: number): DesiredZoneState => ({ power: 'on', target });

describe('applyEvent', () => {
  it('rotate cw raises the target and wakes the zone', () => {
    const r = applyEvent({ kind: 'rotate', direction: 'cw', steps: 1 }, off(70), caps);
    expect(r.next).toEqual({ power: 'on', target: 71 });
    expect(r.temperatureChanged).toBe(true);
    expect(r.powerChanged).toBe(true);
  });

  it('rotate ccw lowers the target by step*steps', () => {
    const r = applyEvent({ kind: 'rotate', direction: 'ccw', steps: 3 }, on(80), caps);
    expect(r.next.target).toBe(77);
    expect(r.powerChanged).toBe(false);
  });

  it('clamps at the max and reports no temperature change', () => {
    const r = applyEvent({ kind: 'rotate', direction: 'cw', steps: 5 }, on(115), caps);
    expect(r.next.target).toBe(115);
    expect(r.temperatureChanged).toBe(false);
  });

  it('clamps at the min', () => {
    const r = applyEvent({ kind: 'rotate', direction: 'ccw', steps: 50 }, on(60), caps);
    expect(r.next.target).toBe(55);
  });

  it('tap toggles power off -> on and on -> off without touching target', () => {
    expect(applyEvent({ kind: 'tap' }, off(70), caps).next).toEqual({ power: 'on', target: 70 });
    expect(applyEvent({ kind: 'tap' }, on(70), caps).next).toEqual({ power: 'off', target: 70 });
  });

  it('longPress toggles standby', () => {
    expect(applyEvent({ kind: 'longPress' }, on(70), caps).next.power).toBe('standby');
    expect(applyEvent({ kind: 'longPress' }, off(70), caps).next.power).toBe('on');
  });
});
