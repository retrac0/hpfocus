#!/bin/bash
# Boot HP-UX on an emulated Series 500.
#
#   tools/runhpux.sh                  5.30 on the 540, to the single-user shell
#   tools/runhpux.sh --multiuser      5.30 on the 540, to a login on every port
#   tools/runhpux.sh --release 5.01   5.01 on the 520, from the release tape
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
# that card. Re-card either slot, or fit more, with -cio0 .. -cio6.
#
# The RTC and configuration NVM persist in software/cct/scm.nv, standing
# for the SCM's battery: the FIRST boot finds an unset clock, sets it and
# writes a default configuration; later boots find them valid, which HP-UX
# requires before it will bring up its device world. Delete the file to
# simulate a dead battery.
#
# Typed input and listening: FOCUS_TAP names the terminals a run listens
# to ('console', '<sc>:<port>'), FOCUS_TYPE='@<term>=...' types at them,
# FOCUS_TYPEAT=<s>,... says when. FOCUS_HELP=1 lists every knob.
# Interactive ports: -cio2:mux6:port0 terminal, or -cio2:mux6:port0
# null_modem -bitb socket.127.0.0.1:4000 and nc.

set -e
source "$(dirname "${BASH_SOURCE[0]}")/lib/focus.sh"
focus_repo
CCT="$FOCUS_REPO/software/cct"

# ------------------------------------------------------------------ options
RELEASE=5.30
MULTIUSER=0
ARGS=()
while [ $# -gt 0 ]; do
	case "$1" in
		--multiuser) MULTIUSER=1 ;;
		--release) RELEASE=$2; shift ;;
		--release=*) RELEASE=${1#--release=} ;;
		*) ARGS+=("$1") ;;
	esac
	shift
done
set -- "${ARGS[@]}"

focus_built

case "$RELEASE" in
5.01|501)
	# The kernel is rebuilt from the tape whenever it is missing: it is a
	# derived file like the assembled 5.30 disc, and the tape is never
	# written. The three drivers spliced in are the ones this machine can
	# actually use -- the internal flexible disc, the serial interface and
	# the memory volume -- which is the customize step HP's installer did.
	TAPE="$FOCUS_REPO/software/97089C.hpi"
	KERNEL="$CCT/hpux501.bin"
	[ -f "$TAPE" ] || { echo "no release tape at $TAPE"; exit 1; }
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
	DISC="$CCT/7914_hpux_volume.bin"
	[ -f "$DISC" ] || { echo "no system disc at $DISC; run tools/runhpux.sh once"; exit 1; }
	# And it mounts READ-ONLY. 5.01 does reach that file system and does
	# write to it -- the HP-IB log is full of its writes -- but those are
	# a 5.01 kernel's writes to a 5.30 installation, which is not a
	# machine that ever existed, and letting them stick would quietly
	# spoil the one system in this tree that boots to a shell. Set
	# FOCUS_HDNOSAVE= to let them through on purpose.
	export FOCUS_HDNOSAVE="${FOCUS_HDNOSAVE-1}"
	export FOCUS_KBDNV="${FOCUS_KBDNV:-$CCT/kbd501.nv}"
	focus_run hp9020 -mpb3 1m -mpb4 1m -cio5 hpib -hard2 "$DISC" \
		-quik "$KERNEL" "$@"
	;;
5.30|530)
	;;
*)
	echo "unknown release '$RELEASE'; this tree has 5.30 and 5.01"
	exit 2
	;;
esac

# ------------------------------------------------------------------ 5.30
TAPE1="$CCT/7914_unitSave_hpux4.0.bin"
TAPE2="$CCT/7914_unitSave_hpux4.0_tape2.bin"
# The system disc is the WHOLE volume the two unit-save tapes hold, put
# back together by tools/mkhpuxdisc.py: a rebuilt block 0, tape 1, the
# two-block hole at the join, tape 2. Tape 1 alone boots the kernel but
# gives it a disc with no volume header and no inode table, and the
# device configuration pass rejects it (docs/HPUX.md 9). The assembled
# file is what HP-UX writes to, so it is a derived file, rebuilt here
# whenever it is missing; the tapes themselves are never written.
DISC="$CCT/7914_hpux_volume.bin"

for t in "$TAPE1" "$TAPE2"; do
	if [ ! -f "$t" ]; then
		echo "decompressing $(basename "$t")..."
		gunzip -k "$t.gz"
	fi
done
if [ ! -f "$DISC" ]; then
	echo "assembling the system disc from the two unit-save tapes..."
	python3 tools/mkhpuxdisc.py "$TAPE1" "$TAPE2" "$DISC" || exit 1
fi

# --multiuser: the same disc, made to boot to run level 2 with a login on
# every port of an HP 27140A six-channel multiplexer at select code 2.
# tools/hpuxconfig.py makes the three edits in place (root's passwd entry,
# the gettys in inittab, the tty01..tty06 device numbers) and is
# idempotent, so it runs on every such boot; the card is fitted unless
# the caller has already said what -cio2 holds. Without the flag nothing
# here changes.
if [ "$MULTIUSER" = 1 ]; then
	python3 tools/hpuxconfig.py "$DISC" || exit 1
	case " $* " in
		*" -cio2 "*) ;;
		*) set -- -cio2 mux6 "$@" ;;
	esac
fi

# RAM. The machine default is HP's 1 MB base configuration, but the
# HP-UX image is 597 KB and its relocated world reaches past 0xb2000:
# together they cannot fit under 1 MB, so this boot pins the two 1 MB
# boards it was brought up and measured on. Fit different boards by
# editing here; a second -mpb3 on the command line is a MAME error.
export FOCUS_SCMNV="${FOCUS_SCMNV:-$CCT/scm.nv}"
focus_run hp9040 -mpb3 1m -mpb4 1m -hard1 "$DISC" "$@"
