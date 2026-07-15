/**
 * Orion control for THIS dial's single zone. Same tool calls + °F<->°C +
 * zone_a/zone_b translation as the hub's OrionMcpClient, scoped to one zone.
 */

import { Mcp } from "mcp";
import type { Auth } from "oauth";
import { ORION } from "config";
import { deviceCToF, fahrenheitToDeviceC, orionZoneToApp, ZONE_TO_ORION } from "zones";

export interface ZoneState {
  power: "on" | "off";
  targetF: number | null;
  currentF: number | null;
  active: boolean;
  relief: "heat" | "cool" | null;
  online: boolean;
}

/** Unwrap an MCP tool result (content[].text is a JSON string) to a value. */
function unwrap(result: unknown): unknown {
  const r = result as {
    structuredContent?: unknown;
    content?: Array<{ type: string; text?: string }>;
  };
  if (r?.structuredContent !== undefined) return r.structuredContent;
  const text = (r?.content ?? [])
    .filter((c) => c.type === "text")
    .map((c) => c.text ?? "")
    .join("\n");
  if (!text) return result;
  try {
    return JSON.parse(text);
  } catch {
    return result;
  }
}

function num(v: unknown): number | null {
  return typeof v === "number" && isFinite(v) ? v : null;
}

export class Orion {
  private readonly mcp: Mcp;
  private serial = ORION.deviceSerial;
  private readonly orionZone = ZONE_TO_ORION[ORION.zone];

  constructor(auth: Auth) {
    this.mcp = new Mcp(auth);
  }

  async connect(): Promise<void> {
    await this.mcp.initialize();
    if (!this.serial) this.serial = await this.discoverSerial();
  }

  private async discoverSerial(): Promise<string> {
    const json = unwrap(await this.mcp.callTool("list_devices", {})) as {
      devices?: Array<{ serial_number?: string }>;
    };
    const devices = json?.devices ?? [];
    const serial = devices[0]?.serial_number;
    if (!serial) throw new Error("no Orion device found on this account");
    return serial;
  }

  async setTemperature(targetF: number): Promise<void> {
    await this.mcp.callTool("set_zone", {
      serial: this.serial,
      zone_id: this.orionZone,
      temp: fahrenheitToDeviceC(targetF),
    });
  }

  async setPower(on: boolean): Promise<void> {
    await this.mcp.callTool("set_zone", { serial: this.serial, zone_id: this.orionZone, on });
  }

  async boost(minutes = 30): Promise<void> {
    await this.mcp.callTool("start_thermal_relief", {
      serial: this.serial,
      type: "heat",
      zones: [this.orionZone],
      duration_minutes: minutes,
    });
  }

  async getState(): Promise<ZoneState> {
    const root = unwrap(await this.mcp.callTool("get_device_state", { serial: this.serial })) as
      | Record<string, unknown>
      | undefined;
    return parseZone(root ?? {}, this.orionZone);
  }
}

/** Extract this zone from a get_device_state payload (defensive; see hub parser). */
function parseZone(root: Record<string, unknown>, orionZoneId: string): ZoneState {
  const online = root.online !== false && root.is_online !== false;
  const rawZones = (root.zones ?? root) as unknown;
  let z: Record<string, unknown> | undefined;

  if (Array.isArray(rawZones)) {
    z = rawZones.find(
      (e) => e && typeof e === "object" && (e as { id?: unknown }).id === orionZoneId,
    ) as Record<string, unknown> | undefined;
  } else if (rawZones && typeof rawZones === "object") {
    z = (rawZones as Record<string, unknown>)[orionZoneId] as Record<string, unknown> | undefined;
  }
  if (!z || orionZoneToApp(orionZoneId) === null) {
    return { power: "off", targetF: null, currentF: null, active: false, relief: null, online };
  }

  const currentC = num(z.temperature ?? z.current_temperature ?? z.current ?? z.temp);
  const targetC = num(z.target ?? z.target_temperature ?? z.set_temperature ?? z.setpoint);
  const on = z.on === true || z.is_on === true || z.power === "on" || z.active === true;
  const reliefRaw = (z.thermal_relief ?? z.relief) as { type?: unknown } | string | null | undefined;
  const reliefType =
    typeof reliefRaw === "string" ? reliefRaw : ((reliefRaw as { type?: unknown })?.type ?? null);

  return {
    power: on ? "on" : "off",
    targetF: targetC === null ? null : deviceCToF(targetC),
    currentF: currentC === null ? null : deviceCToF(currentC),
    active: z.active === true || (on && targetC !== null && currentC !== null && targetC !== currentC),
    relief: reliefType === "heat" || reliefType === "cool" ? reliefType : null,
    online,
  };
}
