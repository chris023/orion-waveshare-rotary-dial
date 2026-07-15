# archive/

Superseded iterations and hardware bring-up experiments, kept for history.
**Nothing in this directory is needed to build or run the product** — that's
[`firmware/dial-idf`](../firmware/dial-idf). Read a subdirectory's own files
before trusting anything in it; none of it is maintained.

| Directory | What it was | Why superseded |
|---|---|---|
| [`ts-hub/`](ts-hub/) | The original architecture: a TypeScript/Node hub receiving dial events over MQTT, driving the topper via Orion's MCP server, with a full test suite and a web simulator. | Superseded when the whole design moved on-device (firmware talks to Orion directly — no hub, no MQTT). Its Orion CLI tools (`src/cli/orion-login.ts`, `src/cli/orion-tools.ts`) still work today for poking the Orion MCP API from a computer — see [docs/ORION_MCP.md](../docs/ORION_MCP.md). |
| [`moddable-dial/`](moddable-dial/) | A Moddable XS/TypeScript firmware attempt for the same board. | Display bring-up worked, but the touch and knob drivers were never gotten working under Moddable; the project pivoted to native ESP-IDF, where all three work. |
| [`dial-idf-companion/`](dial-idf-companion/) | A companion-chip (ESP32-U4WDH) firmware that read the knob and forwarded ticks to the S3 over UART, working around the stock companion firmware consuming the knob locally as a BLE volume control. | Unnecessary once the knob was confirmed readable directly on the main S3's own GPIO8/7 under full board init. |
| [`knob-probe/`](knob-probe/) | A bare interrupt-driven GPIO probe on the S3, used to confirm the encoder was really electrically live on GPIO8/7 (polling had been missing edges). | Bring-up probe; its finding is now just how `dial_knob` reads the encoder in the product firmware. |
| [`knob-probe-companion/`](knob-probe-companion/) | The same kind of probe, run on the companion chip, scanning a wider set of GPIOs to rule out other wiring. | Bring-up probe, same reason. |
| [`reference-dial/`](reference-dial/) | A dependency-free Node.js script implementing the full on-device flow (OAuth discovery, DCR, PKCE, MCP over raw HTTP) as a line-by-line blueprint for the C port. | Its job was done once `firmware/dial-idf/components/dial_oauth` and `dial_mcp` existed; kept as a readable reference for that logic. |
| [`esphome/`](esphome/) | A starter ESPHome YAML config, from when dials were expected to speak MQTT to the hub. | No longer applicable — there's no MQTT link or hub to point it at. |
