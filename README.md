# orion-waveshare-rotary-dial

> **Not affiliated with, endorsed by, or supported by Orion Sleep or Waveshare.**
> This is an independent, community-built project.

Turn a knob to change the temperature of your side of the bed. This project
turns a **Waveshare ESP32-S3 round touch-LCD knob** into a standalone bedside
dial for an **[Orion Sleep](https://orionsleep.com) dual-zone mattress
topper** — no phone app, no hub on your network, no cloud service to run
yourself. The dial talks to Orion directly, over Wi-Fi you already have.

<p align="center">
  <img src="docs/screens/dial.png" width="230" alt="The temperature dial, mid-adjustment">
  <img src="docs/screens/oauth-qr.png" width="230" alt="QR code for pairing with your Orion account">
  <img src="docs/screens/standby.png" width="230" alt="The standby clock face">
</p>

<!-- TODO: photo of the flashed dial on a nightstand -->

## Hardware required

- **Waveshare ESP32-S3-Knob-Touch-LCD-1.8** — the round 1.8" touch-LCD knob
  board (ESP32-S3, rotary encoder + capacitive touch + haptics). See
  [firmware/README.md](firmware/README.md) for the full parts table.
  Sold by Waveshare here:
  [waveshare.com/esp32-s3-knob-touch-lcd-1.8](https://www.waveshare.com/esp32-s3-knob-touch-lcd-1.8.htm?sku=34875)
  — a third-party store with no connection to this project, linked purely for
  convenience. Verified to point at the right board on 2026-08-03; product
  pages move and SKUs get retired, so check the specs on arrival rather than
  trusting the link.
- An Orion Sleep dual-zone topper on the same 2.4 GHz Wi-Fi network, and an
  Orion account.

One dial covers one side of the bed; run two if you want independent control
of both zones.

## Quick start

### ⚡ Flash from your browser (recommended)

**Open [chris023.github.io/orion-waveshare-rotary-dial](https://chris023.github.io/orion-waveshare-rotary-dial/)
in Chrome or Edge, plug the dial in over USB-C, and click Install.**
That's the whole install — no toolchain, no downloads. Everything else
(Wi-Fi setup, pairing your Orion account, choosing a side) happens on the
dial's own screen, and future updates arrive over the air.

> No port showing up in the picker? Rotate the connector 180° in the
> dial's own socket — same cable end, just upside down; the board reaches
> a different chip in each orientation.

<details>
<summary><sub>Build from source (developers)</sub></summary>

```bash
cd firmware/dial-idf
idf.py set-target esp32s3
idf.py build
idf.py -p <PORT> flash
```

Needs **ESP-IDF v6.0** installed first. Full build/flash instructions,
board bring-up notes, and firmware architecture live in
[firmware/dial-idf/README.md](firmware/dial-idf/README.md).

</details>

## Features

- **On-device setup** — Wi-Fi and Orion pairing both happen right on the
  dial. No app, no computer, no config file.
- **Scan-and-approve Orion pairing** — a QR code opens Orion's consent page
  in your phone's browser; the dial registers itself as an OAuth client on
  the spot (Dynamic Client Registration) — no shared secret baked into the
  firmware. The approval comes back through a hosted relay the dial polls, so
  pairing works even if your phone is on cellular or a different Wi-Fi network
  than the dial — no local reachability required. How and why it works this way
  is written up in [docs/linking-relay.md](docs/linking-relay.md).
- **A calm heads-up when the sign-in runs out** — Orion ends sign-in sessions
  periodically on its side (we've observed about 30 days after linking), and
  the dial can't extend them. When it happens the dial shows a quiet "Sign-in
  expired" screen (your bed keeps running its schedule meanwhile) with a
  **Renew sign-in** button, and renewing takes under a minute. *(1.6.0 betas
  and later.)*
- **Dual-zone control** with a side picker, so one dial can drive either
  half of the bed — plus screen rotation, so the same board reads right
  from either nightstand.
- **Absolute or relative temperature** — read the setpoint as a real
  temperature (**°F or °C**, your choice) or as an Orion-style **−10…+10
  level**, switchable in settings, or by tapping the big number on the dial
  face. Fresh dials default to relative.
- **A ring that counts, not sweeps** — on the relative scale the ring breaks
  into 21 segments, one per level, with the one you're on lit and thickened
  and the stretch back to neutral washed in the same colour. The levels
  aren't evenly spaced in degrees and never were, so a continuous sweep was
  quietly lying about them; this way you can read the setting from across a
  dark room without reading the number.
- **Schedule or hold** — a change made on the dial can either ride your
  Orion schedule (the next scheduled step still happens, matching what the
  app does) or hold for the rest of the night. A status chip on the dial
  face says which is in effect, and when it runs out.
- **Boost heat or cool** from the icons at either end of the temperature
  arc — a timed push in one direction that returns you to where you were.
  For sleepy shortcuts, keep turning the knob once you reach the cold or hot
  end: the first detent at the rail arms with a
  single bump, then keep pushing the same way; extra turns stay quiet for about
  a second, then a triple bump confirms and starts the last-used Boost
  duration, defaulting to 15 minutes. The opposite turn
  cancels arming, and once a Boost is running the same arm-then-confirm turn
  in the opposite direction cancels it. Dragging the arc handle by hand only
  sets the temperature — tap the icons for a boost. A side that is switched
  off ignores all of it — knob and handle alike — and breathes the power
  button instead, so a half-asleep nudge can't quietly set a temperature on a
  side that isn't running.
- **Quiet knob, honest haptics** — the encoder's own mechanical detents are
  the feedback when you turn it; the motor only fires where it tells you
  something the clicks can't, like hitting the end of a range. Strength is
  configurable (Off / Low / High / Auto, dimmer at night).
- **Day/night palette** that shifts with your sleep schedule, and a **standby
  clock face** for when it's idle — with separate brightness for the day, the
  night, and the clock itself, plus a configurable screen timeout.
- **OTA updates** straight from this repo's GitHub Releases, with bootloader
  rollback if one goes bad, an optional beta channel, and auto-update that
  installs in the quiet hours after you wake — never while you're asleep.
- **Away mode**, **re-link**, and a tap-twice **factory reset** in settings,
  for when you travel, the pairing goes stale, or you want to start clean.

## Screens

<table>
<tr>
<td align="center"><img src="docs/screens/welcome.png" width="170" alt="Welcome splash screen"></td>
<td align="center"><img src="docs/screens/netpick.png" width="170" alt="Network picker screen"></td>
<td align="center"><img src="docs/screens/passkey.png" width="170" alt="On-screen character wheel for typing a Wi-Fi password"></td>
<td align="center"><img src="docs/screens/oauth-qr.png" width="170" alt="QR code for pairing with Orion"></td>
</tr>
<tr>
<td align="center">First boot</td>
<td align="center">Join Wi-Fi</td>
<td align="center">Type a password</td>
<td align="center">Pair with Orion</td>
</tr>
<tr>
<td align="center"><img src="docs/screens/sidepick.png" width="170" alt="Side picker screen for dual-zone toppers"></td>
<td align="center"><img src="docs/screens/dial.png" width="170" alt="The temperature dial showing a setpoint in degrees Fahrenheit"></td>
<td align="center"><img src="docs/screens/dial-relative.png" width="170" alt="The temperature dial showing an Orion-style relative level instead of a temperature"></td>
<td align="center"><img src="docs/screens/dial-until.png" width="170" alt="The temperature dial with the status pill showing Until H:MM"></td>
</tr>
<tr>
<td align="center">Pick your side</td>
<td align="center">The dial</td>
<td align="center">Relative scale</td><td align="center">Holds until schedule</td>
</tr>
<tr>
<td align="center"><img src="docs/screens/boost.png" width="170" alt="Thermal-relief boost screen"></td>
<td align="center"><img src="docs/screens/adjust-mode.png" width="170" alt="Adjustment mode screen: schedule or hold"></td>
<td align="center"><img src="docs/screens/menu.png" width="170" alt="Menu screen"></td>
<td align="center"><img src="docs/screens/settings.png" width="170" alt="Settings screen"></td>
</tr>
<tr>
<td align="center">Boost</td>
<td align="center">Adjustment mode</td>
<td align="center">Menu</td>
<td align="center">Settings</td>
</tr>
<tr>
<td align="center"><img src="docs/screens/brightness-menu.png" width="170" alt="Brightness menu with separate day, night and night-clock levels"></td>
<td align="center"><img src="docs/screens/brightness.png" width="170" alt="Full-screen brightness picker"></td>
<td align="center"><img src="docs/screens/update-prompt.png" width="170" alt="Update prompt offering to install now or later"></td>
<td align="center"><img src="docs/screens/update.png" width="170" alt="Update screen with auto-update, skip and beta options"></td>
</tr>
<tr>
<td align="center">Brightness</td>
<td align="center">Setting a level</td>
<td align="center">Update prompt</td>
<td align="center">Update options</td>
</tr>
<tr>
<td align="center"><img src="docs/screens/standby.png" width="170" alt="Standby clock face"></td>
<td align="center"><img src="docs/screens/updating.png" width="170" alt="Install progress screen"></td>
<td align="center"><img src="docs/screens/wifi-info.png" width="170" alt="Wi-Fi details screen"></td>
<td align="center"><img src="docs/screens/about.png" width="170" alt="About screen showing firmware version"></td>
</tr>
<tr>
<td align="center">Standby clock</td>
<td align="center">Installing</td>
<td align="center">Wi-Fi details</td>
<td align="center">About</td>
</tr>
<tr>
<td align="center"><img src="docs/screens/link-expired.png" width="170" alt="Calm Sign-in expired screen with a Renew sign-in button"></td>
<td align="center"><img src="docs/screens/link-renew-qr.png" width="170" alt="QR code for renewing the Orion sign-in"></td>
</tr>
<tr>
<td align="center">Sign-in expired</td>
<td align="center">Renew sign-in</td>
</tr>
</table>

The rest — the boost-active dial, the pre-bedtime pause state, the standby
face carrying an update hint, the night-clock brightness picker — are in
[`docs/screens/`](docs/screens/).

### Preview the UI without hardware

Every screenshot above is a pixel-exact render of the real firmware UI,
produced by the simulator in [`simulator/`](simulator/) — no board required:

```bash
cmake -B build -S simulator
cmake --build build
```

Then run `./build/dial_sim` from the repo root; PNGs land in
`docs/screens/`. See
[simulator/README.md](simulator/README.md) for details.

## What's new

[CHANGELOG.md](CHANGELOG.md) covers every release; the same notes appear on
each [GitHub Release](https://github.com/chris023/orion-waveshare-rotary-dial/releases).

## Repo layout

- [`firmware/dial-idf/`](firmware/dial-idf/) — the product: the ESP-IDF (C)
  firmware that actually ships.
- Earlier prototypes (a TypeScript/Node hub, a Moddable display attempt,
  hardware bring-up probes) have been removed from the tree; they're
  preserved in this repo's git history, not needed to build or run the dial.

## License

[PolyForm Noncommercial 1.0.0](LICENSE) © 2026 Chris Meyer. Free to use,
build, modify, and share for **personal and other noncommercial
purposes**. Any commercial use — selling devices or software built on
this project, or using it in or for a business — requires a separate
commercial license: open a GitHub issue to ask. Third-party components
used by the firmware are under their own licenses — see
[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).

## Contributing

Contributions are welcome. By submitting one you agree it's licensed to
the project maintainer under terms that let the project be licensed as
above, including commercially.
