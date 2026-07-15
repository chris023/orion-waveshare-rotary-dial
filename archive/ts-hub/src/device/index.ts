/**
 * Factory that selects the DeviceClient implementation from config.
 */

import type { Config } from '../config/env.js';
import type { ZoneCapabilities } from '../domain/state.js';
import type { Logger } from '../lib/logger.js';
import type { DeviceClient } from './device-client.js';
import { FakeDeviceClient } from './fake-device-client.js';
import { OrionMcpClient } from './orion/mcp-client.js';
import { FileOAuthStore, OrionOAuthProvider } from './orion/oauth-provider.js';
import { resolveToolMap } from './orion/tool-map.js';

export type { DeviceClient, DeviceStatus } from './device-client.js';

function configuredCaps(config: Config): ZoneCapabilities {
  return {
    unit: 'fahrenheit',
    min: config.TEMP_MIN_F,
    max: config.TEMP_MAX_F,
    step: config.TEMP_STEP_F,
  };
}

/** Build the Orion OAuth provider (non-interactive: no browser opener). */
export function createOrionProvider(config: Config): OrionOAuthProvider {
  return new OrionOAuthProvider({
    store: new FileOAuthStore(config.ORION_TOKENS_FILE),
    redirectUri: `http://localhost:${config.ORION_OAUTH_PORT}/callback`,
  });
}

export function createDeviceClient(config: Config, logger: Logger): DeviceClient {
  switch (config.DEVICE_CLIENT) {
    case 'fake':
      return new FakeDeviceClient({ caps: configuredCaps(config) });
    case 'orion':
      return new OrionMcpClient({
        url: config.ORION_MCP_URL,
        provider: createOrionProvider(config),
        toolMap: resolveToolMap(config.ORION_TOOLS),
        caps: configuredCaps(config),
        logger,
      });
  }
}
