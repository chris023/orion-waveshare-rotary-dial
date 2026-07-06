/**
 * REFERENCE DIAL — the on-device algorithm, in runnable Node (zero deps, no SDK).
 *
 * This is throwaway scaffolding whose only job is to be the line-by-line
 * blueprint for the ESP32-S3 firmware. It performs the ENTIRE flow a fully
 * self-contained dial must do, using only Node built-ins + global fetch and
 * RAW HTTP — deliberately NO @modelcontextprotocol/sdk and NO oauth library,
 * because none of those exist on the microcontroller:
 *
 *   1. Discover Orion's OAuth metadata          (GET /.well-known/...)
 *   2. Dynamic Client Registration              (POST /oauth/register)   -> stored in "NVS"
 *   3. PKCE (S256) + build the authorize URL
 *   4. User consent on their PHONE (QR on the real dial) -> loopback catches ?code
 *   5. Exchange code for tokens                  (POST /oauth/token)      -> stored in "NVS"
 *   6. (later runs) refresh the access token     (grant_type=refresh_token)
 *   7. Raw MCP over Streamable HTTP: initialize -> notifications/initialized
 *      -> tools/list  (handles the Mcp-Session-Id header + SSE responses)
 *
 * Each ESP32-equivalent step is flagged with `FIRMWARE:` in comments.
 *
 * Run:  npm run dial:ref            (full interactive flow; opens a browser)
 *       DIAL_PRINT_URL_ONLY=1 npm run dial:ref   (stop after step 3; no consent)
 *
 * State (the "NVS" equivalent) persists to secrets/reference-dial.json (gitignored).
 * On the ESP32 this lives in NVS / encrypted flash.
 */

import { exec } from 'node:child_process';
import { createHash, randomBytes } from 'node:crypto';
import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { createServer } from 'node:http';
import { platform } from 'node:os';
import { dirname } from 'node:path';

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------
const MCP_URL = process.env.ORION_MCP_URL ?? 'https://mcp.orionsleep.com/';
const PORT = Number(process.env.REF_DIAL_PORT ?? 8790);
// FIRMWARE: the real dial advertises a stable mDNS name (e.g. orion-knob-left.local)
// and uses http://<that-name>:<port>/callback so a phone on the same Wi-Fi can
// hand the code back. localhost works here because consent happens on this host.
const REDIRECT_HOST = process.env.REF_DIAL_REDIRECT_HOST ?? 'localhost';
const REDIRECT_URI = `http://${REDIRECT_HOST}:${PORT}/callback`;
const SCOPE = 'orion:mcp';
const STATE_FILE = process.env.REF_DIAL_STATE ?? './secrets/reference-dial.json';
const PRINT_URL_ONLY = process.env.DIAL_PRINT_URL_ONLY === '1';

// ---------------------------------------------------------------------------
// Tiny persistent state ("NVS")
// ---------------------------------------------------------------------------
interface DialState {
  clientId?: string;
  redirectUri?: string;
  tokens?: { access_token: string; refresh_token?: string; expiresAt?: number };
}

function loadState(): DialState {
  if (!existsSync(STATE_FILE)) return {};
  try {
    return JSON.parse(readFileSync(STATE_FILE, 'utf8')) as DialState;
  } catch {
    return {};
  }
}
function saveState(s: DialState): void {
  mkdirSync(dirname(STATE_FILE), { recursive: true });
  writeFileSync(STATE_FILE, `${JSON.stringify(s, null, 2)}\n`, { mode: 0o600 });
}

const b64url = (buf: Buffer): string =>
  buf.toString('base64').replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');

function log(step: string, msg: string): void {
  console.log(`\n[${step}] ${msg}`);
}

// ---------------------------------------------------------------------------
// Step 1 — Discovery
// ---------------------------------------------------------------------------
interface Discovery {
  authorization_endpoint: string;
  token_endpoint: string;
  registration_endpoint: string;
  resource: string;
}

async function discover(): Promise<Discovery> {
  const origin = new URL(MCP_URL).origin;
  // FIRMWARE: an esp_http_client GET; parse JSON with cJSON/ArduinoJson.
  const as = await getJson(`${origin}/.well-known/oauth-authorization-server`);
  const prm = await getJson(`${origin}/.well-known/oauth-protected-resource/`).catch(() => ({}));
  const resource = (prm as { resource?: string }).resource ?? origin;
  return {
    authorization_endpoint: as.authorization_endpoint,
    token_endpoint: as.token_endpoint,
    registration_endpoint: as.registration_endpoint,
    resource,
  };
}

// ---------------------------------------------------------------------------
// Step 2 — Dynamic Client Registration (once; reused after)
// ---------------------------------------------------------------------------
async function ensureClient(disc: Discovery, state: DialState): Promise<string> {
  if (state.clientId && state.redirectUri === REDIRECT_URI) return state.clientId;
  // FIRMWARE: POST JSON; store the returned client_id in NVS. Public client (no secret).
  const res = await fetch(disc.registration_endpoint, {
    method: 'POST',
    headers: { 'content-type': 'application/json', accept: 'application/json' },
    body: JSON.stringify({
      client_name: 'orion-knob-dial',
      redirect_uris: [REDIRECT_URI],
      grant_types: ['authorization_code', 'refresh_token'],
      response_types: ['code'],
      token_endpoint_auth_method: 'none',
      scope: SCOPE,
    }),
  });
  if (!res.ok) throw new Error(`DCR failed: HTTP ${res.status} ${await res.text()}`);
  const reg = (await res.json()) as { client_id: string };
  state.clientId = reg.client_id;
  state.redirectUri = REDIRECT_URI;
  saveState(state);
  return reg.client_id;
}

// ---------------------------------------------------------------------------
// Steps 3-5 — PKCE, consent, token exchange
// ---------------------------------------------------------------------------
async function authorize(disc: Discovery, clientId: string, state: DialState): Promise<void> {
  // FIRMWARE: verifier = 32 random bytes (esp_random) -> base64url; challenge = SHA256.
  const codeVerifier = b64url(randomBytes(32));
  const codeChallenge = b64url(createHash('sha256').update(codeVerifier).digest());
  const oauthState = b64url(randomBytes(16));

  const authUrl = new URL(disc.authorization_endpoint);
  authUrl.search = new URLSearchParams({
    response_type: 'code',
    client_id: clientId,
    redirect_uri: REDIRECT_URI,
    scope: SCOPE,
    state: oauthState,
    code_challenge: codeChallenge,
    code_challenge_method: 'S256',
    resource: disc.resource, // RFC 8707 — MCP requires binding the token to the server
  }).toString();

  log('3', 'Authorize URL built (the real dial renders this as a QR code on its screen):');
  console.log(`\n  ${authUrl.toString()}\n`);

  if (PRINT_URL_ONLY) {
    log('3', 'DIAL_PRINT_URL_ONLY set — stopping before consent. Steps 1-3 verified.');
    return;
  }

  // FIRMWARE: the dial runs a tiny HTTP server on :PORT and waits for GET /callback.
  const code = await waitForCode(oauthState, () => openBrowser(authUrl.toString()));
  log('4', 'Received authorization code from the redirect.');

  // FIRMWARE: POST form-urlencoded to the token endpoint; store tokens in NVS.
  const res = await fetch(disc.token_endpoint, {
    method: 'POST',
    headers: { 'content-type': 'application/x-www-form-urlencoded', accept: 'application/json' },
    body: new URLSearchParams({
      grant_type: 'authorization_code',
      code,
      redirect_uri: REDIRECT_URI,
      client_id: clientId,
      code_verifier: codeVerifier,
      resource: disc.resource,
    }),
  });
  if (!res.ok) throw new Error(`Token exchange failed: HTTP ${res.status} ${await res.text()}`);
  const tok = (await res.json()) as {
    access_token: string;
    refresh_token?: string;
    expires_in?: number;
  };
  state.tokens = {
    access_token: tok.access_token,
    ...(tok.refresh_token ? { refresh_token: tok.refresh_token } : {}),
    ...(tok.expires_in ? { expiresAt: Date.now() + tok.expires_in * 1000 } : {}),
  };
  saveState(state);
  log('5', 'Tokens acquired and saved. The dial is now autonomous (refreshes itself).');
}

// ---------------------------------------------------------------------------
// Step 6 — Refresh (used on later runs when the access token has expired)
// ---------------------------------------------------------------------------
async function refresh(disc: Discovery, clientId: string, state: DialState): Promise<boolean> {
  const rt = state.tokens?.refresh_token;
  if (!rt) return false;
  const res = await fetch(disc.token_endpoint, {
    method: 'POST',
    headers: { 'content-type': 'application/x-www-form-urlencoded', accept: 'application/json' },
    body: new URLSearchParams({
      grant_type: 'refresh_token',
      refresh_token: rt,
      client_id: clientId,
      resource: disc.resource,
      scope: SCOPE,
    }),
  });
  if (!res.ok) return false;
  const tok = (await res.json()) as {
    access_token: string;
    refresh_token?: string;
    expires_in?: number;
  };
  state.tokens = {
    access_token: tok.access_token,
    refresh_token: tok.refresh_token ?? rt,
    ...(tok.expires_in ? { expiresAt: Date.now() + tok.expires_in * 1000 } : {}),
  };
  saveState(state);
  return true;
}

// ---------------------------------------------------------------------------
// Step 7 — Raw MCP client over Streamable HTTP
// ---------------------------------------------------------------------------
class RawMcp {
  private id = 0;
  private sessionId: string | undefined;
  private readonly protocolVersion = '2025-06-18';

  constructor(
    private readonly url: string,
    private readonly accessToken: string,
  ) {}

  // FIRMWARE: one esp_http_client POST. Send Accept for both json and event-stream;
  // read the Mcp-Session-Id response header on initialize; if the response
  // Content-Type is text/event-stream, parse the `data:` lines as JSON-RPC.
  private async rpc(method: string, params?: unknown, notification = false): Promise<unknown> {
    const body: Record<string, unknown> = { jsonrpc: '2.0', method };
    if (params !== undefined) body.params = params;
    if (!notification) body.id = ++this.id;

    const headers: Record<string, string> = {
      'content-type': 'application/json',
      accept: 'application/json, text/event-stream',
      authorization: `Bearer ${this.accessToken}`,
    };
    if (this.sessionId) headers['mcp-session-id'] = this.sessionId;
    if (method !== 'initialize') headers['mcp-protocol-version'] = this.protocolVersion;

    const res = await fetch(this.url, { method: 'POST', headers, body: JSON.stringify(body) });
    const sid = res.headers.get('mcp-session-id');
    if (sid) this.sessionId = sid;

    if (notification) {
      if (res.status !== 202 && !res.ok) {
        throw new Error(`notification ${method} failed: HTTP ${res.status}`);
      }
      return undefined;
    }
    if (!res.ok) throw new Error(`${method} failed: HTTP ${res.status} ${await res.text()}`);

    const message = parseRpcBody(await res.text(), res.headers.get('content-type') ?? '');
    if (message.error) {
      throw new Error(`${method} JSON-RPC error ${message.error.code}: ${message.error.message}`);
    }
    return message.result;
  }

  async initialize(): Promise<unknown> {
    const result = await this.rpc('initialize', {
      protocolVersion: this.protocolVersion,
      capabilities: {},
      clientInfo: { name: 'orion-knob-dial', version: '0.1.0' },
    });
    // FIRMWARE: MUST send this notification after initialize before other calls.
    await this.rpc('notifications/initialized', undefined, true);
    return result;
  }

  async listTools(): Promise<Array<{ name: string; description?: string; inputSchema?: unknown }>> {
    const result = (await this.rpc('tools/list')) as {
      tools: Array<{ name: string; description?: string; inputSchema?: unknown }>;
    };
    return result.tools;
  }

  async callTool(name: string, args: Record<string, unknown>): Promise<unknown> {
    return this.rpc('tools/call', { name, arguments: args });
  }
}

/** Parse a Streamable-HTTP response body: either JSON, or SSE with `data:` frames. */
function parseRpcBody(
  text: string,
  contentType: string,
): { result?: unknown; error?: { code: number; message: string } } {
  if (contentType.includes('text/event-stream')) {
    // FIRMWARE: accumulate `data:` lines until a blank line, then JSON-parse.
    const dataLines = text
      .split('\n')
      .filter((l) => l.startsWith('data:'))
      .map((l) => l.slice(5).trim());
    for (const d of dataLines.reverse()) {
      try {
        const msg = JSON.parse(d);
        if (msg && (msg.result !== undefined || msg.error !== undefined)) return msg;
      } catch {
        /* keep looking */
      }
    }
    throw new Error(`no JSON-RPC message in SSE body: ${text.slice(0, 200)}`);
  }
  return JSON.parse(text);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
async function getJson(url: string): Promise<any> {
  const res = await fetch(url, { headers: { accept: 'application/json' } });
  if (!res.ok) throw new Error(`GET ${url} -> HTTP ${res.status}`);
  return res.json();
}

/** Recursively find the first string value stored under `key` anywhere in a value. */
function findFirstString(value: unknown, key: string): string | undefined {
  if (Array.isArray(value)) {
    for (const item of value) {
      const found = findFirstString(item, key);
      if (found) return found;
    }
  } else if (value && typeof value === 'object') {
    for (const [k, v] of Object.entries(value)) {
      if (k === key && typeof v === 'string') return v;
      const found = findFirstString(v, key);
      if (found) return found;
    }
  }
  return undefined;
}

function waitForCode(expectedState: string, onReady: () => void): Promise<string> {
  return new Promise((resolve, reject) => {
    const server = createServer((req, res) => {
      const url = new URL(req.url ?? '/', REDIRECT_URI);
      if (url.pathname !== '/callback') {
        res.writeHead(404).end();
        return;
      }
      res.writeHead(200, { 'content-type': 'text/html' });
      res.end('<h2>Orion authorization complete.</h2><p>Return to the terminal / dial.</p>');
      const code = url.searchParams.get('code');
      const state = url.searchParams.get('state');
      server.close();
      if (url.searchParams.get('error'))
        reject(new Error(`auth error: ${url.searchParams.get('error')}`));
      else if (state !== expectedState) reject(new Error('state mismatch (possible CSRF)'));
      else if (code) resolve(code);
      else reject(new Error('no code in callback'));
    });
    server.listen(PORT, () => {
      log('4', `Waiting for the redirect on ${REDIRECT_URI} ... approve on your phone/browser.`);
      onReady();
    });
  });
}

function openBrowser(url: string): void {
  const cmd = platform() === 'darwin' ? 'open' : platform() === 'win32' ? 'start ""' : 'xdg-open';
  exec(`${cmd} "${url}"`);
}

// ---------------------------------------------------------------------------
// Orchestration
// ---------------------------------------------------------------------------
async function main(): Promise<void> {
  console.log(`Reference dial → ${MCP_URL}  (redirect ${REDIRECT_URI})`);
  const state = loadState();

  log('1', 'Discovering OAuth + resource metadata...');
  const disc = await discover();
  console.log(`  authorize: ${disc.authorization_endpoint}`);
  console.log(`  token:     ${disc.token_endpoint}`);
  console.log(`  resource:  ${disc.resource}`);

  log('2', 'Ensuring a registered client (DCR)...');
  const clientId = await ensureClient(disc, state);
  console.log(`  client_id: ${clientId}`);

  // Steps 3-6: get a usable access token.
  const valid =
    state.tokens?.access_token &&
    (state.tokens.expiresAt === undefined || Date.now() < state.tokens.expiresAt - 60_000);
  if (!valid) {
    const refreshed = await refresh(disc, clientId, state);
    if (refreshed) log('6', 'Refreshed the access token from the stored refresh token.');
    else await authorize(disc, clientId, state);
  } else {
    log('6', 'Existing access token is still valid — skipping consent.');
  }
  if (PRINT_URL_ONLY || !state.tokens?.access_token) return;

  log('7', 'Opening a raw MCP session (no SDK)...');
  const mcp = new RawMcp(MCP_URL, state.tokens.access_token);
  const init = (await mcp.initialize()) as { serverInfo?: { name?: string; version?: string } };
  console.log(`  connected to: ${init.serverInfo?.name ?? '?'} ${init.serverInfo?.version ?? ''}`);

  const tools = await mcp.listTools();
  console.log(`\n✅ Orion exposes ${tools.length} MCP tool(s):`);
  for (const t of tools) {
    console.log(`\n# ${t.name}${t.description ? ` — ${t.description}` : ''}`);
    console.log(`  input schema: ${JSON.stringify(t.inputSchema)}`);
  }
  console.log(
    '\nThese names/schemas are what the firmware (and the hub) call to control the topper.',
  );

  // Optional read-only probe to capture the OUTPUT shapes we still need to
  // finalize getStatus/listDevices parsing. Read-only: only list_devices +
  // get_device_state (NOT get_insights / no writes). Run: DIAL_READ=1 npm run dial:ref
  if (process.env.DIAL_READ === '1') {
    log('8', 'Read-only probe: list_devices + get_device_state (no writes)...');
    const devices = await mcp.callTool('list_devices', {});
    console.log(`\n=== list_devices result ===\n${JSON.stringify(devices, null, 2)}`);
    const serial = findFirstString(devices, 'serial');
    if (serial) {
      const st = await mcp.callTool('get_device_state', { serial });
      console.log(`\n=== get_device_state(${serial}) result ===\n${JSON.stringify(st, null, 2)}`);
    } else {
      console.log(
        '\n(could not auto-detect a serial — paste list_devices above and I will finalize)',
      );
    }
  }
}

main().catch((err: unknown) => {
  console.error(`\n✖ ${err instanceof Error ? err.message : err}`);
  process.exitCode = 1;
});
