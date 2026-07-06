/**
 * Maps this project's device operations onto the MCP tool names Orion exposes.
 *
 * The real tool names + input schemas are only visible after an authenticated
 * `listTools()` — run `npm run orion:tools` to print them, then override these
 * defaults via the ORION_TOOLS env var (JSON) if they differ. Until confirmed,
 * these are best-guess names; OrionMcpClient fails loudly if a mapped tool is
 * absent from the server's advertised tools.
 */

import type { CallToolResult } from '@modelcontextprotocol/sdk/types.js';

export interface OrionToolMap {
  listDevices: string;
  getStatus: string;
  setTemperature: string;
  setPower: string;
}

export const DEFAULT_TOOL_MAP: OrionToolMap = {
  listDevices: 'list_devices',
  getStatus: 'get_status',
  setTemperature: 'set_temperature',
  setPower: 'set_power',
};

export function resolveToolMap(override: Partial<OrionToolMap> | undefined): OrionToolMap {
  return { ...DEFAULT_TOOL_MAP, ...override };
}

/**
 * Best-effort extraction of a JSON object from an MCP tool result. MCP tools
 * return content blocks (usually text); many servers also populate
 * `structuredContent`. Prefer the structured field, else parse text as JSON.
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
