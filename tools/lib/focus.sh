# Shared machinery for the run scripts -- sourced, not executed.
#
# tools/runbasic.sh, runbasic-hd.sh and runhpux.sh each start a machine
# under MAME, and each of them used to carry its own copy of the same
# three decisions about how to do it, comments and all. Three copies of a
# paragraph is three places for it to drift; this is that paragraph once.
#
#   focus_repo             cd to the repository root, set $FOCUS_REPO
#   focus_built            check the emulator exists, or say how to build it
#   focus_args MACHINE ARGS  set FOCUS_CMD to the full command line: the
#                          throttle and video arguments the rest implies
#   focus_run MACHINE ARGS exec that command (in mame/, as MAME expects)
#   focus_basic_disc_warn IMG  say so if IMG is a BASIC fixed disc built by
#                          an sdfwrite.py older than the free-map fix
#
# HPFOCUS_BIN=<file> runs another build of the emulator than mame/focus --
# a private build, or one made outside the tree. MAME still runs in mame/,
# where it keeps its cfg/ and nvram/ directories.

# The repository root, from the directory this file lives in.
focus_repo() {
	FOCUS_REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
	FOCUS_BIN=${HPFOCUS_BIN:-$FOCUS_REPO/mame/focus}
	cd "$FOCUS_REPO"
}

focus_built() {
	if [ ! -x "$FOCUS_BIN" ]; then
		echo "the emulator is not built ($FOCUS_BIN is missing)." >&2
		echo "From a fresh clone, run:" >&2
		echo "  tools/setup.sh" >&2
		echo "which fetches MAME, wires this tree into it and builds it" >&2
		echo "(INSTALL.md has the prerequisites)." >&2
		exit 1
	fi
	[ -d "$FOCUS_REPO/mame" ] || mkdir -p "$FOCUS_REPO/mame"
}

# Throttling. A batch run wants to go as fast as it can; an interactive
# one wants the window to keep up. Unthrottled, a machine that idles in
# STOP races its emulated clock to thousands of percent, and MAME spends
# so long emulating between presentations that the window never draws and
# the event pump never sees a close or a signal -- a blank window you have
# to kill by hand. So -nothrottle is added only for a run that ends by
# itself, which is what -seconds_to_run means.
#
# Video backend. MAME defaults to opengl, and a driver that cannot present
# through it leaves a window that renders the blinking cursor and never
# updates the text -- the machine running perfectly behind a screen that
# looks dead. These displays are character cells over a modest raster, so
# the software renderer costs nothing and always works. A -video on the
# command line comes after this one and wins, so any backend can still be
# asked for.
#
# MAME's start-up information screen ("Press any key to continue") would
# hold a windowed run before the machine powers on -- and the run scripts'
# own hooks with it -- so it is skipped. The 540 is flagged as working and
# raises no warning screen; the 520 still does, once per run, because its
# driver flags it not working.
#
# A headless run has no business opening a window. MAME initialises SDL's
# video subsystem even for -video none, which costs real time on a loaded
# desktop and, on this machine, intermittently leaves the emulated boot
# dead. SDL_VIDEODRIVER=dummy gives SDL a backend that draws nowhere.
# Hewlett-Packard's software is fetched, not carried (MEDIA.txt). The 540's
# comes from bitsavers by script; the 520's -- the BASIC, demonstration and
# test floppies and the HP-UX 5.0 tape -- comes from the HP Computer Museum,
# whose downloads a person has to make in a browser.
#
#   focus_need_media FILE...   status 0 if every FILE is here; else say
#                              which are not and how to get them, status 1
focus_need_media() {
	local f missing=()
	for f in "$@"; do
		[ -f "$f" ] || missing+=("${f#"$FOCUS_REPO"/}")
	done
	[ ${#missing[@]} -eq 0 ] && return 0
	{
		echo "not here: ${missing[*]}"
		echo "The 520's software is Hewlett-Packard's and is not in the repository. It"
		echo "comes from the HP Computer Museum, by hand: tools/fetch-media.sh --list"
		echo "names the pages. Download the items in a browser, then run"
		echo "  tools/fetch-media.sh --from <the folder or the .zip files>"
	} >&2
	return 1
}

focus_args() {
	local machine=$1; shift
	local throttle=() arg
	for arg in "$@"; do
		case "$arg" in
			-seconds_to_run|-str) throttle=(-nothrottle); break ;;
		esac
	done
	case " $* " in
		*" -video none "*) export SDL_VIDEODRIVER=dummy ;;
	esac
	FOCUS_CMD=("$FOCUS_BIN" "$machine" ${throttle[@]+"${throttle[@]}"} \
		-video soft -sound none -skip_gameinfo "$@")
}

# A fixed disc built by tools/sdfwrite.py before 0ec4035 carries a free list
# where BASIC reads a free-block bitmap: BASIC reports the wrong AVAILABLE
# SPACE and CREATE/STORE answer ERROR 455. Such a volume also gives itself
# away in its superblock: s_maxblk, the LARGEST block number, was written
# as the block count. True (status 0) for a volume built that way.
focus_basic_disc_stale() {
	python3 - "$1" <<'EOF' 2>/dev/null
import os, struct, sys
p = sys.argv[1]
with open(p, 'rb') as f:
    sb = f.read(52)
fmt = struct.unpack('>H', sb[0:2])[0]
blksz, maxblk = struct.unpack('>I', sb[28:32])[0], struct.unpack('>I', sb[48:52])[0]
sys.exit(0 if fmt == 0x0700 and blksz and maxblk >= os.path.getsize(p) // blksz else 1)
EOF
}

focus_basic_disc_warn() {
	focus_basic_disc_stale "$1" || return 0
	cat >&2 <<EOF
warning: $1
  was built by an older tools/sdfwrite.py: BASIC reports the wrong
  AVAILABLE SPACE on it, and CREATE and STORE fail with ERROR 455. To
  rebuild it, copy off what you stored there
    tools/sdfread.py --extract DIR $1
  then delete it and run tools/setup.sh again (tools/mkdiagdisc.sh
  rebuilds the diagnostics disc).
EOF
}

focus_run() {
	focus_args "$@"
	cd "$FOCUS_REPO/mame"
	exec "${FOCUS_CMD[@]}"
}
