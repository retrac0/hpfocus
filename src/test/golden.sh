#!/usr/bin/env bash
# Behaviour-preservation harness: capture the emulator's exact output now,
# compare it after a refactor.
#
# The unit tests cover the pieces; this covers the machine. A refactor that
# is meant to change nothing has to produce byte-identical output from every
# probe below, including two multi-million-instruction traces and three real
# boots. That is the gate -- not "the tests still pass", which they would go
# on doing through a great many real regressions.
#
#   src/test/golden.sh capture     record the current tree as the baseline
#   src/test/golden.sh check       compare the current tree against it
#   src/test/golden.sh list        show what the baseline holds
#
# Options:
#   --only <name>[,<name>...]      run just these probes
#   --quick                        skip the three MAME boots (~30 s saved)
#
# The baseline lives outside the repo, under $FOCUS_GOLDEN (default
# /tmp/focus-golden), because it is a few hundred megabytes of trace and it
# belongs to a working tree state rather than to the project.
#
# This harness compares *output*, not verdicts: a unit harness whose
# failures do not change is a pass here, and one whose output changes at all
# is not. A `check` that finds no baseline to compare against says NOTHING
# COMPARED and exits 2 rather than passing -- the default baseline is in
# /tmp, which a reboot empties.
#
# Exit status: 0 every compared probe identical, 1 something differs, 2 the
# run could not be made (build, media, no baseline, bad arguments).
#
# WHAT IT NEEDS. This is a tool for the person changing the emulator, used
# as a pair -- `capture` on the tree before the change, `check` on the tree
# after -- and not a release test: there is no baseline in the repository,
# and a tree compared with itself passes whatever it does. Of its nineteen
# probes,
#   - ten need only a C++ compiler and the shipped floppies (the six unit
#     harnesses, the disassembler difftest and three bootrun traces):
#     `--quick` runs exactly these;
#   - six boot BASIC under MAME and need what tools/setup.sh makes: the
#     emulator (mame/focus, or the build HPFOCUS_BIN names),
#     software/hd/basic.img and software/hd/diag.img;
#   - three boot HP-UX from software/cct/7914_unitSave_hpux4.0.bin, the
#     first unit-save tape decompressed in place, with software/cct/scm.nv
#     as its battery if there is one. A fresh clone has neither (the tape
#     is fetched compressed (tools/fetch-media.sh) and tools/runhpux.sh
#     assembles its disc under
#     var/), so there these three are skipped, and say so. To have them:
#     gunzip -k software/cct/7914_unitSave_hpux4.0.bin.gz

set -uo pipefail

cd "$(dirname "$0")"
TEST=$(pwd)
ROOT=$(cd ../.. && pwd)
GOLDEN=${FOCUS_GOLDEN:-/tmp/focus-golden}
BASE="$GOLDEN/baseline"
WORK="$GOLDEN/work"
MEDIA="$GOLDEN/media"
# The emulator the MAME probes run: another build than mame/focus if
# HPFOCUS_BIN names one, as for the run scripts.
BIN=${HPFOCUS_BIN:-$ROOT/mame/focus}

# How deep the two bootrun traces go. Large enough that the whole launch
# chain, the trap machinery and the dispatcher are all exercised; small
# enough that a check costs a couple of seconds.
STEPS=${FOCUS_GOLDEN_STEPS:-2000000}

# Nothing here may read the host's wall clock: a probe that does can never be
# compared against a recording. Both are off by default; saying so explicitly
# means a later change of default cannot quietly make every probe unstable.
export FOCUS_RTCTIME=none FOCUS_RTCBATTERY=off

ONLY=""
QUICK=0
MODE=""
while [ $# -gt 0 ]; do
	case "$1" in
		capture|check|list) MODE=$1 ;;
		--only)
			[ $# -ge 2 ] && [ -n "$2" ] \
				|| { echo "--only needs a probe name" >&2; exit 2; }
			ONLY=",$2,"; shift ;;
		--quick) QUICK=1 ;;
		*) echo "unknown argument: $1" >&2; exit 2 ;;
	esac
	shift
done
[ -n "$MODE" ] || { sed -n '2,31p' "$0" | sed 's/^# \?//'; exit 2; }

mkdir -p "$BASE" "$WORK" "$MEDIA"

want() { [ -z "$ONLY" ] || [ "${ONLY#*,$1,}" != "$ONLY" ]; }

# MAME resolves its ini, rompath and plugins relative to its own directory,
# so anything invoking ./focus directly has to be there. tools/runbasic-hd.sh
# already does this for itself; the HP-UX boot below is driven directly.
in_mame() { ( cd "$ROOT/mame" && exec "$@" ); }

# The HP-UX probes write their battery back, and HP-UX keeps counters in the
# NVM that go up every boot -- power-on cycles at 62-63, the checksum at
# 46-47. run() re-runs a probe that differs, so the battery has to be reset
# for EVERY attempt, not once per probe: a retry that inherits the first
# attempt's counters differs again by construction, and the probe can never
# come back identical. Each probe gets its own file for the same reason.
with_nv() {
	local dst=$1; shift
	cp "$ROOT/software/cct/scm.nv" "$dst" 2>/dev/null || rm -f "$dst"
	"$@"
}

fail=0
pass=0
skip=0
retried=0

# run <name> <command...>
#
# Captures stdout+stderr, normalises out the things that legitimately differ
# between two runs of the same binary, and either records or compares.
# capture <name> <command...> -- run it, filter, compress into $WORK.
# Sets `rc` to the command's own exit status and `n` to the line count.
capture_one() {
	local name=$1; shift
	local out="$WORK/$name.txt.gz"
	"$@" 2>&1 \
		| sed -e "s#$GOLDEN#\$GOLDEN#g" -e "s#$ROOT#\$ROOT#g" \
		      -e '/^Average speed:/d' \
		      -e '/NOT FOUND (tried in/d' \
		      -e '/^WARNING: the machine might not run correctly/d' \
		| gzip -1 > "$out"
	rc=${PIPESTATUS[0]}
	n=$(gzip -dc "$out" | wc -l)
}

same_as_baseline() {
	diff -q <(gzip -dc "$BASE/$1.txt.gz") <(gzip -dc "$WORK/$1.txt.gz") \
		>/dev/null
}

# run <name> <command...>
#
# Captures stdout+stderr, normalises out the things that legitimately differ
# between two runs of the same binary, and either records or compares.
#
# A differing probe is retried once. Not to paper over a regression -- a real
# one reproduces, and the retry is reported -- but because the failure mode
# seen on this machine is a MAME run cut short under memory pressure, which
# yields truncated output rather than different output. That is a lost
# measurement, not a change, and treating it as a regression sends the reader
# hunting for a bug that is not there. Two short runs in a row still fail.
run() {
	local name=$1; shift
	want "$name" || return 0
	local rc n

	capture_one "$name" "$@"

	if [ "$MODE" = capture ]; then
		gzip -dc "$WORK/$name.txt.gz" | gzip -9 > "$BASE/$name.txt.gz"
		printf '  %-16s %9s lines  recorded\n' "$name" "$n"
		pass=$((pass + 1))
		return 0
	fi

	if [ ! -f "$BASE/$name.txt.gz" ]; then
		printf '  %-16s %9s lines  NO BASELINE\n' "$name" "$n"
		skip=$((skip + 1))
		return 0
	fi

	if same_as_baseline "$name"; then
		printf '  %-16s %9s lines  identical\n' "$name" "$n"
		pass=$((pass + 1))
		return 0
	fi

	local first=$n
	capture_one "$name" "$@"
	if same_as_baseline "$name"; then
		printf '  %-16s %9s lines  identical on retry (first run: %s)\n' \
				"$name" "$n" "$first"
		pass=$((pass + 1))
		retried=$((retried + 1))
		return 0
	fi

	printf '  %-16s %9s lines  DIFFERS (exit %s)\n' "$name" "$n" "$rc"
	diff <(gzip -dc "$BASE/$name.txt.gz") <(gzip -dc "$WORK/$name.txt.gz") \
		| head -20 | sed 's/^/      /'
	fail=$((fail + 1))
}

if [ "$MODE" = list ]; then
	echo "baseline in $BASE"
	ls -1 "$BASE" 2>/dev/null | sed 's/\.txt\.gz$//;s/^/  /' || echo "  (empty)"
	exit 0
fi

# ---------------------------------------------------------------- media
# The boot images bootrun needs are assembled from the shipped discs, every
# one decoded from its TD0 here: software/basic/img/, which this once read,
# is gitignored and made by nothing, so on a fresh clone bootimage.py died
# and the two bootrun probes recorded its traceback as their baseline. Any
# failure now stops the run. Both steps are cached.
media() {
	[ -f "$MEDIA/basic.bin" ] && [ -f "$MEDIA/diag.bin" ] && return 0
	echo "assembling boot images..."
	local v ok=1
	for v in basic/500BT1 basic/500BT2 test/500ENV1 test/500ENV2 test/500ENV3; do
		python3 "$ROOT/tools/td0.py" "$ROOT/software/$v.TD0" \
			--raw "$MEDIA/${v#*/}.img" >/dev/null || { ok=0; break; }
	done
	[ "$ok" = 1 ] && python3 "$ROOT/tools/bootimage.py" "$MEDIA/basic.bin.part" \
		"$MEDIA/500BT1.img" "$MEDIA/500BT2.img" >/dev/null \
	&& python3 "$ROOT/tools/bootimage.py" "$MEDIA/diag.bin.part" \
		"$MEDIA/500ENV1.img" "$MEDIA/500ENV2.img" "$MEDIA/500ENV3.img" >/dev/null \
	&& mv "$MEDIA/basic.bin.part" "$MEDIA/basic.bin" \
	&& mv "$MEDIA/diag.bin.part" "$MEDIA/diag.bin" \
	|| { echo "could not assemble the boot images from software/*/*.TD0" >&2
	     rm -f "$MEDIA"/*.part; exit 2; }
}

echo "building..."
make -s all || { echo "build failed"; exit 2; }
echo

# ------------------------------------------------------------- the probes
echo "unit harnesses:"
for t in statetest addrtest dsttest exectest ioptest muxtest; do
	run "$t" "$TEST/$t"
done

echo
echo "disassembler:"
run difftest "$TEST/run_difftest.sh"

echo
echo "bootrun ($STEPS steps, full instruction trace):"
if want bootrun-basic || want bootrun-diag; then media; fi
run bootrun-basic "$TEST/bootrun" "$MEDIA/basic.bin" --steps "$STEPS" --trace
run bootrun-diag  "$TEST/bootrun" "$MEDIA/diag.bin"  --steps "$STEPS" --trace

if [ "$QUICK" = 1 ]; then
	echo
	echo "MAME boots: skipped (--quick)"
else
	echo
	echo "MAME boots (state dumps at fixed emulated times):"
	if [ -x "$BIN" ]; then
		# FOCUS_HDNOSAVE keeps the run from writing the fixed disc back,
		# which would make the next run start somewhere else.
		# Headless: no window, no SDL video init (see tools/runbasic-hd.sh).
		export SDL_VIDEODRIVER=dummy
		export FOCUS_HDNOSAVE=1 FOCUS_STATEAT=5,12,19

		run mame-basic env -u FOCUS_SCMNV \
			"$ROOT/tools/runbasic-hd.sh" "$ROOT/software/hd/basic.img" \
			-- -str 20 -video none

		run mame-diag env -u FOCUS_SCMNV \
			"$ROOT/tools/runbasic-hd.sh" "$ROOT/software/hd/diag.img" \
			-- -mpb3 1m -mpb4 1m \
			-flop1 "$ROOT/software/test/500MFTST.TD0" -str 20 -video none

		# HP-UX writes the SCM battery file, so it runs against a copy
		# (with_nv, above, resets it for every attempt). A first boot and a
		# later boot behave differently -- HP-UX wants a set clock -- so
		# both are probed: mame-hpux against a battery, and
		# mame-hpux-firstboot against none, which is the path that sets the
		# clock and writes the default configuration.
		HPUX_DISC="$ROOT/software/cct/7914_unitSave_hpux4.0.bin"
		if { want mame-hpux || want mame-hpux-firstboot; } \
				&& [ -f "$HPUX_DISC" ]; then
			run mame-hpux with_nv "$WORK/hpux-mame.nv" \
				in_mame env FOCUS_SCMNV="$WORK/hpux-mame.nv" \
				"$BIN" hp9040 -mpb3 1m -mpb4 1m -hard1 "$HPUX_DISC" \
				-nothrottle -video none -sound none -str 20
			run mame-hpux-firstboot in_mame env -u FOCUS_SCMNV \
				"$BIN" hp9040 -mpb3 1m -mpb4 1m -hard1 "$HPUX_DISC" \
				-nothrottle -video none -sound none -str 20
		elif want mame-hpux; then
			echo "  mame-hpux        software/cct/7914_unitSave_hpux4.0.bin is not there -- skipped"
			echo "  mame-hpux-firstboot  (the tape is shipped compressed: see the header)"
			skip=$((skip + 2))
		fi
	else
		echo "  $BIN is not built -- the four MAME boots skipped"
		skip=$((skip + 4))
	fi
fi

# --------------------------------------------------------------- knobs
# The probes above run with every FOCUS_* knob unset, so they prove only
# that the default path is unchanged. The knobs are the project's
# instruments and the refactor rewrites all ~100 of them, so they need
# covering too -- switched on in groups, by whichever boot actually
# exercises them. Between them these probes fire every log channel that
# any of the three systems reaches.
echo
echo "knobs:"
if want knobs-bootrun; then media; fi
run knobs-bootrun env FOCUS_IOPLOG=1 FOCUS_IOALL=1 FOCUS_IOPDBG=1 \
	FOCUS_TRYLOG=1 FOCUS_INTLOG=1 FOCUS_SEMALL=1 FOCUS_ICEILOG=1 \
	FOCUS_IRQLOG=1 \
	"$TEST/bootrun" "$MEDIA/basic.bin" --steps "$STEPS" \
	--io --far --semlog --kbdlog --probelog

if [ "$QUICK" = 1 ]; then
	echo "  (MAME knob probes skipped: --quick)"
elif [ ! -x "$BIN" ]; then
	echo "  $BIN is not built -- the five MAME knob probes skipped"
	skip=$((skip + 5))
else
	export FOCUS_HDNOSAVE=1
	unset FOCUS_STATEAT

	# Every log channel the 520 running BASIC reaches.
	run knobs-log env FOCUS_IOPLOG=1 FOCUS_IRQLOG=1 FOCUS_TRYLOG=1 \
		FOCUS_TRAPLOG=1 FOCUS_DISCLOG=1 FOCUS_SC0LOG=1 FOCUS_KBDLOG=1 \
		FOCUS_ESCLOG=1 FOCUS_INTLOG=1 FOCUS_DIMCELLS=1 \
		"$ROOT/tools/runbasic-hd.sh" "$ROOT/software/hd/basic.img" \
		-- -str 20 -video none

	# The watch family: memory ranges, instruction ranges, breakpoints,
	# the trip wire, and the two searches that run from inside a report.
	run knobs-watch env FOCUS_WWATCH=480:4ff FOCUS_RWATCH=480:4ff \
		FOCUS_WATCH=5c50:5d50 FOCUS_BREAK=5d46 FOCUS_TRACEN=20 \
		FOCUS_TRIP=BASIC FOCUS_DUMP=480:16 FOCUS_FIND='"BASIC"' \
		FOCUS_FINDAT=15 FOCUS_QCHECK=1 FOCUS_RUNAWAY=1 FOCUS_PCPROF=1 \
		"$ROOT/tools/runbasic-hd.sh" "$ROOT/software/hd/basic.img" \
		-- -str 20 -video none

	# Effective-address tracing on its own: it is worth half a million
	# lines and would drown anything sharing a probe with it.
	run knobs-eatrace env FOCUS_EATRACE=5d46:20 \
		"$ROOT/tools/runbasic-hd.sh" "$ROOT/software/hd/basic.img" \
		-- -str 20 -video none

	# Configuration overrides, and the keystroke path -- FOCUS_TYPE is
	# how every interactive result in the README was produced.
	run knobs-config env FOCUS_RAMFILL=a5 FOCUS_CSTMAX=64 FOCUS_NVMFILL=5a \
		FOCUS_RAWKEYS=1 FOCUS_TYPE='CAT\n' FOCUS_TYPEAT=15 \
		FOCUS_STATEAT=19 \
		"$ROOT/tools/runbasic-hd.sh" "$ROOT/software/hd/basic.img" \
		-- -str 20 -video none

	# The 540 reaches what the 520 never does: the serial console card,
	# HP-IB, the ICEI command lists and the hardware semaphores.
	if want knobs-hpux && [ -f "$ROOT/software/cct/7914_unitSave_hpux4.0.bin" ]; then
		run knobs-hpux with_nv "$WORK/hpux-knobs.nv" \
			in_mame env FOCUS_SCMNV="$WORK/hpux-knobs.nv" \
			FOCUS_HPIBLOG=1 FOCUS_ASIDBG=1 FOCUS_ASILOG=1 FOCUS_SEMLOG=1 \
			FOCUS_SEMALL=1 FOCUS_ICEILOG=1 FOCUS_IOALL=1 FOCUS_ESCLOG=1 \
			"$BIN" hp9040 -mpb3 1m -mpb4 1m \
			-hard1 "$ROOT/software/cct/7914_unitSave_hpux4.0.bin" \
			-nothrottle -video none -sound none -str 20
	elif want knobs-hpux; then
		echo "  knobs-hpux       software/cct/7914_unitSave_hpux4.0.bin is not there -- skipped"
		skip=$((skip + 1))
	fi
fi

echo
if [ "$MODE" = capture ]; then
	echo "captured $pass probes into $BASE"
	exit 0
fi
printf '%d identical, %d differing, %d skipped' "$pass" "$fail" "$skip"
[ "$retried" -gt 0 ] && printf ' (%d needed a retry)' "$retried"
echo
if [ $((pass + fail)) -eq 0 ]; then
	echo "NOTHING COMPARED -- no baseline in $BASE? run '$0 capture' first"
	exit 2
fi
[ "$skip" -gt 0 ] && echo "($skip probe(s) had no baseline or could not run: not compared)"
[ "$fail" -eq 0 ] && echo PASS || echo FAIL
exit $((fail > 0))
