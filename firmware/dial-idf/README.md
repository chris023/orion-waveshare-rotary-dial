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
   scan it with your phone's camera. That opens Orion's own consent page in
   your phone's browser; approve it there. Your phone can be on any network —
   the dial's Wi-Fi, a different one, or cellular — because the approval comes
   back through a hosted relay the dial polls, not a callback *into* the dial,
   so there's no local-reachability requirement. The dial picks up the link
   automatically — nothing to type back in on the dial itself. (How and why the
   relay works is written up in [../../docs/linking-relay.md](../../docs/linking-relay.md).)
   The network name printed under the code is a link: tap it to open Wi-Fi
   settings if it's the *dial* that's on the wrong network. Swiping left opens
   the menu from here too, so Settings and software update stay reachable before
   the dial is linked.
4. **Pick a side.** On a dual-zone topper, the dial asks "Which side of the
   bed?" once, right after linking — tap the half of the screen for your
   side. (Skipped entirely on a single-zone topper — there's only one side to
   show.)
5. **The dial screen.** From here on the dial shows the live temperature
   dial for your side, with a swipe to the partner's side and to the menu.

## Everyday use

- **Knob:** turn to adjust the target temperature on the dial screen; turn on
  other screens (menus, network/password pickers) to move focus or dial in a
  value. Turning is deliberately silent — the encoder's own mechanical
  detents are the feedback — but hitting the end of a range gives a distinct
  stop pulse. For a sleepy shortcut, keep turning once you reach the cold or
  hot end of the temperature range: the first detent at the rail arms with a
  single bump, further turns stay quiet for about a second, and the next one
  after that confirms with a triple bump and starts the last-used Boost
  duration, defaulting to 15 minutes. Turn the opposite way, or simply stop,
  to cancel arming. While a side is switched off none of this applies: the
  knob and the arc handle won't move its setpoint or start a Boost, and the
  power button breathes instead to point at what turns the side back on.
- **Touch:** tap the power button on the dial screen to turn that side on/off;
  long-press it to open the Schedule/Hold picker. Tap the snowflake or flame
  icon sitting at either end of the temperature arc to start a timed boost
  (cool or heat). Dragging the arc handle only ever sets the temperature —
  the rails are ordinary setpoints there, and the icons are the one-tap way to
  start a boost by hand. Tap
  that boost's end time and an ✕ that cancels it; while a boost is running the
  knob cancels it with the mirror of the gesture that started it — turn away
  from the boost (left during a heat boost, right during a cool one) once to
  arm with a single bump, then again about a second later for the stronger
  confirm-and-cancel. Tap the big number itself to switch between the absolute
  temperature and Orion's −10…+10 level — the same setting Settings → Scale
  writes, so it sticks across a reboot. Swipe left/right to move between your
  side, your partner's side, and the menu. Swipe right on any menu sub-screen
  (or tap its "Back" row) returns to the menu.
- **The level ring.** On the relative scale the dial face swaps its continuous
  arc for 21 discrete segments, one per level. The levels were never evenly
  spaced in degrees — they carry hand-picked °F values across 10–45 °C — so a
  smooth sweep was quietly implying a precision the scale doesn't have. The
  ring carries the scale's own colour, cool at −10 through neutral at 0 to warm
  at +10, which means the segment you're on doesn't have to introduce a new
  colour to stand out: it's the same hue, at full strength and a few pixels
  fatter. The stretch back to neutral stays lit so distance from centre is a
  length rather than arithmetic, level 0 keeps a quiet grey mark of its own so
  the ring reads bipolar at a glance, and a thin underline hugs whichever
  segment the water is actually sitting under right now — while the side is
  switched on. Off, that underline goes away with everything else that would
  make the face look like it were still running. Absolute mode keeps the same
  arc and the same handle, and reports the water in the WATER caption alone:
  the wash that used to shade the arc between the setpoint and the measured
  temperature has been removed, so the one mark on that band is the setting
  you chose.
- **Menu → Settings:** adjustment mode (Schedule/Hold — the default for what a
  dial-side change does), **Brightness** (its own sub-screen: separate Day,
  Night (in use), and Night (clock) levels, each a full-screen 0–100% picker
  you can drag or turn — Night (clock) at 0 reads "Off", and off means off:
  the standby clock goes fully dark, and a touch or a knob turn still wakes
  the dial), screen timeout (5s/15s/30s/1m before the standby clock takes over),
  temperature scale (Absolute °F/°C, or Orion's −10…+10 relative levels), units
  (°C/°F, used for the absolute readouts), haptics (Off / Low / High / Auto —
  Auto is High by day and Low at night), screen rotation, Away mode, and two
  destructive actions guarded by a tap-twice-within-3-seconds confirm
  ("Tap again to confirm"): **Re-link Orion** (forgets the stored Orion
  tokens and restarts into the link step) and **Factory reset** (erases all
  stored state and restarts as a fresh device).
- **Menu → Wi-Fi:** current network, IP, and signal strength, plus **Change
  network** — a full confirmation screen (not tap-twice) since it reboots the
  dial straight into Wi-Fi setup, dropping the current network in the
  process.
- **Menu → Update:** the one place update behavior lives — the installed
  version, **Check for updates** (tap to check, tap-twice to install),
  **Auto-update** (Off / Overnight), **Skip this version** while one is
  pending, and **Beta builds**. The menu row itself carries a dot and the
  pending version number whenever an update is waiting.
- **Menu → About:** firmware version, IDF version, and device serial.
- **OTA updates:** however the dial got its first flash — browser or
  USB — everything after that arrives OTA. The dial checks this repo's GitHub
  releases every 6 hours for a newer firmware build (a `dial-vX.Y.Z` tag with
  an `orion-dial.bin` asset — releases also carry an `orion-dial-merged.bin`
  full-flash image, but that's for reflashing from scratch, not for OTA; see
  Build & flash above). Turning on **Beta builds** makes it consider
  prereleases too.

  When one is found you get an "Update available" line on the dial and
  standby faces, and — at most once a day, only when you wake the screen, and
  never during your sleep window — a sheet offering to install it right then.
  Nothing installs without a tap unless you turn **Auto-update** on, in which
  case it runs unattended in a quiet window after your scheduled wake time and
  leaves the screen dark while it does. Every install path is rollback-guarded:
  if the new firmware doesn't come up and confirm itself, the bootloader
  reverts to the one that worked.

## Troubleshooting / FAQ

- **"That password didn't work. Try again."** — the password screen tells you
  the join was rejected and puts you right back on it for the same network;
  just retype it.
- **My Wi-Fi network doesn't show up / won't connect.** This hardware is
  **2.4 GHz only** — it cannot join 5 GHz-only networks. If your router
  broadcasts both bands under one SSID, make sure the 2.4 GHz radio is
  actually enabled.
- **The Orion QR code doesn't seem to do anything after I scan it.** The
  approval comes back through a hosted relay the dial polls, so your phone's
  network doesn't matter — but the *dial* still needs working internet to reach
  both the relay and Orion. If it hangs, the dial is most likely on a flaky or
  captive Wi-Fi: confirm it's actually online (the network name under the code
  is a link to its Wi-Fi settings), and re-approve the consent page. A dropped
  approval times out on its own and the QR refreshes — just scan and approve
  again.
- **The dial says "Sign-in expired."** Orion ends sign-in sessions
  periodically on its side — we've observed it about 30 days after linking
  (one observation so far) — and nothing the dial does can extend that. It
  isn't a fault: your bed keeps running its schedule in the meantime. Tap
  **Renew sign-in**, scan the QR code that appears, and approve on your phone;
  it takes under a minute. Swipe right on the QR to back out, and if you leave
  it up the dial returns to the calm screen after 5 minutes. While it waits,
  the dial stops contacting Orion and the sign-in relay; it only checks for
  updates if you ask from the menu. (Firmware 1.6.0 and later; older
  firmware shows the setup QR straight away instead.)
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
  temperature/on-off, thermal-relief boost, away mode, and the sleep
  schedule that times overnight temperature writes.
- Onboarding flow (welcome splash → Wi-Fi → OAuth link → side pick → dial),
  a menu face with Settings/Wi-Fi/About sub-screens, day/night
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
