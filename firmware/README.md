# Dial firmware (Waveshare ESP32-S3-Knob-Touch-LCD-1.8)

Each physical dial runs firmware on its ESP32-S3 that:

1. reads the **rotary encoder** (turn) and **capacitive touch** (tap/long-press),
2. publishes those as MQTT events to the hub,
3. subscribes to display updates and renders the current zone temperature/state
   on its round LCD.

> The hub (this repo, in `src/`) is written in TypeScript. The firmware is a
> separate concern that runs on the device. Two supported paths are below.

## Board facts (from research — verify against your unit's wiki)

| Part | Detail |
|------|--------|
| MCU | ESP32-S3R8 (8 MB PSRAM, 16 MB flash) + companion ESP32-U4WDH |
| Display | 1.8" **round** 360×360 IPS, **ST77916** controller over **QSPI**, LVGL |
| Display pins | CS=14, CLK=13, DATA=15/16/17/18, RST=21, backlight(PWM)=47 |
| Rotary encoder | incremental A/B quadrature: **pin_a=GPIO8, pin_b=GPIO7** (not magnetic) |
| Knob click | **none** — GPIO0 is the boot button; use a touchscreen **tap** instead |
| Touch | **CST816S/T** over I²C: SDA=11, SCL=12, INT=9, RST=10 |
| Haptics | DRV2605 LRA driver @ I²C 0x5A |
| Audio | PCM5100A I²S DAC + MEMS mic (no speaker) |

The encoder's phase behavior is slightly non-standard, so ESPHome's stock
`rotary_encoder` can miscount — the community uses a custom polling component.
See the references below.

## MQTT wire protocol (must match `src/hardware/protocol.ts`)

Base topic defaults to `orion-dials`; each dial has a unique `<dialId>`.

- **Device → hub**, topic `orion-dials/<dialId>/event`:
  ```json
  {"type":"rotate","dir":"cw","steps":1}
  {"type":"rotate","dir":"ccw","steps":1}
  {"type":"tap"}
  {"type":"longpress"}
  ```
- **Hub → device**, topic `orion-dials/<dialId>/display` (retained):
  ```json
  {"label":"LEFT","power":"on","target":72,"current":70,"active":true,"offline":false}
  ```
- **Device → hub**, topic `orion-dials/<dialId>/status`: last-will `online`/`offline`.

Point each dial at the hub's broker. Run an embedded broker on the hub with
`BROKER_MODE=embedded` (default port 1883), or use a standalone Mosquitto.

## Path A — ESPHome (recommended, least code)

Working community configs already exist for this board. Start from
[`esphome/orion-knob.yaml`](./esphome/orion-knob.yaml), set your Wi-Fi + MQTT
broker (the hub) and a unique `dial_id` per device, then flash with ESPHome.
This gets encoder + touch + MQTT working quickly; the LVGL round-display UI is
the main thing to build out (see references).

## Path B — Arduino / ESP-IDF + LVGL (full control)

Use Waveshare's official examples for the display/LVGL, add the encoder + touch
handlers, and an MQTT client (`PubSubClient` / `ArduinoMqttClient`) that speaks
the protocol above.

## References

- Waveshare wiki: https://www.waveshare.com/wiki/ESP32-S3-Knob-Touch-LCD-1.8
- ESPHome community config (encoder/display/haptics): https://github.com/KrX3D/WaveShare-Knob-Esp32S3
- Board notes / esptool dumps: https://github.com/nkinnan/Waveshare-ESP32-S3-Knob-Touch-LCD-1.8_and_Guition-K5-Knob-Series-JC3636K518
