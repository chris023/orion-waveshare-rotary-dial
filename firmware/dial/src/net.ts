/**
 * HTTPS helper — the ONE module with real Moddable-transport risk (verify on HW).
 *
 * Uses the classic `http` Request + `securesocket` (TLS). The whole response is
 * buffered as a String (fine for OAuth JSON and for MCP's discrete request/reply,
 * including a single SSE frame). Headers are captured so callers can read
 * `Content-Type` and `Mcp-Session-Id`.
 *
 * VERIFY on hardware: the exact Request callback message constants and the secure
 * options are from the Moddable docs/source but were written without an SDK in
 * the loop. If this misbehaves, the documented fallback is the browser-shaped
 * `fetch` wrapper (examples/packages/fetch), whose Response.headers.get() also
 * exposes Mcp-Session-Id.
 */

import { Request } from "http";
import SecureSocket from "securesocket";
// @ts-expect-error - Resource is a global-ish Moddable class for bundled assets
import Resource from "Resource";

// Root CA (DER) that mcp.orionsleep.com (Cloudflare) chains to. Provide the
// correct cert at firmware/dial/assets/orion-ca.der (see README). VERIFY name.
const ORION_CA = new Resource("orion-ca.der");

export interface HttpResponse {
  status: number;
  headers: Map<string, string>;
  body: string;
}

export interface HttpOptions {
  method?: string;
  headers?: Record<string, string>;
  body?: string;
}

/** Split "https://host/path?query" into { host, path }. */
export function splitUrl(url: string): { host: string; path: string } {
  const noScheme = url.replace(/^https?:\/\//, "");
  const slash = noScheme.indexOf("/");
  if (slash < 0) return { host: noScheme, path: "/" };
  return { host: noScheme.slice(0, slash), path: noScheme.slice(slash) };
}

export function httpsRequest(url: string, opts: HttpOptions = {}): Promise<HttpResponse> {
  const { host, path } = splitUrl(url);
  const flatHeaders: string[] = [];
  if (opts.headers) {
    for (const name in opts.headers) flatHeaders.push(name, opts.headers[name]);
  }

  return new Promise<HttpResponse>((resolve, reject) => {
    const request = new Request({
      host,
      port: 443,
      path,
      method: opts.method ?? "GET",
      headers: flatHeaders, // flat [name, value, ...] — NOT an object
      body: opts.body,
      response: String, // buffer whole body, delivered on responseComplete
      Socket: SecureSocket,
      secure: { protocolVersion: 0x0303, certificate: ORION_CA }, // force TLS 1.2
    } as unknown as object);

    let status = 0;
    const headers = new Map<string, string>();

    (request as unknown as { callback: (m: number, v: unknown, e: unknown) => void }).callback =
      function (message: number, value: unknown, etc: unknown): void {
        switch (message) {
          case Request.status: // VERIFY: value = HTTP status code
            status = value as number;
            break;
          case Request.header: // VERIFY: value = header name, etc = header value
            headers.set(String(value).toLowerCase(), String(etc));
            break;
          case Request.responseComplete: // VERIFY: value = full body (response:String)
            resolve({ status, headers, body: (value as string) ?? "" });
            break;
          default:
            if (Request.error !== undefined && message === Request.error) {
              reject(new Error(`HTTPS transport error for ${host}${path}`));
            }
        }
      };
  });
}

/** POST/GET returning parsed JSON, throwing on non-2xx. */
export async function httpsJson<T = unknown>(url: string, opts: HttpOptions = {}): Promise<T> {
  const res = await httpsRequest(url, opts);
  if (res.status < 200 || res.status >= 300) {
    throw new Error(`HTTP ${res.status} ${url}: ${res.body.slice(0, 200)}`);
  }
  return (res.body ? JSON.parse(res.body) : undefined) as T;
}
