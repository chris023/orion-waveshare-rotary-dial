# Firmware backups

- **`companion-factory-4MB.bin`** — full 4 MB flash read-back of the companion
  ESP32-U4WDH's **factory firmware**, captured before we reflashed it with our
  knob-streamer. This is the exact per-unit state (bootloader + partitions +
  app + NVS/calibration).

## Restore the companion to factory

Flip the USB-C cable so the companion enumerates (e.g. `/dev/cu.usbserial-210`),
then:

```bash
source ~/.local/share/xs-dev-export.sh && source "$IDF_PATH/export.sh"
python -m esptool --chip esp32 --port /dev/cu.usbserial-210 -b 115200 \
  write-flash 0x0 firmware/backups/companion-factory-4MB.bin
```

(Use 115200 — this bridge is unreliable at higher baud. Waveshare's generic
companion app image is also public in their demo BIN zip as
`ESP32-KNOB_ESP32_0.bin`, but that lacks this unit's NVS/calibration.)
