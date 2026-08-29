#!/bin/bash
# Boot HP-UX 5.30 on the emulated HP 9000/540: an ASI terminal window for
# the console, and the 7914 unit-save image mounted as the CS/80 system
# disc on the HP-IB card, booted through the emulated loader.
#
# The RTC and configuration NVM persist in software/cct/scm.nv, standing
# for the SCM's battery: the FIRST boot finds an unset clock, sets it and
# writes a default configuration; later boots find them valid, which HP-UX
# requires before it will bring up its device world. Delete the file to
# simulate a dead battery.
#
# The machine is all backplane: the console serial card is -cio0 and the
# system disc's HP-IB card -cio5, where the CE handbook's installation
# procedure puts them, and the 7914 unit save mounts as the CS/80 drive on
# that card. Re-card either slot, or fit more, with -cio0 .. -cio6.
#
# THE MACHINE BOOTS TO MULTIUSER. The loader reads the image off the
# CS/80 disc, the kernel relocates all 27 modules, types the cards and
# brings up its device world, mounts the root, and init runs /etc/rc and
# starts a getty on the console -- and, with --multiuser, on the six ports
# of an HP 27140A multiplexer at select code 2, each of which accepts a
# root login (docs/HPUX.md 10). Without --multiuser the disc boots as it
# was imaged: to init's single-user shell after the boot report,
# /etc/copyright, /etc/motd and tset's TERM prompt (docs/HPUX.md 9).
#
# Typed input: FOCUS_TYPE='...' FOCUS_TYPEAT=<s>,... types into the
# console, or into a MUX port with FOCUS_TYPEPORT=2:<port>; FOCUS_MUXTAP
# prints a port's output. Interactive ports: -cio2:mux6:port0 terminal,
# or -cio2:mux6:port0 null_modem -bitb socket.127.0.0.1:4000 and nc.

set -e
cd "$(dirname "$0")/.."
CCT="$PWD/software/cct"
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
MULTIUSER=0
ARGS=()
for arg in "$@"; do
    case "$arg" in
        --multiuser) MULTIUSER=1 ;;
        *) ARGS+=("$arg") ;;
    esac
done
set -- "${ARGS[@]}"
if [ "$MULTIUSER" = 1 ]; then
    python3 tools/hpuxconfig.py "$DISC" || exit 1
    case " $* " in
        *" -cio2 "*) ;;
        *) set -- -cio2 mux6 "$@" ;;
    esac
fi

if [ ! -x mame/focus ]; then
    echo "mame/focus is not built; run:"
    echo "  cd mame && make SUBTARGET=focus SOURCES=src/mame/hp/hp9000_5xx.cpp -j8"
    exit 1
fi

cd mame
# Throttling. A batch run wants to go as fast as it can; an interactive
# one wants the window to keep up. Unthrottled, a machine that idles in
# STOP races its emulated clock to thousands of percent, and MAME spends
# so long emulating between presentations that the window never draws and
# the event pump never sees a close or a signal -- a blank window you have
# to kill by hand. So -nothrottle is added only for a run that ends by
# itself, which is what -seconds_to_run means.
THROTTLE=()
for arg in "$@"; do
    case "$arg" in
        -seconds_to_run|-str) THROTTLE=(-nothrottle); break ;;
    esac
done

# Video backend. MAME defaults to opengl, and a driver that cannot present
# through it leaves a window that renders the blinking cursor and never
# updates the text -- the machine running perfectly behind a screen that
# looks dead. This display is 640x390 of character cells, so the software
# renderer costs nothing and always works. A -video on the command line
# comes after this one and wins, so any backend can still be asked for.
# A headless run has no business opening a window. MAME initialises SDL's
# video subsystem even for -video none, which costs real time on a loaded
# desktop and, on this machine, intermittently leaves the emulated boot
# dead. SDL_VIDEODRIVER=dummy gives SDL a backend that draws nowhere.
case " $* " in
    *" -video none "*) export SDL_VIDEODRIVER=dummy ;;
esac

VIDEO=(-video soft)

# RAM. The machine default is HP's 1 MB base configuration, but the
# HP-UX image is 597 KB and its relocated world reaches past 0xb2000:
# together they cannot fit under 1 MB, so this boot pins the two 1 MB
# boards it was brought up and measured on. Fit different boards by
# editing here; a second -mpb3 on the command line is a MAME error.
exec env FOCUS_SCMNV="$CCT/scm.nv" \
    ./focus hp9040 -mpb3 1m -mpb4 1m \
    -hard1 "$DISC" "${THROTTLE[@]}" "${VIDEO[@]}" -sound none "$@"
