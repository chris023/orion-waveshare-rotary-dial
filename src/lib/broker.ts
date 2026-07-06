/**
 * Optional in-process MQTT broker (aedes) so dials can connect directly to this
 * host with no external Mosquitto. Enabled by BROKER_MODE=embedded.
 *
 * Listens on TCP (real ESP32 dials + the hub's own MQTT client) and, when
 * wsPort > 0, also on WebSocket so the browser-based virtual dial (simulator/)
 * can speak the exact same MQTT protocol.
 */

import { createServer as createHttpServer, type Server as HttpServer } from 'node:http';
import { createServer as createNetServer, type Server as NetServer } from 'node:net';
import { Aedes, type Client } from 'aedes';
import { createWebSocketStream, WebSocketServer } from 'ws';
import type { Logger } from './logger.js';

export interface EmbeddedBroker {
  close(): Promise<void>;
}

function listen(server: NetServer | HttpServer, port: number): Promise<void> {
  return new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(port, () => resolve());
  });
}

export async function startEmbeddedBroker(
  port: number,
  wsPort: number,
  logger: Logger,
): Promise<EmbeddedBroker> {
  const log = logger.child({ component: 'broker' });
  // aedes v1: the constructor does async persistence setup — MUST use createBroker().
  const aedes = await Aedes.createBroker();

  aedes.on('client', (client: Client) => log.info({ clientId: client?.id }, 'dial connected'));
  aedes.on('clientDisconnect', (client: Client) =>
    log.info({ clientId: client?.id }, 'dial disconnected'),
  );

  const tcp = createNetServer(aedes.handle);
  await listen(tcp, port);
  log.info({ port }, 'embedded MQTT broker listening (tcp)');

  let httpServer: HttpServer | undefined;
  let wss: WebSocketServer | undefined;
  if (wsPort > 0) {
    httpServer = createHttpServer();
    wss = new WebSocketServer({ server: httpServer });
    wss.on('connection', (socket) => {
      const stream = createWebSocketStream(socket);
      stream.on('error', () => socket.close());
      aedes.handle(stream);
    });
    await listen(httpServer, wsPort);
    log.info({ wsPort }, 'embedded MQTT broker listening (ws)');
  }

  return {
    close: () =>
      new Promise<void>((resolve) => {
        wss?.close();
        httpServer?.close();
        tcp.close(() => aedes.close(() => resolve()));
      }),
  };
}
