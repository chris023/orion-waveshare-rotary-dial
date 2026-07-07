#!/usr/bin/env bash
#
# Generate the ST77916 Moddable driver from the SDK's CO5300 driver, and install
# it + the orion-knob board target into your Moddable SDK. Idempotent-ish: it
# overwrites the generated driver/target each run.
#
# Requires: $MODDABLE set (run `source ~/.local/share/xs-dev-export.sh` first).
#
# Usage:  bash firmware/dial/drivers/st77916/port.sh
#
set -euo pipefail

: "${MODDABLE:?MODDABLE is not set — source your xs-dev export first}"

HERE="$(cd "$(dirname "$0")" && pwd)"
FW="$(cd "$HERE/../.." && pwd)"                       # firmware/dial
SRC_DRV="$MODDABLE/modules/drivers/co5300"
DST_DRV="$MODDABLE/modules/drivers/st77916"
DST_TGT="$MODDABLE/build/devices/esp32/targets/orion-knob"

[ -d "$SRC_DRV" ] || { echo "ERROR: $SRC_DRV not found — need Moddable SDK >= 8.0.0"; exit 1; }

echo "==> Generating ST77916 driver from CO5300"
rm -rf "$DST_DRV"; mkdir -p "$DST_DRV"
cp "$SRC_DRV/modCo5300.c" "$DST_DRV/modST77916.c"

# Rename symbols (case-sensitive covers CO5300_/co5300/xs_co5300/MODDEF_CO5300).
sed -i '' -e 's/CO5300/ST77916/g' -e 's/co5300/st77916/g' "$DST_DRV/modST77916.c"

# Our authored pieces (correct dims/pins/init) overwrite the renamed ones.
cp "$HERE/st77916_init.h" "$DST_DRV/st77916_init.h"
cp "$HERE/manifest.json"  "$DST_DRV/manifest.json"
cp "$SRC_DRV/co5300.js"   "$DST_DRV/st77916.js"
sed -i '' -e 's/CO5300/ST77916/g' -e 's/co5300/st77916/g' "$DST_DRV/st77916.js"

echo "==> Patching modST77916.c (init table + walker)"
python3 - "$DST_DRV/modST77916.c" <<'PY'
import re, sys
p = sys.argv[1]
s = open(p).read()

# 1) Replace the inline gInit[] array with an include of our init table.
s, n1 = re.subn(r'static const uint8_t gInit\[\].*?;',
                '#include "st77916_init.h"', s, count=1, flags=re.S)

# 2) Replace the CO5300 init walker (0xFF-sentinel) with a length-prefixed one
#    that supports a real 0xFF register write and per-command delays.
new_walk = (
    "\tcmds = gInit;\n"
    "\twhile (true) {\n"
    "\t\tuint8_t cmd = c_read8(cmds++);\n"
    "\t\tuint8_t count = c_read8(cmds++);\n"
    "\t\tif (0xFF == count)\n"
    "\t\t\tbreak;\n"
    "\t\tst77916Command(sd, ST77916_CMD(cmd), cmds, count);\n"
    "\t\tcmds += count;\n"
    "\t\tuint8_t ms = c_read8(cmds++);\n"
    "\t\tif (ms)\n"
    "\t\t\tmodDelayMilliseconds(ms);\n"
    "\t}"
)
s, n2 = re.subn(r'cmds = gInit;.*?\n\t\}', new_walk, s, count=1, flags=re.S)

if n1 != 1 or n2 != 1:
    sys.exit(f"PATCH FAILED (init={n1}, walker={n2}) — inspect {p} and edit by hand")
open(p, "w").write(s)
print("   patched OK")
PY

echo "==> Installing board target: $DST_TGT"
rm -rf "$DST_TGT"; mkdir -p "$DST_TGT"
cp -R "$FW/target/orion-knob/." "$DST_TGT/"

echo
echo "Done. Driver -> $DST_DRV"
echo "      Target -> $DST_TGT   (build with: -p esp32/orion-knob)"
echo
echo "Next: light-up test ->"
echo "  cd $FW/test/display-test && UPLOAD_PORT=/dev/cu.usbmodem2101 mcconfig -d -m -p esp32/orion-knob"
