# Orion Sleep integration (MCP)

Orion Sleep exposes an **official MCP server** — so the hub controls the topper
through the Model Context Protocol, authenticated with OAuth 2.1. There is no
REST reverse-engineering involved.

## What we verified

Probing `https://mcp.orionsleep.com/` (2026-07):

- `POST /` → `401` with `WWW-Authenticate: Bearer realm="OAuth", resource_metadata=...`
  → it's an OAuth-protected MCP endpoint (Streamable HTTP).
- `GET /.well-known/oauth-authorization-server`:
  ```json
  {
    "issuer": "https://mcp.orionsleep.com",
    "authorization_endpoint": "https://app.orionsleep.com/oauth/authorize",
    "token_endpoint": "https://mcp.orionsleep.com/oauth/token",
    "registration_endpoint": "https://mcp.orionsleep.com/oauth/register",
    "scopes_supported": ["orion:mcp"],
    "grant_types_supported": ["authorization_code", "refresh_token"],
    "code_challenge_methods_supported": ["plain", "S256"]
  }
  ```
- `GET /.well-known/oauth-protected-resource/` → resource `https://mcp.orionsleep.com`, scope `orion:mcp`, bearer in header.

So: **Authorization Code + PKCE**, with **Dynamic Client Registration** — the hub
registers itself automatically; you just approve it once in the browser.

## How the hub connects

`OrionMcpClient` (`src/device/orion/mcp-client.ts`) uses the official
`@modelcontextprotocol/sdk`:

- `StreamableHTTPClientTransport(new URL(ORION_MCP_URL), { authProvider })`
- `OrionOAuthProvider` (`src/device/orion/oauth-provider.ts`) implements the SDK's
  `OAuthClientProvider`, persisting the registered client + tokens + PKCE verifier
  to `ORION_TOKENS_FILE` (default `./secrets/orion-oauth.json`, gitignored).

### One-time login

```bash
npm run orion:login
```

This starts a loopback server on `ORION_OAUTH_PORT` (default 8788), opens the
Orion authorization page, captures the redirect, exchanges the code, and saves
tokens. After that the long-running hub refreshes tokens automatically and needs
no further interaction.

> The registered redirect URI is `http://localhost:8788/callback`. If Orion's
> app requires pre-registered redirect URIs (rather than DCR), you may need to
> register this client with them; DCR at `/oauth/register` should make that
> automatic.

## Finalizing the tool mapping

The exact **tool names and input/output schemas** the server exposes are only
visible once authenticated. After logging in:

```bash
npm run orion:tools
```

This prints all 17 tools with their JSON input schemas.

The tool mapping is **confirmed** (`src/device/orion/tool-map.ts`): `list_devices`,
`get_device_state`, `set_zone` (both temperature and power), `start_thermal_relief`.
Override any name via `ORION_TOOLS` if Orion renames them.

### Model translation (isolated in `OrionMcpClient`)

The rest of the app works in **°F** and **left/right**; the adapter translates to
the device's units at the boundary:

- **°F ↔ °C** — the API takes Celsius (10–45°C); we convert (`lib/temperature.ts`).
- **left/right ↔ zone_a/zone_b** — `zone_a` is the RIGHT side, `zone_b` the LEFT,
  so `left → zone_b`, `right → zone_a`.
- **deviceId = the device `serial_number`** (from `list_devices`). Set your real
  serial in `DIAL_BINDINGS` for `DEVICE_CLIENT=orion`.
- **long-press → `start_thermal_relief`** (a heat "boost").

`getStatus` parses `get_device_state` defensively; its exact output field names
should be confirmed with a `DIAL_READ=1 npm run dial:ref` run and tightened in
`parseDeviceState()` if they differ.

Keep `DEVICE_CLIENT=fake` (the default) to develop the dial UX against the
in-memory topper simulator + the virtual dial (`simulator/`).

## Security notes

- `secrets/` and `*.har` are gitignored — never commit tokens or captures.
- The tokens file is written with `0600` permissions.
- Automating your own account is expected use of the MCP server, but review
  Orion's terms for third-party client behavior.
