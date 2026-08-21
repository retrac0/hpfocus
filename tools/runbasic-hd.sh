#!/bin/bash
# Boot BASIC from the 520's internal fixed disc -- an installed system,
# rather than the two shipped boot floppies.
#
# A real 520 could carry an internal Winchester, and HP's BUILD_BOOT
# utility installs onto one: it copies the boot from the flexible drive to
# the fixed disc and then adds option binaries into the boot area. The
# emulated media is read-only, so tools/installbasic.py does that job
# offline and this script boots the result. The fixed disc is device 1 of
# select code 7, which then answers identity 5 -- a Winchester -- instead
# of 8, a bare bootstrap loader card.
#
# Build the disc first (both steps are idempotent):
#
#   tools/mkharddisc.py software/hd/basic.img
#   tools/installbasic.py software/hd/basic.img \
#       --basic software/basic/img/500BT1.img software/basic/img/500BT2.img \
#       --option <extracted option binaries...>
#
#   tools/runbasic-hd.sh                 boot the fixed disc
#   tools/runbasic-hd.sh other.img       boot a different one
#   tools/runbasic-hd.sh -- -flop3 x.TD0 also mount a flexible volume

set -e
cd "$(dirname "$0")/.."
DISC="software/hd/basic.img"

if [ -n "${1:-}" ] && [ "${1#-}" = "$1" ]; then
    DISC="$1"
    shift
fi
[ "${1:-}" = "--" ] && shift

if [ ! -f "$DISC" ]; then
    echo "no fixed-disc image at $DISC."
    echo "build one:"
    echo "  tools/mkharddisc.py $DISC"
    echo "  tools/installbasic.py $DISC --basic software/basic/img/500BT1.img \\"
    echo "      software/basic/img/500BT2.img"
    exit 1
fi

if [ ! -x mame/focus ]; then
    echo "mame/focus is not built; run:"
    echo "  cd mame && make SUBTARGET=focus SOURCES=src/mame/hp/hp9000_5xx.cpp -j8"
    exit 1
fi

# An absolute path is used as given; a repo-relative one is resolved
# before the cd, so both forms work.
case "$DISC" in
    /*) ;;
    *)  DISC="$PWD/$DISC" ;;
esac

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
exec ./focus hp9020 -hard1 "$DISC" "${THROTTLE[@]}" -sound none "$@"
