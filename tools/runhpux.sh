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
# The machine is all backplane: the console serial card is -cio0 and the
# system disc's HP-IB card -cio5, where the CE handbook's installation
# procedure puts them, and the 7914 unit save mounts as the CS/80 drive on
# that card. Re-card either slot, or fit more, with -cio0 .. -cio6.
#
# Where the boot currently gets to: the loader reads the image off the CS/80
# disc, the kernel relocates all 27 modules, types both cards, creates its
# tasks and runs the device world -- SDF_DISC, the TTY subsystem, HP27128/30
# and HP27110 all execute, and the HP-IB card gets its documented go-online
# / send-IFC / set-REN bring-up.
#
# THE MACHINE PRINTS TO ITS CONSOLE. What it prints is a panic report --
# the console driver's own open still times out -- but the whole write path
# works: CLC order, request block, the card-configuration subfunctions, and
# 80-column write-device-data, with the message, the register file, the
# stack, the I/O state and the segment table all reaching the terminal.
# The open fails because the emulated card answers Read Transparent Status
# with "nothing to do" and the driver keeps polling. See HARDWARE.md 2.6.3.

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

# Video backend. MAME defaults to opengl, and a driver that cannot present
# through it leaves a window that renders the blinking cursor and never
# updates the text -- the machine running perfectly behind a screen that
# looks dead. This display is 640x390 of character cells, so the software
# renderer costs nothing and always works. A -video on the command line
# comes after this one and wins, so any backend can still be asked for.
# A headless run has no business opening a window. MAME initialises SDL's
# video subsystem even for -video none, which costs real time on a loaded
# desktop and, on this machine, intermittently leaves the emulated boot
# dead. SDL_VIDEODRIVER=dummy gives SDL a backend that draws nowhere.
case " $* " in
    *" -video none "*) export SDL_VIDEODRIVER=dummy ;;
esac

VIDEO=(-video soft)

# RAM. The machine default is HP's 1 MB base configuration, but the
# HP-UX image is 597 KB and its relocated world reaches past 0xb2000:
# together they cannot fit under 1 MB, so this boot pins the two 1 MB
# boards it was brought up and measured on. Fit different boards by
# editing here; a second -mpb3 on the command line is a MAME error.
exec env FOCUS_SCMNV="$CCT/scm.nv" \
    ./focus hp9040 -mpb3 1m -mpb4 1m \
    -hard1 "$DISC" "${THROTTLE[@]}" "${VIDEO[@]}" -sound none "$@"
