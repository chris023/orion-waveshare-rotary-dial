# ST77916 QSPI display driver (Moddable)

Brings up the **Guition JC3636K518 / Waveshare ESP32-S3-Knob 1.8" round 360×360
ST77916 QSPI** panel under Moddable. Confirmed controller: **ST77916** (Guition's
official K518 demo sets `ESP_PANEL_BOARD_LCD_CONTROLLER ST77916`; the K518
ESPHome config agrees). If your unit is instead the **1.85" AMOLED** variant
(SH8601, panel `H0185Y040X`), stop — use Moddable's `co5300` driver directly, it
already is an SH8601-class AMOLED driver.

Rather than hand-write a QSPI DMA driver, this **generates** one from Moddable's
CO5300 driver (identical `esp_lcd` quad pipeline + `0x02`/`0x32` opcode framing)
and swaps in this panel's init sequence.

## One command

```bash
source ~/.local/share/xs-dev-export.sh          # ensure $MODDABLE is set
bash firmware/dial/drivers/st77916/port.sh
```

`port.sh` (idempotent) copies `$MODDABLE/modules/drivers/co5300` →
`.../st77916`, renames symbols, injects `st77916_init.h`, swaps the init walker,
and installs the **orion-knob board target** into
`$MODDABLE/build/devices/esp32/targets/orion-knob`.

## Then: light-up test

```bash
cd firmware/dial/test/display-test
UPLOAD_PORT=/dev/cu.usbmodem2101 mcconfig -d -m -p esp32/orion-knob
```

The round screen should cycle **RED → GREEN → BLUE → WHITE** once/sec, and xsbug
prints `display 360 x 360`.

## Reading the result

| You see | Meaning / fix |
|---|---|
| Cycling colors, correct | 🎉 done — build the full firmware with `-p esp32/orion-knob` |
| Black screen | init/pins/QSPI issue — recheck the target `defines` pins + that PSRAM enabled |
| RED shows as BLUE (R/B swapped) | set MADCTL `0x36` to `0x08` in `st77916_init.h`, or flip target `config.format` `RGB565BE`↔`RGB565LE`, re-run `port.sh` |
| Torn / shifted image | adjust `column_offset` / `row_offset` in `target/orion-knob/manifest.json` |
| Garbage / flicker | lower `hz` (e.g. 20 MHz) in the target `defines` |

## Files here

- `st77916_init.h` — this panel's init table (ported from the vendor ESP-IDF
  sequence: modi12jin/IDF-S3_ST77916-QSPI_CST816T-I2C_LVGL), in the length-
  prefixed `cmd,count,data...,delayMs` format (needed because ST77916 has a real
  `0xFF` register + per-command delays that CO5300's `0xFF`-sentinel format can't
  express). Added: `0x3A`=RGB565, `0x36`=MADCTL. `0x21` (invert) is required.
- `manifest.json` — driver manifest (360×360, offsets 0, esp_lcd dep).
- `port.sh` — the generator/installer.

The generated `modST77916.c` / `st77916.js` live in the SDK, not the repo.
