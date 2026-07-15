#!/usr/bin/env bash
#
# Install the ST77916 Moddable driver + the orion-knob board target into your
# Moddable SDK. The driver wraps Espressif's real esp_lcd_st77916 vendor
# component (vendored here as esp_lcd_st77916_spi.c + headers), so there is no
# code generation/patching — this just copies the authored + vendored sources
# into place. Idempotent: overwrites the installed driver/target each run.
#
# Requires: $MODDABLE set (run `source ~/.local/share/xs-dev-export.sh` first).
#
# Usage:  bash firmware/dial/drivers/st77916/port.sh
#
set -euo pipefail

: "${MODDABLE:?MODDABLE is not set — source your xs-dev export first}"
: "${IDF_PATH:?IDF_PATH is not set — source your xs-dev export first}"

HERE="$(cd "$(dirname "$0")" && pwd)"
FW="$(cd "$HERE/../.." && pwd)"                       # firmware/dial
DST_DRV="$MODDABLE/modules/drivers/st77916"
DST_TGT="$MODDABLE/build/devices/esp32/targets/orion-knob"

# Driver source files to install (authored + vendored esp_lcd_st77916 component).
DRIVER_FILES=(
	modST77916.c
	st77916.js
	st77916_init.h
	manifest.json
	esp_lcd_st77916_spi.c
	esp_lcd_st77916.h
	esp_lcd_st77916_interface.h
)

echo "==> Installing ST77916 driver -> $DST_DRV"
rm -rf "$DST_DRV"; mkdir -p "$DST_DRV"
for f in "${DRIVER_FILES[@]}"; do
	[ -f "$HERE/$f" ] || { echo "ERROR: missing $HERE/$f"; exit 1; }
	cp "$HERE/$f" "$DST_DRV/$f"
done

# The vendored esp_lcd_st77916_spi.c implements esp_lcd_panel_t, so it needs
# esp_lcd's private "interface" headers. Moddable's make.esp32.mk only puts
# esp_lcd/include on the compile path (not esp_lcd/interface), and the driver
# dir IS on the include path — so copy those two headers in from the live IDF
# (version-matched to whatever will link; kept out of the repo).
IFACE="$IDF_PATH/components/esp_lcd/interface"
for h in esp_lcd_panel_interface.h esp_lcd_panel_io_interface.h; do
	[ -f "$IFACE/$h" ] || { echo "ERROR: missing $IFACE/$h (IDF layout changed?)"; exit 1; }
	cp "$IFACE/$h" "$DST_DRV/$h"
done

echo "==> Installing board target -> $DST_TGT"
rm -rf "$DST_TGT"; mkdir -p "$DST_TGT"
cp -R "$FW/target/orion-knob/." "$DST_TGT/"

echo
echo "Done."
echo "  Driver -> $DST_DRV"
echo "  Target -> $DST_TGT   (build with: -p esp32/orion-knob)"
echo
echo "Next: light-up test ->"
echo "  cd $FW/test/display-test && UPLOAD_PORT=/dev/cu.usbmodem2101 mcconfig -i -m -p esp32/orion-knob"
