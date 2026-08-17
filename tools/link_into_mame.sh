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

TARGET=mame/src/devices/cpu/focus
if [ -L "$TARGET" ]; then
	rm "$TARGET"
elif [ -e "$TARGET" ]; then
	echo "$TARGET exists and is not a symlink; refusing to replace it" >&2
	exit 1
fi

ln -s "$ROOT/src/cpu/focus" "$TARGET"
echo "linked $TARGET -> src/cpu/focus"
ls -l "$TARGET"

cat <<'NOTE'

To build the disassembler inside MAME, register it in two places:

  scripts/src/cpu.lua        add a FOCUS block following an existing CPU, e.g.
                             the one for a similarly simple core, listing
                             focusdasm.cpp under the disasm files.
  scripts/target/mame/*.lua  add "FOCUS" to the CPUS list of whichever target
                             you build.

Until the execution core exists (plan step 1.2), only focusdasm.cpp is
buildable; focusdasm.h has no dependency on a device class.
NOTE
