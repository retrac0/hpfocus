# Shared machinery for the run scripts -- sourced, not executed.
#
# tools/runbasic.sh, runbasic-hd.sh and runhpux.sh each start a machine
# under MAME, and each of them used to carry its own copy of the same
# three decisions about how to do it, comments and all. Three copies of a
# paragraph is three places for it to drift; this is that paragraph once.
#
#   focus_repo             cd to the repository root, set $FOCUS_REPO
#   focus_built            check mame/focus exists, or say how to build it
#   focus_run MACHINE ARGS run it, adding the throttle and video arguments
#                          the rest of the command line implies

# The repository root, from the directory this file lives in.
focus_repo() {
	FOCUS_REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
	cd "$FOCUS_REPO"
}

focus_built() {
	if [ ! -x "$FOCUS_REPO/mame/focus" ]; then
		echo "mame/focus is not built; run:"
		echo "  cd mame && make SUBTARGET=focus SOURCES=src/mame/hp/hp9000_5xx.cpp -j8"
		exit 1
	fi
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
# A headless run has no business opening a window. MAME initialises SDL's
# video subsystem even for -video none, which costs real time on a loaded
# desktop and, on this machine, intermittently leaves the emulated boot
# dead. SDL_VIDEODRIVER=dummy gives SDL a backend that draws nowhere.
focus_run() {
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
	cd "$FOCUS_REPO/mame"
	exec ./focus "$machine" "${throttle[@]}" -video soft -sound none "$@"
}
