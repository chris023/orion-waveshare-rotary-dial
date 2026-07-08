# orion-dial (ESP-IDF firmware)

Native **ESP-IDF (C)** firmware for the Waveshare ESP32-S3-Knob-Touch-LCD-1.8
dial. This is the primary firmware direction: the Moddable/TypeScript attempt
(`../dial/`) brought up the display but could not drive the touch or knob
inputs, whereas all three work in Espressif's BSP — so the dial is built here in
C ("everything on the dial": Wi-Fi + OAuth 2.1 + MCP + Orion control on-device).

## Provenance

The hardware bring-up (`components/`, `main/main.c`, `partitions.csv`,
`user_config.h`) is derived from Waveshare's official
`ESP32-S3-Knob-Touch-LCD-1.8` ESP-IDF demo (`08_LVGL_Test` for display + touch +
LVGL, `04_Encoder_Test` for the knob). Modifications for ESP-IDF **v6.0**:

- Flash set to **16 MB** (the demo shipped 8 MB; the board is 16 MB, and its 8 M
  `factory` partition overflowed 8 MB).
- BSP component `CMakeLists.txt` files migrated off the removed catch-all
  `driver` component to the specific `esp_driver_i2c` / `esp_driver_gpio` /
  `esp_driver_ledc` / `esp_timer` components.
- Encoder pins (`EXAMPLE_ENCODER_ECA_PIN=8`, `ECB_PIN=7`) added to
  `main/user_config.h`.

The display uses the managed `esp_lcd_sh8601` QSPI driver (Waveshare drives this
panel via the SH8601 driver + a custom init sequence in `main.c`); touch is the
CST816 on I2C (SDA=11/SCL=12); the knob is `iot_knob` on GPIO8/7.

## Build & flash

```bash
source ~/.local/share/xs-dev-export.sh        # sets IDF_PATH
source "$IDF_PATH/export.sh"                   # idf.py on PATH (v6.0)
idf.py set-target esp32s3                      # first time only
idf.py build
python -m esptool --chip esp32s3 -p /dev/cu.usbmodem2101 -b 921600 \
  --before default-reset --after hard-reset write-flash \
  --flash-mode dio --flash-size 16MB --flash-freq 80m \
  0x0 build/bootloader/bootloader.bin \
  0x8000 build/partition_table/partition-table.bin \
  0x10000 build/orion-dial.bin
```

`build/`, `managed_components/`, and `sdkconfig` are generated (git-ignored);
`idf.py` recreates them.

## Status

Foundation: **builds + flashes + boots**; SH8601 display + LVGL running,
CST816 touch + knob wired. Next: strip the demo UI, then Wi-Fi → OAuth 2.1
(PKCE, NVS token store) → MCP-over-HTTPS → Orion tools → dial UI. The proven
control logic lives in `../reference-dial/` and `../dial/src/*.ts` (reference
for the C port).
