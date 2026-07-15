/**
 * Maps this project's device operations onto the Orion MCP tool names.
 *
 * These are the CONFIRMED tool names (verified against the live server). They can
 * still be overridden via the ORION_TOOLS env var (JSON) if Orion renames them.
 * Both setTemperature and setPower map onto `set_zone`.
 */

import type { CallToolResult } from '@modelcontextprotocol/sdk/types.js';

export interface OrionToolMap {
  listDevices: string;
  getState: string;
  setZone: string;
  thermalRelief: string;
}

export const DEFAULT_TOOL_MAP: OrionToolMap = {
  listDevices: 'list_devices',
  getState: 'get_device_state',
  setZone: 'set_zone',
  thermalRelief: 'start_thermal_relief',
};

export function resolveToolMap(override: Partial<OrionToolMap> | undefined): OrionToolMap {
  return { ...DEFAULT_TOOL_MAP, ...override };
}

/**
 * Extract a JSON value from an MCP tool result. Orion returns the payload as a
 * text content block (a JSON string); some tools also set structuredContent.
 */
export function extractJson(result: CallToolResult): unknown {
  if (result.structuredContent !== undefined) return result.structuredContent;
  const text = result.content
    ?.filter((c): c is { type: 'text'; text: string } => c.type === 'text')
    .map((c) => c.text)
    .join('\n');
  if (!text) return undefined;
  try {
    return JSON.parse(text);
  } catch {
    return text;
  }
}

export function isToolError(result: CallToolResult): boolean {
  return result.isError === true;
}
