# orion-waveshare-rotary-dial

> **Not affiliated with, endorsed by, or supported by Orion Sleep or Waveshare.**
> This is an independent, community-built project.

Turn a knob to change the temperature of your side of the bed. This project
turns a **Waveshare ESP32-S3 round touch-LCD knob** into a standalone bedside
dial for an **[Orion Sleep](https://orionsleep.com) dual-zone mattress
topper** — no phone app, no hub on your network, no cloud service to run
yourself. The dial talks to Orion directly, over Wi-Fi you already have.

<!-- TODO: photo of the flashed dial on a nightstand -->

## Hardware required

- **Waveshare ESP32-S3-Knob-Touch-LCD-1.8** — the round 1.8" touch-LCD knob
  board (ESP32-S3, rotary encoder + capacitive touch + haptics). See
  [firmware/README.md](firmware/README.md) for the full parts table.
- An Orion Sleep dual-zone topper on the same 2.4 GHz Wi-Fi network, and an
  Orion account.

One dial covers one side of the bed; run two if you want independent control
of both zones.

## Quick start

Everything else — Wi-Fi setup, pairing your Orion account, choosing a side —
happens on the device itself after first boot. To build and flash:

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash
```

That needs **ESP-IDF v6.0** installed first. Full build/flash instructions,
board bring-up notes, and firmware architecture live in
[firmware/dial-idf/README.md](firmware/dial-idf/README.md).

## Features

- **On-device Wi-Fi setup** — join over a phone-browser captive portal, or
  pick a network and type a password with the knob, no app required.
- **Own-account Orion pairing** — scan a QR code, approve in your phone's
  browser, done. The dial registers itself as an OAuth client (no shared
  secret baked into the firmware).
- **Dual-zone control** — a side picker switches which half of the bed the
  knob is turning.
- **°F or °C** — pick your unit in settings.
- **OTA updates** — checks GitHub Releases for new firmware and installs
  over the air.
- **Factory reset** — tap-twice-confirm in settings to unpair and start over.

## Repo layout

- [`firmware/dial-idf/`](firmware/dial-idf/) — the product: the ESP-IDF (C)
  firmware that actually ships.
- Earlier prototypes (a TypeScript/Node hub, a Moddable display attempt,
  hardware bring-up probes) have been removed from the tree; they're
  preserved in this repo's git history, not needed to build or run the dial.

## License

[MIT](LICENSE) © 2026 Chris Meyer. Third-party components used by the
firmware are under their own licenses — see
[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).
