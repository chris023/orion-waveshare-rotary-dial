# orion-waveshare-rotary-dial

Control an **[Orion Sleep](https://orionsleep.com) dual-zone liquid-cooled
mattress topper** with physical **Waveshare ESP32-S3 rotary knob dials** — turn a
knob to set each side's temperature, tap to toggle it on/off, and see the current
state on the knob's round screen.

> **Status:** early scaffold. The hub runs end-to-end today against a built-in
> topper **simulator** and a **mock** dial, with a full test suite. Talking to the
> real Orion hardware needs a one-time `orion:login` and finalizing the tool
> mapping (see below); flashing the real dials needs the firmware in `firmware/`.

## How it works

Two cooperating pieces (see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)):

1. **The hub** — this repo, a **TypeScript / Node 20** service. It receives dial
   events over MQTT, maps them to per-zone commands, and drives the topper via
   **Orion's official MCP server** (`https://mcp.orionsleep.com/`, OAuth 2.1). It
   also pushes state back to each dial's display.
2. **The dials** — Waveshare **ESP32-S3-Knob-Touch-LCD-1.8** boards running their
   own firmware ([firmware/](firmware/)), talking MQTT over Wi-Fi.

```
knob dial (ESP32-S3)  ──MQTT──►  Node/TS hub  ──MCP/OAuth──►  Orion Sleep cloud ──►  topper
        ▲                            │
        └────── display state ───────┘
```

The design is **ports-and-adapters**, so both the hardware and the cloud are
swappable and the whole thing runs with **no hardware and no Orion account** for
development and CI.

## Quick start (no hardware needed)

```bash
npm install
npm run check      # typecheck + lint + tests
npm run dev        # runs the hub with a MOCK dial + FAKE topper simulator
```

By default (`DIAL_TRANSPORT=mock`, `DEVICE_CLIENT=fake`) it wires up two dials —
`dial-left` and `dial-right` — against an in-memory dual-zone topper.

Copy `.env.example` to `.env` and tweak as needed:

```bash
cp .env.example .env
node --env-file=.env dist/main.js   # after `npm run build`
```

## Connecting the real Orion topper

Orion is controlled through its official MCP server — no reverse engineering.
Full details in [docs/ORION_MCP.md](docs/ORION_MCP.md).

```bash
npm run orion:login    # one-time browser OAuth; saves tokens to ./secrets/ (gitignored)
npm run orion:tools    # prints the MCP tools + schemas the server exposes
```

Then set `DEVICE_CLIENT=orion`, and (if the tool names differ from the defaults)
set `ORION_TOOLS` and adjust the argument/response mapping in
`src/device/orion/mcp-client.ts`.

## Connecting real dials

Flash each ESP32-S3 knob from [`firmware/`](firmware/) (an ESPHome starter is in
`firmware/esphome/orion-knob.yaml`), point it at the hub's MQTT broker, and give
it a unique `dial_id`. Run an embedded broker on the hub with
`BROKER_MODE=embedded`, or use a standalone Mosquitto. Map dials to zones with
`DIAL_BINDINGS` (see `.env.example`).

## Configuration

All config is validated at startup (`src/config/env.ts`). Key vars:

| Var | Default | Meaning |
|-----|---------|---------|
| `DIAL_TRANSPORT` | `mock` | `mock` or `mqtt` |
| `DEVICE_CLIENT` | `fake` | `fake` simulator or real `orion` |
| `MQTT_URL` | `mqtt://localhost:1883` | broker the dials use |
| `BROKER_MODE` | `none` | `embedded` runs an in-process MQTT broker |
| `ORION_MCP_URL` | `https://mcp.orionsleep.com/` | Orion MCP endpoint |
| `TEMP_MIN_F` / `TEMP_MAX_F` / `TEMP_STEP_F` | `55` / `115` / `1` | dial range (°F) |
| `WRITE_DEBOUNCE_MS` | `300` | coalesce a fast spin into one write |
| `DIAL_BINDINGS` | left+right | which dial controls which zone |

## Scripts

| Command | What |
|---------|------|
| `npm run dev` | run the hub with hot reload |
| `npm run check` | typecheck + lint + test |
| `npm test` | run the Vitest suite |
| `npm run build` | compile to `dist/` |
| `npm run orion:login` | one-time Orion OAuth login |
| `npm run orion:tools` | list Orion MCP tools + schemas |

## Tech

TypeScript (ESM, Node 20) · MQTT (`mqtt` / `aedes`) · `@modelcontextprotocol/sdk`
· `zod` · `pino` · Vitest · Biome.

## License

[MIT](LICENSE) © 2026 Chris Meyer
