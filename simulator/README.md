# Virtual dial simulator

A browser stand-in for the Waveshare ESP32-S3 knob, with **hot reload**. It
renders the round 360×360 screen on a canvas, has knob + touch controls, and
speaks the **exact same MQTT protocol as the real firmware** (over WebSocket) to
the hub's embedded broker. Turn a knob → the hub drives the topper → the screen
updates — the whole loop, on your Mac, no hardware.

It doubles as the design reference for the eventual on-device LVGL UI.

## Run it

Two terminals:

```bash
# 1) the hub with an embedded MQTT broker (TCP + WebSocket) and the fake topper
npm run hub:sim

# 2) the simulator (Vite dev server, hot reload)
npm run sim
```

Then open <http://localhost:5178/>. You should see the status go green
(`connected · ws://localhost:8888`) and two dials, LEFT and RIGHT.

Controls per dial: **−/+** rotate (also scroll-wheel over the screen), **Tap**
toggles on/off (also click the screen), **Boost** = long-press (thermal relief).

## Config (URL query)

- `?dials=dial-left,dial-right` — which dials to render (must match the hub's
  `DIAL_BINDINGS`).
- `?broker=ws://localhost:8888` — the hub's WebSocket MQTT port (`BROKER_WS_PORT`).
- `?base=orion-dials` — the base MQTT topic (`MQTT_BASE_TOPIC`).

## Protocol

Mirrors `src/hardware/protocol.ts` (the single source of truth):

```
publish  orion-dials/<dialId>/event    {"type":"rotate","dir":"cw","steps":1} | {"type":"tap"} | {"type":"longpress"}
receive  orion-dials/<dialId>/display  {"label","power","target","current","active","offline"}
```

> Temperatures shown are whatever the hub publishes. With the current fake topper
> that's the placeholder scale; once the hub is switched to the real Orion model
> it will be °C (10–45). The simulator auto-detects the range for colour/gauge.
