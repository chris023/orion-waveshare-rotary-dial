/**
 * Raw MCP client over Streamable HTTP — a direct port of RawMcp in
 * firmware/reference-dial/dial.ts. No SDK: JSON-RPC 2.0 over HTTPS, with the
 * Mcp-Session-Id header and SSE-or-JSON response parsing. Refreshes on 401.
 */

import { httpsRequest } from "net";
import type { Auth } from "oauth";
import { ORION } from "config";

interface RpcMessage {
  result?: unknown;
  error?: { code: number; message: string };
}

export class Mcp {
  private id = 0;
  private sessionId: string | undefined;
  private readonly protocolVersion = "2025-06-18";

  constructor(private readonly auth: Auth) {}

  private async rpc(method: string, params?: unknown, notification = false): Promise<unknown> {
    return this.rpcOnce(method, params, notification, true);
  }

  private async rpcOnce(
    method: string,
    params: unknown,
    notification: boolean,
    allowRefresh: boolean,
  ): Promise<unknown> {
    const token = await this.auth.getAccessToken();
    const body: Record<string, unknown> = { jsonrpc: "2.0", method };
    if (params !== undefined) body.params = params;
    if (!notification) body.id = ++this.id;

    const headers: Record<string, string> = {
      "content-type": "application/json",
      accept: "application/json, text/event-stream",
      authorization: `Bearer ${token}`,
    };
    if (this.sessionId) headers["mcp-session-id"] = this.sessionId;
    if (method !== "initialize") headers["mcp-protocol-version"] = this.protocolVersion;

    const res = await httpsRequest(ORION.mcpUrl, {
      method: "POST",
      headers,
      body: JSON.stringify(body),
    });

    if (res.status === 401 && allowRefresh) {
      await this.auth.refresh();
      return this.rpcOnce(method, params, notification, false);
    }

    const sid = res.headers.get("mcp-session-id");
    if (sid) this.sessionId = sid;

    if (notification) return undefined;
    if (res.status < 200 || res.status >= 300) {
      throw new Error(`${method} HTTP ${res.status}: ${res.body.slice(0, 200)}`);
    }
    const msg = parseRpcBody(res.body, res.headers.get("content-type") ?? "");
    if (msg.error) throw new Error(`${method} JSON-RPC error ${msg.error.code}: ${msg.error.message}`);
    return msg.result;
  }

  async initialize(): Promise<unknown> {
    const result = await this.rpc("initialize", {
      protocolVersion: this.protocolVersion,
      capabilities: {},
      clientInfo: { name: ORION.clientName, version: "0.1.0" },
    });
    await this.rpc("notifications/initialized", undefined, true);
    return result;
  }

  callTool(name: string, args: Record<string, unknown>): Promise<unknown> {
    return this.rpc("tools/call", { name, arguments: args });
  }

  async listTools(): Promise<Array<{ name: string }>> {
    const r = (await this.rpc("tools/list")) as { tools: Array<{ name: string }> };
    return r.tools;
  }
}

/** Parse an application/json body, or extract the JSON-RPC frame from SSE. */
function parseRpcBody(text: string, contentType: string): RpcMessage {
  if (contentType.indexOf("text/event-stream") >= 0) {
    const dataLines = text
      .split("\n")
      .filter((l) => l.indexOf("data:") === 0)
      .map((l) => l.slice(5).trim());
    for (let i = dataLines.length - 1; i >= 0; i -= 1) {
      try {
        const msg = JSON.parse(dataLines[i]) as RpcMessage;
        if (msg && (msg.result !== undefined || msg.error !== undefined)) return msg;
      } catch {
        /* keep scanning */
      }
    }
    throw new Error(`no JSON-RPC message in SSE body: ${text.slice(0, 120)}`);
  }
  return JSON.parse(text) as RpcMessage;
}
