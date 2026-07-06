/**
 * Temperature conversion helpers.
 *
 * The app/dial works in °F (matching how the Orion app presents to US users —
 * the device advertises a fahrenheit scale of 50–113°F). The Orion MCP API,
 * however, speaks Celsius (10–45°C). Only the OrionMcpClient adapter converts;
 * everything above it stays in °F.
 */

export function fToC(f: number): number {
  return ((f - 32) * 5) / 9;
}

export function cToF(c: number): number {
  return (c * 9) / 5 + 32;
}

/** Round to the device's Celsius granularity (0.5°C). */
export function roundHalfC(c: number): number {
  return Math.round(c * 2) / 2;
}

/** °F setpoint -> °C, snapped to the device's 0.5°C steps. */
export function fahrenheitToDeviceC(f: number): number {
  return roundHalfC(fToC(f));
}

/** °C from the device -> whole °F for display. */
export function deviceCToFahrenheit(c: number): number {
  return Math.round(cToF(c));
}
