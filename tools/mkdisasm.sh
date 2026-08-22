#!/usr/bin/env bash
# Extract and disassemble the diagnostics-only modules from the 500ENV1-3
# boot image into disasm/.
#
# 500ENV1-3 (see tools/mkdiagdisc.sh) assemble into a 29-segment BASIC boot
# image. Thirteen of those segments are the ordinary BASIC 2.0 boot and
# eleven more are option/device-driver modules shared with it; the five that
# are unique to the diagnostics environment -- because Clear_screen and the
# rest of the 9020 System Functional Test driver need them and no shipped
# option disc carries them -- are SRM, GRAPHICS, HP98770, HP97060 and
# HP97062 (docs/DIAGNOSTICS.md section 1).
#
#   tools/mkdisasm.sh
#
# writes, for each of those five, disasm/<NAME>.bin (the segment's code,
# header and name stripped) and disasm/<NAME>.raw.dis (tools/focusdis.py's
# plain listing of it, for diffing).
#
# disasm/<NAME>.dis is NOT one of those -- it is the annotated listing
# tools/structdis.py produces, which this script deliberately does not
# regenerate once it exists. Extraction is mechanical and reproducible;
# .dis may carry structural comments (and, eventually, hand corrections)
# worth keeping. Delete a .dis yourself and re-run to get a fresh one, or
# run `python3 tools/structdis.py disasm/<NAME>.bin > disasm/<NAME>.dis`
# directly.
set -euo pipefail
cd "$(dirname "$0")/.."
OUT=disasm
MODULES=(SRM GRAPHICS HP98770 HP97060 HP97062)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

for v in 500ENV1 500ENV2 500ENV3; do
	python3 tools/td0.py --raw "$WORK/$v.img" "software/test/$v.TD0" >/dev/null
done
python3 tools/bootimage.py "$WORK/env.bin" \
	"$WORK/500ENV1.img" "$WORK/500ENV2.img" "$WORK/500ENV3.img" >/dev/null

mkdir -p "$OUT"
python3 tools/segdump.py "$WORK/env.bin" --extract "$OUT" "${MODULES[@]}"
for m in "${MODULES[@]}"; do
	python3 tools/focusdis.py "$OUT/$m.bin" --count 0 > "$OUT/$m.raw.dis"
	if [ -e "$OUT/$m.dis" ]; then
		echo "keeping existing $OUT/$m.dis (annotated)"
	else
		python3 tools/structdis.py "$OUT/$m.bin" > "$OUT/$m.dis"
	fi
done
