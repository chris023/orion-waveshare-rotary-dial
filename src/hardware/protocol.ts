/**
 * Wire protocol between the ESP32-S3 knob firmware and the hub, over MQTT.
 *
 * Topics (base topic is configurable, default "orion-dials"):
 *   <base>/<dialId>/event    device -> hub   (retained: false)
 *   <base>/<dialId>/display  hub -> device   (retained: true, so a rebooting
 *                                             dial immediately re-renders)
 *   <base>/<dialId>/status   device -> hub   ("online"/"offline", LWT)
 *
 * Messages are compact JSON so ESPHome/Arduino firmware can emit them easily.
 */

import { z } from 'zod';
import type { DialEvent } from '../domain/events.js';
import type { DialDisplay } from './dial-transport.js';

export const EVENT_SUFFIX = 'event';
export const DISPLAY_SUFFIX = 'display';
export const STATUS_SUFFIX = 'status';

export function eventTopic(base: string, dialId: string): string {
  return `${base}/${dialId}/${EVENT_SUFFIX}`;
}
export function displayTopic(base: string, dialId: string): string {
  return `${base}/${dialId}/${DISPLAY_SUFFIX}`;
}
export function eventTopicFilter(base: string): string {
  return `${base}/+/${EVENT_SUFFIX}`;
}

/** Extract the dialId from a `<base>/<dialId>/<suffix>` topic, or null. */
export function dialIdFromTopic(base: string, topic: string): string | null {
  const prefix = `${base}/`;
  if (!topic.startsWith(prefix)) return null;
  const rest = topic.slice(prefix.length).split('/');
  if (rest.length !== 2) return null;
  const [dialId, suffix] = rest;
  if (!dialId || suffix !== EVENT_SUFFIX) return null;
  return dialId;
}

const inboundEventSchema = z.discriminatedUnion('type', [
  z.object({
    type: z.literal('rotate'),
    dir: z.enum(['cw', 'ccw']),
    steps: z.coerce.number().int().positive().default(1),
  }),
  z.object({ type: z.literal('tap') }),
  z.object({ type: z.literal('longpress') }),
]);

/** Parse a raw MQTT event payload into a DialEvent, or throw on malformed input. */
export function parseEvent(payload: string | Buffer): DialEvent {
  const json: unknown = JSON.parse(
    typeof payload === 'string' ? payload : payload.toString('utf8'),
  );
  const parsed = inboundEventSchema.parse(json);
  switch (parsed.type) {
    case 'rotate':
      return { kind: 'rotate', direction: parsed.dir, steps: parsed.steps };
    case 'tap':
      return { kind: 'tap' };
    case 'longpress':
      return { kind: 'longPress' };
  }
}

/** Serialize a display update into the JSON payload the firmware renders. */
export function serializeDisplay(display: DialDisplay): string {
  return JSON.stringify({
    label: display.label,
    power: display.power,
    target: display.target,
    current: display.current,
    active: display.active,
    offline: display.offline,
  });
}
