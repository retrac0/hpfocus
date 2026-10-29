#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Joel Heikkila
#
# Does the emulator work here? PASS or FAIL in a couple of minutes.
#
#   tools/smoketest.sh              the FOCUS CPU's recursive factorial (ERS
#                                   section 7), and multiuser boots (2 MB and
#                                   4 MB) of fresh copies of the pristine
#                                   HP-UX machine to a root login on a
#                                   27140A port
#   tools/smoketest.sh --twice      then boot again on what the first boot
#                                   left (disc and battery): what a user's
#                                   second `tools/runhpux.sh --multiuser` sees
#   tools/smoketest.sh --keep       keep the work directory and the logs
#
# Nothing of yours is touched: the machine is a fresh copy of the pristine
# one in a work directory under var/ (deleted afterwards unless --keep),
# booted headless and unthrottled by tests/run_tests.py, the regression
# suite this is a cut of; run that for everything. The pristine machine is
# made first if it is not there (tools/runhpux.sh --prepare).
#
# Exit status 0 only if everything passed. Put the output in a bug report.

set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)
TWICE=0
KEEP=0
for a in "$@"; do
	case "$a" in
		--twice) TWICE=1 ;;
		--keep) KEEP=1 ;;
		-h|--help) sed -n '5,/^# Exit status/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
		*) echo "unknown option '$a' (try --help)" >&2; exit 2 ;;
	esac
done

BIN=${HPFOCUS_BIN:-$ROOT/mame/focus}
[ -x "$BIN" ] || { echo "the emulator is not built ($BIN); run tools/setup.sh" >&2; exit 1; }
echo "smoke test: $(basename "$BIN") built $(date -r "$BIN" '+%Y-%m-%d %H:%M' 2>/dev/null || echo '?')," \
	"$(git -C mame log -1 --format='mame %h' 2>/dev/null || echo 'mame ?')," \
	"$(git log -1 --format='tree %h' 2>/dev/null || echo 'tree ?')"

VAR=${HPFOCUS_VAR:-$ROOT/var}
mkdir -p "$VAR"
WORK=$(mktemp -d "$VAR/smoke.XXXXXX")
[ "$KEEP" = 1 ] || trap 'rm -rf "$WORK"' EXIT

status=0
python3 tests/run_tests.py --mame "$BIN" --work "$WORK" isa-factorial hpux-multi || status=1
if [ "$TWICE" = 1 ]; then
	echo "second boot, on the disc and battery the first one left:"
	python3 tests/run_tests.py --mame "$BIN" --work "$WORK" hpux-multi || status=1
fi
[ "$KEEP" = 1 ] && echo "kept: $WORK"
[ "$status" = 0 ] && echo "smoke test: PASS" || echo "smoke test: FAIL"
exit "$status"
