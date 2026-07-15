/**
 * OAuth 2.1 client provider for the Orion Sleep MCP server.
 *
 * Implements the MCP SDK's OAuthClientProvider, persisting the dynamically
 * registered client, tokens, and PKCE verifier to a JSON file (under secrets/,
 * gitignored). The SDK drives Dynamic Client Registration, PKCE, the
 * authorization-code exchange, and refresh; this class just supplies config +
 * storage and forwards the authorization URL to a caller-provided opener.
 *
 * Orion's discovery (verified 2026-07-06):
 *   authorization_endpoint = https://app.orionsleep.com/oauth/authorize
 *   token_endpoint         = https://mcp.orionsleep.com/oauth/token
 *   registration_endpoint  = https://mcp.orionsleep.com/oauth/register
 *   scope                  = orion:mcp   (PKCE S256, code + refresh_token)
 */

import { existsSync, mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname } from 'node:path';
import type { OAuthClientProvider } from '@modelcontextprotocol/sdk/client/auth.js';
import type {
  OAuthClientInformationFull,
  OAuthClientInformationMixed,
  OAuthClientMetadata,
  OAuthTokens,
} from '@modelcontextprotocol/sdk/shared/auth.js';

export const ORION_SCOPE = 'orion:mcp';

interface PersistedState {
  clientInformation?: OAuthClientInformationFull;
  tokens?: OAuthTokens;
  codeVerifier?: string;
}

/** File-backed store for OAuth state. */
export class FileOAuthStore {
  constructor(private readonly path: string) {}

  private read(): PersistedState {
    if (!existsSync(this.path)) return {};
    try {
      return JSON.parse(readFileSync(this.path, 'utf8')) as PersistedState;
    } catch {
      return {};
    }
  }

  private write(state: PersistedState): void {
    mkdirSync(dirname(this.path), { recursive: true });
    writeFileSync(this.path, `${JSON.stringify(state, null, 2)}\n`, { mode: 0o600 });
  }

  private update(patch: Partial<PersistedState>): void {
    this.write({ ...this.read(), ...patch });
  }

  get(): PersistedState {
    return this.read();
  }
  setClientInformation(info: OAuthClientInformationFull): void {
    this.update({ clientInformation: info });
  }
  setTokens(tokens: OAuthTokens): void {
    this.update({ tokens });
  }
  setCodeVerifier(verifier: string): void {
    this.update({ codeVerifier: verifier });
  }
  clear(): void {
    this.write({});
  }
}

export interface OrionOAuthProviderOptions {
  store: FileOAuthStore;
  /** Loopback URL the auth server redirects back to (e.g. http://localhost:8788/callback). */
  redirectUri: string;
  clientName?: string;
  /** Called when the SDK needs the user to authorize. Interactive CLIs open a browser. */
  onAuthorizationUrl?: (url: URL) => void | Promise<void>;
}

export class OrionOAuthProvider implements OAuthClientProvider {
  constructor(private readonly options: OrionOAuthProviderOptions) {}

  get redirectUrl(): string {
    return this.options.redirectUri;
  }

  get clientMetadata(): OAuthClientMetadata {
    return {
      client_name: this.options.clientName ?? 'orion-waveshare-rotary-dial',
      redirect_uris: [this.options.redirectUri],
      grant_types: ['authorization_code', 'refresh_token'],
      response_types: ['code'],
      token_endpoint_auth_method: 'none',
      scope: ORION_SCOPE,
    };
  }

  clientInformation(): OAuthClientInformationMixed | undefined {
    return this.options.store.get().clientInformation;
  }

  saveClientInformation(info: OAuthClientInformationMixed): void {
    this.options.store.setClientInformation(info as OAuthClientInformationFull);
  }

  tokens(): OAuthTokens | undefined {
    return this.options.store.get().tokens;
  }

  saveTokens(tokens: OAuthTokens): void {
    this.options.store.setTokens(tokens);
  }

  saveCodeVerifier(codeVerifier: string): void {
    this.options.store.setCodeVerifier(codeVerifier);
  }

  codeVerifier(): string {
    const verifier = this.options.store.get().codeVerifier;
    if (!verifier) throw new Error('Missing PKCE code verifier — restart the login flow.');
    return verifier;
  }

  async redirectToAuthorization(authorizationUrl: URL): Promise<void> {
    if (this.options.onAuthorizationUrl) {
      await this.options.onAuthorizationUrl(authorizationUrl);
      return;
    }
    throw new Error(
      `Authorization required. Run \`npm run orion:login\` to authorize. URL: ${authorizationUrl.toString()}`,
    );
  }

  invalidateCredentials(scope: 'all' | 'client' | 'tokens' | 'verifier' | 'discovery'): void {
    const state = this.options.store.get();
    if (scope === 'all') {
      this.options.store.clear();
      return;
    }
    if (scope === 'tokens') this.options.store.setTokens(undefined as never);
    if (scope === 'verifier') this.options.store.setCodeVerifier(undefined as never);
    void state;
  }
}
