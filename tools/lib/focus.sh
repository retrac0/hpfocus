# Shared machinery for the run scripts -- sourced, not executed.
#
#   focus_repo             cd to the repository root, set $FOCUS_REPO
#   focus_built            check mame/focus exists, or say how to build it
#   focus_hd SRC NAME      a raw fixed-disc image as MAME's hard disk device
#                          takes it: a .hd link beside the source
#   focus_run MACHINE ARGS run it, adding the throttle and video arguments
#                          the rest of the command line implies

focus_repo() {
	FOCUS_REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
	cd "$FOCUS_REPO"
}

focus_built() {
	if [ ! -x "$FOCUS_REPO/mame/focus" ]; then
		echo "mame/focus is not built; run:"
		echo "  cd mame && make SUBTARGET=focus SOURCES=src/mame/hp/hp9k_5xx.cpp -j8"
		exit 1
	fi
}

# MAME's hard disk device takes a raw image by its extension. The image is
# the machine's own disc and is written as the system writes it.
focus_hd() {
	local src=$1 name=$2
	case "$src" in /*) ;; *) src="$FOCUS_REPO/$src" ;; esac
	local link="$(dirname "$src")/$name.hd"
	ln -sf "$(basename "$src")" "$link"
	echo "$link"
}

# A run that ends by itself (-seconds_to_run) goes as fast as it can; an
# interactive one is throttled so the window keeps up. The software
# renderer always works for these character-cell displays; a -video on the
# command line comes later and wins. A headless run gives SDL a video
# backend that draws nowhere.
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
