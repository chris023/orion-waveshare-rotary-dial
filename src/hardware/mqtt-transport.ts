/**
 * MQTT DialTransport: the real bridge to Waveshare ESP32-S3 knobs over Wi-Fi.
 *
 * Subscribes to `<base>/+/event` for inbound dial events and publishes retained
 * `<base>/<dialId>/display` messages so each knob can render its zone's state.
 */

import mqtt, { type MqttClient } from 'mqtt';
import type { Logger } from '../lib/logger.js';
import type { DialDisplay, DialEventHandler, DialTransport } from './dial-transport.js';
import {
  dialIdFromTopic,
  displayTopic,
  eventTopicFilter,
  parseEvent,
  serializeDisplay,
} from './protocol.js';

export interface MqttTransportOptions {
  url: string;
  baseTopic: string;
  username?: string;
  password?: string;
  logger: Logger;
  now?: () => number;
}

export class MqttDialTransport implements DialTransport {
  private readonly handlers: DialEventHandler[] = [];
  private client: MqttClient | undefined;
  private readonly log: Logger;
  private readonly now: () => number;

  constructor(private readonly options: MqttTransportOptions) {
    this.log = options.logger.child({ component: 'mqtt-transport' });
    this.now = options.now ?? Date.now;
  }

  start(): Promise<void> {
    const { url, username, password, baseTopic } = this.options;
    return new Promise((resolve, reject) => {
      const client = mqtt.connect(url, {
        ...(username !== undefined ? { username } : {}),
        ...(password !== undefined ? { password } : {}),
        reconnectPeriod: 2000,
      });
      this.client = client;

      client.on('message', (topic, payload) => this.handleMessage(topic, payload));
      client.on('error', (err) => this.log.error({ err }, 'mqtt error'));
      client.on('reconnect', () => this.log.warn('mqtt reconnecting'));

      client.once('connect', () => {
        const filter = eventTopicFilter(baseTopic);
        client.subscribe(filter, { qos: 1 }, (err) => {
          if (err) {
            reject(err);
            return;
          }
          this.log.info({ url, filter }, 'mqtt connected and subscribed');
          resolve();
        });
      });
      client.once('error', reject);
    });
  }

  private handleMessage(topic: string, payload: Buffer): void {
    const dialId = dialIdFromTopic(this.options.baseTopic, topic);
    if (!dialId) return;
    let event: ReturnType<typeof parseEvent>;
    try {
      event = parseEvent(payload);
    } catch (err) {
      this.log.warn({ err, topic }, 'dropping malformed dial event');
      return;
    }
    const message = { dialId, event, at: this.now() };
    for (const handler of this.handlers) handler(message);
  }

  onEvent(handler: DialEventHandler): void {
    this.handlers.push(handler);
  }

  publishDisplay(dialId: string, display: DialDisplay): Promise<void> {
    const client = this.client;
    if (!client) return Promise.reject(new Error('MqttDialTransport not started'));
    const topic = displayTopic(this.options.baseTopic, dialId);
    return new Promise((resolve, reject) => {
      client.publish(topic, serializeDisplay(display), { qos: 1, retain: true }, (err) =>
        err ? reject(err) : resolve(),
      );
    });
  }

  stop(): Promise<void> {
    const client = this.client;
    if (!client) return Promise.resolve();
    this.handlers.length = 0;
    return new Promise((resolve) => client.end(false, {}, () => resolve()));
  }
}
