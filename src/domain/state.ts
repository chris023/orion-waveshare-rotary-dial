/**
 * Core domain model for the Orion Sleep topper: a dual-zone (left/right) cover
 * where each zone has an independent power state and target temperature.
 */

/** The two independently-controlled sides of a King dual-zone cover. */
export type Zone = 'left' | 'right';

export const ZONES: readonly Zone[] = ['left', 'right'] as const;

export function isZone(value: string): value is Zone {
  return value === 'left' || value === 'right';
}

/** Power state of a zone. The Orion API models this as a simple boolean on/off. */
export type Power = 'on' | 'off';

/** Temporary max heat/cool boost ("thermal relief" in the Orion API). */
export type ReliefType = 'heat' | 'cool';

/**
 * The unit the app/dial works in. We use °F (matching how the Orion app presents
 * to US users); the Orion MCP adapter converts to/from the device's Celsius.
 */
export type TemperatureUnit = 'fahrenheit' | 'celsius';

export interface ZoneCapabilities {
  readonly unit: TemperatureUnit;
  readonly min: number;
  readonly max: number;
  /** Smallest honored increment (e.g. 1°F or 1 level). */
  readonly step: number;
}

/** The hub's desired setpoint for a zone (what the dial has dialed in). */
export interface DesiredZoneState {
  readonly power: Power;
  readonly target: number;
}

/** A snapshot of a zone as reported by the device. */
export interface ZoneStatus {
  readonly zone: Zone;
  readonly power: Power;
  /** Desired setpoint, or null if unknown. */
  readonly target: number | null;
  /** Measured temperature if the API reports it, else null. */
  readonly current: number | null;
  /** True when actively heating/cooling toward the target. */
  readonly active: boolean;
  /** Active thermal-relief boost, if any. */
  readonly relief: ReliefType | null;
}

export function clampToCapabilities(value: number, caps: ZoneCapabilities): number {
  const snapped = Math.round((value - caps.min) / caps.step) * caps.step + caps.min;
  return Math.min(caps.max, Math.max(caps.min, snapped));
}
