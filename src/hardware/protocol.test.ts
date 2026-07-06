import { describe, expect, it } from 'vitest';
import {
  dialIdFromTopic,
  displayTopic,
  eventTopic,
  parseEvent,
  serializeDisplay,
} from './protocol.js';

describe('protocol topics', () => {
  it('builds event and display topics', () => {
    expect(eventTopic('orion-dials', 'dial-left')).toBe('orion-dials/dial-left/event');
    expect(displayTopic('orion-dials', 'dial-left')).toBe('orion-dials/dial-left/display');
  });

  it('extracts dialId only from event topics under the base', () => {
    expect(dialIdFromTopic('orion-dials', 'orion-dials/dial-left/event')).toBe('dial-left');
    expect(dialIdFromTopic('orion-dials', 'orion-dials/dial-left/display')).toBeNull();
    expect(dialIdFromTopic('orion-dials', 'other/dial-left/event')).toBeNull();
    expect(dialIdFromTopic('orion-dials', 'orion-dials/a/b/event')).toBeNull();
  });
});

describe('parseEvent', () => {
  it('parses a rotate event with default steps', () => {
    expect(parseEvent('{"type":"rotate","dir":"cw"}')).toEqual({
      kind: 'rotate',
      direction: 'cw',
      steps: 1,
    });
  });

  it('parses rotate with explicit steps, tap, and longpress', () => {
    expect(parseEvent('{"type":"rotate","dir":"ccw","steps":4}')).toEqual({
      kind: 'rotate',
      direction: 'ccw',
      steps: 4,
    });
    expect(parseEvent('{"type":"tap"}')).toEqual({ kind: 'tap' });
    expect(parseEvent('{"type":"longpress"}')).toEqual({ kind: 'longPress' });
  });

  it('accepts a Buffer payload', () => {
    expect(parseEvent(Buffer.from('{"type":"tap"}'))).toEqual({ kind: 'tap' });
  });

  it('throws on malformed or unknown events', () => {
    expect(() => parseEvent('not json')).toThrow();
    expect(() => parseEvent('{"type":"explode"}')).toThrow();
    expect(() => parseEvent('{"type":"rotate","dir":"sideways"}')).toThrow();
  });
});

describe('serializeDisplay', () => {
  it('round-trips display fields to JSON', () => {
    const json = serializeDisplay({
      label: 'LEFT',
      power: 'on',
      target: 72,
      current: 70,
      active: true,
      offline: false,
    });
    expect(JSON.parse(json)).toEqual({
      label: 'LEFT',
      power: 'on',
      target: 72,
      current: 70,
      active: true,
      offline: false,
    });
  });
});
