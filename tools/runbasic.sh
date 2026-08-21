#!/bin/bash
# Boot BASIC 2.0 on the emulated HP 9000/520, interactively: a window with
# the 520's display, taking keystrokes as the machine's keyboard.
#
# The boot needs the two BASIC system volumes in the drive, decoded from the
# Teledisk images in software/basic. This script decodes them on first use
# (they are derived files, gitignored) and launches MAME windowed.
#
# The banner "BASIC Ready for use." arrives after ~55 emulated seconds --
# a few minutes of wall clock at current emulation speed. Type at the
# window; Enter executes the line. What works and what does not is tracked
# in docs/IMPLEMENTATION.md and the hpux-boot-frontier notes.
#
#   tools/runbasic.sh              boot from the BASIC system volumes
#   tools/runbasic.sh 500OPT1      leave a different volume in the drive
#                                  after boot (any name from software/basic)

set -e
cd "$(dirname "$0")/.."

IMG=software/basic/img
mkdir -p "$IMG"
for t in 500BT1 500BT2 "${1:-}"; do
    [ -z "$t" ] && continue
    if [ ! -f "$IMG/$t.img" ]; then
        echo "decoding $t.TD0 -> $IMG/$t.img"
        python3 tools/td0.py "software/basic/$t.TD0" --raw "$IMG/$t.img"
    fi
done

FLOPPIES="$IMG/500BT1.img,$IMG/500BT2.img"
if [ -n "${1:-}" ]; then
    FLOPPIES="$FLOPPIES,$IMG/$1.img"
    shift
fi

if [ ! -x mame/focus ]; then
    echo "mame/focus is not built; run:"
    echo "  cd mame && make SUBTARGET=focus SOURCES=src/mame/hp/hp9000_5xx.cpp -j8"
    exit 1
fi

cd mame
exec env FOCUS_DISC=1 FOCUS_FLOPPY="$FLOPPIES" \
    ./focus hp9020 -nothrottle -sound none "$@"
