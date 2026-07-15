/**
 * Orion Sleep DeviceClient backed by the official MCP server
 * (https://mcp.orionsleep.com/). Connects with the MCP SDK over Streamable
 * HTTP, authenticates via OAuth 2.1 (oauth-provider.ts), and calls the real
 * tools: list_devices, get_device_state, set_zone, start_thermal_relief.
 *
 * This adapter is the ONLY place that knows the device's quirks:
 *   - the API speaks Celsius (10–45°C); the app/dial works in °F. We convert.
 *   - the API zones are zone_a (RIGHT side) / zone_b (LEFT side); the app uses
 *     left/right. We translate.
 *
 * `deviceId` in this project maps to the Orion device serial_number.
 */

import type { OAuthClientProvider } from '@modelcontextprotocol/sdk/client/auth.js';
import { UnauthorizedError } from '@modelcontextprotocol/sdk/client/auth.js';
import { Client } from '@modelcontextprotocol/sdk/client/index.js';
import { StreamableHTTPClientTransport } from '@modelcontextprotocol/sdk/client/streamableHttp.js';
import type { CallToolResult, Tool } from '@modelcontextprotocol/sdk/types.js';
import type { Power, ReliefType, Zone, ZoneCapabilities, ZoneStatus } from '../../domain/state.js';
import type { Logger } from '../../lib/logger.js';
import { deviceCToFahrenheit, fahrenheitToDeviceC } from '../../lib/temperature.js';
import { type DeviceClient, type DeviceStatus, NotImplementedError } from '../device-client.js';
import { extractJson, isToolError, type OrionToolMap } from './tool-map.js';

export const ORION_MCP_URL = 'https://mcp.orionsleep.com/';

/** app zone -> Orion zone id. zone_a = right side, zone_b = left side. */
const ZONE_TO_ORION: Record<Zone, string> = { left: 'zone_b', right: 'zone_a' };
function orionZoneToApp(id: string): Zone | null {
  if (id === 'zone_a') return 'right';
  if (id === 'zone_b') return 'left';
  return null;
}

interface OrionDevice {
  id: string;
  serial_number: string;
  temperature_range?: { min: number; max: number };
}

export interface OrionMcpClientOptions {
  url: string;
  provider: OAuthClientProvider;
  toolMap: OrionToolMap;
  caps: ZoneCapabilities; // fallback °F caps if the device omits a range
  logger: Logger;
  clientName?: string;
  clientVersion?: string;
}

export class OrionMcpClient implements DeviceClient {
  private readonly client: Client;
  private readonly transport: StreamableHTTPClientTransport;
  private readonly toolMap: OrionToolMap;
  private readonly fallbackCaps: ZoneCapabilities;
  private readonly log: Logger;
  private tools: Map<string, Tool> | undefined;
  private devices: OrionDevice[] | undefined;

  constructor(options: OrionMcpClientOptions) {
    this.toolMap = options.toolMap;
    this.fallbackCaps = options.caps;
    this.log = options.logger.child({ component: 'orion-mcp' });
    this.client = new Client({
      name: options.clientName ?? 'orion-waveshare-rotary-dial',
      version: options.clientVersion ?? '0.1.0',
    });
    this.transport = new StreamableHTTPClientTransport(new URL(options.url), {
      authProvider: options.provider,
    });
  }

  async connect(): Promise<void> {
    try {
      await this.client.connect(this.transport);
    } catch (err) {
      if (err instanceof UnauthorizedError) {
        throw new Error(
          'Not authorized with Orion. Run `npm run orion:login` once to authorize this hub, then restart.',
        );
      }
      throw err;
    }
    await this.loadTools();
  }

  /** Discover and cache the server's tools; also used by the orion:tools CLI. */
  async loadTools(): Promise<Tool[]> {
    const { tools } = await this.client.listTools();
    this.tools = new Map(tools.map((t) => [t.name, t]));
    this.log.info({ tools: tools.map((t) => t.name) }, 'discovered Orion MCP tools');
    return tools;
  }

  private requireTool(name: string): void {
    if (!this.tools) throw new Error('connect() must be called before using tools');
    if (!this.tools.has(name)) {
      const available = [...this.tools.keys()].join(', ') || '(none)';
      throw new NotImplementedError(
        `Orion MCP tool "${name}" not found. Available: ${available}. Set ORION_TOOLS to the correct names.`,
      );
    }
  }

  private async call(name: string, args: Record<string, unknown>): Promise<CallToolResult> {
    this.requireTool(name);
    const result = (await this.client.callTool({ name, arguments: args })) as CallToolResult;
    if (isToolError(result)) {
      throw new Error(`Orion tool "${name}" returned an error: ${JSON.stringify(result.content)}`);
    }
    return result;
  }

  private async fetchDevices(): Promise<OrionDevice[]> {
    const json = extractJson(await this.call(this.toolMap.listDevices, {})) as
      | { devices?: OrionDevice[] }
      | undefined;
    this.devices = Array.isArray(json?.devices) ? json.devices : [];
    return this.devices;
  }

  async listDevices(): Promise<string[]> {
    const devices = await this.fetchDevices();
    return devices.map((d) => d.serial_number).filter((s): s is string => typeof s === 'string');
  }

  async getCapabilities(deviceId: string): Promise<Record<Zone, ZoneCapabilities>> {
    const devices = this.devices ?? (await this.fetchDevices());
    const device = devices.find((d) => d.serial_number === deviceId);
    const range = device?.temperature_range;
    const caps: ZoneCapabilities = range
      ? {
          unit: 'fahrenheit',
          min: deviceCToFahrenheit(range.min),
          max: deviceCToFahrenheit(range.max),
          step: 1,
        }
      : this.fallbackCaps;
    return { left: caps, right: caps };
  }

  async getStatus(deviceId: string): Promise<DeviceStatus> {
    const json = extractJson(await this.call(this.toolMap.getState, { serial: deviceId }));
    const parsed = parseDeviceState(json);
    return { deviceId, online: parsed.online, zones: parsed.zones, fetchedAt: Date.now() };
  }

  async setTemperature(deviceId: string, zone: Zone, target: number): Promise<void> {
    await this.call(this.toolMap.setZone, {
      serial: deviceId,
      zone_id: ZONE_TO_ORION[zone],
      temp: fahrenheitToDeviceC(target),
    });
  }

  async setPower(deviceId: string, zone: Zone, power: Power): Promise<void> {
    await this.call(this.toolMap.setZone, {
      serial: deviceId,
      zone_id: ZONE_TO_ORION[zone],
      on: power === 'on',
    });
  }

  async startThermalRelief(
    deviceId: string,
    zone: Zone,
    type: ReliefType,
    minutes: number,
  ): Promise<void> {
    await this.call(this.toolMap.thermalRelief, {
      serial: deviceId,
      type,
      zones: [ZONE_TO_ORION[zone]],
      duration_minutes: minutes,
    });
  }

  async close(): Promise<void> {
    await this.client.close();
    await this.transport.close();
  }
}

/**
 * Defensive parser for get_device_state output (converting °C -> °F and Orion
 * zone ids -> left/right). The exact field names weren't captured yet, so this
 * accepts several plausible spellings and falls back to nulls per field. Verify
 * against a real `DIAL_READ=1 npm run dial:ref` and tighten if needed.
 */
function parseDeviceState(json: unknown): { online: boolean; zones: Record<Zone, ZoneStatus> } {
  const root = (json ?? {}) as Record<string, unknown>;
  const online = root.online !== false && root.is_online !== false;
  const rawZones = root.zones ?? root;
  const byApp = new Map<Zone, ZoneStatus>();

  const consume = (orionId: string, z: Record<string, unknown>): void => {
    const zone = orionZoneToApp(orionId);
    if (!zone) return;
    const currentC = num(z.temperature ?? z.current_temperature ?? z.current ?? z.temp);
    const targetC = num(z.target ?? z.target_temperature ?? z.set_temperature ?? z.setpoint);
    const on = z.on === true || z.is_on === true || z.power === 'on' || z.active === true;
    byApp.set(zone, {
      zone,
      power: on ? 'on' : 'off',
      target: targetC === null ? null : deviceCToFahrenheit(targetC),
      current: currentC === null ? null : deviceCToFahrenheit(currentC),
      active:
        z.active === true || (on && targetC !== null && currentC !== null && targetC !== currentC),
      relief: reliefOf(z.thermal_relief ?? z.relief),
    });
  };

  if (Array.isArray(rawZones)) {
    for (const z of rawZones) {
      if (z && typeof z === 'object' && typeof (z as { id?: unknown }).id === 'string') {
        consume((z as { id: string }).id, z as Record<string, unknown>);
      }
    }
  } else if (rawZones && typeof rawZones === 'object') {
    for (const [id, z] of Object.entries(rawZones)) {
      if (z && typeof z === 'object') consume(id, z as Record<string, unknown>);
    }
  }

  return {
    online,
    zones: {
      left: byApp.get('left') ?? offZone('left'),
      right: byApp.get('right') ?? offZone('right'),
    },
  };
}

function offZone(zone: Zone): ZoneStatus {
  return { zone, power: 'off', target: null, current: null, active: false, relief: null };
}
function num(v: unknown): number | null {
  return typeof v === 'number' && Number.isFinite(v) ? v : null;
}
function reliefOf(v: unknown): ReliefType | null {
  const t = typeof v === 'string' ? v : ((v as { type?: unknown } | null)?.type ?? null);
  return t === 'heat' || t === 'cool' ? t : null;
}
