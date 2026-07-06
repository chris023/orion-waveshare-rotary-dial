import { describe, expect, it } from 'vitest';
import { FakeDeviceClient } from './fake-device-client.js';

describe('FakeDeviceClient', () => {
  it('lists a device and reports both zones', async () => {
    const client = new FakeDeviceClient({ deviceId: 'orion-1' });
    expect(await client.listDevices()).toEqual(['orion-1']);
    const status = await client.getStatus('orion-1');
    expect(Object.keys(status.zones).sort()).toEqual(['left', 'right']);
    expect(status.online).toBe(true);
  });

  it('records setTemperature and setPower calls and reflects them in status', async () => {
    const client = new FakeDeviceClient();
    await client.setPower('orion-1', 'left', 'on');
    await client.setTemperature('orion-1', 'left', 72);
    expect(client.calls).toEqual([
      { method: 'setPower', deviceId: 'orion-1', zone: 'left', value: 'on' },
      { method: 'setTemperature', deviceId: 'orion-1', zone: 'left', value: 72 },
    ]);
    const status = await client.getStatus('orion-1');
    expect(status.zones.left.power).toBe('on');
    expect(status.zones.left.target).toBe(72);
  });

  it('drifts current temperature toward the target while on', async () => {
    const client = new FakeDeviceClient({
      caps: { unit: 'fahrenheit', min: 55, max: 115, step: 1 },
    });
    await client.setPower('orion-1', 'right', 'on');
    await client.setTemperature('orion-1', 'right', 100);
    const first = (await client.getStatus('orion-1')).zones.right.current ?? 0;
    const second = (await client.getStatus('orion-1')).zones.right.current ?? 0;
    expect(second).toBeGreaterThan(first);
    expect(second).toBeLessThanOrEqual(100);
  });

  it('reports capabilities for both zones', async () => {
    const caps = { unit: 'fahrenheit', min: 60, max: 110, step: 2 } as const;
    const client = new FakeDeviceClient({ caps });
    const result = await client.getCapabilities('orion-1');
    expect(result.left).toEqual(caps);
    expect(result.right).toEqual(caps);
  });
});
