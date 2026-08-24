#!/bin/bash
# Boot BASIC 2.0 on the emulated HP 9000/520, interactively: a window with
# the 520's display, taking keystrokes as the machine's keyboard.
#
# The discs mount as MAME media -- the Teledisk images straight from
# software/basic, decoded by the driver's own media device -- and the
# machine boots from them through the emulated loader, exactly as a real
# 520 would from its internal drive. Swap discs at runtime through MAME's
# file manager (Tab menu).
#
# The banner "BASIC Ready for use." arrives after ~55 emulated seconds --
# a few minutes of wall clock at current emulation speed. Type at the
# window; Enter executes the line. What works and what does not is tracked
# in docs/IMPLEMENTATION.md and the hpux-boot-frontier notes.
#
#   tools/runbasic.sh              boot the BASIC system volumes
#   tools/runbasic.sh 500OPT1      also mount a third volume for after boot

set -e
cd "$(dirname "$0")/.."
BASIC="$PWD/software/basic"

if [ ! -x mame/focus ]; then
    echo "mame/focus is not built; run:"
    echo "  cd mame && make SUBTARGET=focus SOURCES=src/mame/hp/hp9000_5xx.cpp -j8"
    exit 1
fi

EXTRA=()
if [ -n "${1:-}" ] && [ "${1#-}" = "$1" ]; then
    EXTRA=(-flop3 "$BASIC/$1.TD0")
    shift
fi

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

cd mame
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

exec ./focus hp9020 -flop1 "$BASIC/500BT1.TD0" -flop2 "$BASIC/500BT2.TD0" \
    "${EXTRA[@]}" "${THROTTLE[@]}" "${VIDEO[@]}" -sound none "$@"
