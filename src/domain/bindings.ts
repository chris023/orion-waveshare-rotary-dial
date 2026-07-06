/**
 * Binds each physical dial to the device zone it controls. This is the heart of
 * "which knob controls which side of the bed".
 */

import { z } from 'zod';

export const dialBindingSchema = z.object({
  /** Stable id the firmware announces (e.g. from the ESP32 MAC or a config). */
  dialId: z.string().min(1),
  /** The topper this dial controls. */
  deviceId: z.string().min(1),
  /** Which zone of that topper. */
  zone: z.enum(['left', 'right']),
  /** Human label shown on the dial's screen (e.g. "LEFT"). */
  label: z.string().min(1),
});

export type DialBinding = z.infer<typeof dialBindingSchema>;

export const dialBindingsSchema = z.array(dialBindingSchema);

/** Default two-dial layout for a King dual-zone cover. */
export const DEFAULT_BINDINGS: readonly DialBinding[] = [
  { dialId: 'dial-left', deviceId: 'orion-1', zone: 'left', label: 'LEFT' },
  { dialId: 'dial-right', deviceId: 'orion-1', zone: 'right', label: 'RIGHT' },
];

/** Index bindings by dialId for O(1) lookup, rejecting duplicate dial ids. */
export function indexBindings(bindings: readonly DialBinding[]): Map<string, DialBinding> {
  const byDial = new Map<string, DialBinding>();
  for (const binding of bindings) {
    if (byDial.has(binding.dialId)) {
      throw new Error(`Duplicate dialId in bindings: ${binding.dialId}`);
    }
    byDial.set(binding.dialId, binding);
  }
  return byDial;
}
