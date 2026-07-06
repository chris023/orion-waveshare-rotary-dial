# ST77916 QSPI display driver — port from CO5300

The one part that needs on-hardware work. Good news from the research: it's a
**bounded driver port, not new-pipeline development** — Moddable's **CO5300**
driver (SDK ≥ 8.0.0, `modules/drivers/co5300/`) already implements the exact
QSPI mechanism the ST77916 needs: ESP-IDF `esp_lcd` quad panel-IO, DMA
double-buffering, and the **identical 32-bit opcode framing**
(`cmd = 0x02<<24 | reg<<8`, `color = 0x32<<24 | 0x2C<<8`, `lcd_cmd_bits=32`,
`flags.quad_mode=true`). The ST77916 uses the same `0x02`/`0x32` opcodes and
standard MIPI DCS windowing (CASET/RASET/RAMWR), so the driver logic is reusable
verbatim — you swap the **init sequence** and **dimensions**.

## Steps

1. Copy the reference driver:
   ```bash
   cp -r "$MODDABLE/modules/drivers/co5300" ./ && mv co5300 st77916.src
   # keep the files here: st77916.js (from co5300.js), modST77916.c (from modCo5300.c)
   ```
   Rename the `co5300`/`CO5300` symbols to `st77916`/`ST77916`.

2. Get **this board's exact ST77916 init sequence + pin map** from the Guition
   OEM demo — `pan.jczn1688.com` → *HMI display* → `JC3636K518CN_knob_EN.zip`
   (or https://github.com/modi12jin/IDF-S3_ST77916-QSPI_CST816T-I2C_LVGL). The
   vendor init is a list like `{0xF0,{0x08},1}, {0x9B,{0x51},1}, ...` (~40–60
   register writes, gamma/voltage).

3. Translate that into the driver's `static const uint8_t gInit[]` table form:
   `cmd, count, data..., ` with `0xFF, ms` for delays and `0xFF, 0` to end
   (exactly how `modCo5300.c`'s `gInit[]` works). Set width/height = 360×360 and
   the column/row offsets for the round panel.

4. Keep `esp_lcd_new_panel_io_spi(..., flags.quad_mode = true)`, the `0x02/0x32`
   opcode macros, and the CASET/RASET/RAMWR windowing unchanged. Enable the tear
   line (`0x35`) and even-coordinate clamping if you see tearing (CO5300 already
   does this).

5. Rename `manifest.template.json` → `manifest.json` here and confirm the pin
   `defines` match the board (CS=14, SCK=13, DATA0-3=15/16/17/18, RST=21,
   backlight=47 — from the hub hardware notes; verify against the OEM schematic).

6. Bring up in isolation first: fill the screen a solid color, then draw a
   bitmap, **before** wiring the Piu UI.

## References

- Moddable CO5300 driver: `$MODDABLE/modules/drivers/co5300/`
- Espressif ST77916 component (opcodes/QSPI): esp-bsp / esp_lcd_st77916
- Board hardware notes: `../../../README.md` and the hub's research
