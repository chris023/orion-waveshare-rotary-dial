/**
 * In-memory DialTransport for tests and hardware-free local dev.
 *
 * Tests drive it by calling `emit(...)`; the app can also use it to simulate
 * dials from the console. Captured display updates are inspectable via
 * `lastDisplay(dialId)`.
 */

import type { DialEvent } from '../domain/events.js';
import type { DialDisplay, DialEventHandler, DialTransport } from './dial-transport.js';

export class MockDialTransport implements DialTransport {
  private readonly handlers: DialEventHandler[] = [];
  private readonly displays = new Map<string, DialDisplay>();
  private started = false;
  private now: () => number;

  constructor(now: () => number = Date.now) {
    this.now = now;
  }

  start(): Promise<void> {
    this.started = true;
    return Promise.resolve();
  }

  stop(): Promise<void> {
    this.started = false;
    this.handlers.length = 0;
    return Promise.resolve();
  }

  onEvent(handler: DialEventHandler): void {
    this.handlers.push(handler);
  }

  publishDisplay(dialId: string, display: DialDisplay): Promise<void> {
    this.displays.set(dialId, display);
    return Promise.resolve();
  }

  /** Simulate a dial emitting an event. */
  emit(dialId: string, event: DialEvent): void {
    if (!this.started) throw new Error('MockDialTransport.emit called before start()');
    const message = { dialId, event, at: this.now() };
    for (const handler of this.handlers) handler(message);
  }

  /** Inspect the most recent display pushed to a dial (for assertions). */
  lastDisplay(dialId: string): DialDisplay | undefined {
    return this.displays.get(dialId);
  }
}
