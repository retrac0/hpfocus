#!/usr/bin/env bash
# Extract and disassemble the modules worth reading out of the 500ENV1-3
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
# Two more are here because they are the machine's own specification of
# hardware no manual in the collection documents:
#
#   PWP SELF TEST  3,712 bytes, the smallest segment in the image and the
#                  most useful. Its local entries 17-32 are HP's power-up
#                  self test for the display, and they name every graphics
#                  command, its parameter count and both display memory
#                  sizes -- the whole of HARDWARE.md 2.7.4's command table
#                  was read off this listing.
#   SUN OS KERNEL  the register and DMA primitives every driver above it
#                  calls (entries 17/18 write an FR, 19 reads one, 11/14
#                  run the DMA legs), so a command in a driver's listing
#                  only becomes a register access by way of this one.
#
#   tools/mkdisasm.sh
#
# writes, for each module, disasm/<NAME>.bin (the segment's code, header and
# name stripped) and disasm/<NAME>.raw.dis (tools/focusdis.py's plain
# listing of it, for diffing), and keeps the assembled boot image itself as
# disasm/env.bin -- tools/segdump.py and tools/stt.py both want the whole
# image rather than one extracted segment, and rebuilding it from three TD0
# volumes for every query is a waste.
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
MODULES=(SRM GRAPHICS HP98770 HP97060 HP97062 "PWP SELF TEST" "SUN OS KERNEL")
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

mkdir -p "$OUT"
for v in 500ENV1 500ENV2 500ENV3; do
	python3 tools/td0.py --raw "$WORK/$v.img" "software/test/$v.TD0" >/dev/null
done
python3 tools/bootimage.py "$OUT/env.bin" \
	"$WORK/500ENV1.img" "$WORK/500ENV2.img" "$WORK/500ENV3.img" >/dev/null

python3 tools/segdump.py "$OUT/env.bin" --extract "$OUT" "${MODULES[@]}"
for m in "${MODULES[@]}"; do
	python3 tools/focusdis.py "$OUT/$m.bin" --count 0 > "$OUT/$m.raw.dis"
	if [ -e "$OUT/$m.dis" ]; then
		echo "keeping existing $OUT/$m.dis (annotated)"
	else
		python3 tools/structdis.py "$OUT/$m.bin" > "$OUT/$m.dis"
	fi
done
