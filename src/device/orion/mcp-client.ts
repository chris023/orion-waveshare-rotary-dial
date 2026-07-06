/**
 * Orion Sleep DeviceClient backed by the official MCP server
 * (https://mcp.orionsleep.com/). Connects with the MCP SDK over Streamable
 * HTTP, authenticates via OAuth 2.1 (see oauth-provider.ts), discovers the
 * advertised tools, and maps our device operations onto them (see tool-map.ts).
 *
 * The exact tool argument/response schemas are only known after an authenticated
 * `listTools()`. Discovery + basic tool invocation are fully implemented; the
 * response coercion in getStatus()/listDevices() is defensive and throws an
 * actionable error if the real shape differs, so finalizing the mapping is a
 * localized change once `npm run orion:tools` reveals the schemas.
 */

import type { OAuthClientProvider } from '@modelcontextprotocol/sdk/client/auth.js';
import { UnauthorizedError } from '@modelcontextprotocol/sdk/client/auth.js';
import { Client } from '@modelcontextprotocol/sdk/client/index.js';
import { StreamableHTTPClientTransport } from '@modelcontextprotocol/sdk/client/streamableHttp.js';
import type { CallToolResult, Tool } from '@modelcontextprotocol/sdk/types.js';
import type { Power, Zone, ZoneCapabilities, ZoneStatus } from '../../domain/state.js';
import { isZone } from '../../domain/state.js';
import type { Logger } from '../../lib/logger.js';
import { type DeviceClient, type DeviceStatus, NotImplementedError } from '../device-client.js';
import { extractJson, isToolError, type OrionToolMap } from './tool-map.js';

export const ORION_MCP_URL = 'https://mcp.orionsleep.com/';

export interface OrionMcpClientOptions {
  url: string;
  provider: OAuthClientProvider;
  toolMap: OrionToolMap;
  caps: ZoneCapabilities;
  logger: Logger;
  clientName?: string;
  clientVersion?: string;
}

export class OrionMcpClient implements DeviceClient {
  private readonly client: Client;
  private readonly transport: StreamableHTTPClientTransport;
  private readonly toolMap: OrionToolMap;
  private readonly caps: ZoneCapabilities;
  private readonly log: Logger;
  private tools: Map<string, Tool> | undefined;

  constructor(options: OrionMcpClientOptions) {
    this.toolMap = options.toolMap;
    this.caps = options.caps;
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

  /** Discover and cache the server's tools; used by callers and the CLI. */
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
        `Orion MCP tool "${name}" not found. Available: ${available}. Run \`npm run orion:tools\` and set ORION_TOOLS to the correct names.`,
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

  async listDevices(): Promise<string[]> {
    const result = await this.call(this.toolMap.listDevices, {});
    const json = extractJson(result);
    const ids = coerceDeviceIds(json);
    if (!ids) {
      throw new NotImplementedError(
        `Could not read device ids from "${this.toolMap.listDevices}" output. Inspect it via \`npm run orion:tools\` and adjust coerceDeviceIds() / ORION_TOOLS.`,
      );
    }
    return ids;
  }

  getCapabilities(_deviceId: string): Promise<Record<Zone, ZoneCapabilities>> {
    // Orion likely does not expose capabilities as a tool; use configured bounds.
    return Promise.resolve({ left: this.caps, right: this.caps });
  }

  async getStatus(deviceId: string): Promise<DeviceStatus> {
    const result = await this.call(this.toolMap.getStatus, { deviceId });
    const json = extractJson(result);
    const zones = coerceZones(json);
    if (!zones) {
      throw new NotImplementedError(
        `Could not read zone status from "${this.toolMap.getStatus}" output. Inspect it via \`npm run orion:tools\` and finalize coerceZones() in mcp-client.ts.`,
      );
    }
    return { deviceId, online: true, zones, fetchedAt: Date.now() };
  }

  async setTemperature(deviceId: string, zone: Zone, target: number): Promise<void> {
    await this.call(this.toolMap.setTemperature, { deviceId, zone, target });
  }

  async setPower(deviceId: string, zone: Zone, power: Power): Promise<void> {
    await this.call(this.toolMap.setPower, { deviceId, zone, power });
  }

  async close(): Promise<void> {
    await this.client.close();
    await this.transport.close();
  }
}

/** Defensive: pull device ids from common shapes ([ids], {devices:[...]}, [{id}]). */
function coerceDeviceIds(json: unknown): string[] | undefined {
  const arr = Array.isArray(json)
    ? json
    : typeof json === 'object' &&
        json !== null &&
        Array.isArray((json as { devices?: unknown }).devices)
      ? (json as { devices: unknown[] }).devices
      : undefined;
  if (!arr) return undefined;
  const ids = arr
    .map((d) =>
      typeof d === 'string'
        ? d
        : ((d as { id?: unknown; deviceId?: unknown })?.id ??
          (d as { deviceId?: unknown })?.deviceId),
    )
    .filter((d): d is string => typeof d === 'string');
  return ids.length > 0 ? ids : undefined;
}

/** Defensive: pull per-zone status from a {zones:{left,right}} or [{zone,...}] shape. */
function coerceZones(json: unknown): Record<Zone, ZoneStatus> | undefined {
  if (typeof json !== 'object' || json === null) return undefined;
  const rawZones = (json as { zones?: unknown }).zones ?? json;
  const out: Partial<Record<Zone, ZoneStatus>> = {};

  const put = (zone: string, value: Record<string, unknown>): void => {
    if (!isZone(zone)) return;
    out[zone] = {
      zone,
      power: coercePower(value.power),
      target: numOrNull(value.target ?? value.targetTemperature),
      current: numOrNull(value.current ?? value.currentTemperature),
      active: value.active === true,
    };
  };

  if (Array.isArray(rawZones)) {
    for (const z of rawZones) {
      if (z && typeof z === 'object' && typeof (z as { zone?: unknown }).zone === 'string') {
        put((z as { zone: string }).zone, z as Record<string, unknown>);
      }
    }
  } else if (typeof rawZones === 'object' && rawZones !== null) {
    for (const [zone, value] of Object.entries(rawZones)) {
      if (value && typeof value === 'object') put(zone, value as Record<string, unknown>);
    }
  }
  return out.left && out.right ? (out as Record<Zone, ZoneStatus>) : undefined;
}

function coercePower(value: unknown): Power {
  if (value === 'on' || value === 'off' || value === 'standby') return value;
  if (value === true) return 'on';
  if (value === false) return 'off';
  return 'off';
}

function numOrNull(value: unknown): number | null {
  return typeof value === 'number' && Number.isFinite(value) ? value : null;
}
