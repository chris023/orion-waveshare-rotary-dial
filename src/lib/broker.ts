/**
 * Optional in-process MQTT broker (aedes) so the ESP32 dials can connect
 * directly to this host with no external Mosquitto. Enabled by BROKER_MODE=embedded.
 */

import { createServer, type Server } from 'node:net';
import { Aedes, type Client } from 'aedes';
import type { Logger } from './logger.js';

export interface EmbeddedBroker {
  close(): Promise<void>;
}

export function startEmbeddedBroker(port: number, logger: Logger): Promise<EmbeddedBroker> {
  const log = logger.child({ component: 'broker' });
  const aedes = new Aedes();
  const server: Server = createServer(aedes.handle);

  aedes.on('client', (client: Client) => log.info({ clientId: client?.id }, 'dial connected'));
  aedes.on('clientDisconnect', (client: Client) =>
    log.info({ clientId: client?.id }, 'dial disconnected'),
  );

  return new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(port, () => {
      log.info({ port }, 'embedded MQTT broker listening');
      resolve({
        close: () =>
          new Promise<void>((res) => {
            server.close(() => aedes.close(() => res()));
          }),
      });
    });
  });
}
