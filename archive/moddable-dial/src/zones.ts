/**
 * Unit + zone translation — identical rules to the hub's lib/temperature.ts and
 * OrionMcpClient. The dial works in °F + left/right; Orion speaks °C + zone_a/b.
 */

import type { Zone } from "config";

/** app zone -> Orion zone id. zone_a = RIGHT side, zone_b = LEFT side. */
export const ZONE_TO_ORION: Record<Zone, string> = { left: "zone_b", right: "zone_a" };

export function orionZoneToApp(id: string): Zone | null {
  if (id === "zone_a") return "right";
  if (id === "zone_b") return "left";
  return null;
}

export function fToC(f: number): number {
  return ((f - 32) * 5) / 9;
}
export function cToF(c: number): number {
  return (c * 9) / 5 + 32;
}
/** °F setpoint -> °C snapped to the device's 0.5°C granularity. */
export function fahrenheitToDeviceC(f: number): number {
  return Math.round(fToC(f) * 2) / 2;
}
/** °C from the device -> whole °F for display. */
export function deviceCToF(c: number): number {
  return Math.round(cToF(c));
}
export function clamp(v: number, min: number, max: number): number {
  return Math.min(max, Math.max(min, v));
}
