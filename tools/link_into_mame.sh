#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Joel Heikkila
#
# Wire this project's FOCUS sources into the MAME checkout in mame/.
#
#   tools/link_into_mame.sh            link and register (idempotent)
#   tools/link_into_mame.sh --check    say what is missing, change nothing
#
# Two things must be true of the MAME tree before it can build the
# hp9020/hp9040 driver, and this script makes both true. Run it after any
# fresh checkout of mame/, and as often as you like: a second run changes
# nothing. tools/setup.sh runs it for you.
#
#   1. src/cpu/focus, src/iop and src/mame/hp9000_5xx.cpp live outside the
#      submodule, under this repository's version control. MAME sees them
#      through three symlinks at the paths its build expects. They are
#      RELATIVE links, so the clone can live anywhere and be moved. (They
#      used to be absolute, into the maintainer's home directory, and were
#      committed that way inside the MAME commit the submodule pinned.)
#
#   2. MAME must be told the CPU and the driver exist. That is
#      tools/mame/focus-mame.patch, made against the upstream commit the
#      mame gitlink pins (a3ee360d): a FOCUS block in scripts/src/cpu.lua,
#      whose "--@src/devices/cpu/focus/focus.h,CPUS["FOCUS"] = true" line is
#      what lets a SOURCES= build find the CPU at all, and the two machines
#      in src/mame/mame.lst, without which a SOURCES= build links a binary
#      that says "0 driver(s) found". It is applied unless it already is;
#      a tree it neither applies to nor is already applied to is refused.
#
# The patch leaves two tracked files modified and the links untracked in
# the submodule; .gitmodules says `ignore = dirty`, so the superproject's
# `git status` does not report either.

set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)
PATCH="$ROOT/tools/mame/focus-mame.patch"
CHECK=0
case "${1:-}" in
	--check) CHECK=1 ;;
	-h|--help) sed -n '5,/^set -euo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
	"") ;;
	*) echo "unknown option '$1' (try --help)" >&2; exit 2 ;;
esac

[ -d mame/src/devices/cpu ] && [ -f mame/scripts/src/cpu.lua ] || {
	echo "error: the MAME source tree in mame/ is missing or empty." >&2
	echo "Run tools/setup.sh, which fetches the MAME revision this tree is" >&2
	echo "built against and then runs this script." >&2
	exit 2
}

status=0

# ------------------------------------------------------------ the links
# Each target is relative to the directory the link lives in.
link() {
	local link=$1 rel=$2
	if [ -L "$link" ] && [ "$(readlink "$link")" = "$rel" ] && [ -e "$link" ]; then
		echo "ok      $link -> $rel"
		return
	fi
	if [ "$CHECK" = 1 ]; then
		echo "MISSING $link -> $rel"
		status=1
		return
	fi
	if [ -L "$link" ]; then
		rm "$link"
	elif [ -e "$link" ]; then
		echo "error: $link exists and is not a symlink; refusing to replace it" >&2
		exit 1
	fi
	ln -s "$rel" "$link"
	if [ ! -e "$link" ]; then
		echo "error: $link -> $rel does not resolve" >&2
		exit 1
	fi
	echo "linked  $link -> $rel"
}

link mame/src/devices/cpu/focus      ../../../../src/cpu/focus
link mame/src/devices/iop            ../../../src/iop
link mame/src/mame/hp/hp9000_5xx.cpp ../../../../src/mame/hp9000_5xx.cpp

# ------------------------------------------------------------ the patch
# Already applied -> nothing; applies cleanly -> apply; anything else (a
# half-applied tree, another MAME revision) -> refuse. `git apply` all
# round, because it is strict where patch(1) guesses: patch's "assume
# yes" modes apply a hunk that is already there, or take an unapplied
# patch for a reversed one. When mame/ is not a git checkout of its own
# (an unpacked MAME tarball, say), GIT_CEILING_DIRECTORIES stops git
# finding this repository above it, and `git apply` then works as a
# plain patch program on the files.
export GIT_CEILING_DIRECTORIES="$ROOT"
applied() { git -C mame apply --reverse --check "$PATCH" >/dev/null 2>&1; }
applies() { git -C mame apply --check "$PATCH" >/dev/null 2>&1; }
apply()   { git -C mame apply "$PATCH"; }

if applied; then
	echo "ok      tools/mame/focus-mame.patch is applied"
elif [ "$CHECK" = 1 ]; then
	echo "MISSING tools/mame/focus-mame.patch is not applied"
	status=1
elif applies; then
	apply
	echo "applied tools/mame/focus-mame.patch"
else
	cat >&2 <<'EOF'
error: tools/mame/focus-mame.patch neither applies to mame/ nor is applied.
It is made against the MAME commit the mame gitlink pins, which
tools/setup.sh checks out. If the two files were edited or half-patched,
put them back and run this again:

  git -C mame checkout -- scripts/src/cpu.lua src/mame/mame.lst

On another MAME revision, make the same two changes by hand: the FOCUS
block in scripts/src/cpu.lua and the hp9000_5xx.cpp entries in
src/mame/mame.lst.
EOF
	exit 1
fi

[ "$CHECK" = 1 ] && exit "$status"
cat <<'NOTE'

The MAME tree is ready. Build the emulator with tools/setup.sh, or by hand:

  cd mame && make SUBTARGET=focus SOURCES=src/mame/hp/hp9000_5xx.cpp -j8

The result is mame/focus. The driver links as one file; a new source file
in src/mame would need its own link above.
NOTE
