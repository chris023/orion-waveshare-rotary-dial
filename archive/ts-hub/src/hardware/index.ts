/**
 * Factory that picks the DialTransport implementation from config, keeping the
 * composition root (app.ts) free of adapter details.
 */

import type { Config } from '../config/env.js';
import type { Logger } from '../lib/logger.js';
import type { DialTransport } from './dial-transport.js';
import { MockDialTransport } from './mock-transport.js';
import { MqttDialTransport } from './mqtt-transport.js';

export type { DialDisplay, DialTransport } from './dial-transport.js';

export function createDialTransport(config: Config, logger: Logger): DialTransport {
  switch (config.DIAL_TRANSPORT) {
    case 'mock':
      return new MockDialTransport();
    case 'mqtt':
      return new MqttDialTransport({
        url: config.MQTT_URL,
        baseTopic: config.MQTT_BASE_TOPIC,
        ...(config.MQTT_USERNAME !== undefined ? { username: config.MQTT_USERNAME } : {}),
        ...(config.MQTT_PASSWORD !== undefined ? { password: config.MQTT_PASSWORD } : {}),
        logger,
      });
  }
}
