# ST77916 QSPI display driver (Moddable)

Brings up the **Guition JC3636K518 / Waveshare ESP32-S3-Knob 1.8" round 360×360
ST77916 QSPI** panel under Moddable. **Working, confirmed on hardware.** If your
unit is instead the **1.85" AMOLED** variant (SH8601, panel `H0185Y040X`), stop —
use Moddable's `co5300` driver directly.

## How it works

This driver **wraps Espressif's real `esp_lcd_st77916` vendor component** rather
than adapting another controller's driver. The vendor code (vendored here as
`esp_lcd_st77916_spi.c` + headers) owns everything panel-specific: the QSPI
opcode framing (`0x02` cmd / `0x32` color), CASET/RASET windowing, COLMOD,
MADCTL, and the init walk. `modST77916.c` just creates the panel and blits each
Poco band with `esp_lcd_panel_draw_bitmap`.

- **Synchronous + buffer-safe:** `on_color_trans_done` gives a semaphore that
  `send()` waits on, and each band is staged through an internal DMA-capable
  bounce buffer (never PSRAM — avoids SPI-DMA cache-coherency corruption).
- **Init:** `st77916_init.h` is the panel-specific **`JC3636W518V2`** sequence
  (from ESPHome `mipi_spi/models/jc.py`). Generic ST77916 inits (modi12jin's,
  Espressif's default) produce a shimmering cross-hatch on this glass — the V2
  power/gamma/gate-timing values are what it needs. Toggle in `modST77916.c`:
  `ST77916_USE_LOCAL_INIT` (1 = this V2 table, 0 = Espressif's built-in default).
- **Backlight is NOT here** — it is a separate GPIO47 line the app drives via
  **LEDC PWM** (`pins/pwm`), not static digital-high. On DC it lights briefly
  then fades off. See `test/display-test/main.js`.

## Install

```bash
source ~/.local/share/xs-dev-export.sh          # ensure $MODDABLE + $IDF_PATH set
bash firmware/dial/drivers/st77916/port.sh
```

`port.sh` (idempotent) copies the authored + vendored driver into
`$MODDABLE/modules/drivers/st77916`, copies esp_lcd's private `interface/`
headers from the live IDF (Moddable's make only puts esp_lcd/`include` on the
compile path, not `interface`), and installs the **orion-knob board target**.

## Light-up test

```bash
cd firmware/dial/test/display-test
UPLOAD_PORT=/dev/cu.usbmodem2101 mcconfig -i -m -p esp32/orion-knob
```

Static **white / red / green / blue** stripes on black, held steady. Serial
prints `display 360 x 360`. (Reading serial without a TTY: use the idf python
that has pyserial at `~/.espressif/python_env/idf6.0_py3.14_env/bin/python`;
idf_monitor needs a TTY and fails in a backgrounded shell.)

## Confirmed-good config (don't re-litigate)

- **Pins** (Waveshare Knob): CS=14, SCK=13, DATA0-3=15/16/17/18, RST=21,
  backlight=47, touch I2C SDA=11/SCL=12. Verified against 3 sources.
- **Byte order** `RGB565BE` + invert `0x21` = correct colors.
- **Clock** 40 MHz. **PSRAM** Octal, 8 MB.

## Files here

- `modST77916.c` — the Moddable/Poco driver (authored).
- `st77916.js` — the PixelsOut class (`async` = false, synchronous).
- `st77916_init.h` — JC3636W518V2 init table (vendor `st77916_lcd_init_cmd_t`
  format) + appended SLPOUT(0x11)/INVON(0x21).
- `esp_lcd_st77916_spi.c`, `esp_lcd_st77916.h`, `esp_lcd_st77916_interface.h` —
  vendored Espressif component (Apache-2.0). `esp_lcd_panel_interface.h` /
  `esp_lcd_panel_io_interface.h` are copied from IDF by `port.sh`, not committed.
- `manifest.json` — driver manifest (360×360, esp_lcd dep).
- `port.sh` — the installer.
