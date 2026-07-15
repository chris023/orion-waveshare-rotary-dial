# Architecture

The dial is a single ESP32-S3 firmware image ([`firmware/dial-idf`](../firmware/dial-idf)).
There is no server, hub, or MQTT broker in the picture — the device does Wi-Fi
provisioning, OAuth 2.1, and MCP-over-HTTPS itself, and talks straight to
Orion's cloud:

```
knob dial (ESP32-S3)  ──Wi-Fi──►  Orion MCP server (cloud)  ──►  topper
```

This doc is a contributor orientation to `main/main.c` and `components/`, not
a spec — read the source (especially the comment block at the top of
`main.c`) for the load-bearing details.

## Boot phases

`app_state_t.phase` (`components/dial_state/dial_state.h`) drives both the UI
router and the worker's own state machine:

1. **`PH_WIFI_CONNECTING`** — joining stored (or seeded) credentials.
2. **`PH_WIFI_PORTAL`** — no usable credentials; SoftAP + captive portal is
   up, waiting for the phone (or the on-device network picker + character-wheel
   password entry) to supply them.
3. **`PH_WIFI_LOST`** — had a connection, lost it; retrying with backoff.
4. **`PH_OAUTH_DISCOVER`** — OAuth discovery + Dynamic Client Registration
   against Orion's MCP server.
5. **`PH_OAUTH_WAIT_CONSENT`** — a QR code is on screen; waiting for the user
   to approve the dial in their phone's browser and for the LAN redirect to
   land.
6. **`PH_MCP_CONNECTING`** — token in hand; opening the MCP session and
   discovering the paired Orion device (`list_devices`).
7. **`PH_READY`** — steady state: the command/poll loop below.
8. **`PH_DEGRADED`** — network is up but Orion calls are failing; retrying
   with backoff, most recent error shown on screen.

Every failure is a phase + backoff, never a dead end — there is no state that
requires a reboot to escape.

## Threading model

Two tasks, deliberately kept from touching each other's data structures
directly:

- **The LVGL task** (`dial_display`, priority 5, core 1) owns the display,
  touch, and the screen router (`dial_ui`: `ui_router` + one `scr_*.c` per
  screen). It renders from `dial_state` snapshots and never blocks on the
  network.
- **The worker task** (`worker_task` in `main.c`, priority 3, core 0 — below
  the LVGL task and on the Wi-Fi/lwIP core, so a TLS handshake can never
  stall the UI) is the single network task: Wi-Fi bring-up, then a
  supervisor loop through OAuth and MCP, then the steady-state
  command/poll loop. It never touches LVGL.

The two meet only through `dial_state`: the worker commits state (bumping a
generation counter), the router's dispatcher timer notices the change and
re-renders. Knob detents flow the other way — the encoder's `esp_timer`
callback only feeds an atomic accumulator that the dispatcher drains into the
active screen; it never calls into the router or LVGL directly, because it
must stay fast enough not to stall `lv_tick_inc`.

In the steady state, the worker drains queued UI commands (coalescing
same-zone writes), and separately polls the device back — gated so a poll can
never land mid-interaction: it waits for a quiet period after the last input,
then reads at a fast cadence right after a write (the bed takes a few seconds
to actually respond) and falls back to an idle cadence otherwise.

## Components

| Component | Role |
|---|---|
| `dial_display` | QSPI panel + touch + LVGL bring-up; owns the LVGL task and lock |
| `dial_knob` | Rotary encoder decoding (`bidi_switch_knob`) |
| `dial_state` | The single state snapshot, UI→worker command queue, NVS-backed prefs |
| `dial_ui` | Screen router + all `scr_*` screens (connecting, Wi-Fi portal/picker/passkey, OAuth QR, dial, menu, settings, standby, quick actions, boost, about, error, welcome, side-pick) |
| `dial_net` | Wi-Fi bring-up, credential storage, SoftAP portal, network scan |
| `dial_oauth` | OAuth 2.1 discovery, Dynamic Client Registration, PKCE authorize/token, refresh |
| `dial_mcp` | Raw MCP-over-HTTP client (JSON-RPC `tools/call`, session id, SSE parsing) |
| `dial_ota` | GitHub Releases version check + `esp_https_ota` download/apply/rollback |
| `dial_haptics` | DRV2605 LRA effects (tick / stop / confirm / error), queued off the hot paths |
| `dial_power` | Idle-driven backlight dimming/standby + "first input after wake is consumed" rule |
| `dial_time` | SNTP + IANA→POSIX timezone resolution, so the clock survives reboots |
| `i2c_bsp`, `lcd_bl_pwm_bsp`, `lcd_touch_bsp` | Low-level board bring-up (I2C bus, backlight PWM, touch controller) shared by the above |

## Where state lives

Everything persists to **NVS**, split by namespace so a factory reset or a
single feature's bug can't corrupt unrelated state:

| NVS namespace | Owner | Holds |
|---|---|---|
| `wifi` | `dial_net` | SSID/password |
| `oauth` | `dial_oauth` | Registered client id, tokens, PKCE verifier |
| `ui` | `dial_state` | Side (zone), °F/°C, haptics on/off, screen rotation |
| `time` | `dial_time` | Resolved POSIX TZ string |

Factory reset clears these namespaces and reboots into `PH_WIFI_PORTAL` as a
fresh device.

## Orion MCP call surface

The worker talks to Orion's MCP server (`dial_mcp_call_tool`) with these
tools: `list_devices` (device discovery on link-up), `get_device_state`
(poll), `set_zone` / `set_zones` (temperature + power writes), `start_thermal_relief`
/ `cancel_thermal_relief` (boost), `get_sleep_schedules`,
`override_sleep_schedule_tonight` / `revert_sleep_schedule_override`, and
`set_away`. See [ORION_MCP.md](ORION_MCP.md) for how these were discovered
and their auth model.

## Updates

`dial_ota` checks `chris023/orion-waveshare-rotary-dial`'s GitHub Releases
once per day (and on demand from the About screen) for a tag matching the
firmware's version scheme, and applies it over the air into the inactive OTA
partition; the bootloader rolls back automatically if the new image fails its
first successful poll.
