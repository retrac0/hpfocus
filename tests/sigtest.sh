#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Joel Heikkila
#
# Does a signal to tools/runhpux.sh stop the emulator cleanly?
#
#   tests/sigtest.sh
#
# A headless MAME takes no notice of SIGINT or SIGTERM -- its SDL front end
# turns them into a quit event nothing pumps -- so runhpux.sh traps the
# signal itself and makes the stop file tools/mame/stopfile.lua watches for,
# and MAME exits the way -seconds_to_run takes: every exit notifier runs,
# so the battery and the disc are written as a clean exit writes them.
#
# The case: a single-user 540 on a throwaway copy of the pristine machine
# (--state, in a temporary directory under var/), the console on TCP, so
# MAME runs headless and throttled. Once the terminal hub is up and the
# machine has run for a few seconds, ONE SIGTERM goes to the script, which
# must then end by itself within 30 s with status 0, with MAME gone, the
# hub gone, the lock and the stop file removed, and the battery written at
# the stop. (SIGTERM rather than ^C's SIGINT: a background job of a
# non-interactive shell starts with SIGINT ignored, and a trap cannot catch
# what was ignored on entry. A ^C at a terminal runs the same trap.)
#
# Exit status 0 if it passed, 1 if it failed.

set -uo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)
VAR=${HPFOCUS_VAR:-$ROOT/var}
mkdir -p "$VAR"
M=$(mktemp -d "$VAR/sigtest.XXXXXX")
LOG=$M/runhpux.log
fail() { echo "FAIL: $*"; tail -5 "$LOG" 2>/dev/null | sed 's/^/  | /'; exit 1; }
trap 'rm -rf "$M"' EXIT

tools/runhpux.sh --prepare >/dev/null || fail "tools/runhpux.sh --prepare"
setsid tools/runhpux.sh --state "$M" --console tcp --port-base 4600 < /dev/null > "$LOG" 2>&1 &
SCRIPT=$!
for _ in $(seq 60); do
	[ -f "$M/hub.ready" ] && break
	kill -0 "$SCRIPT" 2>/dev/null || fail "runhpux.sh ended before its hub was up"
	sleep 0.5
done
[ -f "$M/hub.ready" ] || fail "the terminal hub did not come up"
sleep 8
MAME=$(pgrep -f -- "-nvram_directory $M/nvram" | head -1)
[ -n "$MAME" ] || fail "no MAME running on $M"
before=$(date +%s)
sleep 1

kill -TERM "$SCRIPT"
for i in $(seq 30); do
	kill -0 "$SCRIPT" 2>/dev/null || break
	sleep 1
done
kill -0 "$SCRIPT" 2>/dev/null && { kill -KILL "$MAME" "$SCRIPT" 2>/dev/null; fail "still running 30 s after SIGTERM"; }
wait "$SCRIPT"; rc=$?
[ "$rc" = 0 ] || fail "runhpux.sh exited $rc"
kill -0 "$MAME" 2>/dev/null && { kill -KILL "$MAME"; fail "MAME outlived the script"; }
pgrep -f -- "--ready $M/hub.ready" >/dev/null && fail "the terminal hub outlived the script"
grep -q 'stop requested; exiting cleanly' "$LOG" || fail "MAME did not stop through the stop file"
for f in run.pid stop hub.ready; do [ -e "$M/$f" ] && fail "$f was left behind"; done
nv=$M/nvram/hp9k540/scm_rtc
[ -f "$nv" ] || fail "no battery was written"
[ "$(date -r "$nv" +%s)" -ge "$before" ] || fail "the battery was not written at the stop"
echo "PASS: SIGTERM stopped the run cleanly in ${i}s; the battery was written as it ended"
