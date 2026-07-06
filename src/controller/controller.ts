/**
 * The Controller wires dial events to device commands and pushes state back to
 * the dial displays. It is the composition of the pure mapping with the two
 * ports (DialTransport in, DeviceClient out), plus two pieces of real-world
 * pragmatism:
 *   - debounce/coalesce rapid knob turns into a single setpoint write, so a fast
 *     spin doesn't hammer (and get throttled by) the cloud API;
 *   - poll device state periodically to keep the dial screens accurate.
 *
 * Timer functions are injectable so the behavior is deterministically testable.
 */

import type { Config } from '../config/env.js';
import type { DeviceClient } from '../device/device-client.js';
import type { DialBinding } from '../domain/bindings.js';
import { indexBindings } from '../domain/bindings.js';
import type { DialMessage } from '../domain/events.js';
import type { DesiredZoneState, Zone, ZoneCapabilities, ZoneStatus } from '../domain/state.js';
import type { DialDisplay, DialTransport } from '../hardware/dial-transport.js';
import type { Logger } from '../lib/logger.js';
import { applyEvent } from './mapping.js';

type TimerId = ReturnType<typeof setTimeout>;

/** Duration of the long-press "boost" (thermal relief), in minutes. */
const RELIEF_MINUTES = 30;

export interface ControllerDeps {
  config: Config;
  transport: DialTransport;
  device: DeviceClient;
  logger: Logger;
  now?: () => number;
  setTimeoutFn?: (handler: () => void, ms: number) => TimerId;
  clearTimeoutFn?: (id: TimerId) => void;
  setIntervalFn?: (handler: () => void, ms: number) => TimerId;
  clearIntervalFn?: (id: TimerId) => void;
}

interface DialRuntime {
  binding: DialBinding;
  caps: ZoneCapabilities;
  desired: DesiredZoneState;
  status: ZoneStatus | undefined;
  online: boolean;
  writeTimer: TimerId | undefined;
  pendingTarget: number | undefined;
}

export class Controller {
  private readonly config: Config;
  private readonly transport: DialTransport;
  private readonly device: DeviceClient;
  private readonly log: Logger;
  private readonly setTimeoutFn: (handler: () => void, ms: number) => TimerId;
  private readonly clearTimeoutFn: (id: TimerId) => void;
  private readonly setIntervalFn: (handler: () => void, ms: number) => TimerId;
  private readonly clearIntervalFn: (id: TimerId) => void;

  private readonly dials = new Map<string, DialRuntime>();
  private pollTimer: TimerId | undefined;

  constructor(deps: ControllerDeps) {
    this.config = deps.config;
    this.transport = deps.transport;
    this.device = deps.device;
    this.log = deps.logger.child({ component: 'controller' });
    this.setTimeoutFn = deps.setTimeoutFn ?? ((h, ms) => setTimeout(h, ms));
    this.clearTimeoutFn = deps.clearTimeoutFn ?? ((id) => clearTimeout(id));
    this.setIntervalFn = deps.setIntervalFn ?? ((h, ms) => setInterval(h, ms));
    this.clearIntervalFn = deps.clearIntervalFn ?? ((id) => clearInterval(id));
  }

  /** Connect to the device and load per-dial capabilities + initial state. */
  async initialize(): Promise<void> {
    await this.device.connect();
    const byDial = indexBindings(this.config.bindings);

    for (const [dialId, binding] of byDial) {
      const caps = await this.loadCaps(binding);
      const status = await this.readZone(binding);
      this.dials.set(dialId, {
        binding,
        caps,
        desired: this.desiredFromStatus(status, caps),
        status,
        online: status !== undefined,
        writeTimer: undefined,
        pendingTarget: undefined,
      });
    }
    this.log.info({ dials: [...this.dials.keys()] }, 'controller initialized');
  }

  /** Begin handling dial events and polling for state. */
  async start(): Promise<void> {
    if (this.dials.size === 0) await this.initialize();
    this.transport.onEvent((message) => {
      void this.handleEvent(message).catch((err) =>
        this.log.error({ err, dialId: message.dialId }, 'error handling dial event'),
      );
    });
    await this.transport.start();
    await this.renderAll();
    this.pollTimer = this.setIntervalFn(() => {
      void this.poll().catch((err) => this.log.error({ err }, 'poll failed'));
    }, this.config.POLL_INTERVAL_MS);
  }

  async stop(): Promise<void> {
    if (this.pollTimer !== undefined) this.clearIntervalFn(this.pollTimer);
    for (const runtime of this.dials.values()) {
      if (runtime.writeTimer !== undefined) this.clearTimeoutFn(runtime.writeTimer);
    }
    await this.transport.stop();
    await this.device.close();
  }

  /** Handle a single dial event end-to-end. Public so tests can await it. */
  async handleEvent(message: DialMessage): Promise<void> {
    const runtime = this.dials.get(message.dialId);
    if (!runtime) {
      this.log.warn({ dialId: message.dialId }, 'event from unknown dial; ignoring');
      return;
    }
    const { binding } = runtime;

    // Long-press is a "boost": start a thermal-relief heat cycle on the zone.
    if (message.event.kind === 'longPress') {
      await this.device.startThermalRelief(binding.deviceId, binding.zone, 'heat', RELIEF_MINUTES);
      runtime.desired = { ...runtime.desired, power: 'on' };
      await this.render(runtime);
      return;
    }

    const result = applyEvent(message.event, runtime.desired, runtime.caps);
    runtime.desired = result.next;

    if (result.powerChanged) {
      await this.device.setPower(binding.deviceId, binding.zone, result.next.power);
    }
    if (result.temperatureChanged) {
      if (this.config.WRITE_DEBOUNCE_MS === 0) {
        // No debounce: write immediately and deterministically.
        runtime.pendingTarget = runtime.desired.target;
        await this.flushWrite(runtime);
      } else {
        this.scheduleWrite(runtime);
      }
    }
    await this.render(runtime);
  }

  private scheduleWrite(runtime: DialRuntime): void {
    runtime.pendingTarget = runtime.desired.target;
    if (runtime.writeTimer !== undefined) this.clearTimeoutFn(runtime.writeTimer);
    runtime.writeTimer = this.setTimeoutFn(() => {
      void this.flushWrite(runtime).catch((err) =>
        this.log.error({ err, dialId: runtime.binding.dialId }, 'temperature write failed'),
      );
    }, this.config.WRITE_DEBOUNCE_MS);
  }

  private async flushWrite(runtime: DialRuntime): Promise<void> {
    const target = runtime.pendingTarget;
    runtime.writeTimer = undefined;
    runtime.pendingTarget = undefined;
    if (target === undefined) return;
    const { binding } = runtime;
    await this.device.setTemperature(binding.deviceId, binding.zone, target);
    this.log.debug({ dialId: binding.dialId, target }, 'wrote setpoint');
  }

  private async poll(): Promise<void> {
    for (const runtime of this.dials.values()) {
      const status = await this.readZone(runtime.binding);
      runtime.status = status;
      runtime.online = status !== undefined;
      await this.render(runtime);
    }
  }

  private async loadCaps(binding: DialBinding): Promise<ZoneCapabilities> {
    try {
      const caps = await this.device.getCapabilities(binding.deviceId);
      return caps[binding.zone];
    } catch (err) {
      this.log.warn(
        { err, deviceId: binding.deviceId },
        'getCapabilities failed; using config defaults',
      );
      return {
        unit: 'fahrenheit',
        min: this.config.TEMP_MIN_F,
        max: this.config.TEMP_MAX_F,
        step: this.config.TEMP_STEP_F,
      };
    }
  }

  private async readZone(binding: DialBinding): Promise<ZoneStatus | undefined> {
    try {
      const status = await this.device.getStatus(binding.deviceId);
      return status.online ? status.zones[binding.zone] : undefined;
    } catch (err) {
      this.log.warn({ err, deviceId: binding.deviceId }, 'getStatus failed');
      return undefined;
    }
  }

  private desiredFromStatus(
    status: ZoneStatus | undefined,
    caps: ZoneCapabilities,
  ): DesiredZoneState {
    const mid = Math.round((caps.min + caps.max) / 2);
    return {
      power: status?.power ?? 'off',
      target: status?.target ?? mid,
    };
  }

  private async renderAll(): Promise<void> {
    for (const runtime of this.dials.values()) await this.render(runtime);
  }

  private render(runtime: DialRuntime): Promise<void> {
    const display: DialDisplay = {
      label: runtime.binding.label,
      power: runtime.desired.power,
      target: runtime.desired.target,
      current: runtime.status?.current ?? null,
      active: runtime.status?.active ?? false,
      offline: !runtime.online,
      relief: runtime.status?.relief ?? null,
    };
    return this.transport.publishDisplay(runtime.binding.dialId, display);
  }

  /** Test/debug helper: current desired state of a dial. */
  desiredOf(dialId: string): DesiredZoneState | undefined {
    return this.dials.get(dialId)?.desired;
  }
}

export type { Zone };
