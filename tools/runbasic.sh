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
# MAME flags the 520 as not working, so the window opens on its red
# warning screen: press any key and the machine starts. The banner "BASIC
# Ready for use." arrives after ~55 emulated seconds. Type at the window.
# Return is the 520's ENTER key, which stores a numbered program line;
# EXECUTE, which runs a command (RUN, LIST, DISP 7), is Insert or Ctrl-J.
# A command ended with Return answers IMPROPER LINE NUMBER, Esc clears the
# input line, and a typed program needs its END. INSTALL.md section 8 and KNOWN-ISSUES.md have the
# rest.
#
#   tools/runbasic.sh              boot the BASIC system volumes
#   tools/runbasic.sh 500OPT1      also mount a third volume for after boot

set -e
source "$(dirname "${BASH_SOURCE[0]}")/lib/focus.sh"
focus_repo
focus_built
BASIC="$FOCUS_REPO/software/basic"

EXTRA=()
if [ -n "${1:-}" ] && [ "${1#-}" = "$1" ]; then
    EXTRA=(-flop3 "$BASIC/$1.TD0")
    shift
fi

focus_run hp9020 -flop1 "$BASIC/500BT1.TD0" -flop2 "$BASIC/500BT2.TD0" \
    "${EXTRA[@]}" "$@"
