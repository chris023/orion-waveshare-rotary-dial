/**
 * Virtual dial: a browser stand-in for the Waveshare ESP32-S3 knob.
 *
 * Speaks the SAME MQTT protocol as the real firmware (see src/hardware/protocol.ts),
 * over WebSocket, to the hub's embedded broker:
 *   publish  orion-dials/<dialId>/event    {"type":"rotate","dir":"cw","steps":1} | {"type":"tap"} | {"type":"longpress"}
 *   receive  orion-dials/<dialId>/display  {"label","power","target","current","active","offline"}
 *
 * Config via URL query: ?broker=ws://host:8888&base=orion-dials&dials=dial-left,dial-right
 */

import mqtt from 'mqtt';
import { type DialDisplay, render } from './renderer.js';

const params = new URLSearchParams(location.search);
const BROKER = params.get('broker') ?? 'ws://localhost:8888';
const BASE = params.get('base') ?? 'orion-dials';
const DIALS = (params.get('dials') ?? 'dial-left,dial-right')
  .split(',')
  .map((s) => s.trim())
  .filter(Boolean);
const SIZE = 300;

const eventTopic = (id: string) => `${BASE}/${id}/event`;
const displayFilter = `${BASE}/+/display`;
const decode = (p: Uint8Array) => new TextDecoder().decode(p);

function dialIdFromDisplayTopic(topic: string): string | null {
  const parts = topic.split('/');
  return parts.length === 3 && parts[0] === BASE && parts[2] === 'display' ? parts[1] : null;
}

type Publish = (topic: string, payload: string) => void;

class Dial {
  private readonly ctx: CanvasRenderingContext2D;
  private display: DialDisplay;

  constructor(
    readonly id: string,
    private readonly publish: Publish,
  ) {
    this.display = {
      label: id,
      power: 'off',
      target: null,
      current: null,
      active: false,
      offline: true,
    };

    const root = document.getElementById('dials') as HTMLElement;
    const section = el('section', 'dial');
    const canvas = document.createElement('canvas');
    canvas.width = SIZE;
    canvas.height = SIZE;
    canvas.className = 'screen';
    canvas.title = 'scroll to turn · click to tap';
    const ctx = canvas.getContext('2d');
    if (!ctx) throw new Error('no 2d context');
    this.ctx = ctx;

    // Interactions on the "screen".
    canvas.addEventListener('click', () => this.tap());
    canvas.addEventListener(
      'wheel',
      (e) => {
        e.preventDefault();
        this.rotate(e.deltaY < 0 ? 'cw' : 'ccw');
      },
      { passive: false },
    );

    const controls = el('div', 'controls');
    controls.append(
      button('−', () => this.rotate('ccw'), 'cooler'),
      button('Tap', () => this.tap(), 'toggle on/off'),
      button('+', () => this.rotate('cw'), 'warmer'),
      button('Boost', () => this.longpress(), 'thermal relief (long-press)'),
    );

    const caption = el('div', 'caption');
    caption.textContent = id;

    section.append(canvas, controls, caption);
    root.append(section);
    this.paint();
  }

  setDisplay(d: DialDisplay): void {
    this.display = d;
    this.paint();
  }

  private paint(): void {
    render(this.ctx, SIZE, this.display);
  }

  private rotate(dir: 'cw' | 'ccw', steps = 1): void {
    this.publish(eventTopic(this.id), JSON.stringify({ type: 'rotate', dir, steps }));
  }
  private tap(): void {
    this.publish(eventTopic(this.id), JSON.stringify({ type: 'tap' }));
  }
  private longpress(): void {
    this.publish(eventTopic(this.id), JSON.stringify({ type: 'longpress' }));
  }
}

function el(tag: string, className: string): HTMLElement {
  const e = document.createElement(tag);
  e.className = className;
  return e;
}
function button(label: string, onClick: () => void, title: string): HTMLButtonElement {
  const b = document.createElement('button');
  b.textContent = label;
  b.title = title;
  b.addEventListener('click', onClick);
  return b;
}

// --- Wire up MQTT + dials ---------------------------------------------------
const statusEl = document.getElementById('status') as HTMLElement;
const setStatus = (text: string, kind: 'ok' | 'warn' | 'err') => {
  statusEl.textContent = text;
  statusEl.className = `status ${kind}`;
};

const client = mqtt.connect(BROKER, { reconnectPeriod: 1500 });
const publish: Publish = (topic, payload) => client.publish(topic, payload, { qos: 1 });
const dials = new Map(DIALS.map((id) => [id, new Dial(id, publish)]));

client.on('connect', () => {
  setStatus(`connected · ${BROKER}`, 'ok');
  client.subscribe(displayFilter, { qos: 1 });
});
client.on('reconnect', () => setStatus(`reconnecting · ${BROKER}`, 'warn'));
client.on('close', () =>
  setStatus(`disconnected · is the hub running? (BROKER_MODE=embedded)`, 'err'),
);
client.on('error', (e) => setStatus(`error: ${e.message}`, 'err'));
client.on('message', (topic, payload) => {
  const id = dialIdFromDisplayTopic(topic);
  const dial = id && dials.get(id);
  if (!dial) return;
  try {
    dial.setDisplay(JSON.parse(decode(payload)) as DialDisplay);
  } catch {
    /* ignore malformed */
  }
});
