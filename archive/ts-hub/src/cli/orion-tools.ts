/**
 * Print the tools the Orion Sleep MCP server exposes, with their input schemas.
 *
 *   npm run orion:tools
 *
 * Requires a prior `npm run orion:login`. Use the output to finalize the
 * tool-name map (ORION_TOOLS) and the response coercion in mcp-client.ts.
 */

import { loadConfig } from '../config/env.js';
import { createOrionProvider } from '../device/index.js';
import { OrionMcpClient } from '../device/orion/mcp-client.js';
import { resolveToolMap } from '../device/orion/tool-map.js';
import { initLogger } from '../lib/logger.js';

async function main(): Promise<void> {
  const config = loadConfig();
  const logger = initLogger({ level: 'warn', pretty: true });

  const client = new OrionMcpClient({
    url: config.ORION_MCP_URL,
    provider: createOrionProvider(config),
    toolMap: resolveToolMap(config.ORION_TOOLS),
    caps: {
      unit: 'fahrenheit',
      min: config.TEMP_MIN_F,
      max: config.TEMP_MAX_F,
      step: config.TEMP_STEP_F,
    },
    logger,
  });

  await client.connect();
  const tools = await client.loadTools();

  if (tools.length === 0) {
    console.log('The server advertised no tools.');
  }
  for (const tool of tools) {
    console.log(`\n# ${tool.name}`);
    if (tool.description) console.log(tool.description);
    console.log(`input schema: ${JSON.stringify(tool.inputSchema, null, 2)}`);
  }

  await client.close();
}

main().catch((err: unknown) => {
  console.error(`\n${err instanceof Error ? err.message : err}`);
  console.error('If you have not authorized yet, run: npm run orion:login');
  process.exitCode = 1;
});
