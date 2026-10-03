#!/usr/bin/env bash
# Does BASIC compute the right answer?
#
# src/test/golden.sh proves the emulator did not *change*. It says nothing
# about whether BASIC is *right*, and every check of that so far has been a
# hand-built FOCUS_TYPE run whose screen someone read by eye. This is the
# missing half: a table of (keystrokes, expected screen line) cases, booted
# headless and scored.
#
#   src/test/basictest.sh              run every case
#   src/test/basictest.sh --only add   run just these (comma-separated)
#   src/test/basictest.sh --list       show the case table
#   src/test/basictest.sh --keep       keep the raw logs and say where
#
# Each case gets its own boot. That is slow -- about 35 s of wall clock per
# case -- but a case that halts must not take the ones after it with it, and
# a shared boot would make later cases fail for a reason that is not theirs.
#
# The prompt arrives around 15 emulated seconds; keystrokes go in at 20 and
# the run stops at 30. FOCUS_TRAPLOG is armed from 19 s so a case that faults
# reports its trap, without the boot's own faults filling the log.

set -uo pipefail

cd "$(dirname "$0")"
ROOT=$(cd ../.. && pwd)
WORK=${FOCUS_BASICTEST_WORK:-/tmp/focus-basictest}
DISC=${FOCUS_BASICTEST_DISC:-$ROOT/software/hd/basic.img}

TYPEAT=${FOCUS_BASICTEST_TYPEAT:-20}
STOP=${FOCUS_BASICTEST_STOP:-30}

# ---------------------------------------------------------------- the cases
#
#   name | keystrokes | the line the screen should show [| typeat | stop]
#
# A case that needs its statements spread out gives its own timings: the
# keystrokes become several ';'-separated scripts and `typeat` lists one
# emulated second per script.
#
# The keystrokes are a FOCUS_TYPE script: \n is EXECUTE. The expectation is
# matched against the reconstructed display, one line, leading and trailing
# space ignored. A case whose expectation is met passes; everything else
# prints what it actually did, so a change of symptom is visible long before
# it becomes a pass.
#
# Cases marked WORKS are regression guards -- they pass today and must go on
# passing. All 25 do. A case that is known to be wrong goes in KNOWN_FAIL
# below: it scores XFAIL (or XPASS, once fixed -- then take it off the
# list) and does not count against the exit status. The list is empty:
# `concat` (SCON's bound, S-4 not S-14) and `real-add` (LADD,B dropping
# the carry between limbs) were its last two entries.
#
# Exit status: 0 every scored case as expected, 1 a case failed that should
# pass, 2 nothing could be scored (no binary, no disc, BASIC never booted).
CASES=(
	# --- these pass today ------------------------------------- WORKS
	"disp-int      | DISP 7\n              | 7"
	"disp-string   | DISP \"HI\"\n         | HI"
	"disp-paren    | DISP (7)\n            | 7"
	"disp-neg-lit  | DISP -7\n             | -7"
	# --- arithmetic ------------------------------------------- WORKS
	"add           | DISP 1+1\n            | 2"
	"add-zero      | DISP 1+0\n            | 1"
	"add-big       | DISP 100+200\n        | 300"
	"sub           | DISP 9-8\n            | 1"
	"mul           | DISP 3*4\n            | 12"
	"div           | DISP 8/2\n            | 4"
	# --- functions -------------------------------------------- WORKS
	"sqr           | DISP SQR(4)\n         | 2"
	# --- statements ------------------------------------------- WORKS
	"let           | LET A=6\nDISP A\n     | 6"
	"new           | NEW\nDISP 7\n         | 7"
	"two-stmt      | DISP 1\nDISP 2\nDISP 3\n | 3"
	# --- these three separated the failure modes ---------------------
	# They were the sharpest discriminators while every operator printed 9.
	# All three pass; string concatenation faulted outright until SCON's
	# target bound was read as S-4, as the ERS page image has it.
	"two-item      | DISP 4,5\n           | 4                   5"
	"compare       | DISP 1=1\n           | 1"
	"concat        | DISP \"A\"&\"B\"\n      | AB"
	# AVAILABLE MEMORY is 64 bytes lower than it was before the graphics
	# interface was fitted at select code 1: one more registered device is
	# one more device record. FOCUS_NOGFX restores the old figure, which
	# is the check that nothing else moved.
	# --- the second-statement crash, fixed -------------------------
	# Two LISTs in a row used to halt with TRAP 001 in the container's tail
	# TRY: the create belongs to the statement's own compiled prologue, and
	# the compiler was emitting the wrong template (docs/SUNOS.md 16).
	"list          | LIST\n                | AVAILABLE MEMORY = 487696"
	"list-twice    | LIST\n;LIST\n         | AVAILABLE MEMORY = 487696 | 20,26 | 34"
	# --- division and functions: the compiler emits real arithmetic --
	# These exercise BTD, DTB and POT end to end, including the digits of
	# an irrational.
	"frac-div      | DISP 9/2\n           | 4.5"
	"recip         | DISP 1/3\n           | .333333333333"
	"big-div       | DISP 100/8\n         | 12.5"
	"sqrt2         | DISP SQR(2)\n        | 1.41421356237"
	# --- real addition -------------------------------------------------
	# Came back a few ulp low (5.49999999907) while LADD,B dropped the
	# carry between the limbs of BASIC's decimal-to-binary conversion.
	"real-add      | DISP 4.5+1\n         | 5.5"
	# --- a typed program ----------------------------------------------
	# Program lines are stored with ENTER (\r) -- EXECUTE (\n) runs a line
	# as a command -- and a program needs its END (else ERROR 5, Improper
	# context terminator). PRINT scrolls, so all three values stay on screen.
	"prog-loop     | 10 FOR I=1 TO 3\r;20 PRINT I*I\r;30 NEXT I\r;40 END\r;RUN\n | 9 | 20,26,32,38,44 | 56"
)

# Cases known to be wrong today, space-separated with a space at each end.
KNOWN_FAIL=" "

# ------------------------------------------------------------------ options
ONLY=""
KEEP=0
LIST=0
while [ $# -gt 0 ]; do
	case "$1" in
		--only)
			[ $# -ge 2 ] && [ -n "$2" ] \
				|| { echo "--only needs a case name" >&2; exit 2; }
			ONLY=",$2,"; shift ;;
		--list) LIST=1 ;;
		--keep) KEEP=1 ;;
		*) echo "unknown option: $1" >&2; exit 2 ;;
	esac
	shift
done

field() { echo "$1" | cut -d'|' -f"$2" | sed 's/^ *//;s/ *$//'; }

if [ "$LIST" = 1 ]; then
	printf '%-14s %-24s %s\n' NAME TYPES EXPECTS
	for c in "${CASES[@]}"; do
		printf '%-14s %-24s %s\n' \
			"$(field "$c" 1)" "$(field "$c" 2)" "$(field "$c" 3)"
	done
	exit 0
fi

if [ ! -x "$ROOT/mame/focus" ]; then
	echo "mame/focus is not built; run:"
	echo "  cd mame && make SUBTARGET=focus SOURCES=src/mame/hp/hp9000_5xx.cpp -j8"
	exit 2
fi
[ -f "$DISC" ] || { echo "no fixed disc at $DISC (tools/mkharddisc.py)"; exit 2; }

# Never open a window: these runs are headless by definition. The run
# scripts set this themselves for -video none, but basictest also wants it
# in the environment for anything it invokes directly.
export SDL_VIDEODRIVER=dummy

# GNU timeout guards against a run that never ends; macOS has it only as
# gtimeout (coreutils), and without either the runs go unguarded.
if command -v timeout >/dev/null; then TIMEOUT=timeout
elif command -v gtimeout >/dev/null; then TIMEOUT=gtimeout
else TIMEOUT=""
fi
guard() { if [ -n "$TIMEOUT" ]; then "$TIMEOUT" "$@"; else shift; "$@"; fi; }

mkdir -p "$WORK"
pass=0; fail=0; skip=0; xfail=0; xpass=0

echo "BASIC behaviour ($(basename "$DISC"), keystrokes at ${TYPEAT}s, stop at ${STOP}s)"
echo

for c in "${CASES[@]}"; do
	name=$(field "$c" 1)
	keys=$(field "$c" 2)
	want=$(field "$c" 3)
	at=$(field "$c" 4);   at=${at:-$TYPEAT}
	stop=$(field "$c" 5); stop=${stop:-$STOP}

	if [ -n "$ONLY" ] && [ "${ONLY#*,$name,}" = "$ONLY" ]; then continue; fi

	log="$WORK/$name.log"

	# The boot intermittently fails on a loaded machine: the run finishes,
	# but BASIC never reaches its prompt and the emulated CPU idles at
	# thousands of percent. That is a lost measurement, not an answer, and
	# scoring it as a failure makes the total meaningless. "BASIC Ready
	# for use." appears at some frame of every successful boot, so its
	# absence anywhere in the log is the signal. Retry once, then skip --
	# the same discipline golden.sh uses for its MAME probes.
	booted=0
	for attempt in 1 2; do
		FOCUS_HDNOSAVE=1 FOCUS_DIMDUMP=180 \
		FOCUS_TRAPLOG=1 FOCUS_TRAPFROM=$((${at%%,*} - 1)) \
		FOCUS_TYPE="$keys" FOCUS_TYPEAT="$at" \
			guard $((stop * 20)) "$ROOT/tools/runbasic-hd.sh" "$DISC" \
				-- -str "$stop" -video none >"$log" 2>&1
		rc=$?
		if grep -q 'BASIC Ready for use' "$log"; then booted=1; break; fi
	done

	if [ "$booted" = 0 ]; then
		printf '  %-14s SKIP  BASIC never reached its prompt (twice)\n' "$name"
		skip=$((skip + 1))
		continue
	fi

	# The banner is on screen for every case; it is not an answer.
	screen=$(python3 "$ROOT/tools/screen.py" <"$log" 2>/dev/null \
		| grep -v 'Hewlett-Packard Company\|CLOCK AND DATE NOT SET')
	traps=$(grep -c '^\[trap' "$log" 2>/dev/null)
	traps=${traps:-0}

	if [ -z "$screen" ] && [ "$rc" != 0 ]; then
		printf '  %-14s SKIP  the run did not finish (exit %s)\n' "$name" "$rc"
		skip=$((skip + 1))
		continue
	fi

	known=0
	case "$KNOWN_FAIL" in *" $name "*) known=1 ;; esac

	if echo "$screen" | sed 's/^ *//;s/ *$//' | grep -qxF -- "$want"; then
		if [ "$known" = 1 ]; then
			printf '  %-14s XPASS %s  (fixed? take it off KNOWN_FAIL)\n' \
				"$name" "$want"
			xpass=$((xpass + 1))
		else
			printf '  %-14s PASS  %s\n' "$name" "$want"
			pass=$((pass + 1))
		fi
	else
		if echo "$screen" | grep -q 'System halted'; then
			got=$(echo "$screen" | grep -o 'TRAP:[0-9A-F]*' | head -1)
			got="halted, ${got:-unknown trap}"
		else
			got=$(echo "$screen" | sed 's/^ *//;s/ *$//' | grep -v '^$' \
				| tail -2 | tr '\n' '/' | sed 's:/$::')
		fi
		verdict=FAIL
		[ "$known" = 1 ] && verdict=XFAIL
		printf '  %-14s %-5s want %-8s got %s%s\n' "$name" "$verdict" "$want" \
			"${got:-<nothing>}" \
			"$([ "$traps" -gt 0 ] && echo "  [$traps traps]")"
		if [ "$known" = 1 ]; then xfail=$((xfail + 1)); else fail=$((fail + 1)); fi
	fi
done

echo
echo "$pass passed, $fail failed$([ $xfail -gt 0 ] && echo ", $xfail known failures")$([ $xpass -gt 0 ] && echo ", $xpass unexpectedly passed")$([ $skip -gt 0 ] && echo ", $skip skipped")"
[ "$KEEP" = 1 ] && echo "logs in $WORK"
if [ $((pass + fail + xfail + xpass)) -eq 0 ]; then
	echo "nothing was scored"
	exit 2
fi
[ "$fail" = 0 ]
