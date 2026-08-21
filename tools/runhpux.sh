#!/bin/bash
# Boot HP-UX 5.30 on the emulated HP 9000/540: an ASI terminal window for
# the console, and the 7914 unit-save image mounted as the CS/80 system
# disc on the HP-IB card, booted through the emulated loader.
#
# The RTC and configuration NVM persist in software/cct/scm.nv, standing
# for the SCM's battery: the FIRST boot finds an unset clock, sets it and
# writes a default configuration; later boots find them valid, which HP-UX
# requires before it will bring up its device world. Delete the file to
# simulate a dead battery.
#
# Where the boot currently gets to is tracked in the hpux-boot-frontier
# notes: a trap-free multi-tasking idle, no banner yet.

set -e
cd "$(dirname "$0")/.."
CCT="$PWD/software/cct"
DISC="$CCT/7914_unitSave_hpux4.0.bin"

if [ ! -f "$DISC" ]; then
    echo "decompressing the 7914 unit save..."
    gunzip -k "$DISC.gz"
fi

if [ ! -x mame/focus ]; then
    echo "mame/focus is not built; run:"
    echo "  cd mame && make SUBTARGET=focus SOURCES=src/mame/hp/hp9000_5xx.cpp -j8"
    exit 1
fi

cd mame
# Throttling. A batch run wants to go as fast as it can; an interactive
# one wants the window to keep up. Unthrottled, a machine that idles in
# STOP races its emulated clock to thousands of percent, and MAME spends
# so long emulating between presentations that the window never draws and
# the event pump never sees a close or a signal -- a blank window you have
# to kill by hand. So -nothrottle is added only for a run that ends by
# itself, which is what -seconds_to_run means.
THROTTLE=()
for arg in "$@"; do
    case "$arg" in
        -seconds_to_run|-str) THROTTLE=(-nothrottle); break ;;
    esac
done

exec env FOCUS_SCMNV="$CCT/scm.nv" FOCUS_ASIID=0 \
    ./focus hp9040 -hard1 "$DISC" "${THROTTLE[@]}" -sound none "$@"
