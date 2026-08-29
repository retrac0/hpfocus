#!/usr/bin/env python3
"""Build a bootable HP-UX kernel image from the 97089C release tape.

The 5.01 tape ships the kernel the way `osmgr` installs it, not the way a
loader hands it to the CPU. Two things have to happen to it:

  * The stored chain simply ENDS. `SYSTEM POWERUP` walks the segment chain
    verifying byte-sum checksums and stops on a negative word; with no
    terminator it walks straight past the last segment and checksums
    whatever happens to follow in memory. So the file is cut at the end of
    the chain and `ffffffff` put there.

  * The kernel on its own drives nothing but what is compiled into it.
    The `.opt` files beside it in the fileset are the drivers `customize`
    adds for the peripherals a particular machine has, and they are chain
    modules in exactly the boot image's format -- length at +4, name at
    +28, byte-sum checksum at length+8 -- so adding one is splicing it
    into the chain before the terminator. `--option` does that, which is
    this side of what HP's own customize step did.

Both filesets on the tape are readable, but only one of them is whole:

    S97080C   HP-UX Model 520      Release 05.01   16 segments, all verify
    S97089C   HP-UX Model 530/540/550 05.01        CORE SYSTEM is corrupt

so the 520 kernel is the one that can be booted, and the 520 is therefore
the machine it is booted on. (`--fileset` will still build the other, and
will still say that its chain does not verify.)

    tools/hpuxboot.py software/97089C.hpi --list
    tools/hpuxboot.py software/97089C.hpi -o software/cct/hpux501.bin
    tools/hpuxboot.py software/97089C.hpi -o hpux501.bin --option HPIB_FLEX

    tools/runhpux.sh --release 5.01
"""
import argparse
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import cpiolist
import lifdir

# The fileset that carries a kernel, and which machine that kernel is for.
FILESETS = {
    "S97080C": "HP-UX Model 520",
    "S97089C": "HP-UX Model 530/540/550",
}


def fileset(tape: bytes, name: str) -> bytes:
    """The cpio archive of one LIF fileset."""
    vol = lifdir.read_volume(tape)
    for e in lifdir.read_dir(tape, vol):
        if e["name"] == name:
            start = e["start"] * lifdir.BLOCK
            return tape[start:start + e["bytes"]]
    sys.exit(f"{name}: no such fileset on this tape")


def member(archive: bytes, want: str) -> bytes:
    """One file out of a cpio archive, by the tail of its path."""
    for name, mode, size, off in cpiolist.walk(archive):
        if name == "<lost sync>":
            break
        if name.endswith("/" + want) or name == want:
            return archive[off:off + size]
    return b""


def chain_end(image: bytes) -> tuple:
    """Walk the segment chain; return (offset just past it, [segment names],
    index of the first segment whose checksum is wrong or None).

    Segment 0 leads with an extra word; its header is at +4 and its
    checksum word sits at +len. Later segments' headers follow the previous
    checksum word directly, with the checksum at hdr + len + 8. Each
    checksum covers every byte after the previous checksum word -- which is
    also why a module spliced in here needs no adjustment: its own checksum
    already covers exactly its own bytes.
    """
    prev, hdr, seg, names, bad = 0, 4, 0, [], None
    while hdr + 8 <= len(image):
        ln = struct.unpack(">I", image[hdr + 4:hdr + 8])[0]
        name = image[hdr + 28:hdr + 44].decode("ascii", "replace").rstrip()
        ck_at = ln if seg == 0 else hdr + ln + 8
        if ck_at + 4 > len(image):
            bad = seg if bad is None else bad
            break
        ck = struct.unpack(">I", image[ck_at:ck_at + 4])[0]
        if sum(image[prev:ck_at]) & 0xFFFFFFFF != ck and bad is None:
            bad = seg
        names.append(name)
        prev = hdr = ck_at + 4
        seg += 1
        if hdr + 4 > len(image) or struct.unpack(">i", image[hdr:hdr + 4])[0] < 0 \
                or image[hdr:hdr + 2] == b"\x00\x00":
            break
    return hdr, names, bad


def option(mod: bytes, path: str) -> bytes:
    """One .opt driver module, cut through its checksum word."""
    if len(mod) < 44:
        sys.exit(f"{path}: too short to be a chain module")
    ln = struct.unpack(">I", mod[4:8])[0]
    take = ln + 12
    if take > len(mod):
        sys.exit(f"{path}: claims {take} bytes, has {len(mod)}")
    want = struct.unpack(">I", mod[ln + 8:ln + 12])[0]
    if sum(mod[:ln + 8]) & 0xFFFFFFFF != want:
        sys.exit(f"{path}: checksum does not verify; it is not a chain module")
    return mod[:take]


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("tape", type=Path, help="the 97089C.hpi release tape")
    ap.add_argument("-o", "--out", type=Path, help="where to write the image")
    ap.add_argument("--fileset", default="S97080C", choices=sorted(FILESETS),
                    help="which machine's kernel (default: the 520's, the "
                         "only one whose chain verifies)")
    ap.add_argument("--option", action="append", default=[], metavar="NAME",
                    help="a driver module to splice into the chain, named "
                         "as it appears in the fileset (HPIB_FLEX, HP27128, "
                         "HP27140, SERIAL, ...); repeatable")
    ap.add_argument("--list", action="store_true",
                    help="say what the fileset holds and stop")
    args = ap.parse_args()

    tape = args.tape.read_bytes()
    arc = fileset(tape, args.fileset)

    if args.list:
        print(f"{args.fileset}: {FILESETS[args.fileset]}")
        for name, mode, size, off in cpiolist.walk(arc):
            if name == "<lost sync>":
                print("  *** cpio sync lost -- the rest is damaged ***")
                break
            if size:
                print(f"  {size:>9,}  {name}")
        return 0

    kernel = member(arc, "os.req")
    if not kernel:
        sys.exit(f"{args.fileset}: no os.req in the fileset")
    rev = member(arc, "os.rev").decode("ascii", "replace").strip()
    end, names, bad = chain_end(kernel)
    print(f"{args.fileset}: os.req, {len(kernel):,} bytes"
          + (f" -- {rev}" if rev else ""))
    print(f"  {len(names)} segments, chain ends at {end:#x}: "
          + " ".join(n or "?" for n in names))
    if bad is not None:
        print(f"  *** segment {bad} ({names[bad] if bad < len(names) else '?'})"
              " does not verify: this kernel is damaged on the tape ***")

    image = bytearray(kernel[:end])
    for want in args.option:
        mod = member(arc, want if want.endswith(".opt") else want + ".opt")
        if not mod:
            sys.exit(f"{want}: no such option module in {args.fileset}")
        take = option(mod, want)
        name = take[28:44].decode("ascii", "replace").rstrip()
        print(f"  + {name} ({len(take):,} bytes) from {want}.opt")
        image += take
    image += b"\xff\xff\xff\xff"

    if not args.out:
        print("(no --out: nothing written)")
        return 1 if bad is not None else 0
    args.out.write_bytes(bytes(image))
    print(f"wrote {args.out} -- {len(image):,} bytes")
    return 1 if bad is not None else 0


if __name__ == "__main__":
    raise SystemExit(main())
