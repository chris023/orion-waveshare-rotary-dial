# Architecture

This project is a **hub service** that bridges physical rotary dials to the
Orion Sleep mattress topper. It is deliberately split into two systems:

```
┌────────────────────┐     Wi-Fi / MQTT      ┌────────────────────────┐   MCP over HTTPS   ┌───────────────────┐
│  Waveshare ESP32-S3 │  ───────────────────► │   Node/TS hub (this repo) │ ─────────────────► │  Orion Sleep MCP  │
│  knob dial(s)       │  ◄─────────────────── │   controller + adapters   │ ◄───────────────── │  server (cloud)   │
│  (firmware/)        │   display updates     │                           │   tool results     │  mattress topper  │
└────────────────────┘                       └────────────────────────┘                    └───────────────────┘
```

1. **The dials** (`firmware/`) run their own firmware on the ESP32-S3. Each reads
   its rotary encoder + touch, renders a round display, and talks MQTT over Wi-Fi.
2. **The hub** (`src/`, the TypeScript deliverable) receives dial events, maps
   them to topper commands, calls the Orion MCP server, and pushes state back to
   the dial screens.

## Ports & adapters (hexagonal)

The core is pure and depends only on two interfaces ("ports"), so the hardware
and the cloud are both swappable — and the whole thing runs with **no hardware
and no Orion account** in dev/CI.

| Port | File | Real adapter | Test/dev adapter |
|------|------|--------------|------------------|
| `DialTransport` (dials in / displays out) | `src/hardware/dial-transport.ts` | `MqttDialTransport` | `MockDialTransport` |
| `DeviceClient` (topper control) | `src/device/device-client.ts` | `OrionMcpClient` | `FakeDeviceClient` |

```
src/
  config/env.ts          # zod-validated config; fails fast
  domain/                # pure types: dial events, zone state, commands, bindings
  hardware/              # INPUT adapter: DialTransport port + MQTT/mock impls + wire protocol
  device/                # OUTPUT adapter: DeviceClient port + Orion MCP client + fake simulator
  controller/            # CORE: pure mapping (dial event -> zone state) + orchestrating Controller
  lib/                   # logger, retry/backoff, embedded MQTT broker
  cli/                   # orion:login (OAuth) and orion:tools (discover MCP tools)
  app.ts                 # composition root (selects adapters from config)
  main.ts                # entrypoint + graceful shutdown
```

## Data flow for one knob turn

1. The dial firmware publishes `{"type":"rotate","dir":"cw","steps":1}` to
   `orion-dials/<dialId>/event`.
2. `MqttDialTransport` parses it (`protocol.ts`) into a `DialEvent`.
3. `Controller.handleEvent` looks up the dial's **binding** (which device + zone)
   and applies the pure `mapping.ts` function → next desired zone state.
4. Power changes are written immediately; temperature changes are **debounced**
   (a fast spin coalesces into a single write) so we don't hammer the cloud.
5. The write goes through `DeviceClient` → `OrionMcpClient` → an MCP tool call.
6. The updated state is pushed back to `orion-dials/<dialId>/display` (retained),
   so the knob's screen re-renders — even after a reboot.

## Why these choices

- **MQTT** for the dial link: natural pub/sub for *multiple* dials, bidirectional
  (events up, display down), resilient to reconnects, and ESPHome speaks it
  natively. An embedded broker (`aedes`, `BROKER_MODE=embedded`) means you can run
  the whole thing on one host with no external Mosquitto.
- **MCP** for Orion: it's the vendor's official, OAuth-secured interface — see
  [ORION_MCP.md](./ORION_MCP.md). No reverse engineering required.
- **Debounce + retry/backoff**: a rotary encoder emits many ticks/second; the
  controller coalesces them and the Orion client honors `429 Retry-After`.
