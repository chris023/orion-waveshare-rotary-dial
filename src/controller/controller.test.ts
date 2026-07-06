import { setImmediate as setImmediatePromise } from 'node:timers/promises';
import { pino } from 'pino';
import { afterEach, describe, expect, it } from 'vitest';
import { loadConfig } from '../config/env.js';
import { FakeDeviceClient } from '../device/fake-device-client.js';
import type { DialEvent } from '../domain/events.js';
import { MockDialTransport } from '../hardware/mock-transport.js';
import { Controller } from './controller.js';

const silent = pino({ level: 'silent' });
// Default fake caps midpoint = round((55+115)/2) = 85.
const MID = 85;

function makeController(envOverrides: Record<string, string> = {}) {
  const config = loadConfig({ POLL_INTERVAL_MS: '3600000', ...envOverrides });
  const transport = new MockDialTransport(() => 1000);
  const device = new FakeDeviceClient();
  const controller = new Controller({ config, transport, device, logger: silent });
  return { config, transport, device, controller };
}

const msg = (dialId: string, event: DialEvent) => ({ dialId, event, at: 1000 });

let running: Controller | undefined;
afterEach(async () => {
  await running?.stop();
  running = undefined;
});

describe('Controller (mock transport + fake device)', () => {
  it('rotate turns the zone on and writes the new setpoint, then renders the dial', async () => {
    const { transport, device, controller } = makeController({ WRITE_DEBOUNCE_MS: '0' });
    running = controller;
    await controller.start();

    await controller.handleEvent(msg('dial-left', { kind: 'rotate', direction: 'cw', steps: 2 }));

    expect(device.calls).toContainEqual({
      method: 'setPower',
      deviceId: 'orion-1',
      zone: 'left',
      value: 'on',
    });
    expect(device.calls).toContainEqual({
      method: 'setTemperature',
      deviceId: 'orion-1',
      zone: 'left',
      value: MID + 2,
    });

    const display = transport.lastDisplay('dial-left');
    expect(display?.label).toBe('LEFT');
    expect(display?.power).toBe('on');
    expect(display?.target).toBe(MID + 2);
  });

  it('tap toggles power without writing a temperature', async () => {
    const { device, controller } = makeController({ WRITE_DEBOUNCE_MS: '0' });
    running = controller;
    await controller.start();

    await controller.handleEvent(msg('dial-right', { kind: 'tap' }));

    expect(device.calls).toEqual([
      { method: 'setPower', deviceId: 'orion-1', zone: 'right', value: 'on' },
    ]);
  });

  it('ignores events from an unknown dial', async () => {
    const { device, controller } = makeController({ WRITE_DEBOUNCE_MS: '0' });
    running = controller;
    await controller.start();

    await controller.handleEvent(msg('dial-unknown', { kind: 'tap' }));
    expect(device.calls).toEqual([]);
  });

  it('coalesces a fast spin into a single setpoint write when debounced', async () => {
    const timers: Array<() => void> = [];
    const config = loadConfig({ WRITE_DEBOUNCE_MS: '300', POLL_INTERVAL_MS: '3600000' });
    const transport = new MockDialTransport(() => 1000);
    const device = new FakeDeviceClient();
    const controller = new Controller({
      config,
      transport,
      device,
      logger: silent,
      setTimeoutFn: (cb) => {
        timers.push(cb);
        return timers.length as unknown as ReturnType<typeof setTimeout>;
      },
      clearTimeoutFn: () => {},
      setIntervalFn: () => 0 as unknown as ReturnType<typeof setTimeout>,
      clearIntervalFn: () => {},
    });
    running = controller;
    await controller.start();

    for (let i = 0; i < 3; i += 1) {
      await controller.handleEvent(msg('dial-left', { kind: 'rotate', direction: 'cw', steps: 1 }));
    }

    // No temperature write yet — only the initial power-on.
    expect(device.calls.filter((c) => c.method === 'setTemperature')).toHaveLength(0);

    // Fire the pending debounce timer.
    timers.at(-1)?.();
    await setImmediatePromise();

    const writes = device.calls.filter((c) => c.method === 'setTemperature');
    expect(writes).toHaveLength(1);
    expect(writes[0]?.value).toBe(MID + 3);
  });
});
