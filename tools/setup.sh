#!/usr/bin/env bash
# From a fresh clone to a machine that boots HP-UX, in one command.
#
#   tools/setup.sh                 everything: check, fetch MAME, wire, build,
#                                  prepare the media
#   tools/setup.sh --check         only check the prerequisites
#   tools/setup.sh --no-build      everything but the (long) MAME build
#   tools/setup.sh --no-media      everything but the media
#   tools/setup.sh -j N            build with N jobs (default: one per CPU,
#                                  fewer if memory is short)
#   tools/setup.sh --mame-source URL|DIR
#                                  fetch MAME from there instead of the URL in
#                                  .gitmodules (a mirror, or a local MAME clone
#                                  that has the pinned commit)
#   tools/setup.sh -- ARGS...      extra arguments for MAME's make, e.g.
#                                  -- USE_QTDEBUG=0 NOWERROR=1
#
# Every step is idempotent, so the script is also how to resume after an
# interruption or a fix: run it again and it carries on from what is
# missing. Nothing here needs root; missing packages are reported with the
# command that installs them, and the script stops.
#
# THE STEPS
#   1. prerequisites -- a C++20 compiler (GCC 11+ or clang 13+, as MAME
#      a3ee360d's docs/source/initialsetup/compilingmame.rst asks), make,
#      git, Python 3.8+, gzip, and the libraries MAME's SDL build links:
#      SDL2 2.0.14+, SDL2_ttf, fontconfig, X11 (Xinerama, Xi, Xext), OpenGL,
#      ALSA; PulseAudio and Qt 6 are optional here (the emulator runs with
#      -sound none, and Qt is only MAME's debugger window), so without them
#      the build gets NO_USE_PULSEAUDIO=1 / USE_QTDEBUG=0.
#   2. MAME -- the commit the mame gitlink pins (upstream a3ee360d), fetched
#      shallow by SHA straight into mame/: about 210 MB of download and
#      1.2 GB on disc. `git submodule update --init --depth 1 mame` would
#      work as well now the pin is an upstream commit, but it first clones
#      MAME's current master and only then fetches the pinned commit:
#      twice the download. A mame/ that already holds a MAME tree is used
#      as it is (with a warning if it is not the pinned commit).
#   3. wiring -- tools/link_into_mame.sh: three relative symlinks and
#      tools/mame/focus-mame.patch.
#   4. build -- make SUBTARGET=focus SOURCES=src/mame/hp/hp9000_5xx.cpp in
#      mame/, which gives mame/focus. About 1,190 files: measured at 2 h at
#      -j4 on a heavily loaded 8-core machine; expect 20-40 minutes on an
#      idle 8-core one. Peak disc use about 700 MB more. The log is
#      var/setup/build.log.
#   5. media -- tools/fetch-media.sh: Hewlett-Packard's software, which the
#      repository points at (MEDIA.txt) and does not carry, downloaded from
#      bitsavers.org (46 MB) and checked against its SHA-256s. Then
#      tools/runhpux.sh --prepare: the pristine HP-UX 5.30 disc
#      assembled from the tapes (read-only), its primed battery, and the
#      working copy runs use (var/, about 400 MB); and BASIC's fixed disc,
#      software/hd/basic.img, for tools/runbasic-hd.sh: BASIC 2.0 with its
#      ERRORS_ENG, IO and MS options and the CAT_BOOT and BUILD_BOOT
#      utilities, all off HP's floppies; and the 9020 diagnostics
#      disc, software/hd/diag.img (tools/mkdiagdisc.sh). An image that is
#      already there is kept, and named if an older sdfwrite.py built it
#      (BASIC cannot CREATE or STORE on such a disc: delete it and rerun).

set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)

# The MAME commit, if this is not a git checkout to read the gitlink from.
# Keep it equal to the gitlink: `git ls-tree HEAD mame`.
MAME_PIN_FALLBACK=a3ee360dccc8986c5766b898bf5f5b13562a0ed7

CHECK_ONLY=0
BUILD=1
MEDIA=1
JOBS=
SOURCE=${HPFOCUS_MAME_SOURCE:-}
MAKE_EXTRA=()
while [ $# -gt 0 ]; do
	case "$1" in
		--check) CHECK_ONLY=1 ;;
		--no-build) BUILD=0 ;;
		--no-media) MEDIA=0 ;;
		-j) JOBS=${2:?-j needs a number}; shift ;;
		-j*) JOBS=${1#-j} ;;
		--mame-source) SOURCE=${2:?--mame-source needs a URL or directory}; shift ;;
		--) shift; MAKE_EXTRA=("$@"); break ;;
		-h|--help) sed -n '2,/^set -euo/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
		*) echo "unknown option '$1' (try --help)" >&2; exit 2 ;;
	esac
	shift
done

say()  { echo "== $*"; }
die()  { echo "error: $*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }
OS=$(uname -s)

# ------------------------------------------------------------ 1. prerequisites
say "checking prerequisites ($OS $(uname -m))"
missing=()
optional=()
AUTO_MAKE=()

for c in git make gzip; do have "$c" || missing+=("$c"); done

if have python3 && python3 -c 'import sys; sys.exit(sys.version_info < (3, 8))'; then
	:
else
	missing+=("python3 3.8 or later (have: $(python3 -V 2>&1 || echo none))")
fi

# The compiler MAME's make will use: $CXX, else g++ (Linux) or clang++.
CXXC=${CXX:-}
if [ -z "$CXXC" ]; then
	if [ "$OS" = Darwin ]; then CXXC=clang++; else CXXC=g++; fi
	have "$CXXC" || { have c++ && CXXC=c++; }
fi
if have "$CXXC"; then
	ver=$("$CXXC" --version 2>/dev/null | head -1)
	major=$("$CXXC" -dumpversion 2>/dev/null | cut -d. -f1)
	case "$ver" in
		*clang*) need=13 ;;
		*) need=11 ;;
	esac
	if [ -n "$major" ] && [ "$major" -lt "$need" ] 2>/dev/null; then
		missing+=("a C++20 compiler: $ver is older than $( [ $need = 13 ] && echo clang 13 || echo GCC 11)")
	fi
else
	missing+=("a C++ compiler (GCC 11+ or clang 13+)")
fi

if [ "$OS" = Linux ]; then
	if have pkg-config; then
		pc_need() { pkg-config --exists "$1" 2>/dev/null || missing+=("$2 (pkg-config: $1)"); }
		pc_need "sdl2 >= 2.0.14" "SDL2 2.0.14+ development files"
		pc_need SDL2_ttf "SDL2_ttf development files"
		pc_need fontconfig "fontconfig development files"
		pc_need x11 "libX11 development files"
		pc_need xinerama "libXinerama development files"
		pc_need xi "libXi development files"
		pc_need xext "libXext development files"
		pc_need gl "OpenGL (libGL) development files"
		pc_need alsa "ALSA development files"
		if ! pkg-config --exists libpulse 2>/dev/null; then
			optional+=("PulseAudio development files: building with NO_USE_PULSEAUDIO=1 (the emulator runs -sound none)")
			AUTO_MAKE+=(NO_USE_PULSEAUDIO=1)
		fi
		if ! pkg-config --exists Qt6Widgets 2>/dev/null; then
			optional+=("Qt 6 development files: building with USE_QTDEBUG=0 (no MAME debugger window)")
			AUTO_MAKE+=(USE_QTDEBUG=0)
		fi
	else
		missing+=("pkg-config")
	fi
elif [ "$OS" = Darwin ]; then
	xcode-select -p >/dev/null 2>&1 || missing+=("Xcode command line tools (xcode-select --install)")
	if ! have sdl2-config && [ ! -d /Library/Frameworks/SDL2.framework ]; then
		missing+=("SDL2 (brew install sdl2 sdl2_ttf, or SDL2.framework in /Library/Frameworks)")
	fi
else
	echo "warning: $OS is not a platform this project has been built on (Linux, macOS)" >&2
fi

# Disc space: MAME's checkout and build, and the media.
need_kb=3500000
[ -x mame/focus ] && need_kb=600000
avail_kb=$(df -Pk . | awk 'NR == 2 { print $4 }')
if [ -n "$avail_kb" ] && [ "$avail_kb" -lt "$need_kb" ]; then
	missing+=("disc space: $((need_kb / 1024)) MB free here, have $((avail_kb / 1024)) MB")
fi

for o in ${optional[@]+"${optional[@]}"}; do echo "   note: no $o"; done
if [ ${#missing[@]} -gt 0 ]; then
	echo "missing:" >&2
	for m in "${missing[@]}"; do echo "   $m" >&2; done
	distro=
	if [ -r /etc/os-release ]; then
		distro=$(. /etc/os-release; echo "${ID:-} ${ID_LIKE:-}")
	fi
	echo >&2
	case "$OS $distro" in
	*debian*|*ubuntu*)
		echo "On Debian/Ubuntu:" >&2
		echo "  sudo apt-get install git build-essential python3 pkg-config libsdl2-dev \\" >&2
		echo "    libsdl2-ttf-dev libfontconfig-dev libx11-dev libxinerama-dev libxi-dev \\" >&2
		echo "    libxext-dev libgl-dev libasound2-dev libpulse-dev qt6-base-dev" >&2 ;;
	*fedora*|*rhel*|*centos*)
		echo "On Fedora:" >&2
		echo "  sudo dnf install git gcc gcc-c++ make python3 pkgconf-pkg-config SDL2-devel \\" >&2
		echo "    SDL2_ttf-devel fontconfig-devel libX11-devel libXinerama-devel libXi-devel \\" >&2
		echo "    libXext-devel mesa-libGL-devel alsa-lib-devel pulseaudio-libs-devel qt6-qtbase-devel" >&2 ;;
	*arch*)
		echo "On Arch:" >&2
		echo "  sudo pacman -S --needed base-devel git python pkgconf sdl2 sdl2_ttf fontconfig \\" >&2
		echo "    libx11 libxinerama libxi libxext libglvnd alsa-lib libpulse qt6-base" >&2 ;;
	Darwin*)
		echo "On macOS (14.5+ with Xcode 16.2+, as MAME asks):" >&2
		echo "  xcode-select --install" >&2
		echo "  brew install sdl2 sdl2_ttf python3" >&2 ;;
	*)
		echo "INSTALL.md lists the packages for Debian/Ubuntu, Fedora, Arch and macOS." >&2 ;;
	esac
	exit 1
fi
echo "   ok: $("$CXXC" --version | head -1), $(python3 -V 2>&1), git $(git --version | awk '{print $3}')"
[ "$CHECK_ONLY" = 1 ] && exit 0

# ------------------------------------------------------------ 2. MAME
PIN=
if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
	PIN=$(git ls-tree HEAD mame 2>/dev/null | awk '$2 == "commit" { print $3 }')
fi
if [ -z "$PIN" ]; then
	PIN=$MAME_PIN_FALLBACK
elif [ "$PIN" != "$MAME_PIN_FALLBACK" ]; then
	echo "   note: the mame gitlink pins $PIN; tools/setup.sh's fallback says $MAME_PIN_FALLBACK"
fi
if [ -z "$SOURCE" ]; then
	SOURCE=$(git config -f .gitmodules submodule.mame.url 2>/dev/null || echo https://github.com/mamedev/mame.git)
fi
case "$SOURCE" in
	/*|http*|git@*|ssh:*|git:*|file:*) ;;
	*) [ -d "$SOURCE" ] && SOURCE=$(cd "$SOURCE" && pwd) ;;
esac

if [ -f mame/scripts/src/cpu.lua ] && [ -d mame/src/devices/cpu ]; then
	have_rev=$(git -C mame rev-parse HEAD 2>/dev/null || echo unknown)
	if [ "$have_rev" = "$PIN" ]; then
		say "MAME: mame/ is at the pinned commit ${PIN:0:12}"
	else
		say "MAME: using the tree already in mame/ (at ${have_rev:0:12}, not the pinned ${PIN:0:12})"
	fi
else
	say "MAME: fetching ${PIN:0:12} from $SOURCE (shallow: about 210 MB, 1.2 GB on disc)"
	# Register the submodule's URL in .git/config, as `git submodule init`
	# would, so later git submodule commands know it.
	git submodule init mame >/dev/null 2>&1 || true
	mkdir -p mame
	if [ ! -e mame/.git ]; then
		git -C mame init -q
	fi
	git -C mame remote remove origin >/dev/null 2>&1 || true
	git -C mame remote add origin "$SOURCE"
	# Resumable: an interrupted fetch leaves nothing half-written that a
	# second fetch trips over, so running setup again simply fetches again.
	if ! git -C mame fetch --depth 1 origin "$PIN"; then
		die "could not fetch MAME ${PIN:0:12} from $SOURCE. Check the network, or
fetch it yourself into mame/ (any clone with that commit checked out
will do) and run tools/setup.sh again."
	fi
	git -C mame -c advice.detachedHead=false checkout -q --detach FETCH_HEAD
	# Move mame/.git into .git/modules/mame, where git keeps a submodule's
	# repository, so `git submodule status` treats it as the submodule.
	git submodule absorbgitdirs mame >/dev/null 2>&1 || true
	say "MAME: checked out $(git -C mame rev-parse --short=12 HEAD)"
fi

# ------------------------------------------------------------ 3. wiring
say "wiring this tree into MAME"
tools/link_into_mame.sh | awk '/^(ok|linked|applied) / { print "   " $0 }'

# ------------------------------------------------------------ 4. build
if [ "$BUILD" = 1 ]; then
	if [ -z "$JOBS" ]; then
		cpus=$( (nproc || sysctl -n hw.ncpu) 2>/dev/null || echo 2)
		# A MAME compile job can take 1.5 GB at -O3; more jobs than memory
		# allows thrashes or gets killed, which is slower than fewer jobs.
		mem_kb=$(awk '/^MemAvailable:/ { print $2 }' /proc/meminfo 2>/dev/null || true)
		if [ -z "$mem_kb" ] && [ "$OS" = Darwin ]; then
			mem_kb=$(( $(sysctl -n hw.memsize) / 1024 ))
		fi
		JOBS=$cpus
		if [ -n "$mem_kb" ]; then
			by_mem=$((mem_kb / 1500000))
			[ "$by_mem" -lt 1 ] && by_mem=1
			[ "$by_mem" -lt "$JOBS" ] && JOBS=$by_mem
		fi
	fi
	MAKEARGS=(SUBTARGET=focus SOURCES=src/mame/hp/hp9000_5xx.cpp
		${AUTO_MAKE[@]+"${AUTO_MAKE[@]}"} ${MAKE_EXTRA[@]+"${MAKE_EXTRA[@]}"})
	# MAME's build generates its project files once; a different option set
	# needs them regenerated (REGENIE=1), so remember what the last build used.
	mkdir -p var/setup
	OPTS_FILE=var/setup/make-options
	if [ -f "$OPTS_FILE" ] && [ "$(cat "$OPTS_FILE")" != "${MAKEARGS[*]}" ]; then
		echo "   the make options changed since the last build: regenerating MAME's projects"
		MAKEARGS+=(REGENIE=1)
	fi
	say "building mame/focus with $JOBS jobs: make ${MAKEARGS[*]}"
	echo "   a first build compiles MAME's core: 20-40 minutes on an idle 8-core"
	echo "   machine, longer on fewer cores; the log is var/setup/build.log"
	run_make() {
		(cd mame && make -j"$JOBS" "${MAKEARGS[@]}") > var/setup/build.log 2>&1 &
		mpid=$!
		# A line of progress a minute, so a long build is visibly alive.
		while kill -0 "$mpid" 2>/dev/null; do
			for _ in $(seq 60); do kill -0 "$mpid" 2>/dev/null || break; sleep 1; done
			kill -0 "$mpid" 2>/dev/null && echo "   $(grep -c '^Compiling' var/setup/build.log) files compiled..."
		done
		wait "$mpid"
	}
	set +e
	run_make; rc=$?
	# A MAME tree built before the compiler was last upgraded keeps a
	# precompiled header the new compiler refuses ("PCH files were found,
	# but they were invalid"), and make does not know to rebuild it: every
	# later build fails on its first file. Remove the stale header and build
	# again; everything that includes it is recompiled, which is most of
	# MAME's core, so this takes about as long as a first build.
	if [ "$rc" != 0 ] && grep -q 'PCH files were found, but they were invalid' var/setup/build.log; then
		echo "   MAME's precompiled header was made by an older compiler; removing it"
		echo "   and building again (this recompiles MAME's core: as long as a first build)"
		find mame/build -name '*.gch' -exec rm -rf {} + 2>/dev/null
		run_make; rc=$?
	fi
	set -e
	echo "${MAKEARGS[*]}" | sed 's/ REGENIE=1$//' > "$OPTS_FILE"
	if [ "$rc" != 0 ]; then
		tail -20 var/setup/build.log >&2
		die "the MAME build failed (exit $rc); the whole log is var/setup/build.log.
If the failure is a warning treated as an error on a newer compiler, try:
  tools/setup.sh -- NOWERROR=1"
	fi
	# (Into a variable, not straight into grep -q: under pipefail an early
	# grep exit could fail the pipeline on SIGPIPE.)
	listed=$(./mame/focus -listfull hp9040 2>/dev/null || true)
	if ! printf '%s\n' "$listed" | grep '^hp9040' >/dev/null; then
		die "mame/focus was built but does not know the hp9040 machine:
is tools/mame/focus-mame.patch applied? (tools/link_into_mame.sh --check)"
	fi
	say "built mame/focus"
else
	say "skipping the build (--no-build). To build later: tools/setup.sh --no-media"
fi

# ------------------------------------------------------------ 5. media
if [ "$MEDIA" = 1 ]; then
	# HP's software is not in the repository: MEDIA.txt points at it, and
	# this downloads it (46 MB, from bitsavers.org or its mirror), checking
	# every file's SHA-256. The manuals are a separate, larger download:
	# tools/fetch-media.sh --manuals.
	say "fetching Hewlett-Packard's software (MEDIA.txt)"
	tools/fetch-media.sh | sed 's/^/   /' || die "the software could not be fetched; see the messages above.
Run tools/setup.sh again when the network is there (finished files are kept),
or tools/setup.sh --no-media to stop after the build."
	say "preparing the HP-UX 5.30 machine"
	tools/runhpux.sh --prepare | awk '/== volume header/ { skip = 1 } !skip { print "   " $0 }'
	# BASIC's fixed disc for tools/runbasic-hd.sh: HP's two boot
	# floppies, with the ERRORS_ENG, IO and MS option binaries and the
	# CAT_BOOT and BUILD_BOOT utilities off the first option disc. That is
	# the disc src/test/basictest.sh's figures were taken on (the options
	# change AVAILABLE MEMORY, and bring the error texts and CREATE).
	# software/basic/img holds the floppies' raw decodes (golden.sh and
	# older recipes name them there); both directories are gitignored.
	# Both 520 discs are built from the HP Computer Museum's floppies, which
	# are downloaded by hand (MEDIA.txt). Without them the 540 and HP-UX
	# 5.30 are complete; the 520's discs are built when the floppies arrive
	# and this script is run again.
	have_floppies=1
	for f in software/basic/500BT1.TD0 software/basic/500BT2.TD0 software/basic/500OPT1.TD0 \
			software/test/500ENV1.TD0 software/test/500ENV2.TD0 software/test/500ENV3.TD0; do
		[ -f "$f" ] || have_floppies=0
	done
	if [ "$have_floppies" = 0 ] && { [ ! -f software/hd/basic.img ] || [ ! -f software/hd/diag.img ]; }; then
		say "the 520's discs (BASIC, the diagnostics) are not built: its floppies are not here"
		echo "   They come from the HP Computer Museum by hand. tools/fetch-media.sh --list names"
		echo "   the pages; then: tools/fetch-media.sh --from <folder or .zip> && tools/setup.sh --no-build"
	fi
	if [ ! -f software/hd/basic.img ] && [ "$have_floppies" = 0 ]; then
		:
	elif [ ! -f software/hd/basic.img ]; then
		say "building BASIC's fixed disc, software/hd/basic.img"
		mkdir -p software/basic/img software/hd
		for d in 500BT1 500BT2; do
			[ -f software/basic/img/$d.img ] || \
				python3 tools/td0.py --raw software/basic/img/$d.img software/basic/$d.TD0 >/dev/null
		done
		opt=$(mktemp -d software/hd/opt.XXXXXX)
		python3 tools/td0.py --raw "$opt/500OPT1.img" software/basic/500OPT1.TD0 >/dev/null
		python3 tools/sdfread.py --extract "$opt/files" "$opt/500OPT1.img" >/dev/null
		python3 tools/mkharddisc.py software/hd/basic.img.new >/dev/null
		python3 tools/installbasic.py software/hd/basic.img.new \
			--basic software/basic/img/500BT1.img software/basic/img/500BT2.img \
			--option "$opt/files/ERRORS_ENG" "$opt/files/IO" "$opt/files/MS" \
			--copy software/basic/500OPT1.TD0:CAT_BOOT,BUILD_BOOT >/dev/null
		rm -rf "$opt"
		mv -f software/hd/basic.img.new software/hd/basic.img
	else
		# An existing disc is kept: it may hold the user's own BASIC files.
		# One built by an older sdfwrite.py is named, with the way out.
		source tools/lib/focus.sh
		focus_basic_disc_warn software/hd/basic.img
	fi
	# The 9020 diagnostics disc: HP's System Functional Test environment
	# (the three 500ENV floppies) installed on a fixed disc of its own.
	# The release tests run the test driver on it (hpuxtest.py's diag
	# case), and docs/DIAGNOSTICS.md's recipes boot it.
	if [ ! -f software/hd/diag.img ] && [ "$have_floppies" = 0 ]; then
		:
	elif [ ! -f software/hd/diag.img ]; then
		say "building the 9020 diagnostics disc, software/hd/diag.img"
		tools/mkdiagdisc.sh software/hd/diag.img.new >/dev/null
		mv -f software/hd/diag.img.new software/hd/diag.img
	else
		source tools/lib/focus.sh
		focus_basic_disc_warn software/hd/diag.img
	fi
else
	say "skipping the media (--no-media); tools/runhpux.sh prepares them on first use"
fi

echo
echo "Ready. Next:"
[ -x mame/focus ] && echo "  tools/smoketest.sh                 a one-minute check that HP-UX boots here"
echo "  tools/runhpux.sh --multiuser       HP-UX 5.30, the console in a window, six terminal ports"
echo "  INSTALL.md                         logging in, more terminals, stopping, resetting"
