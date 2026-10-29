#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Joel Heikkila
#
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
#     --headless         no window; the console is on TCP (--console tcp)
#     --seconds N        stop after N emulated seconds, unthrottled (batch);
#                        -str N / -seconds_to_run N mean the same
#     --console MODE     window | tcp | pty -- where the console is
#                        (tcp: port N below, `tools/hpuxterm.py console`)
#     --ports MODE       tcp (default) | pty | none -- how the six terminal
#                        ports of the multiplexer reach this host
#     --port-base N      the console's TCP port, if tcp; tty0K is N+K
#                        (default 4000, so tty01..tty06 are 4001..4006;
#                        HPFOCUS_PORT_BASE sets it for this script and for
#                        tools/hpuxterm.py alike)
#     --ram MB           2 (default), 3 or 4 MB of memory
#     --card 27140|27130 the multiplexer --multiuser fits (default 27140)
#     --sc N             its select code (default 2; 0 with --card 27130 is
#                        the disc's own machine, the console on its port 0)
#
#   which machine
#     --state DIR        the working machine to boot (default var/hpux530)
#     --scratch          boot a throwaway copy of the pristine machine;
#                        nothing this run does is kept
#     --fresh            put the working machine back to pristine, then boot
#     --disc FILE        boot this disc image as it is (with the battery in
#                        DIR/nvram beside it); no copies, no configuration
#
#   other
#     --reset            put the working machine back to pristine and exit
#     --status           say what the working machine is and exit
#     --prepare          make the pristine machine and the working copy, exit
#     --dry-run          print the MAME command line instead of running it
#     -h, --help         this
#
# Anything else on the command line is passed to MAME, after the script's
# own arguments, so it wins: e.g. -cio3 asi, -debug.
#
# THE DISC AND THE BATTERY ARE STATE (tools/lib/hpux.sh has the layout).
# The tapes are assembled once into a read-only pristine disc; runs boot
# a working copy of it, var/hpux530/disc.bin, with its own battery (the
# SCM's clock and configuration memory, in var/hpux530/nvram), and keep
# what HP-UX writes there. --reset (or deleting var/hpux530) is the way
# back. What a boot does depends on that state, so say whether it was
# --fresh when you report a problem; `tools/runhpux.sh --status` says what
# the machine is. Stopping the emulator is a power cut to HP-UX: its
# buffer cache holds what it has not yet written, the free blocks it has
# taken for swap space stay marked in use, and HP-UX does not check its
# file system at boot (its /etc/bcheckrc is empty). So stop HP-UX first:
# `cd /` and `/etc/stopsys` at a root shell, and when it has said "The
# system is stopped." stop the run. (`sync` instead keeps the files and
# leaks the swap blocks, 0.3 to 0.9 MB a time: KNOWN-ISSUES.md.)
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
# that card. --multiuser fits an HP 27140A six-channel multiplexer at -cio2
# (tty0K on its port K-1), or with --card 27130 a 27130A (tty0K on port K).
# Re-card any slot, or fit more, with -cio0 .. -cio6.
#
# The clock: MAME sets the SCM's clock from the host's at every power-up,
# and HP-UX keeps GMT in the chip and converts with its TZ (PST8PDT on this
# disc). HP-UX rewrites a battery it has never initialised, so the pristine
# battery is primed once (tools/lib/hpux.sh) and every machine copies it.

set -eu
source "$(dirname "${BASH_SOURCE[0]}")/lib/focus.sh"
source "$(dirname "${BASH_SOURCE[0]}")/lib/hpux.sh"
focus_repo
hpux_paths

usage() { sed -n '5,/^# Anything else/p' "$0" | sed '$d; s/^# \{0,1\}//'; }

# ------------------------------------------------------------------ options
RELEASE=5.30
MULTIUSER=0
HEADLESS=0
SECONDS_RUN=
CONSOLE=
PORTS=tcp
PORT_BASE=${HPFOCUS_PORT_BASE:-4000}
RAM=2
CARD=27140
SC=2
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
		--card) need "$@"; CARD=$2; shift ;;
		--card=*) CARD=${1#--card=} ;;
		--sc) need "$@"; SC=$2; shift ;;
		--sc=*) SC=${1#--sc=} ;;
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
	2|3|4) ;;
	*) echo "--ram takes 2, 3 or 4 (MB): what HP-UX 5.30 has been measured on" >&2; exit 2 ;;
esac
case "$CARD" in
	27140) MUXOPT=mux ;;
	27130) MUXOPT=mux8 ;;
	*) echo "--card takes 27140 or 27130" >&2; exit 2 ;;
esac
if [ "$SC" = 0 ] && [ "$CARD" != 27130 ]; then
	echo "select code 0 is the console's, and /dev/console is an HP27128/30 port; only --card 27130 can take it" >&2
	exit 2
fi
[ -n "$SECONDS_RUN" ] && HEADLESS=1
case " $* " in *" -video none "*) HEADLESS=1 ;; esac
if [ -z "$CONSOLE" ]; then
	[ "$HEADLESS" = 1 ] && CONSOLE=tcp || CONSOLE=window
fi
case "$CONSOLE" in
	window) [ "$HEADLESS" = 1 ] && { echo "--console window needs a window; drop --headless" >&2; exit 2; } ;;
	tcp|pty) HEADLESS=1 ;;
	socket) CONSOLE=tcp; HEADLESS=1 ;;
	stdout) echo "--console stdout needs the standalone emulator's console tap, which the MAME driver does not have; use tcp or pty" >&2; exit 2 ;;
	*) echo "--console takes window, tcp or pty" >&2; exit 2 ;;
esac

# ------------------------------------------------------------------ actions
# The legacy disc: runhpux.sh used to boot software/cct/7914_hpux_volume.bin
# in place. It is accumulated state, not pristine, so it is neither adopted
# nor deleted; it is named, once per run, with the way to keep using it.
legacy_note() {
	local old=$CCT/7914_hpux_volume.bin
	[ -f "$old" ] || return 0
	echo "note: $old is from an earlier runhpux.sh and is no longer booted."
	echo "      To keep using it: mv it to $STATE_DEFAULT/disc.bin, or boot it with --disc."
}

status() {
	echo "pristine disc   $( [ -f "$PRISTINE_DISC" ] && echo "$PRISTINE_DISC" || echo "not assembled yet")"
	echo "pristine battery $( [ -f "$PRISTINE_NV" ] && echo "$PRISTINE_NV" || echo "not made yet (needs the emulator)")"
	echo "machine         $STATE"
	if [ -f "$STATE/disc.bin" ]; then
		[ -f "$STATE/ORIGIN" ] && sed 's/^/                /' "$STATE/ORIGIN"
		echo "  last written  $(date -r "$STATE/disc.bin" '+%Y-%m-%d %H:%M:%S' 2>/dev/null || echo '?')"
		echo "  battery       $( [ -f "$STATE/nvram/hp9k540/scm_rtc" ] && echo present || echo 'none (the next boot is a first boot)')"
		python3 tools/hpuxconfig.py "$STATE/disc.bin" --check 2>&1 | sed 's/^/  /'
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

case "$RELEASE" in
5.01|501)
	# The kernel is rebuilt from the tape whenever it is missing: a derived
	# file like the assembled 5.30 disc, and the tape is never written.
	# The three drivers spliced in are the ones this machine can actually
	# use -- the internal flexible disc, the serial interface and the memory
	# volume -- which is the customize step HP's installer did.
	TAPE="$FOCUS_REPO/software/97089C.hpi"
	KERNEL="$CCT/hpux501.bin"
	if [ ! -f "$KERNEL" ]; then
		focus_need_media "$TAPE" || exit 1
		echo "building the 5.01 boot image from the release tape..."
		python3 "$FOCUS_REPO/tools/hpuxboot.py" "$TAPE" -o "$KERNEL" \
			--option HPIB_FLEX --option SERIAL --option MEMORY_VOL || exit 1
	fi
	# 5.01 has no root of its own on the tape, so a copy of the 5.30 volume
	# is what it is given to mount, on an HP-IB card at select code 5 -- a
	# 520's own select codes 0, 1, 6 and 7 are its display, graphics,
	# keyboard and internal disc. The kernel boots from a fixed disc built
	# around it. Both are written by the running system, so they live in
	# their own machine, var/hpux501, never the 5.30 one.
	M501=$VAR/hpux501
	mkdir -p "$M501/nvram"
	if [ -n "$DISC_GIVEN" ]; then
		ROOT501=$DISC_GIVEN
	else
		ROOT501=$M501/root.bin
		if [ ! -f "$ROOT501" ] && [ "$DRYRUN" = 0 ]; then
			hpux_make_pristine
			hpux_copy "$PRISTINE_DISC" "$ROOT501"
		fi
	fi
	BOOT=$M501/boot.img
	if [ ! -f "$BOOT" ] && [ "$DRYRUN" = 0 ]; then
		python3 "$FOCUS_REPO/tools/mkharddisc.py" "$BOOT" >/dev/null || exit 1
		python3 "$FOCUS_REPO/tools/installbasic.py" "$BOOT" --boot-image "$KERNEL" >/dev/null || exit 1
	fi
	MARGS=(-ram 2m -hard1 "$(focus_hd "$BOOT" boot)" -cio5 hpib
		-hard2 "$(focus_hd "$ROOT501" root)" -nvram_directory "$M501/nvram")
	[ "$HEADLESS" = 1 ] && MARGS+=(-video none)
	[ -n "$SECONDS_RUN" ] && MARGS+=(-str "$SECONDS_RUN")
	focus_args hp9k520 "${MARGS[@]}" "$@"
	if [ "$DRYRUN" = 1 ]; then printf '%q ' "${FOCUS_CMD[@]}"; echo; exit 0; fi
	cd "$FOCUS_REPO/mame"
	exec "${FOCUS_CMD[@]}"
	;;
5.30|530)
	;;
*)
	echo "unknown release '$RELEASE'; this tree has 5.30 and 5.01" >&2
	exit 2
	;;
esac

# ------------------------------------------------------------------ 5.30
CLEANUP_DIRS=()
RUNDIR=
HUB_PID=
MAME_PID=
cleanup() {
	if [ -n "$HUB_PID" ]; then
		kill "$HUB_PID" 2>/dev/null && wait "$HUB_PID" 2>/dev/null
		rm -f "$RUNDIR/hub.ready"
	fi
	[ -n "${STOPFILE:-}" ] && rm -f "$STOPFILE"
	hpux_unlock
	local d
	for d in ${CLEANUP_DIRS[@]+"${CLEANUP_DIRS[@]}"}; do
		rm -rf "$d"/disc.bin "$d"/disc.hd "$d"/nvram "$d"/ORIGIN \
			"$d"/hub.ready "$d"/stop "$d"/run.pid
		rmdir "$d" 2>/dev/null || true
	done
}
trap cleanup EXIT

if [ -n "$DISC_GIVEN" ]; then
	[ "$SCRATCH" = 0 ] && [ "$FRESH" = 0 ] || { echo "--disc boots a disc as it is; it does not combine with --scratch or --fresh" >&2; exit 2; }
	[ -f "$DISC_GIVEN" ] || hpux_die "no disc image at $DISC_GIVEN"
	DISC=$DISC_GIVEN
	STATE=$(dirname "$DISC")
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
	RUNDIR=$STATE
	[ "$SCRATCH" = 1 ] || legacy_note
fi
NVDIR=$STATE/nvram
mkdir -p "$NVDIR"

# What init starts in, and for run level 2 the multiplexer's gettys and
# device numbers (tools/hpuxconfig.py; it changes nothing on a disc already
# as asked, and never undoes what HP-UX or its user has done).
LEVEL=s
[ "$MULTIUSER" = 1 ] && LEVEL=2
if [ -z "$DISC_GIVEN" ] && [ "$DRYRUN" = 0 ]; then
	python3 tools/hpuxconfig.py "$DISC" --runlevel "$LEVEL" --card "$CARD" --sc "$SC" >/dev/null || exit 1
fi

MARGS=(-ram "${RAM}m" -hard "$(focus_hd "$DISC" disc)" -nvram_directory "$NVDIR")
user_set() { local a; for a in ${ARGS[@]+"${ARGS[@]}"}; do [ "$a" = "$1" ] && return 0; done; return 1; }

# The terminals this run puts on the host: "NAME SLOT" pairs, NAME being
# console or the tty number. A 27130A at select code 0 replaces the console
# card, and its port 0 is the console.
TERMS=()
CONSOLE_SLOT=cio0:asi:rs232
if [ "$MULTIUSER" = 1 ] && [ "$SC" = 0 ]; then
	CONSOLE_SLOT=cio0:mux8:port0
	user_set -cio0 || MARGS+=(-cio0 mux8)
fi
if [ "$CONSOLE" = tcp ] || [ "$CONSOLE" = pty ]; then
	TERMS+=("console $CONSOLE_SLOT")
elif [ "$CONSOLE_SLOT" != cio0:asi:rs232 ]; then
	MARGS+=("-$CONSOLE_SLOT" terminal)
fi
for a in ${ARGS[@]+"${ARGS[@]}"}; do
	case "$a" in
		-cio[0-6]:mux*:port*|-bitb*|-bitbanger*)
			if [ "$PORTS" != none ]; then
				echo "note: ports fitted on the command line; the script fits none of its own" >&2
				PORTS=none
			fi
			[ "$CONSOLE" = tcp ] && { echo "--console tcp cannot be combined with a -bitb of your own" >&2; exit 2; } ;;
	esac
done
if [ "$MULTIUSER" = 1 ]; then
	if [ "$SC" != 0 ] && user_set "-cio$SC"; then
		: # the caller has said what that slot holds; ports are theirs to fit
	else
		[ "$SC" = 0 ] || MARGS+=("-cio$SC" "$MUXOPT")
		if [ "$PORTS" != none ]; then
			for k in 1 2 3 4 5 6; do
				if [ "$CARD" = 27130 ]; then p=$k; else p=$((k - 1)); fi
				TERMS+=("$k cio$SC:$MUXOPT:port$p")
			done
		fi
	fi
fi

# TCP terminals go through tools/hpuxterm.py's hub, which holds the public
# ports (4000 the console, 4001.. tty01..), keeps the last screenful for a
# user who connects late, and lets a user come and go without MAME seeing
# a disconnect. MAME connects to the hub's inner ports.
mode_of() { [ "$1" = console ] && echo "$CONSOLE" || echo "$PORTS"; }
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
		via=$name; [ "$PORT_BASE" = 4000 ] || via="--base $PORT_BASE $name"
		CONNECT+=("$(printf '  %-8s tools/hpuxterm.py %-8s (TCP 127.0.0.1:%s)' "$dev" "$via" "$pub")")
		;;
	pty)
		MARGS+=("-$slot" pty)
		;;
	esac
done

# The clean stop: tools/mame/stopfile.lua exits MAME when this file
# appears, which writes the battery as a closed window does; a headless
# MAME takes no notice of a signal.
STOPFILE=$RUNDIR/stop
rm -f "$STOPFILE"
user_set -autoboot_script || MARGS+=(-autoboot_script "$FOCUS_REPO/tools/mame/stopfile.lua")
export HPFOCUS_STOPFILE="$STOPFILE"
[ "$HEADLESS" = 1 ] && MARGS+=(-video none)
[ -n "$SECONDS_RUN" ] && MARGS+=(-str "$SECONDS_RUN")
focus_args hp9k540 "${MARGS[@]}" "$@"

if [ "$DRYRUN" = 1 ]; then
	printf '%q ' "${FOCUS_CMD[@]}"; echo
	exit 0
fi

# Say what this is and how to reach it.
{
	echo "HP-UX 5.30 on the emulated HP 9000/540 -- $( [ "$MULTIUSER" = 1 ] && echo "multiuser (run level 2)" || echo "single user")"
	echo "  machine   $DISC"
	echo "  battery   $NVDIR"
	[ "$SCRATCH" = 1 ] && echo "  (scratch: nothing this run does is kept)"
	case "$CONSOLE" in
		window) echo "  console   the MAME window (Scroll Lock toggles MAME's own keys; Esc in the UI quits)" ;;
		tcp) ;;
		pty) echo "  console   a pseudo terminal, named below once MAME opens it" ;;
	esac
	for c in ${CONNECT[@]+"${CONNECT[@]}"}; do echo "$c"; done
	if [ "$MULTIUSER" = 1 ] && [ "$PORTS" = pty ]; then
		echo "  tty01-06  pseudo terminals, named below once MAME opens them"
		echo "            (attach with tools/hpuxterm.py /dev/pts/N)"
	fi
	if [ "$MULTIUSER" = 1 ]; then
		echo "  login     root, no password; Return at the two TERM = prompts"
		echo "            (the logins appear within a minute of power-on)"
	else
		echo "  shell     root's shell on the console; Return at its TERM = prompt"
	fi
	how=$( [ "$CONSOLE" = window ] && echo "close the window" || echo "^C here")
	if [ -n "$SECONDS_RUN" ]; then
		echo "  stops     after $SECONDS_RUN emulated seconds"
	elif [ "$SCRATCH" = 1 ]; then
		echo "  stop      $how"
	else
		echo "  stop      at a root shell: cd /   then   /etc/stopsys"
		echo "            and after \"The system is stopped.\", $how"
	fi
	echo
} >&2

# A window needs a display. Without one MAME says only "Could not
# initialize SDL No available video device" and exits.
if [ "$CONSOLE" = window ] && [ "$(uname -s)" = Linux ] \
		&& [ -z "${DISPLAY:-}" ] && [ -z "${WAYLAND_DISPLAY:-}" ]; then
	echo "note: no DISPLAY or WAYLAND_DISPLAY is set, so MAME has nowhere to open its" >&2
	echo "      window (an ssh session?). Without one, use --console tcp" >&2
	echo "      (tools/hpuxterm.py console) or --console pty." >&2
fi

on_signal() {
	SIGNALS=$((SIGNALS + 1))
	case "$SIGNALS" in
		1) echo "[runhpux: stopping cleanly -- the disc and battery are being saved; ^C again kills it]" >&2
		   # A trap must not die of set -e: if the stop file cannot be
		   # made, the next best stop is MAME's own.
		   : > "$STOPFILE" 2>/dev/null || kill -TERM "$MAME_PID" 2>/dev/null || true ;;
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
"${FOCUS_CMD[@]}" <&0 &
MAME_PID=$!
trap on_signal INT TERM HUP
status=0
while :; do
	wait "$MAME_PID" && status=0 || status=$?
	kill -0 "$MAME_PID" 2>/dev/null || break
done
# MAME has gone. A further ^C must not kill this script before its EXIT
# trap has stopped the hub and removed the lock: a terminal's ^C reaches
# MAME as well as this script, so MAME is often gone by the second one.
trap '' INT TERM HUP
exit "$status"
