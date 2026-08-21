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

Two facts make it straightforward, both established against the shipped
media rather than assumed:

  * The boot image is a chain of segments, each with its length at +4, a
    16-byte name at +28, and a byte-sum checksum at length+8 covering
    everything since the previous checksum (tools/bootimage.py verifies it).
  * An option binary is one such segment on its own, checksum included, so
    installing options means splicing them into the chain ahead of its
    terminator. Appending past the terminator does nothing at all: the
    powerup walk stops there and the segments are never registered.

    tools/installbasic.py disc.img --basic software/basic/img/500BT1.img \\
        software/basic/img/500BT2.img --option /tmp/opt1/ERRORS_ENG ...
"""

import argparse
import struct
import subprocess
import sys
from pathlib import Path

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


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("disc", type=Path, help="fixed-disc image to install onto")
    ap.add_argument("--basic", type=Path, nargs="+", required=True,
            help="the BASIC boot volumes, in order (500BT1, 500BT2)")
    ap.add_argument("--option", type=Path, nargs="*", default=[],
            help="option binaries to add to the boot area")
    ap.add_argument("--image", type=Path,
            help="write the assembled boot image here as well")
    args = ap.parse_args()

    # Assemble the boot image from its volumes with the existing tool, so
    # there is one implementation of the volume-descriptor rules.
    tmp = args.image or Path("/tmp/hpfocus-boot.bin")
    r = subprocess.run([sys.executable, str(Path(__file__).parent / "bootimage.py"),
            str(tmp), *[str(p) for p in args.basic]],
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

    disc = bytearray(args.disc.read_bytes())
    blocksize = word(bytes(disc), 0x1c)      # 1024 on boot media
    boot_blocks = word(bytes(disc), 0x24)
    room = boot_blocks * blocksize - SLICE
    if boot_blocks == 0:
        print(f"\n{args.disc} has no boot area -- make one with "
                f"tools/mkharddisc.py")
        return 1
    if len(image) > room:
        print(f"\nthe boot area holds {room:,} bytes and the image needs "
                f"{len(image):,}; rebuild the disc with more --boot-blocks")
        return 1

    # The boot descriptor: one volume, the whole image, starting at 0x800 --
    # the same fields the two-disc BASIC set carries, with the split removed.
    struct.pack_into(">I", disc, DESC + 0x00, 0xe9c28206)   # product
    struct.pack_into(">I", disc, DESC + 0x04, len(image))   # total size
    struct.pack_into(">I", disc, DESC + 0x08, 1)            # volumes
    struct.pack_into(">I", disc, DESC + 0x0c, 1)            # this volume
    struct.pack_into(">I", disc, DESC + 0x10, 0)            # image offset
    struct.pack_into(">I", disc, DESC + 0x14, 8)            # as volume 1
    struct.pack_into(">I", disc, DESC + 0x18, 5)
    label = b"BASIC LANGUAGE SYSTEM  REV 2.00 (installed)"
    disc[DESC + 0x30:DESC + 0x30 + len(label)] = label

    disc[SLICE:SLICE + len(image)] = image
    args.disc.write_bytes(bytes(disc))

    print(f"\ninstalled onto {args.disc}: {len(image):,} bytes in a "
            f"{room:,}-byte boot area")
    print("boot it with:  tools/runbasic-hd.sh")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
