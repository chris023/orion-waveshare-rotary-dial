# firmware/

- **[`dial-idf/`](dial-idf/)** — the firmware. Native ESP-IDF (C) for the
  Waveshare ESP32-S3-Knob-Touch-LCD-1.8 dial: Wi-Fi, OAuth 2.1, and MCP-over-HTTPS
  all on-device. See [`dial-idf/README.md`](dial-idf/README.md) to build and flash.
- **[`backups/`](backups/)** — local-only flash dumps of specific physical
  units (companion-chip factory image, etc.), for restoring a unit to its
  out-of-box state. The `.bin` files are gitignored; only the README here is
  tracked. Not needed to build or run the product.

## Board facts

| Part | Detail |
|------|--------|
| Board | **Waveshare ESP32-S3-Knob-Touch-LCD-1.8** (Guition JC3636K518) |
| MCU | ESP32-S3R8 (8 MB PSRAM, 16 MB flash) + companion ESP32-U4WDH |
| Display | 1.8" **round** 360×360 panel over **QSPI**, driven by the **SH8601** command set (`esp_lcd_sh8601`), LVGL |
| Display pins | CS=14, CLK=13, DATA=15/16/17/18, RST=21, backlight (PWM)=47 |
| Rotary encoder | incremental A/B quadrature: **pin_a=GPIO8, pin_b=GPIO7** (not magnetic; only live under full board init) |
| Knob click | **none** — GPIO0 is the boot button; use a touchscreen **tap** instead |
| Touch | **CST816** over I²C: SDA=11, SCL=12, INT=9, RST=10 |
| Haptics | DRV2605 LRA driver @ I²C 0x5A |
| Audio | PCM5100A I²S DAC + MEMS mic (not used by this firmware) |

The current firmware ([`dial-idf`](dial-idf/)) is built on Waveshare's own
`ESP32-S3-Knob-Touch-LCD-1.8` ESP-IDF demo, whose display bring-up uses
Espressif's `esp_lcd_sh8601` component, and it boots and renders on real
hardware. An earlier prototype (the abandoned [Moddable
attempt](../archive/moddable-dial), see its
[`drivers/st77916/README.md`](../archive/moddable-dial/drivers/st77916/README.md))
was written against a different assumption — that this exact board used an
**ST77916** controller, reserving SH8601 for a separate 1.85" AMOLED variant.
That assumption predates having Waveshare's own demo in hand; the panel this
firmware ships against is confirmed driven via the SH8601 command set, not
ST77916. If you're bringing up a unit and LVGL renders garbage, that's the
first thing to double check against your specific board revision.

## References

- Waveshare wiki: https://www.waveshare.com/wiki/ESP32-S3-Knob-Touch-LCD-1.8
- Board notes / esptool dumps: https://github.com/nkinnan/Waveshare-ESP32-S3-Knob-Touch-LCD-1.8_and_Guition-K5-Knob-Series-JC3636K518
