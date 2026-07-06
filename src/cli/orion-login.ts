/**
 * One-time interactive OAuth login to the Orion Sleep MCP server.
 *
 *   npm run orion:login
 *
 * Opens the Orion authorization page in your browser, captures the redirect on
 * a loopback server, and persists the resulting tokens to ORION_TOKENS_FILE
 * (default ./secrets/orion-oauth.json). After this, the hub can run
 * non-interactively using the stored refresh token.
 */

import { exec } from 'node:child_process';
import { createServer } from 'node:http';
import { platform } from 'node:os';
import { UnauthorizedError } from '@modelcontextprotocol/sdk/client/auth.js';
import { Client } from '@modelcontextprotocol/sdk/client/index.js';
import { StreamableHTTPClientTransport } from '@modelcontextprotocol/sdk/client/streamableHttp.js';
import { loadConfig } from '../config/env.js';
import { FileOAuthStore, OrionOAuthProvider } from '../device/orion/oauth-provider.js';

function openBrowser(url: string): void {
  const cmd = platform() === 'darwin' ? 'open' : platform() === 'win32' ? 'start ""' : 'xdg-open';
  exec(`${cmd} "${url}"`);
}

async function main(): Promise<void> {
  const config = loadConfig();
  const store = new FileOAuthStore(config.ORION_TOKENS_FILE);
  const port = config.ORION_OAUTH_PORT;
  const redirectUri = `http://localhost:${port}/callback`;

  let resolveCode!: (code: string) => void;
  let rejectCode!: (err: Error) => void;
  const codePromise = new Promise<string>((res, rej) => {
    resolveCode = res;
    rejectCode = rej;
  });

  const server = createServer((req, res) => {
    const url = new URL(req.url ?? '/', redirectUri);
    if (url.pathname !== '/callback') {
      res.writeHead(404).end();
      return;
    }
    res.writeHead(200, { 'content-type': 'text/html' });
    res.end('<h2>Orion authorization complete.</h2><p>You can close this tab.</p>');
    const code = url.searchParams.get('code');
    const error = url.searchParams.get('error');
    if (error) rejectCode(new Error(`Authorization error: ${error}`));
    else if (code) resolveCode(code);
    else rejectCode(new Error('Callback received without an authorization code'));
  });
  await new Promise<void>((resolve) => server.listen(port, resolve));
  console.log(`Waiting for the OAuth redirect on ${redirectUri} ...`);

  const provider = new OrionOAuthProvider({
    store,
    redirectUri,
    onAuthorizationUrl: (u) => {
      console.log(`\nAuthorize this hub with Orion by opening:\n  ${u.toString()}\n`);
      openBrowser(u.toString());
    },
  });

  const client = new Client({ name: 'orion-waveshare-rotary-dial', version: '0.1.0' });
  const transport = new StreamableHTTPClientTransport(new URL(config.ORION_MCP_URL), {
    authProvider: provider,
  });

  try {
    await client.connect(transport);
    console.log('Already authorized — existing tokens are still valid.');
  } catch (err) {
    if (!(err instanceof UnauthorizedError)) throw err;
    const code = await codePromise;
    await transport.finishAuth(code);
    await client.connect(transport);
    console.log(`\n✅ Authorized! Tokens saved to ${config.ORION_TOKENS_FILE}`);
  }

  const { tools } = await client.listTools();
  console.log(
    `\nOrion exposes ${tools.length} MCP tool(s): ${tools.map((t) => t.name).join(', ') || '(none)'}`,
  );
  console.log('Run `npm run orion:tools` to view their full input schemas.');

  await client.close();
  server.close();
}

main().catch((err: unknown) => {
  console.error(err instanceof Error ? err.message : err);
  process.exitCode = 1;
});
