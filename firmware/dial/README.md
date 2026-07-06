# Orion knob firmware (Moddable XS, TypeScript)

Self-contained firmware for the Waveshare **ESP32-S3-Knob-Touch-LCD-1.8**
(Guition JC3636K518). The dial does **everything on-device**: Wi-Fi, on-device
OAuth 2.1 (PKCE) to Orion, a hand-rolled MCP client over HTTPS, the rotary
encoder + touch, and the round LVGL-style UI. No hub, no computer at runtime —
after a one-time phone consent, it refreshes its own token forever.

Written in **Moddable XS** so the app is TypeScript, flashed as an image.

## ⚠️ Status — read this first

This is a **structured skeleton**, not a flash-ready binary. It was authored
without a Moddable SDK or the board in the loop, so:

| Layer | Confidence | Notes |
|-------|-----------|-------|
| Project + manifest + build setup | High | standard Moddable, from docs |
| **OAuth + MCP + Orion logic** (`src/oauth.ts`, `mcp.ts`, `orion.ts`, `net.ts`) | High | direct port of the *verified* `firmware/reference-dial/dial.ts` onto Moddable's `http`/`securesocket`/`crypt` APIs |
| Encoder input (`src/input.ts`) | Med-High | `pins/pulsecount` (HW quadrature) |
| Touch driver (`src/cst816.ts`) | Medium | JS/SMBus, modeled on ft6206 — tune on HW |
| UI (`src/ui.ts`) | Medium | Piu layout mirroring the simulator |
| **Display driver** (`drivers/st77916/`) | **Port required** | copy Moddable's CO5300 QSPI driver, swap the init sequence from the OEM demo — see that folder |

Everything is heavily commented with `VERIFY:` (check against your SDK) and
`TODO(hw):` (needs on-hardware work) markers.

## Prerequisites

- Moddable SDK **≥ 8.0.0** (for the CO5300 QSPI reference driver) with ESP-IDF 5.x — https://github.com/Moddable-OpenSource/moddable
- `npm i` here (pulls `@moddable/typings` + `typescript` for type-checking)
- The board's **ST77916 init sequence + pin map** from the Guition OEM demo
  (`pan.jczn1688.com` → HMI display → `JC3636K518CN_knob_EN.zip`), or
  https://github.com/modi12jin/IDF-S3_ST77916-QSPI_CST816T-I2C_LVGL

## Build & flash

```bash
# Wi-Fi passed on the command line so it never lands in git:
UPLOAD_PORT=/dev/cu.usbmodemXXXX \
  mcconfig -d -m -p esp32/esp32s3 ssid="YOUR_WIFI" password="YOUR_PASS"
```

`-d -m` = debug build + launch xsbug (the JS debugger). First boot with no saved
token shows a **QR code**; scan it on your phone, approve in the Orion app, and
the dial catches the redirect on its own `orion-knob.local` address and stores
the token in NVS.

## Bring-up order (de-risk one subsystem at a time)

1. **Serial/blink** — confirm toolchain + flashing.
2. **Display** (hardest — do first): port `drivers/st77916/`, show a solid fill
   then a bitmap. Nothing else matters until the panel lights up.
3. **Touch** — `src/cst816.ts` printing x/y over serial.
4. **Encoder** — `src/input.ts` printing rotation deltas.
5. **UI** — `src/ui.ts` rendering the dial from a fake state.
6. **Net** — `src/oauth.ts` + `mcp.ts` against real Orion (the logic is already
   proven by the reference dial; verify TLS + the `http.Request` callback shapes).

## File map

```
firmware/dial/
  manifest.json        # Moddable build descriptor (includes, config, pins, modules, resources)
  tsconfig.json        # editor type-checking against @moddable/typings
  package.json
  main.ts              # orchestration: wifi -> auth -> MCP -> input/UI loop
  src/
    config.ts          # constants (Orion URL, scope, ports, temp range, bindings)
    net.ts             # httpsJson(): HTTPS via http.Request + securesocket, Promise-wrapped
    oauth.ts           # DCR + PKCE + token exchange/refresh + loopback Server + mDNS + Preference
    mcp.ts             # raw MCP client: initialize, tools/call, Mcp-Session-Id, SSE parse
    orion.ts           # device client: list_devices/get_device_state/set_zone/thermal_relief (+ °F<->°C, zone map)
    input.ts           # rotary encoder (pins/pulsecount) + button
    cst816.ts          # capacitive touch driver (pins/smbus)
    ui.ts              # Piu round-dial screen (+ setup QR)
    zones.ts           # left/right <-> zone_a/zone_b + °F<->°C helpers (shared with hub logic)
  drivers/st77916/     # QSPI display driver PORT (from Moddable CO5300) — see its README
```

The net/oauth/mcp/orion modules are a faithful port of the repo's
`firmware/reference-dial/dial.ts` (which was verified end-to-end against the live
Orion server) and the hub's `src/device/orion/mcp-client.ts` — so the protocol is
already correct; only the Moddable transport details need on-hardware confirmation.
