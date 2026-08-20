#!/usr/bin/env bash
# Differential test: the MAME C++ disassembler against tools/focusdis.py.
#
# Two independent implementations of the same spec, compared instruction by
# instruction over real FOCUS code. Any divergence is a bug in one of them.
# Both are driven from docs/opcodes.csv, so this checks the decoding logic,
# not the opcode table -- that is validated separately (docs/FOCUS.md Appendix B).

set -euo pipefail

cd "$(dirname "$0")"
ROOT=$(cd ../.. && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

DASM=./dasmtest
[ -x "$DASM" ] || { echo "build dasmtest first: make"; exit 2; }

echo "extracting test corpora..."
python3 "$ROOT/tools/lifdir.py" "$ROOT/software/97089C.hpi" \
	--extract S97080C --out "$WORK/S97080C" >/dev/null
python3 "$ROOT/tools/cpiolist.py" "$WORK/S97080C" \
	--extract system/97080C/os.req --out "$WORK/os.req" >/dev/null
python3 "$ROOT/tools/lifdir.py" "$ROOT/software/97089C.hpi" \
	--extract SHPUX_MAN --out "$WORK/SHPUX_MAN" >/dev/null
python3 "$ROOT/tools/td0.py" "$ROOT/software/basic/500BT1.TD0" \
	--raw "$WORK/BT1.img" >/dev/null
python3 "$ROOT/tools/td0.py" "$ROOT/software/basic/500UT3.TD0" \
	--raw "$WORK/UT3.img" >/dev/null

status=0
run() {
	local name=$1 file=$2 start=$3 end=$4
	python3 "$ROOT/tools/focusdis.py" "$file" --start "$start" --end "$end" \
		--count 0 2>/dev/null \
		| sed -E 's/^([0-9a-f]{8})  [0-9a-f]+ +/\1  /; s/  <<< misaligned$//' \
		> "$WORK/py.txt"
	"$DASM" "$file" "$start" "$end" > "$WORK/cc.txt"
	local n
	n=$(wc -l < "$WORK/py.txt")
	if diff -q "$WORK/py.txt" "$WORK/cc.txt" >/dev/null; then
		printf '  %-16s %9s instructions  identical\n' "$name" "$n"
	else
		printf '  %-16s %9s instructions  MISMATCH\n' "$name" "$n"
		diff "$WORK/py.txt" "$WORK/cc.txt" | head -20
		status=1
	fi
}

echo "comparing..."
run "HP-UX kernel"  "$WORK/os.req"    0 0x58000
run "BASIC boot"    "$WORK/BT1.img"   0 0x46000
run "BASIC util"    "$WORK/UT3.img"   0 0x40000
run "control text"  "$WORK/SHPUX_MAN" 0 0x40000

echo
[ $status -eq 0 ] && echo "PASS" || echo "FAIL"
exit $status
