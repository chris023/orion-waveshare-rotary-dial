/**
 * Entrypoint. Loads + validates config, starts the app, and shuts down cleanly
 * on SIGINT/SIGTERM (closing serial/MQTT connections and flushing).
 *
 * Load environment from a file with Node's built-in flag, e.g.:
 *   node --env-file=.env dist/main.js
 */

import { createApp } from './app.js';
import { loadConfig } from './config/env.js';
import { initLogger } from './lib/logger.js';

async function main(): Promise<void> {
  const config = loadConfig();
  const logger = initLogger({
    level: config.LOG_LEVEL,
    pretty: config.NODE_ENV !== 'production',
  });
  const app = createApp(config, logger);

  let stopping = false;
  const shutdown = async (signal: string): Promise<void> => {
    if (stopping) return;
    stopping = true;
    logger.info({ signal }, 'shutting down');
    try {
      await app.stop();
      process.exit(0);
    } catch (err) {
      logger.error({ err }, 'error during shutdown');
      process.exit(1);
    }
  };
  process.on('SIGINT', () => void shutdown('SIGINT'));
  process.on('SIGTERM', () => void shutdown('SIGTERM'));

  await app.start();
}

main().catch((err: unknown) => {
  // Startup/config failure — logger may not exist yet, so use console.
  console.error(err instanceof Error ? err.message : err);
  process.exit(1);
});
