/**
 * Composition root: assemble the concrete adapters chosen by config, wire them
 * into the Controller, and expose start/stop. Kept separate from main.ts so it
 * can be constructed in tests without touching process signals.
 */

import type { Config } from './config/env.js';
import { Controller } from './controller/controller.js';
import { createDeviceClient } from './device/index.js';
import { createDialTransport } from './hardware/index.js';
import { type EmbeddedBroker, startEmbeddedBroker } from './lib/broker.js';
import { getLogger, type Logger } from './lib/logger.js';

export interface App {
  start(): Promise<void>;
  stop(): Promise<void>;
}

export function createApp(config: Config, logger: Logger = getLogger()): App {
  const transport = createDialTransport(config, logger);
  const device = createDeviceClient(config, logger);
  const controller = new Controller({ config, transport, device, logger });
  let broker: EmbeddedBroker | undefined;

  return {
    async start(): Promise<void> {
      if (config.BROKER_MODE === 'embedded') {
        broker = await startEmbeddedBroker(config.BROKER_PORT, config.BROKER_WS_PORT, logger);
      }
      await controller.start();
      logger.info(
        {
          transport: config.DIAL_TRANSPORT,
          device: config.DEVICE_CLIENT,
          dials: config.bindings.length,
        },
        'orion hub started',
      );
    },
    async stop(): Promise<void> {
      await controller.stop();
      if (broker) await broker.close();
    },
  };
}
