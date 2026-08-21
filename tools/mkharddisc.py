#!/usr/bin/env python3
"""Make a clean Series 500 fixed-disc image, with or without a boot area.

The 520's internal Winchester is 9.896 MB formatted (530/540 Service, 4849),
which at the 256-byte blocks the CS/80 path uses is 38,656 blocks. A disc is
laid out the way the flexible discs are:

    0x000  SDF volume header -- the label, block size, and the two numbers
           that matter to a boot: how many blocks the boot area occupies and
           where the file area starts after it
    0x400  boot descriptor, present only when there is a boot area
    0x800  the boot image itself, running to the end of the boot area
    then   the file area

A disc with no boot area is a data disc: `boot 1+0`, file area right after
the header, which is what the option and utility discs are. A disc with one
is bootable, and `tools/installbasic.py` is what fills it.

The header fields are taken from the shipped discs rather than invented --
tools/sdfread.py reads the same ones back, so a produced image can be checked
against a real one field for field.

    tools/mkharddisc.py disc.img                 9.896 MB, boot area for BASIC
    tools/mkharddisc.py disc.img --no-boot       data disc, no boot area
    tools/mkharddisc.py disc.img --blocks 20000  a smaller disc
"""

import argparse
import struct
from pathlib import Path

BLOCK = 256        # the sector the CS/80 path reads
BOOTBLK = 1024     # the volume block size boot media use

# 9.896 MB formatted at 256 bytes a block.
DEFAULT_BLOCKS = 38656

# Room for the 372,932-byte BASIC image plus every option binary on both
# option discs, which come to about 267 KB more, with room to spare --
# counted in the 1024-byte blocks the volume header uses.
DEFAULT_BOOT_BLOCKS = 650


def volume_header(label: str, sectors: int, boot_blocks: int) -> bytes:
    """The SDF volume header, field for field as 500BT1 carries it.

    The fields the bootstrap loader actually reads are the block size at
    +0x1c and the first free block at +0x28; it rejects a volume whose
    block size is not a whole number of 256-byte sectors, and reads the
    boot descriptor from the block after the header -- which is byte 0x400
    when the block size is 1024, where boot media put it.
    """
    h = bytearray(BOOTBLK)
    struct.pack_into(">I", h, 0x00, 0x07000000)
    h[0x04:0x04 + 16] = label.encode("ascii").ljust(16)[:16]
    struct.pack_into(">I", h, 0x1c, BOOTBLK)            # block size, 1024
    struct.pack_into(">I", h, 0x20, 1)                  # first boot block
    struct.pack_into(">I", h, 0x24, boot_blocks)        # boot area blocks
    struct.pack_into(">I", h, 0x28, 1 + boot_blocks)    # first free block
    struct.pack_into(">I", h, 0x2c, 0x42285)            # format version
    struct.pack_into(">I", h, 0x30, sectors // (BOOTBLK // BLOCK))
    return bytes(h)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("out", type=Path)
    ap.add_argument("--blocks", type=int, default=DEFAULT_BLOCKS,
            help=f"disc size in {BLOCK}-byte blocks (default {DEFAULT_BLOCKS})")
    ap.add_argument("--boot-blocks", type=int, default=DEFAULT_BOOT_BLOCKS,
            help=f"{BOOTBLK}-byte blocks reserved for the boot area")
    ap.add_argument("--no-boot", action="store_true",
            help="make a data disc with no boot area")
    ap.add_argument("--label", default="HPFOCUS-FIXED-01")
    args = ap.parse_args()

    boot_blocks = 0 if args.no_boot else args.boot_blocks
    if boot_blocks * (BOOTBLK // BLOCK) >= args.blocks:
        print("the boot area does not fit on the disc")
        return 1

    image = bytearray(args.blocks * BLOCK)
    header = volume_header(args.label, args.blocks, boot_blocks)
    image[0:len(header)] = header
    args.out.write_bytes(bytes(image))

    mb = args.blocks * BLOCK / 1e6
    print(f"wrote {args.out} -- {args.blocks:,} blocks, {mb:.3f} MB, "
            f"label {args.label!r}")
    if boot_blocks:
        print(f"  boot area: blocks 1..{boot_blocks} of {BOOTBLK} bytes, "
                f"{boot_blocks * BOOTBLK:,} bytes; file area from block "
                f"{1 + boot_blocks}")
        print("  empty -- run tools/installbasic.py to make it bootable")
    else:
        print("  no boot area (data disc)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
