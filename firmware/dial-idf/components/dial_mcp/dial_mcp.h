#pragma once
#include <stdbool.h>
#include <stddef.h>

/*
 * Raw MCP client over Streamable HTTP for the Orion server
 * (https://mcp.orionsleep.com/). Port of the RawMcp class in
 * archive/reference-dial/dial.ts.
 *
 * One esp_http_client POST per JSON-RPC call: sends the Bearer access token
 * (from dial_oauth), Accept: application/json + text/event-stream, captures the
 * Mcp-Session-Id response header on initialize and replays it (plus the
 * MCP-Protocol-Version) on subsequent calls, and parses either a JSON body or
 * an SSE (text/event-stream) body's `data:` frames.
 */

// initialize + notifications/initialized. Captures the session id. On success
// *server_out (if non-NULL) receives a malloc'd "name version" string.
bool dial_mcp_connect(char **server_out);

// tools/list — returns the number of tools, or -1 on error.
int dial_mcp_list_tools_count(void);

// tools/call. args_json is a JSON object string ("{}" for no args). On success
// *result_out is a malloc'd string of the UNWRAPPED tool result (structuredContent
// if present, else the concatenated text content — which for Orion is itself JSON).
// Caller frees. Returns false on transport / JSON-RPC / tool error.
bool dial_mcp_call_tool(const char *name, const char *args_json, char **result_out);

// The most recent MCP error (HTTP status or JSON-RPC message), for display.
const char *dial_mcp_last_error(void);
