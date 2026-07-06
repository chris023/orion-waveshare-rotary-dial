/**
 * On-device OAuth 2.1 (Dynamic Client Registration + PKCE) against Orion.
 *
 * Faithful port of firmware/reference-dial/dial.ts (verified against the live
 * server) onto Moddable: crypt.Digest (SHA-256), RNG (random), Preference (NVS
 * token storage), http.Server (loopback catcher) + mdns (<name>.local so a phone
 * can reach the redirect). One-time phone consent; refreshes itself forever after.
 */

import { Server } from "http";
import { Digest } from "crypt";
// @ts-expect-error - Moddable modules without bundled typings
import RNG from "rng";
// @ts-expect-error
import MDNS from "mdns";
// @ts-expect-error
import Preference from "preference";
import { httpsJson, splitUrl } from "net";
import { ORION } from "config";

const PREF = "orionauth"; // <= 15 chars (NVS domain)

interface Discovery {
  authorization_endpoint: string;
  token_endpoint: string;
  registration_endpoint: string;
  resource: string;
}
interface Tokens {
  access_token: string;
  refresh_token?: string;
  expiresAt?: number;
}

export interface Auth {
  /** A valid access token, refreshing if needed/possible. */
  getAccessToken(): Promise<string>;
  /** Force a refresh (e.g. after a 401). Returns the new token. */
  refresh(): Promise<string>;
}

// --- helpers ---------------------------------------------------------------
function b64url(buf: ArrayBuffer): string {
  // XS ships the TC39 Uint8Array<->base64 proposal.
  return (new Uint8Array(buf) as unknown as { toBase64(o: object): string }).toBase64({
    alphabet: "base64url",
    omitPadding: true,
  });
}
function sha256(input: string): ArrayBuffer {
  const d = new Digest("SHA256");
  d.write(input);
  return d.close();
}
function origin(url: string): string {
  return url.replace(/^(https?:\/\/[^/]+).*$/, "$1");
}
function queryParam(pathWithQuery: string, key: string): string | undefined {
  const q = pathWithQuery.indexOf("?");
  if (q < 0) return undefined;
  for (const pair of pathWithQuery.slice(q + 1).split("&")) {
    const eq = pair.indexOf("=");
    if (eq > 0 && pair.slice(0, eq) === key) return decodeURIComponent(pair.slice(eq + 1));
  }
  return undefined;
}
function form(params: Record<string, string>): string {
  const parts: string[] = [];
  for (const k in params) parts.push(`${encodeURIComponent(k)}=${encodeURIComponent(params[k])}`);
  return parts.join("&");
}

// --- persistence (NVS) -----------------------------------------------------
function saveTokens(t: Tokens): void {
  Preference.set(PREF, "access", t.access_token);
  if (t.refresh_token) Preference.set(PREF, "refresh", t.refresh_token);
  if (t.expiresAt) Preference.set(PREF, "expires", t.expiresAt);
}
function loadTokens(): Tokens | undefined {
  const access = Preference.get(PREF, "access") as string | undefined;
  if (!access) return undefined;
  return {
    access_token: access,
    refresh_token: Preference.get(PREF, "refresh") as string | undefined,
    expiresAt: Preference.get(PREF, "expires") as number | undefined,
  };
}

// --- flow ------------------------------------------------------------------
async function discover(): Promise<Discovery> {
  const base = origin(ORION.mcpUrl);
  const as = await httpsJson<Discovery>(`${base}/.well-known/oauth-authorization-server`);
  let resource = base;
  try {
    const prm = await httpsJson<{ resource?: string }>(
      `${base}/.well-known/oauth-protected-resource/`,
    );
    if (prm.resource) resource = prm.resource;
  } catch {
    /* optional */
  }
  return { ...as, resource };
}

async function ensureClient(disc: Discovery, redirectUri: string): Promise<string> {
  const saved = Preference.get(PREF, "client") as string | undefined;
  if (saved) return saved;
  const reg = await httpsJson<{ client_id: string }>(disc.registration_endpoint, {
    method: "POST",
    headers: { "content-type": "application/json", accept: "application/json" },
    body: JSON.stringify({
      client_name: ORION.clientName,
      redirect_uris: [redirectUri],
      grant_types: ["authorization_code", "refresh_token"],
      response_types: ["code"],
      token_endpoint_auth_method: "none",
      scope: ORION.scope,
    }),
  });
  Preference.set(PREF, "client", reg.client_id);
  return reg.client_id;
}

async function exchangeToken(
  disc: Discovery,
  body: Record<string, string>,
): Promise<Tokens> {
  const tok = await httpsJson<{ access_token: string; refresh_token?: string; expires_in?: number }>(
    disc.token_endpoint,
    {
      method: "POST",
      headers: { "content-type": "application/x-www-form-urlencoded", accept: "application/json" },
      body: form(body),
    },
  );
  return {
    access_token: tok.access_token,
    refresh_token: tok.refresh_token,
    expiresAt: tok.expires_in ? Date.now() + tok.expires_in * 1000 : undefined,
  };
}

/** Advertise <name>.local + run a loopback server; resolve with the auth code. */
function waitForCode(expectedState: string): Promise<string> {
  return new Promise((resolve, reject) => {
    // VERIFY: mdns constructor + advertising shape.
    const mdns = new MDNS({ hostName: ORION.mdnsName }, () => {});
    try {
      mdns.add({ name: "http", protocol: "tcp", port: ORION.oauthPort, txt: {} });
    } catch {
      /* advertising is best-effort; LAN IP still works as a fallback redirect */
    }

    const server = new Server({ port: ORION.oauthPort });
    let path = "/";
    (server as unknown as { callback: (m: number, v: unknown, e: unknown) => unknown }).callback =
      function (message: number, value: unknown, _etc: unknown): unknown {
        if (message === Server.status) {
          path = value as string; // full path incl. ?query
        } else if (message === Server.prepareResponse) {
          const code = queryParam(path, "code");
          const state = queryParam(path, "state");
          Promise.resolve().then(() => {
            server.close();
            if (state !== expectedState) reject(new Error("OAuth state mismatch"));
            else if (code) resolve(code);
            else reject(new Error("callback without code"));
          });
          return {
            status: 200,
            headers: ["Content-Type", "text/html"],
            body: "<h2>Orion authorized.</h2><p>Return to the dial.</p>",
          };
        }
        return undefined;
      };
  });
}

/** Get a working Auth, doing one interactive consent only if there's no token. */
export async function ensureAuthorized(onAuthorizeUrl: (url: string) => void): Promise<Auth> {
  const redirectUri = `http://${ORION.mdnsName}.local:${ORION.oauthPort}/callback`;
  const disc = await discover();
  const clientId = await ensureClient(disc, redirectUri);

  const doRefresh = async (refreshToken: string): Promise<Tokens> => {
    const t = await exchangeToken(disc, {
      grant_type: "refresh_token",
      refresh_token: refreshToken,
      client_id: clientId,
      resource: disc.resource,
      scope: ORION.scope,
    });
    if (!t.refresh_token) t.refresh_token = refreshToken;
    saveTokens(t);
    return t;
  };

  let tokens = loadTokens();
  if (!tokens) {
    // Interactive: PKCE + QR consent + loopback capture.
    const verifier = b64url(RNG.get(32));
    const challenge = b64url(sha256(verifier));
    const state = b64url(RNG.get(16));
    const authUrl =
      `${disc.authorization_endpoint}?response_type=code` +
      `&client_id=${encodeURIComponent(clientId)}` +
      `&redirect_uri=${encodeURIComponent(redirectUri)}` +
      `&scope=${encodeURIComponent(ORION.scope)}` +
      `&state=${encodeURIComponent(state)}` +
      `&code_challenge=${challenge}&code_challenge_method=S256` +
      `&resource=${encodeURIComponent(disc.resource)}`;
    onAuthorizeUrl(authUrl); // UI renders this as a QR code
    const code = await waitForCode(state);
    tokens = await exchangeToken(disc, {
      grant_type: "authorization_code",
      code,
      redirect_uri: redirectUri,
      client_id: clientId,
      code_verifier: verifier,
      resource: disc.resource,
    });
    saveTokens(tokens);
  }

  let current = tokens;
  return {
    async getAccessToken(): Promise<string> {
      const expired = current.expiresAt !== undefined && Date.now() >= current.expiresAt - 60_000;
      if (expired && current.refresh_token) current = await doRefresh(current.refresh_token);
      return current.access_token;
    },
    async refresh(): Promise<string> {
      if (!current.refresh_token) throw new Error("no refresh token; re-run setup");
      current = await doRefresh(current.refresh_token);
      return current.access_token;
    },
  };
}
