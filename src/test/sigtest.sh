#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Joel Heikkila
#
# Does a host signal stop the emulator cleanly, whatever it is doing?
#
#   src/test/sigtest.sh [binary]      (default: $HPFOCUS_BIN, as for the run
#                                     scripts, else mame/focus of this tree)
#
# A 540 with no disc is started headless (nothing is written: FOCUS_HDNOSAVE,
# a battery file in a temporary directory, and MAME run in that directory so
# its cfg/ and nvram/ go with it), and each case sends ONE signal and
# expects the run to end by itself, exit status 0, with the driver's
# "signal N: stopping the run" -- the exit -seconds_to_run takes, with its
# exit notifiers (batteries, discs). A run still alive after the signal is a
# failure: its only way out is a second signal, the default action, which
# skips the exit.
#
# The machine runs unthrottled and has nothing to do, so on an idle host it
# covers a hundred emulated seconds in each real one. It is therefore given
# 100,000 emulated seconds to live (its end if this script is killed), and
# a run that has ended before its signal was sent is reported as that --
# the test could not be made -- and not as the emulator ignoring a signal.
#
#   running   SIGTERM to a machine that is running
#   paused    SIGINT to a machine paused the way F5 or MAME's menu pauses it
#             (emu.pause from a Lua script): emulated time stands still, so
#             a check made only on an emulated tick never sees the signal
#
# Exit status 0 only if every case passed; 1 if a case failed; 2 if the
# test could not be made (no emulator, or a run that ended early).

set -u
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd "$here/../.." && pwd)
bin=${1:-${HPFOCUS_BIN:-$repo/mame/focus}}
[ -x "$bin" ] || { echo "sigtest: no emulator at $bin (tools/setup.sh builds mame/focus)" >&2; exit 2; }
bin=$(realpath "$bin")
work=$(mktemp -d "${TMPDIR:-/tmp}/sigtest.XXXXXX")
trap 'rm -rf "$work"' EXIT

cat > "$work/req.lua" <<'EOF'
local path = os.getenv("SIGTEST_REQ")
emu.register_periodic(function()
	local f = io.open(path, "r")
	if not f then return end
	local what = f:read("*l") or ""
	f:close(); os.remove(path)
	if what == "pause" then emu.pause(); print("[sigtest: paused]") end
end)
EOF

failed=""
unmade=""
one() {     # one <name> <signal> <pause: 0|1>
	local name=$1 sig=$2 pause=$3 d="$work/$1"
	mkdir -p "$d"
	(cd "$d" && SIGTEST_REQ="$d/req" SDL_VIDEODRIVER=dummy \
		FOCUS_SCMNV="$d/scm.nv" FOCUS_HDNOSAVE=1 \
		exec "$bin" hp9040 -nothrottle -video none -sound none \
			-autoboot_script "$work/req.lua" -str 100000) > "$d/out.log" 2>&1 &
	local pid=$! i
	sleep 3
	if [ "$pause" = 1 ]; then
		# Renamed into place: the Lua script polls many times a second and
		# deletes what it reads, so a file it caught between its creation
		# and its first write would be an empty request, lost.
		echo pause > "$d/req.new" && mv "$d/req.new" "$d/req"
		for i in $(seq 1 50); do grep -q "sigtest: paused" "$d/out.log" && break; sleep 0.2; done
		if ! grep -q "sigtest: paused" "$d/out.log" && kill -0 "$pid" 2>/dev/null; then
			kill -9 "$pid"; wait "$pid" 2>/dev/null
			echo "  ???? $name: the machine never paused (no answer from the Lua script in 10 s)"
			unmade="$unmade $name"; return
		fi
	fi
	if ! kill -"$sig" "$pid" 2>/dev/null; then
		wait "$pid"
		echo "  ???? $name: the run had ended (exit $?) before SIG$sig was sent: $(tail -1 "$d/out.log")"
		unmade="$unmade $name"; return
	fi
	for i in $(seq 1 50); do kill -0 "$pid" 2>/dev/null || break; sleep 0.2; done
	if kill -0 "$pid" 2>/dev/null; then
		kill -9 "$pid"; wait "$pid" 2>/dev/null
		echo "  FAIL $name: still running 10 s after SIG$sig"; failed="$failed $name"; return
	fi
	wait "$pid"; local rc=$?
	if [ "$rc" = 0 ] && grep -q "stopping the run" "$d/out.log"; then
		echo "  pass $name: SIG$sig, exit 0"
	else
		echo "  FAIL $name: SIG$sig, exit $rc"; failed="$failed $name"
	fi
}

echo "sigtest: $bin"
one running TERM 0
one paused INT 1
if [ -n "$failed" ]; then echo "sigtest: FAILED:$failed"; exit 1; fi
if [ -n "$unmade" ]; then echo "sigtest: NOT TESTED:$unmade"; exit 2; fi
echo "sigtest: every case passed"
