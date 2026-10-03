#!/usr/bin/env python3
"""Install BASIC onto a fixed-disc image, the way BUILD_BOOT would.

HP's own BUILD_BOOT utility, on the option disc, describes the job:

    It will copy the BASIC boot from the source device (usually :INTERNAL)
    to the destination device (usually :CS80,7) ... After copying the boot,
    it will allow any number of option binaries to be added to the boot
    area. The binaries are read from the source device, and may be on any
    number of volumes.

That is what this does, offline. Running the real utility would need
writable emulated media and an interactive session; the result is the same
image either way, and doing it here means a bootable disc can be built and
rebuilt from the shipped discs in one command.

Two facts make the boot area straightforward, both established against the
shipped media rather than assumed:

  * The boot image is a chain of segments, each with its length at +4, a
    16-byte name at +28, and a byte-sum checksum at length+8 covering
    everything since the previous checksum (tools/bootimage.py verifies it).
  * An option binary is one such segment on its own, checksum included, so
    installing options means splicing them into the chain ahead of its
    terminator. Appending past the terminator does nothing at all: the
    powerup walk stops there and the segments are never registered.

The file area is not optional. BASIC mounts the device it booted from as
its system volume, so a fixed disc needs a real SDF file system next to the
boot area or the boot ends in ERROR 455 -- and with the IO and MS options
installed it does not even get that far, dying in the kernel on a trap 012
as the first task is dispatched. The file system is rebuilt here rather
than patched, so `--copy` can put files in it; tools/sdfwrite.py holds the
structures.

    tools/installbasic.py disc.img --basic software/basic/500BT1.TD0 \\
        software/basic/500BT2.TD0 --option /tmp/opt1/ERRORS_ENG ...
    tools/installbasic.py disc.img --basic ... \\
        --copy software/basic/500OPT1.TD0:BUILD_BOOT,CAT_BOOT

A volume may be given as the shipped Teledisk file or as a raw image
(tools/td0.py --raw); a .TD0 is decoded here, so nothing has to make the
gitignored software/basic/img/ first.
"""
from __future__ import annotations  # PEP 604/585 hints on Python 3.8

import argparse
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import sdfread
import sdfwrite
import td0

BLOCK = 256
DESC = 0x400
SLICE = 0x800


def word(d: bytes, off: int) -> int:
    return struct.unpack(">I", d[off:off + 4])[0]


def chain_end(image: bytes) -> int:
    """Offset just past the last segment's checksum -- where options go.

    The same walk tools/bootimage.py verifies with: segment 0's checksum
    sits at its length, every later one at its header + length + 8.
    """
    hdr, seg = 4, 0
    while hdr + 8 <= len(image):
        ln = word(image, hdr + 4)
        ck = ln if seg == 0 else hdr + ln + 8
        if ck + 4 > len(image):
            break
        hdr = ck + 4
        seg += 1
        if hdr + 4 > len(image) or struct.unpack(">i", image[hdr:hdr + 4])[0] < 0 \
                or image[hdr:hdr + 2] == b"\0\0":
            break
    return hdr, seg


def module(path: Path) -> tuple[bytes, str]:
    """An option binary, trimmed to exactly its chain segment."""
    d = path.read_bytes()
    if len(d) < 44:
        raise SystemExit(f"{path}: too short to be a boot module")
    ln = word(d, 4)
    take = ln + 12                      # header + body + checksum word
    if take > len(d):
        raise SystemExit(f"{path}: claims {take} bytes, file has {len(d)}")
    if sum(d[:ln + 8]) & 0xffffffff != word(d, ln + 8):
        raise SystemExit(f"{path}: checksum does not verify -- not a module?")
    return d[:take], d[28:44].decode("ascii", "replace").rstrip()


def volume_bytes(path) -> bytes:
    """A volume's raw image: a Teledisk file is decoded the way
    `td0.py --raw` decodes it, sectors in (cylinder, head, sector) order."""
    data = Path(path).read_bytes()
    if data[:2] == b"TD":
        good = sorted((s for s in td0.parse(data) if s[5] is not None),
                      key=lambda s: (s[0], s[1], s[2]))
        return b"".join(s[5] for s in good)
    return data


def copies(specs: list[str]) -> list[tuple[str, bytes, int]]:
    """Files pulled out of shipped SDF volumes, BASIC file type and all.

    `image` takes every file in the volume's root, `image:A,B` takes those
    named. The type code at +6 of the i-node travels with the file: it is
    how BASIC tells a stored program from data, so a copy that dropped it
    would land on the disc as something the system will not open.
    """
    out = []
    for spec in specs:
        path, _, names = spec.partition(":")
        vol = sdfread.SDF(volume_bytes(path))
        want = [n for n in names.split(",") if n] or None
        have = {name: vol.inode(ino) for name, ino in vol.readdir(vol.root())}
        for name in (want if want is not None else list(have)):
            ino = have.get(name)
            if ino is None:
                raise SystemExit(f"{path}: no file named {name!r}")
            out.append((name, vol.read_file(ino), ino["uftype"] & 0xffff))
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("disc", type=Path, help="fixed-disc image to install onto")
    ap.add_argument("--basic", type=Path, nargs="+", required=True,
            help="the BASIC boot volumes, in order (500BT1, 500BT2)")
    ap.add_argument("--option", type=Path, nargs="*", default=[],
            help="option binaries to add to the boot area")
    ap.add_argument("--copy", action="append", default=[], metavar="IMAGE[:A,B]",
            help="copy files from a shipped SDF volume into the file area")
    ap.add_argument("--image", type=Path,
            help="write the assembled boot image here as well")
    args = ap.parse_args()

    # Assemble the boot image from its volumes with the existing tool, so
    # there is one implementation of the volume-descriptor rules.
    # A private directory holds the decoded volumes and the image, so two
    # runs never share a file.
    with tempfile.TemporaryDirectory() as work:
        vols = []
        for i, p in enumerate(args.basic):
            raw = Path(work) / f"volume{i + 1}.img"
            raw.write_bytes(volume_bytes(p))
            vols.append(str(raw))
        tmp = args.image or Path(work) / "boot.bin"
        r = subprocess.run([sys.executable,
                str(Path(__file__).parent / "bootimage.py"), str(tmp), *vols],
                capture_output=True, text=True)
        sys.stdout.write(r.stdout)
        if r.returncode:
            sys.stderr.write(r.stderr)
            return r.returncode
        image = tmp.read_bytes()

    end, segs = chain_end(image)
    print(f"\nboot image: {len(image):,} bytes, {segs} segments, "
            f"chain ends at {end:#x}")

    if args.option:
        head, tail = image[:end], image[end:]
        added = []
        for p in args.option:
            mod, name = module(p)
            added.append((name, len(mod)))
            head += mod
        image = head + tail
        for name, n in added:
            print(f"  installed option {name:<16} {n:,} bytes")
        print(f"boot image with options: {len(image):,} bytes")

    # The disc supplies the geometry; the volume is rebuilt around it, so
    # the file system and the boot area are laid out by one piece of code.
    old = args.disc.read_bytes()
    blocksize = word(old, 0x1c)              # 1024 on boot media
    boot_blocks = word(old, 0x24)
    label = old[0x04:0x14].decode("ascii", "replace").rstrip("\0 ")
    room = boot_blocks * blocksize - SLICE
    if boot_blocks == 0:
        print(f"\n{args.disc} has no boot area -- make one with "
                f"tools/mkharddisc.py")
        return 1
    if len(image) > room:
        print(f"\nthe boot area holds {room:,} bytes and the image needs "
                f"{len(image):,}; rebuild the disc with more --boot-blocks")
        return 1

    vol = sdfwrite.Volume(len(old) // BLOCK, blksz=blocksize,
            boot_blocks=boot_blocks, label=label, sector=BLOCK)
    for name, data, uftype in copies(args.copy):
        vol.add_file(name, data, uftype)
        print(f"  copied {name:<16} {len(data):,} bytes  type {uftype:#06x}")
    disc = bytearray(vol.build())

    # The boot descriptor: one volume, the whole image, starting at 0x800 --
    # the same fields the two-disc BASIC set carries, with the split removed.
    struct.pack_into(">I", disc, DESC + 0x00, 0xe9c28206)   # product
    struct.pack_into(">I", disc, DESC + 0x04, len(image))   # total size
    struct.pack_into(">I", disc, DESC + 0x08, 1)            # volumes
    struct.pack_into(">I", disc, DESC + 0x0c, 1)            # this volume
    struct.pack_into(">I", disc, DESC + 0x10, 0)            # image offset
    struct.pack_into(">I", disc, DESC + 0x14, 8)            # as volume 1
    struct.pack_into(">I", disc, DESC + 0x18, 5)
    boot_label = b"BASIC LANGUAGE SYSTEM  REV 2.00 (installed)"
    disc[DESC + 0x30:DESC + 0x30 + len(boot_label)] = boot_label

    disc[SLICE:SLICE + len(image)] = image
    args.disc.write_bytes(bytes(disc))

    print(f"\ninstalled onto {args.disc}: {len(image):,} bytes in a "
            f"{room:,}-byte boot area")
    print(f"  {vol.describe()}")
    print("boot it with:  tools/runbasic-hd.sh")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
