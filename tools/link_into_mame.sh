#!/usr/bin/env bash
# Link this project's FOCUS sources into the MAME submodule tree.
#
# src/cpu/focus/ is kept outside the submodule so it stays under this repo's
# version control; MAME sees it through a symlink at the path its build system
# expects. Re-run after a fresh submodule checkout.

set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)

[ -d mame/src/devices/cpu ] || {
	echo "MAME submodule not checked out. Run:" >&2
	echo "  git submodule update --init --depth 1 mame" >&2
	exit 2
}

link() {
	local target=$1 source=$2
	if [ -L "$target" ]; then
		rm "$target"
	elif [ -e "$target" ]; then
		echo "$target exists and is not a symlink; refusing to replace it" >&2
		exit 1
	fi
	ln -s "$ROOT/$source" "$target"
	echo "linked $target -> $source"
}

link mame/src/devices/cpu/focus  src/cpu/focus
link mame/src/devices/iop        src/iop
link mame/src/mame/hp/hp9000_5xx.cpp src/mame/hp9000_5xx.cpp

cat <<'NOTE'

To build the disassembler inside MAME, register it in two places:

  scripts/src/cpu.lua        add a FOCUS block following an existing CPU, e.g.
                             the one for a similarly simple core, listing
                             focusdasm.cpp under the disasm files.
  scripts/target/mame/*.lua  add "FOCUS" to the CPUS list of whichever target
                             you build.

The execution core, the IOP and the machine driver all build now:

  cd mame && make SUBTARGET=focus SOURCES=src/mame/hp/hp9000_5xx.cpp -j8

Note that the driver is linked as a single file, and the three run
scripts (tools/runbasic.sh, runbasic-hd.sh, runhpux.sh) name that same
path in their SOURCES=. Splitting it would mean linking each new file
here as well.
NOTE
