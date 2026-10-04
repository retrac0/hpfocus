#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Joel Heikkila
#
# Build a fixed disc that boots HP's 9020 System Functional Test environment.
#
# The test programs are not BASIC 2.0 programs. 500ENV1-3 are a three-volume
# boot image labelled "BASIC LANGUAGE SYSTEM REV 2.00, BINARIES FOR SYS. FUNC.
# TEST", and they carry five binaries the shipped option discs do not have at
# all -- GRAPHICS, SRM, HP98770, HP97060 and HP97062. Booted on plain BASIC
# 2.0 the test driver loads but reports
#
#   SUBPROGRAM Clear_screen  REFERS TO MISSING OPTIONS; IT IS NOT STORABLE
#
# because Clear_screen needs GRAPHICS. Booted on this environment it does not.
# (docs/DIAGNOSTICS.md said the environment discs "carry no SDF volume, so they
# are not part of the same file world" -- true, and the reason is that they are
# a boot image rather than a filesystem.)
#
#   tools/mkdiagdisc.sh                  build software/hd/diag.img
#   tools/mkdiagdisc.sh other.img        build somewhere else
#
# Then boot it with the test disc in the flexible drive:
#
#   tools/runbasic-hd.sh software/hd/diag.img -- -mpb3 1m -mpb4 1m \
#       -flop1 "$PWD/software/test/500MFTST.TD0"
#
# and, at the BASIC prompt, LOAD "TEST" and RUN -- each ended with Ctrl-J
# (or Insert), which is the 520's EXECUTE key in the window; Return is its
# ENTER key.
# tools/setup.sh builds software/hd/diag.img with this script, and
# `src/test/hpuxtest.py matrix --only diag` runs the test driver on it.

set -euo pipefail
cd "$(dirname "$0")/.."
OUT="${1:-software/hd/diag.img}"
# software/hd is gitignored, so a fresh clone has no such directory until
# tools/setup.sh's media step has run.
mkdir -p "$(dirname "$OUT")"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

source tools/lib/focus.sh
FOCUS_REPO=$(pwd)
focus_need_media software/test/500ENV1.TD0 software/test/500ENV2.TD0 software/test/500ENV3.TD0 || exit 1
for v in 500ENV1 500ENV2 500ENV3; do
	python3 tools/td0.py --raw "$WORK/$v.img" "software/test/$v.TD0" >/dev/null
done

# 800 blocks of 1024 leaves room for the 680,668-byte image; the default 650
# does not. The volume is rebuilt around the boot area either way.
python3 tools/mkharddisc.py "$OUT" --boot-blocks 800 --label HPFOCUS-DIAG-01
python3 tools/installbasic.py "$OUT" \
	--basic "$WORK/500ENV1.img" "$WORK/500ENV2.img" "$WORK/500ENV3.img"
