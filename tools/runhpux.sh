#!/bin/bash
# Boot HP-UX on an emulated Series 500.
#
#   tools/runhpux.sh                  5.30 on the 540, single user: root's
#                                     shell on the console
#   tools/runhpux.sh --multiuser      5.30 on the 540, run level 2: a login on
#                                     the console and on six terminal ports
#   tools/runhpux.sh --release 5.01   5.01 on the 520, from the release tape
#                                     (its banner; there is no 5.01 userland)
#
#   how it runs
#     (default)          a window: the window IS the console terminal
#     --headless         no window; the console is this terminal's stdout,
#                        type with FOCUS_TYPE; ^C stops the run cleanly
#     --seconds N        stop after N emulated seconds, unthrottled (batch);
#                        -str N / -seconds_to_run N mean the same
#     --console MODE     window | stdout | tcp | pty -- where the console is.
#                        tcp and pty need a build whose console card has an
#                        rs232 port (checked; the terminals lane's change)
#     --ports MODE       tcp (default) | pty | none -- how the six terminal
#                        ports of the multiplexer reach this host
#     --port-base N      the console's TCP port, if tcp; tty0K is N+K
#                        (default 4000, so tty01..tty06 are 4001..4006)
#     --ram MB           2 (default), 3 or 4 MB of memory
#
#   which machine
#     --state DIR        the working machine to boot (default var/hpux530)
#     --scratch          boot a throwaway copy of the pristine machine;
#                        nothing this run does is kept
#     --fresh            put the working machine back to pristine, then boot
#     --disc FILE        boot this disc image as it is, with the battery
#                        FOCUS_SCMNV names (or none); no copies, no config
#
#   other
#     --reset            put the working machine back to pristine and exit
#     --status           say what the working machine is and exit
#     --prepare          make the pristine machine and the working copy, exit
#     --dry-run          print the MAME command line instead of running it
#     -h, --help         this
#
# Anything else on the command line is passed to MAME, after the script's
# own arguments, so it wins: e.g. -cio2:mux6:port0 terminal, -debug.
#
# THE DISC AND THE BATTERY ARE STATE (tools/lib/hpux.sh has the layout).
# The tapes are assembled once into a read-only pristine disc; runs boot
# a working copy of it, var/hpux530/disc.bin, with its own battery file,
# and keep what HP-UX writes there. --reset (or deleting var/hpux530) is
# the way back. What a boot does depends on that state, so say whether it
# was --fresh when you report a problem; `tools/runhpux.sh --status` says
# what the machine is. Before stopping a run, `sync` at a root shell:
# HP-UX does not check its file system at boot (its /etc/bcheckrc is
# empty). TODO(storage lane): until the emulator writes HP-UX's writes
# through as they land, the disc reaches its file only when MAME exits
# cleanly -- the window closed, ^C to a run this script started, the end
# of --seconds -- and a kill -9 loses the whole run.
#
# TWO RELEASES, TWO MACHINES, AND TWO QUITE DIFFERENT KINDS OF MEDIA. It
# is worth being clear about which is which, because they arrive here by
# opposite routes:
#
#   5.30  is an INSTALLED SYSTEM. software/cct holds a unit save of a real
#         HP 7914 disc -- kernel, root file system, userland and one site's
#         live data -- so the machine boots the way that machine booted:
#         the emulated loader reads the image off the CS/80 disc and the
#         kernel mounts the disc it came from. This is the supported path
#         and the one that reaches a shell.
#
#   5.01  is a RELEASE TAPE with no file system on it, and most of it did
#         not survive imaging: software/97089C.hpi has lost SCOMMANDS
#         entirely, which is /bin and /usr/bin -- every command there is.
#         What did survive whole is the 520's kernel, so this path builds
#         that kernel into a bootable image (tools/hpuxboot.py) and hands
#         it to the 520 through the loader's own handoff. There is no 5.01
#         root to mount; see docs/HPUX.md 11 for how far it gets and on
#         what it stops.
#
# The 5.30 machine is all backplane: the console serial card is -cio0 and
# the system disc's HP-IB card -cio5, where the CE handbook's installation
# procedure puts them, and the 7914 unit save mounts as the CS/80 drive on
# that card. --multiuser fits an HP 27140A six-channel multiplexer at -cio2.
# Re-card any slot, or fit more, with -cio0 .. -cio6.
#
# The clock: the battery file is the SCM's battery. The script seeds the
# clock from the host at every power-up (FOCUS_RTCTIME=now, kept as GMT:
# FOCUS_RTCUTC=1, because HP-UX keeps GMT in the chip and converts with
# its TZ, PST8PDT on this disc); set FOCUS_RTCTIME=none for the unset
# clock, or an ISO date to pin one. A build without the clock lane's RTC
# fix ignores the seed and HP-UX dates everything February 2036.
#
# Typed input and listening: FOCUS_TAP names the terminals a run listens
# to ('console', '<sc>:<port>'), FOCUS_TYPE='@<term>=...' types at them,
# FOCUS_TYPEAT=<s>,... says when. FOCUS_HELP=1 lists every knob.

set -eu
source "$(dirname "${BASH_SOURCE[0]}")/lib/focus.sh"
source "$(dirname "${BASH_SOURCE[0]}")/lib/hpux.sh"
focus_repo
hpux_paths

usage() { sed -n '2,/^# Anything else/p' "$0" | sed '$d; s/^# \{0,1\}//'; }

# ------------------------------------------------------------------ options
RELEASE=5.30
MULTIUSER=0
HEADLESS=0
SECONDS_RUN=
CONSOLE=
PORTS=tcp
PORT_BASE=${HPFOCUS_PORT_BASE:-4000}
RAM=2
STATE=
SCRATCH=0
FRESH=0
DISC_GIVEN=
ACTION=run
DRYRUN=0
ARGS=()
need() { [ $# -ge 2 ] || { echo "$1 needs a value (try --help)" >&2; exit 2; }; }
while [ $# -gt 0 ]; do
	case "$1" in
		--multiuser|--multi) MULTIUSER=1 ;;
		--single|--singleuser) MULTIUSER=0 ;;
		--release) need "$@"; RELEASE=$2; shift ;;
		--release=*) RELEASE=${1#--release=} ;;
		--headless) HEADLESS=1 ;;
		--seconds) need "$@"; SECONDS_RUN=$2; shift ;;
		-str|-seconds_to_run) need "$@"; SECONDS_RUN=$2; shift ;;
		--console) need "$@"; CONSOLE=$2; shift ;;
		--ports) need "$@"; PORTS=$2; shift ;;
		--port-base) need "$@"; PORT_BASE=$2; shift ;;
		--ram) need "$@"; RAM=$2; shift ;;
		--state) need "$@"; STATE=$2; shift ;;
		--scratch) SCRATCH=1 ;;
		--fresh) FRESH=1 ;;
		--disc) need "$@"; DISC_GIVEN=$2; shift ;;
		--reset) ACTION=reset ;;
		--status) ACTION=status ;;
		--prepare) ACTION=prepare ;;
		--dry-run) DRYRUN=1 ;;
		-h|--help) usage; exit 0 ;;
		--) shift; ARGS+=("$@"); break ;;
		*) ARGS+=("$1") ;;
	esac
	shift
done
set -- ${ARGS[@]+"${ARGS[@]}"}

# A relative --state or --disc names a path from where the user is, but
# focus_repo has already moved to the repository root.
abspath() { case "$1" in /*) echo "$1" ;; *) echo "$OLDPWD/$1" ;; esac; }
[ -n "$STATE" ] && STATE=$(abspath "$STATE")
[ -n "$DISC_GIVEN" ] && DISC_GIVEN=$(abspath "$DISC_GIVEN")
STATE=${STATE:-$STATE_DEFAULT}

case "$PORTS" in tcp|socket) PORTS=tcp ;; pty|none) ;;
	*) echo "--ports takes tcp, pty or none" >&2; exit 2 ;; esac
case "$RAM" in
	2) RAMARGS=(-mpb3 1m -mpb4 1m) ;;
	3) RAMARGS=(-mpb3 1m -mpb4 1m -mpb5 1m) ;;
	4) RAMARGS=(-mpb3 1m -mpb4 1m -mpb5 1m -mpb6 1m) ;;
	*) echo "--ram takes 2, 3 or 4 (MB): what HP-UX 5.30 has been measured on" >&2; exit 2 ;;
esac
[ -n "$SECONDS_RUN" ] && HEADLESS=1
case " $* " in *" -video none "*) HEADLESS=1 ;; esac
if [ -z "$CONSOLE" ]; then
	[ "$HEADLESS" = 1 ] && CONSOLE=stdout || CONSOLE=window
fi
case "$CONSOLE" in
	window) [ "$HEADLESS" = 1 ] && { echo "--console window needs a window; drop --headless" >&2; exit 2; } ;;
	stdout|tcp|pty) HEADLESS=1 ;;
	socket) CONSOLE=tcp; HEADLESS=1 ;;
	*) echo "--console takes window, stdout, tcp or pty" >&2; exit 2 ;;
esac

# ------------------------------------------------------------------ actions
# The legacy disc: runhpux.sh used to boot software/cct/7914_hpux_volume.bin
# in place. It is the maintainer's accumulated state, not pristine, so it
# is neither adopted nor deleted; it is named, once per run, with the way
# to keep using it.
legacy_note() {
	local old=$CCT/7914_hpux_volume.bin
	[ -f "$old" ] || return 0
	echo "note: $old is from an earlier runhpux.sh and is no longer booted."
	echo "      To keep using it: mv it to $STATE_DEFAULT/disc.bin (and"
	echo "      software/cct/scm.nv to $STATE_DEFAULT/scm.nv), or boot it with --disc."
}

status() {
	echo "pristine disc   $( [ -f "$PRISTINE_DISC" ] && echo "$PRISTINE_DISC" || echo "not assembled yet")"
	echo "pristine battery $( [ -f "$PRISTINE_NV" ] && echo "$PRISTINE_NV" || echo "not made yet (needs the emulator)")"
	echo "machine         $STATE"
	if [ -f "$STATE/disc.bin" ]; then
		[ -f "$STATE/ORIGIN" ] && sed 's/^/                /' "$STATE/ORIGIN"
		echo "  last written  $(date -r "$STATE/disc.bin" '+%Y-%m-%d %H:%M:%S' 2>/dev/null || echo '?')"
		echo "  battery       $( [ -f "$STATE/scm.nv" ] && echo present || echo 'none (the next boot is a first boot)')"
		if python3 tools/hpuxconfig.py --help 2>/dev/null | grep -q -- --runlevel; then
			python3 tools/hpuxconfig.py "$STATE/disc.bin" --check 2>&1 | sed 's/^/  /'
		fi
		[ -f "$STATE/disc.bin.unsaved" ] && echo "  WARNING       $STATE/disc.bin.unsaved exists: a run could not save to disc.bin"
	else
		echo "  (not made yet: the next run copies the pristine machine)"
	fi
	legacy_note
}

case "$ACTION" in
reset)
	[ "$SCRATCH" = 1 ] || [ -n "$DISC_GIVEN" ] && { echo "--reset resets --state DIR (default $STATE_DEFAULT)" >&2; exit 2; }
	hpux_reset "$STATE"
	exit 0 ;;
status)
	status; exit 0 ;;
prepare)
	hpux_make_pristine
	[ -x "$FOCUS_BIN" ] && hpux_prime_battery
	hpux_state_init "$STATE"
	status; exit 0 ;;
esac

[ "$DRYRUN" = 1 ] || focus_built

# ------------------------------------------------------------------ 5.01
case "$RELEASE" in
5.01|501)
	# The kernel is rebuilt from the tape whenever it is missing: it is a
	# derived file like the assembled 5.30 disc, and the tape is never
	# written. The three drivers spliced in are the ones this machine can
	# actually use -- the internal flexible disc, the serial interface and
	# the memory volume -- which is the customize step HP's installer did.
	TAPE="$FOCUS_REPO/software/97089C.hpi"
	KERNEL="$CCT/hpux501.bin"
	[ -f "$TAPE" ] || hpux_die "no release tape at $TAPE"
	if [ ! -f "$KERNEL" ]; then
		echo "building the 5.01 boot image from the release tape..."
		python3 "$FOCUS_REPO/tools/hpuxboot.py" "$TAPE" -o "$KERNEL" \
			--option HPIB_FLEX --option SERIAL --option MEMORY_VOL || exit 1
	fi
	# 5.01 has no root of its own on the tape, so the 5.30 volume is what
	# it is given to mount -- the same disc, the same HP-IB card, one slot
	# along from where a 540 keeps it because a 520's own select codes 0,
	# 1, 6 and 7 are its display, graphics, keyboard and internal disc.
	# Without a disc at all the kernel raises escape 116 against the NULL
	# driver before it ever reaches its banner.
	#
	# It is the PRISTINE 5.30 disc, mounted READ-ONLY (FOCUS_HDNOSAVE, on
	# a file that is mode 0444 besides). 5.01 does reach that file system
	# and does write to it -- the HP-IB log is full of its writes -- but
	# those are a 5.01 kernel's writes to a 5.30 installation, which is not
	# a machine that ever existed. (FOCUS_HDNOSAVE is a presence flag: the
	# emulator takes FOCUS_HDNOSAVE= and FOCUS_HDNOSAVE=0 as set, too.)
	# --disc FILE gives it another disc; FOCUS_HDNOSAVE stays set.
	if [ -n "$DISC_GIVEN" ]; then
		DISC=$DISC_GIVEN
	else
		hpux_make_pristine
		DISC=$PRISTINE_DISC
	fi
	export FOCUS_HDNOSAVE=1
	mkdir -p "$VAR/hpux501"
	export FOCUS_KBDNV="${FOCUS_KBDNV:-$VAR/hpux501/kbd.nv}"
	# The 520's console is its own screen, which a headless run does not
	# have: FOCUS_DIMDUMP prints the frame buffer at the end of the run, and
	# tools/screen.py turns that into the screen a person would have seen --
	# here, when the output is a terminal; into a pipe or a file the dump
	# goes as it is, so the old recipe's `| tools/screen.py` still works.
	if [ "$HEADLESS" = 1 ]; then
		[ -n "$SECONDS_RUN" ] || SECONDS_RUN=100
		export FOCUS_DIMDUMP="${FOCUS_DIMDUMP-1}"
		focus_args hp9020 -mpb3 1m -mpb4 1m -cio5 hpib -hard2 "$DISC" \
			-quik "$KERNEL" -video none -str "$SECONDS_RUN" "$@"
		if [ "$DRYRUN" = 1 ]; then echo "${FOCUS_CMD[*]}"; exit 0; fi
		cd "$FOCUS_REPO/mame"
		if [ -t 1 ]; then
			"${FOCUS_CMD[@]}" | python3 "$FOCUS_REPO/tools/screen.py"
			exit "${PIPESTATUS[0]}"
		fi
		exec "${FOCUS_CMD[@]}"
	fi
	if [ "$DRYRUN" = 1 ]; then
		focus_args hp9020 -mpb3 1m -mpb4 1m -cio5 hpib -hard2 "$DISC" -quik "$KERNEL" "$@"
		echo "${FOCUS_CMD[*]}"; exit 0
	fi
	focus_run hp9020 -mpb3 1m -mpb4 1m -cio5 hpib -hard2 "$DISC" \
		-quik "$KERNEL" "$@"
	;;
5.30|530)
	;;
*)
	echo "unknown release '$RELEASE'; this tree has 5.30 and 5.01" >&2
	exit 2
	;;
esac

# ------------------------------------------------------------------ 5.30
# Which machine. A working machine is made from the pristine one the first
# time it is booted; --scratch makes one for this run only.
CLEANUP_DIRS=()
RUNDIR=
HUB_PID=
MAME_PID=
cleanup() {
	if [ -n "$HUB_PID" ]; then
		kill "$HUB_PID" 2>/dev/null && wait "$HUB_PID" 2>/dev/null
		rm -f "$RUNDIR/hub.ready"
	fi
	hpux_unlock
	local d
	for d in ${CLEANUP_DIRS[@]+"${CLEANUP_DIRS[@]}"}; do
		rm -f "$d"/disc.bin "$d"/disc.bin.unsaved "$d"/scm.nv "$d"/ORIGIN \
			"$d"/hub.ready "$d"/stop "$d"/console.log "$d"/run.pid
		rmdir "$d" 2>/dev/null || true
	done
}
trap cleanup EXIT

if [ -n "$DISC_GIVEN" ]; then
	[ "$SCRATCH" = 0 ] && [ "$FRESH" = 0 ] || { echo "--disc boots a disc as it is; it does not combine with --scratch or --fresh" >&2; exit 2; }
	[ -f "$DISC_GIVEN" ] || hpux_die "no disc image at $DISC_GIVEN"
	DISC=$DISC_GIVEN
	STATE=$(dirname "$DISC")
	NV=${FOCUS_SCMNV:-}
	RUNDIR=$VAR/run
	mkdir -p "$RUNDIR"
else
	if [ "$SCRATCH" = 1 ]; then
		mkdir -p "$VAR"
		STATE=$(mktemp -d "$VAR/scratch.XXXXXX")
		CLEANUP_DIRS+=("$STATE")
	fi
	if [ "$DRYRUN" = 0 ]; then
		hpux_lock "$STATE"
		if [ "$FRESH" = 1 ]; then
			hpux_unlock
			hpux_reset "$STATE"
			hpux_lock "$STATE"
		fi
		hpux_state_init "$STATE"
	fi
	DISC=$STATE/disc.bin
	NV=${FOCUS_SCMNV:-$STATE/scm.nv}
	RUNDIR=$STATE
	[ "$SCRATCH" = 1 ] || legacy_note
fi

# Configure the disc for the run level asked for: hpuxconfig.py is
# idempotent and atomic, and on a disc already so it writes nothing.
# s: initdefault s, init goes straight to root's shell on the console
#    (the shipped disc has no initdefault and stops at ENTER RUN LEVEL).
# 2: root's passwd entry usable, the six gettys on, the tty01..06 nodes
#    pointed at the 27140A at select code 2 -- docs/HPUX.md 10.2.
# A disc given with --disc is booted exactly as it is.
LEVEL=s
[ "$MULTIUSER" = 1 ] && LEVEL=2
if [ -z "$DISC_GIVEN" ] && [ "$DRYRUN" = 0 ]; then
	if python3 tools/hpuxconfig.py --help 2>/dev/null | grep -q -- --runlevel; then
		python3 tools/hpuxconfig.py "$DISC" --runlevel "$LEVEL" >/dev/null || exit 1
	elif [ "$LEVEL" = 2 ]; then
		# An hpuxconfig.py from before --runlevel: its only edit is this.
		python3 tools/hpuxconfig.py "$DISC" >/dev/null || exit 1
	else
		echo "note: this tools/hpuxconfig.py has no --runlevel, so init will ask"
		echo "      ENTER RUN LEVEL: type s and Return for the single-user shell."
	fi
fi

# ------------------------------------------------------------------ MAME
MARGS=("${RAMARGS[@]}" -hard1 "$DISC")
export FOCUS_SCMNV="$NV"
[ -n "$NV" ] || unset FOCUS_SCMNV
export FOCUS_RTCTIME="${FOCUS_RTCTIME-now}"
[ -n "${FOCUS_RTCUTC+x}" ] || export FOCUS_RTCUTC=1

user_set() { local a; for a in ${ARGS[@]+"${ARGS[@]}"}; do [ "$a" = "$1" ] && return 0; done; return 1; }

# Feature check: the console card's rs232 port came with the terminals
# lane's change. Ask the binary rather than guess.
console_has_rs232() {
	"$FOCUS_BIN" hp9040 -listslots 2>/dev/null | grep -q 'asi:rs232'
}
if [ "$CONSOLE" = tcp ] || [ "$CONSOLE" = pty ]; then
	if [ "$DRYRUN" = 0 ] && ! console_has_rs232; then
		echo "error: this build's console card has no rs232 port, so --console $CONSOLE" >&2
		echo "cannot work; use the window, or --headless (the console on stdout)." >&2
		exit 1
	fi
fi

# Terminals that go to the host. Each entry: name, MAME slot.
TERMS=()
if [ "$CONSOLE" = tcp ] || [ "$CONSOLE" = pty ]; then
	TERMS+=("console cio0:asi:rs232")
fi
# A caller who fits any port or bitbanger on the command line takes the
# ports over: MAME numbers its bitbangers bitb1, bitb2, ... across the
# whole machine, so this script's -bitbN and the caller's -bitb could not
# both be right (docs/HPUX.md 10.5's recipes fit their own).
for a in ${ARGS[@]+"${ARGS[@]}"}; do
	case "$a" in
		-cio2:mux6:port*|-bitb*|-bitbanger*)
			if [ "$PORTS" != none ]; then
				echo "note: ports fitted on the command line; the script fits none of its own" >&2
				PORTS=none
			fi
			[ "$CONSOLE" = tcp ] && { echo "--console tcp cannot be combined with a -bitb of your own" >&2; exit 2; } ;;
	esac
done
if [ "$MULTIUSER" = 1 ]; then
	if user_set -cio2; then
		: # the caller has said what -cio2 holds; ports are theirs to fit
	else
		MARGS+=(-cio2 mux6)
		if [ "$PORTS" != none ]; then
			for k in 1 2 3 4 5 6; do
				TERMS+=("$k cio2:mux6:port$((k - 1))")
			done
		fi
	fi
fi
# The console goes the way --console says; the ports the way --ports says.
mode_of() { [ "$1" = console ] && echo "$CONSOLE" || echo "$PORTS"; }

# TCP: through the hub (tools/hpuxterm.py), which MAME connects to, so a
# user can leave and come back. MAME names its bitbangers bitb1, bitb2, ...
# in device order (the console card, cio0, before the multiplexer, cio2)
# -- or plain bitb when there is only one.
NTCP=0
for t in ${TERMS[@]+"${TERMS[@]}"}; do
	[ "$(mode_of "${t%% *}")" = tcp ] && NTCP=$((NTCP + 1))
done
if [ "$NTCP" -gt 0 ] && [ "$DRYRUN" = 0 ]; then
	HUBNAMES=
	for t in "${TERMS[@]}"; do
		[ "$(mode_of "${t%% *}")" = tcp ] && HUBNAMES="$HUBNAMES${HUBNAMES:+,}${t%% *}"
	done
	rm -f "$RUNDIR/hub.ready"
	python3 tools/hpuxterm.py hub --ports "$HUBNAMES" --base "$PORT_BASE" \
		--ready "$RUNDIR/hub.ready" &
	HUB_PID=$!
	for _ in $(seq 50); do
		[ -f "$RUNDIR/hub.ready" ] && break
		kill -0 "$HUB_PID" 2>/dev/null || { HUB_PID=; exit 1; }
		sleep 0.1
	done
	[ -f "$RUNDIR/hub.ready" ] || hpux_die "the terminal hub did not start"
fi
n=0
CONNECT=()
for t in ${TERMS[@]+"${TERMS[@]}"}; do
	name=${t%% *}; slot=${t#* }
	case "$(mode_of "$name")" in
	tcp)
		n=$((n + 1))
		b=bitb$n; [ "$NTCP" = 1 ] && b=bitb
		inner=0
		[ "$DRYRUN" = 0 ] && inner=$(awk -v n="$name" '$1 == n { print $2 }' "$RUNDIR/hub.ready")
		MARGS+=("-$slot" null_modem "-$b" "socket.127.0.0.1:$inner")
		if [ "$name" = console ]; then pub=$PORT_BASE; dev=console; else pub=$((PORT_BASE + name)); dev=tty0$name; fi
		CONNECT+=("$(printf '  %-8s tools/hpuxterm.py %-8s (TCP 127.0.0.1:%s)' "$dev" "$name" "$pub")")
		;;
	pty)
		MARGS+=("-$slot" pty)
		;;
	esac
done

# Stopping. A run this script starts is stopped cleanly, whatever stops it:
# MAME is run as a child, and ^C, kill or a closed terminal reach this
# script, which asks MAME to exit through tools/mame/stopfile.lua (MAME
# then runs every exit notifier: the disc and the battery are written as
# at any clean exit). A second ^C sends SIGTERM, a third SIGKILL.
STOPFILE=$RUNDIR/stop
rm -f "$STOPFILE"
user_set -autoboot_script || MARGS+=(-autoboot_script "$FOCUS_REPO/tools/mame/stopfile.lua")
export HPFOCUS_STOPFILE="$STOPFILE"

[ "$HEADLESS" = 1 ] && MARGS+=(-video none)
[ -n "$SECONDS_RUN" ] && MARGS+=(-str "$SECONDS_RUN")
focus_args hp9040 "${MARGS[@]}" "$@"

if [ "$DRYRUN" = 1 ]; then
	printf '%q ' "${FOCUS_CMD[@]}"; echo
	exit 0
fi

# Say what this is and how to reach it.
{
	echo "HP-UX 5.30 on the emulated HP 9000/540 -- $( [ "$MULTIUSER" = 1 ] && echo "multiuser (run level 2)" || echo "single user")"
	echo "  machine   $DISC"
	[ -n "$NV" ] && echo "  battery   $NV"
	[ "$SCRATCH" = 1 ] && echo "  (scratch: nothing this run does is kept)"
	case "$CONSOLE" in
		window) echo "  console   the MAME window (Scroll Lock toggles MAME's own keys; Esc in the UI quits)" ;;
		stdout) echo "  console   this terminal (output); type with FOCUS_TYPE, or use --console tcp|pty" ;;
		tcp) ;;
		pty) echo "  console   a pseudo terminal, named below once MAME opens it" ;;
	esac
	for c in ${CONNECT[@]+"${CONNECT[@]}"}; do echo "$c"; done
	if [ "$MULTIUSER" = 1 ] && [ "$PORTS" = pty ]; then
		echo "  tty01-06  pseudo terminals, named below once MAME opens them"
		echo "            (a build without the terminals lane's change does not name them)"
	fi
	if [ "$MULTIUSER" = 1 ]; then
		echo "  login     root, no password; Return at the two TERM = prompts"
		echo "            (the logins appear about 90 s after power-on)"
	else
		echo "  shell     root's csh on the console; Return at its TERM = prompt"
	fi
	if [ -n "$SECONDS_RUN" ]; then
		echo "  stops     after $SECONDS_RUN emulated seconds"
	else
		echo "  stop      sync at a root shell, then $( [ "$CONSOLE" = window ] && echo "close the window" || echo "^C here")"
	fi
	echo
} >&2

on_signal() {
	SIGNALS=$((SIGNALS + 1))
	case "$SIGNALS" in
		1) echo "[runhpux: stopping cleanly -- the disc and battery are being saved; ^C again to hurry]" >&2
		   : > "$STOPFILE" ;;
		2) echo "[runhpux: SIGTERM to MAME]" >&2; kill -TERM "$MAME_PID" 2>/dev/null ;;
		*) echo "[runhpux: SIGKILL to MAME: what was not yet written is lost]" >&2; kill -KILL "$MAME_PID" 2>/dev/null ;;
	esac
}
SIGNALS=0
cd "$FOCUS_REPO/mame"
# MAME runs in the background so the traps run while it does (bash runs a
# trap only between commands, and a foreground child would hold it off
# until it exits). Its stdin is kept: a background job of a non-
# interactive shell would otherwise get /dev/null.
# In a window the console is the window, and what the tap echoes to stdout
# (every byte the console card sends) goes to console.log instead: it is a
# transcript for a bug report, not something to read while typing. Pseudo
# terminal names are printed on stdout too, so a run with ptys keeps it.
if [ "$CONSOLE" = window ] && [ "$PORTS" != pty ]; then
	echo "(the console transcript goes to $RUNDIR/console.log)" >&2
	"${FOCUS_CMD[@]}" <&0 > "$RUNDIR/console.log" &
else
	"${FOCUS_CMD[@]}" <&0 &
fi
MAME_PID=$!
trap on_signal INT TERM HUP
status=0
while :; do
	wait "$MAME_PID" && status=0 || status=$?
	kill -0 "$MAME_PID" 2>/dev/null || break
done
trap - INT TERM HUP
exit "$status"
