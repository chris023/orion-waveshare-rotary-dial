# orion-dial (ESP-IDF firmware)

Native **ESP-IDF (C)** firmware for the Waveshare ESP32-S3 rotary knob dial.
This is the product: a standalone device that joins your Wi-Fi, links your
Orion Sleep account, and drives an Orion dual-zone mattress topper directly —
no hub, no phone app, no server of ours in between. Everything (Wi-Fi
provisioning, OAuth 2.1 + Dynamic Client Registration, and MCP-over-HTTPS to
Orion's servers) runs on the dial itself.

## Just want to use the dial?

Flash it from your browser at
[https://chris023.github.io/orion-waveshare-rotary-dial/](https://chris023.github.io/orion-waveshare-rotary-dial/)
— no toolchain needed. After that first flash, every future update arrives
over the air (Menu → About → Software update), so you shouldn't need
anything below this point again. The rest of this README covers building
and modifying the firmware yourself.

## Hardware

- **Board:** Waveshare `ESP32-S3-Knob-Touch-LCD-1.8` — round touch LCD +
  rotary encoder knob, ESP32-S3.
- **Cable:** USB-C, connected straight to your computer for flashing and
  serial monitoring. No adapter or extra wiring needed.
- **Port:** the board enumerates as a USB-serial device — e.g.
  `/dev/cu.usbmodem2101` on macOS, `/dev/ttyUSB0` or `/dev/ttyACM0` on Linux,
  `COM<N>` on Windows. Pass it to `idf.py` with `-p <PORT>`; if you omit `-p`,
  `idf.py` will try to auto-detect it.
- **No port, or the wrong one?** The dial's single USB-C socket reaches a
  different chip depending on which way the connector sits — rotate the
  same connector 180° in the **dial's own socket** (don't swap which end
  of the cable goes where) to switch. For flashing and monitoring you want
  the **S3**, which shows up as `usbmodem*` / "USB JTAG/serial debug unit";
  the other orientation reaches a companion chip that enumerates as
  `usbserial*` instead.

## Prerequisites

**This section and Build & flash below are the developer path** —
building or modifying the firmware yourself. If you just want a working
dial, use the browser flasher above instead; nothing here is required for
that.

This firmware is pinned to **ESP-IDF v6.0**, target **esp32s3**. Install it
with Espressif's standard flow (no project-specific scripts needed):

```bash
git clone -b v6.0 --recursive https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh esp32s3
. ./export.sh              # puts idf.py on PATH for this shell session
```

`export.sh` only sets up the current shell — re-source it (`. $IDF_PATH/export.sh`)
in every new terminal before running `idf.py`. See Espressif's own
[Get Started guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/index.html)
for platform-specific prerequisites (Python, USB drivers, etc.) and
troubleshooting.

## Build & flash

With ESP-IDF installed from above, this produces your own build and puts it
on a dial over USB:

```bash
cd firmware/dial-idf
idf.py set-target esp32s3     # first time only
idf.py build
idf.py -p <PORT> flash monitor
```

No configuration or secrets file is required — a freshly flashed dial boots
straight into on-device first-run setup (see below). `build/`,
`managed_components/`, and `sdkconfig` are generated (git-ignored); `idf.py`
recreates them.

**Optional, developers only:** to skip re-entering Wi-Fi credentials on every
reflash during development, `cp main/secrets.h.example main/secrets.h` and
fill in your network. `secrets.h` is git-ignored and only pre-seeds NVS the
first time there are no stored credentials — it changes nothing for anyone
who doesn't create it.

### Advanced/reference: manual esptool flashing

Not needed for normal `idf.py flash` development — this is for scripting a
production flash or understanding what `idf.py flash` does under the hood.

Each GitHub Release publishes two images: `orion-dial.bin`, the OTA app
image (what the dial fetches for itself over the air), and
`orion-dial-merged.bin`, a full-flash image with bootloader + partition
table + app already combined at their real offsets, meant to be written
starting at `0x0`. That's the image the browser flasher writes, and you can
write it yourself the same way with a release download in hand:

```bash
python -m esptool --chip esp32s3 -p <PORT> -b 921600 write-flash 0x0 orion-dial-merged.bin
```

If you're building locally and want to reproduce what `idf.py flash` does
with a manual `esptool` invocation instead, the individual offsets come from
[`partitions.csv`](partitions.csv) and `build/flash_args` after a build:

```bash
python -m esptool --chip esp32s3 -p <PORT> -b 921600 \
  --before default-reset --after hard-reset write-flash \
  --flash-mode dio --flash-size 16MB --flash-freq 80m \
  0x0     build/bootloader/bootloader.bin \
  0x8000  build/partition_table/partition-table.bin \
  0x19000 build/ota_data_initial.bin \
  0x20000 build/orion-dial.bin
```

Prefer `idf.py -p <PORT> flash` for everyday development — it derives these
offsets itself and is far less likely to go stale if the partition table
ever changes.

## First boot

A freshly flashed (or factory-reset) dial walks through setup on the device
itself:

1. **Welcome.** A splash screen ("ORION DIAL — Turn the knob or tap to
   begin"). Any tap dismisses it; the dial is already working in the
   background from this point on.
2. **Wi-Fi.** The dial can't reach anything until it has your network, and it
   offers two ways to give it that — pick whichever is easier:
   - **From your phone:** the dial's screen names a temporary network (something
     like `OrionDial-XXXX`). Join it from your phone's Wi-Fi settings, and a
     setup page opens on its own (it hijacks DNS so most phones pop the page
     automatically); if it doesn't, open any page in a browser. Pick your home
     network from the list and enter its password.
   - **On the dial:** tap "Set up on the dial" to skip the phone entirely.
     Turn the knob to pick your network from a scanned list, then use the
     on-screen character wheel to type the password: spin the knob to a
     letter/digit/symbol, tap the checkmark to add it to the password, and tap
     the Wi-Fi glyph to connect. There's a backspace disc too. A wrong
     password sends you right back to this screen for the same network with
     a message, not back to square one.

   2.4 GHz only — this hardware doesn't support 5 GHz networks.
3. **Link your Orion account.** Once Wi-Fi is up, the dial shows a QR code —
   scan it **with your phone on the same Wi-Fi network** as the dial. That
   opens Orion's own consent page in your phone's browser; approve it there.
   The dial is watching for the callback on your LAN and picks up the link
   automatically — nothing to type back in on the dial itself. If your phone
   is on a different network (e.g. still on cellular), the dial can't receive
   that callback and the QR will effectively hang; switch your phone to the
   same Wi-Fi and rescan.
4. **Pick a side.** On a dual-zone topper, the dial asks "Which side of the
   bed?" once, right after linking — tap the half of the screen for your
   side. (Skipped entirely on a single-zone topper — there's only one side to
   show.)
5. **The dial screen.** From here on the dial shows the live temperature
   dial for your side, with a swipe to the partner's side and to the menu.

## Everyday use

- **Knob:** turn to adjust the target temperature on the dial screen; turn on
  other screens (menus, network/password pickers) to move focus or dial in a
  value. Detents give a haptic tick; hitting the end of a range gives a
  distinct stop pulse.
- **Touch:** tap the power glyph on the dial screen to turn that side on/off.
  Swipe left/right to move between your side, your partner's side, and the
  menu. Swipe right on any menu sub-screen (or tap its "Back" row) returns to
  the menu. A long-press on the dial screen opens quick actions (bed off,
  boost heat/cool, etc.).
- **Menu → Tonight:** tonight's schedule and a one-night override.
- **Menu → Settings:** units (°C/°F), screen rotation, haptics on/off, and two
  destructive actions guarded by a tap-twice-within-3-seconds confirm
  ("Tap again to confirm"): **Re-link Orion** (forgets the stored Orion
  tokens and restarts into the link step) and **Factory reset** (erases all
  stored state and restarts as a fresh device).
- **Menu → Wi-Fi:** current network, IP, and signal strength, plus **Change
  network** — a full confirmation screen (not tap-twice) since it reboots the
  dial straight into Wi-Fi setup, dropping the current network in the
  process.
- **Menu → About:** firmware version, IDF version, device serial, and
  **Software update** — see OTA below.
- **OTA updates:** however the dial got its first flash — browser or
  USB — everything after that arrives OTA. The dial periodically checks
  this repo's GitHub releases for a newer firmware build (a `dial-vX.Y.Z`
  tag with an `orion-dial.bin` asset — releases also carry an
  `orion-dial-merged.bin` full-flash image, but that's for reflashing from
  scratch, not for OTA; see Build & flash above). It only ever *checks* on
  its own; installing always needs a tap-twice confirm on the About screen's
  Software update row. The currently running version is shown there and on
  About's Firmware row.

## Troubleshooting / FAQ

- **"That password didn't work. Try again."** — the password screen tells you
  the join was rejected and puts you right back on it for the same network;
  just retype it.
- **My Wi-Fi network doesn't show up / won't connect.** This hardware is
  **2.4 GHz only** — it cannot join 5 GHz-only networks. If your router
  broadcasts both bands under one SSID, make sure the 2.4 GHz radio is
  actually enabled.
- **The Orion QR code doesn't seem to do anything after I scan it.** Your
  phone almost certainly isn't on the same Wi-Fi network as the dial — the
  dial listens for the OAuth callback on its own LAN, so a phone on cellular
  data or a different network can approve the consent page but the dial will
  never see it land. Put your phone on the same network as the dial and scan
  again.
- **The dial is stuck on "Connecting to Wi-Fi..." / "Orion unreachable."**
  These screens show the actual error and a retry countdown; the dial keeps
  retrying with backoff on its own. If it's stuck for more than a few
  minutes, double-check the network/password, then consider a factory reset
  (below).
- **Factory reset (from the dial):** Menu → Settings → Factory reset, tap
  twice within 3 seconds to confirm. This erases all stored Wi-Fi
  credentials, Orion tokens, and preferences, and restarts the dial as if
  freshly flashed.
- **Recovering a bricked/misbehaving unit:** if the dial won't boot cleanly
  or a factory reset from Settings isn't reachable, the easy path is the
  [browser flasher](https://chris023.github.io/orion-waveshare-rotary-dial/)
  again — choose the option to erase the device before installing, for a
  clean slate. No toolchain needed, and it works the same whether or not
  you built this yourself. The developer equivalent, from a checkout with
  ESP-IDF set up:
  ```bash
  idf.py -p <PORT> erase-flash flash
  ```
  This repository *is* the factory image for the dial's own ESP32-S3 — there
  is no separate stock firmware to restore it to. (`firmware/backups/` holds
  local flash backups made during hardware bring-up, but that backup is of
  the companion probe chip used during development, not the dial's own
  flash — it isn't a path back to a "factory" dial image.) If you want to
  return the board to Waveshare's own stock demo instead of this project,
  see Waveshare's wiki for the `ESP32-S3-Knob-Touch-LCD-1.8` product.
- **Filing a bug report:** run `idf.py -p <PORT> monitor` while reproducing
  the issue and include the log output — most failures (Wi-Fi, OAuth, MCP
  calls) log a specific reason on this console.

## Status

Implemented and working end-to-end:

- Display/touch/knob bring-up (SH8601 QSPI LCD + LVGL, CST816 touch, `iot_knob`
  rotary encoder).
- On-device Wi-Fi provisioning: phone captive portal *and* a fully on-device
  network picker + character-wheel password entry, with rejected-password
  recovery and reconnect-with-backoff.
- OAuth 2.1 (Dynamic Client Registration, PKCE, QR-code interactive consent,
  NVS token storage, 401-triggered refresh) against Orion's MCP server.
- MCP-over-HTTPS device control: reading live zone state, setting
  temperature/on-off, thermal-relief boost, away mode, "match my side",
  tonight's sleep schedule + one-night override.
- Onboarding flow (welcome splash → Wi-Fi → OAuth link → side pick → dial),
  a menu face with Tonight/Settings/Wi-Fi/About sub-screens, day/night
  palette, haptics, screen rotation, and single- vs. dual-zone topper
  support.
- OTA updates from this repo's GitHub releases, with bootloader
  rollback/rollback-confirm on a bad update.

Open items / known gaps:
- Access-token expiry is inferred from a 401 on the next call, not tracked
  against `expires_in` — functional, but not the most efficient path.
- No automated test suite for this firmware (unlike the archived TypeScript
  hub); changes are verified by building, flashing, and exercising the
  device.

## Provenance

The hardware bring-up (`components/`, `main/main.c`, `partitions.csv`,
`components/dial_display/user_config.h`) is derived from Waveshare's official
`ESP32-S3-Knob-Touch-LCD-1.8` ESP-IDF demo (`08_LVGL_Test` for display + touch
+ LVGL, `04_Encoder_Test` for the knob). Modifications for ESP-IDF **v6.0**:

- Flash set to **16 MB** (the demo shipped 8 MB; the board is 16 MB, and its
  8 MB `factory` partition overflowed 8 MB) — see `partitions.csv` for the
  dual-OTA layout this firmware uses instead.
- BSP component `CMakeLists.txt` files migrated off the removed catch-all
  `driver` component to the specific `esp_driver_i2c` / `esp_driver_gpio` /
  `esp_driver_ledc` / `esp_timer` components.
- Encoder pins (`EXAMPLE_ENCODER_ECA_PIN=8`, `ECB_PIN=7`) in
  `components/dial_display/user_config.h`.

The display uses the managed `esp_lcd_sh8601` QSPI driver (Waveshare drives
this panel via the SH8601 driver + a custom init sequence in `main.c`); touch
is the CST816 on I2C (SDA=GPIO11/SCL=GPIO12); the knob is `iot_knob` on
GPIO8/7.

Everything above `components/` and `main/` — Wi-Fi provisioning, OAuth 2.1,
MCP client, and the whole dial UI (`components/dial_ui/`) — was written for
this project from that bring-up baseline; none of it comes from Waveshare's
demo.
