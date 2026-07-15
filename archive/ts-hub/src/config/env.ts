/**
 * Startup configuration: parse + validate process.env ONCE with zod, and fail
 * fast with a readable message if anything is wrong. Everything downstream
 * receives a typed, frozen `Config`.
 */

import { z } from 'zod';
import type { OrionToolMap } from '../device/orion/tool-map.js';
import { DEFAULT_BINDINGS, type DialBinding, dialBindingsSchema } from '../domain/bindings.js';

const toolMapOverrideSchema = z
  .object({
    listDevices: z.string(),
    getState: z.string(),
    setZone: z.string(),
    thermalRelief: z.string(),
  })
  .partial();

const toolMapFromJson = z
  .string()
  .optional()
  .transform((raw, ctx): Partial<OrionToolMap> => {
    if (!raw) return {};
    let parsed: unknown;
    try {
      parsed = JSON.parse(raw);
    } catch {
      ctx.addIssue({ code: z.ZodIssueCode.custom, message: 'ORION_TOOLS is not valid JSON' });
      return z.NEVER;
    }
    const result = toolMapOverrideSchema.safeParse(parsed);
    if (!result.success) {
      ctx.addIssue({
        code: z.ZodIssueCode.custom,
        message: `ORION_TOOLS invalid: ${result.error.message}`,
      });
      return z.NEVER;
    }
    return result.data;
  });

const bindingsFromJson = z
  .string()
  .optional()
  .transform((raw, ctx): DialBinding[] => {
    if (!raw) return [...DEFAULT_BINDINGS];
    let parsed: unknown;
    try {
      parsed = JSON.parse(raw);
    } catch {
      ctx.addIssue({ code: z.ZodIssueCode.custom, message: 'DIAL_BINDINGS is not valid JSON' });
      return z.NEVER;
    }
    const result = dialBindingsSchema.safeParse(parsed);
    if (!result.success) {
      ctx.addIssue({
        code: z.ZodIssueCode.custom,
        message: `DIAL_BINDINGS invalid: ${result.error.message}`,
      });
      return z.NEVER;
    }
    return result.data;
  });

const envSchema = z.object({
  NODE_ENV: z.enum(['development', 'production', 'test']).default('development'),
  LOG_LEVEL: z.enum(['fatal', 'error', 'warn', 'info', 'debug', 'trace']).default('info'),

  DIAL_TRANSPORT: z.enum(['mock', 'mqtt']).default('mock'),
  DEVICE_CLIENT: z.enum(['fake', 'orion']).default('fake'),

  MQTT_URL: z.string().default('mqtt://localhost:1883'),
  MQTT_USERNAME: z.string().optional(),
  MQTT_PASSWORD: z.string().optional(),
  MQTT_BASE_TOPIC: z.string().default('orion-dials'),
  BROKER_MODE: z.enum(['none', 'embedded']).default('none'),
  BROKER_PORT: z.coerce.number().int().positive().default(1883),
  // WebSocket MQTT port for browser dials (the virtual dial simulator). 0 = off.
  BROKER_WS_PORT: z.coerce.number().int().nonnegative().default(8888),

  // Orion Sleep is controlled via its official MCP server (OAuth 2.1), not REST.
  ORION_MCP_URL: z.string().default('https://mcp.orionsleep.com/'),
  // Loopback port used only during the one-time interactive `orion:login`.
  ORION_OAUTH_PORT: z.coerce.number().int().positive().default(8788),
  // Where the OAuth client registration + tokens are persisted (gitignored).
  ORION_TOKENS_FILE: z.string().default('./secrets/orion-oauth.json'),
  // Optional JSON overriding the default MCP tool-name map (see orion/tool-map.ts).
  ORION_TOOLS: toolMapFromJson,

  POLL_INTERVAL_MS: z.coerce.number().int().positive().default(15_000),
  WRITE_DEBOUNCE_MS: z.coerce.number().int().nonnegative().default(300),
  // Dial works in °F (matching the Orion app); device range is 50–113°F (10–45°C).
  TEMP_MIN_F: z.coerce.number().default(50),
  TEMP_MAX_F: z.coerce.number().default(113),
  TEMP_STEP_F: z.coerce.number().positive().default(1),

  DIAL_BINDINGS: bindingsFromJson,
});

export type Config = z.infer<typeof envSchema> & { readonly bindings: DialBinding[] };

export function loadConfig(env: NodeJS.ProcessEnv = process.env): Config {
  const parsed = envSchema.safeParse(env);
  if (!parsed.success) {
    const issues = parsed.error.issues
      .map((i) => `  - ${i.path.join('.') || '(root)'}: ${i.message}`)
      .join('\n');
    throw new Error(`Invalid configuration:\n${issues}`);
  }
  if (parsed.data.TEMP_MIN_F >= parsed.data.TEMP_MAX_F) {
    throw new Error('Invalid configuration: TEMP_MIN_F must be less than TEMP_MAX_F');
  }
  const config: Config = Object.freeze({ ...parsed.data, bindings: parsed.data.DIAL_BINDINGS });
  return config;
}
