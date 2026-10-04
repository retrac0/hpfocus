#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Joel Heikkila
#
# Boot BASIC from the 520's internal fixed disc -- an installed system,
# rather than HP's two boot floppies.
#
# A real 520 could carry an internal Winchester, and HP's BUILD_BOOT
# utility installs onto one: it copies the boot from the flexible drive to
# the fixed disc and then adds option binaries into the boot area. The
# emulated media is read-only, so tools/installbasic.py does that job
# offline and this script boots the result. The fixed disc is device 1 of
# select code 7, which then answers identity 5 -- a Winchester -- instead
# of 8, a bare bootstrap loader card.
#
# Build the disc first. tools/setup.sh does it (software/hd is gitignored,
# so a fresh clone has none): BASIC 2.0 with the ERRORS_ENG, IO and MS
# options and the CAT_BOOT and BUILD_BOOT utilities. By hand:
#
#   mkdir -p software/hd
#   python3 tools/td0.py --raw /tmp/500OPT1.img software/basic/500OPT1.TD0
#   python3 tools/sdfread.py --extract /tmp/opt1 /tmp/500OPT1.img
#   tools/mkharddisc.py software/hd/basic.img
#   tools/installbasic.py software/hd/basic.img \
#       --basic software/basic/500BT1.TD0 software/basic/500BT2.TD0 \
#       --option /tmp/opt1/ERRORS_ENG /tmp/opt1/IO /tmp/opt1/MS \
#       --copy software/basic/500OPT1.TD0:CAT_BOOT,BUILD_BOOT
#
# Without --option the disc is plain BASIC: it boots, CATs and STOREs, but
# errors are bare numbers and CREATE is an IMPROPER COMMAND.
#
# A disc built before sdfwrite.py's free-map fix (0ec4035) boots, but BASIC
# cannot CREATE or STORE on it (ERROR 455): this script says so when it is
# given one. Delete it and build it again.
#
# The keys are tools/runbasic.sh's: Return is ENTER (store a program line),
# Insert or Ctrl-J is EXECUTE (run a command).
#
#   tools/runbasic-hd.sh                 boot the fixed disc
#   tools/runbasic-hd.sh other.img       boot a different one
#   tools/runbasic-hd.sh -- -flop3 x.TD0 also mount a flexible volume

set -e
source "$(dirname "${BASH_SOURCE[0]}")/lib/focus.sh"
focus_repo
DISC="software/hd/basic.img"

if [ -n "${1:-}" ] && [ "${1#-}" = "$1" ]; then
    DISC="$1"
    shift
fi
[ "${1:-}" = "--" ] && shift

if [ ! -f "$DISC" ]; then
    echo "no fixed-disc image at $DISC."
    echo "tools/setup.sh builds software/hd/basic.img; the header of this script"
    echo "has the recipe for building one by hand. Either needs the BASIC floppies,"
    echo "which come from the HP Computer Museum by hand: tools/fetch-media.sh --list,"
    echo "then tools/fetch-media.sh --from <what you downloaded>."
    exit 1
fi

focus_built
focus_basic_disc_warn "$DISC"

# An absolute path is used as given; a repo-relative one is resolved
# before the cd into the MAME directory, so both forms work.
case "$DISC" in
    /*) ;;
    *)  DISC="$FOCUS_REPO/$DISC" ;;
esac

focus_run hp9020 -hard1 "$DISC" "$@"
