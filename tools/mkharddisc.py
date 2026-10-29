#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Joel Heikkila
"""Make a clean Series 500 fixed-disc image, with or without a boot area.

The 520's internal Winchester is 9.896 MB formatted (530/540 Service, 4849),
which at the 256-byte blocks the CS/80 path uses is 38,656 blocks. A disc is
laid out the way the flexible discs are:

    0x000  SDF volume header -- the label, block size, and the two numbers
           that matter to a boot: how many blocks the boot area occupies and
           where the File Attributes file starts after it
    0x400  boot descriptor, present only when there is a boot area
    0x800  the boot image itself, running to the end of the boot area
    then   the FA file, the root directory, and the file area

A disc with no boot area is a data disc: `boot 1+0`, file area right after
the header, which is what the option and utility discs are. A disc with one
is bootable, and `tools/installbasic.py` is what fills it.

Both kinds get a real, empty SDF file system -- see tools/sdfwrite.py for
the structures and where each field was read off. A disc whose file area is
merely zeroed has no FA file and no root directory, and BASIC cannot mount
it as its system volume: the boot ends in ERROR 455 instead of the banner.

    tools/mkharddisc.py disc.img                 9.896 MB, boot area for BASIC
    tools/mkharddisc.py disc.img --no-boot       data disc, no boot area
    tools/mkharddisc.py disc.img --blocks 20000  a smaller disc
"""

import argparse
from pathlib import Path

import sdfwrite

BLOCK = 256        # the sector the CS/80 path reads
BOOTBLK = 1024     # the volume block size boot media use

# 9.896 MB formatted at 256 bytes a block.
DEFAULT_BLOCKS = 38656

# Room for the 372,932-byte BASIC image plus every option binary on both
# option discs, which come to about 267 KB more, with room to spare --
# counted in the 1024-byte blocks the volume header uses.
DEFAULT_BOOT_BLOCKS = 650


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

    vol = sdfwrite.Volume(args.blocks, blksz=BOOTBLK, boot_blocks=boot_blocks,
            label=args.label, sector=BLOCK)
    args.out.write_bytes(vol.build())

    mb = args.blocks * BLOCK / 1e6
    print(f"wrote {args.out} -- {args.blocks:,} blocks, {mb:.3f} MB, "
            f"label {args.label!r}")
    print(f"  {vol.describe()}")
    if boot_blocks:
        print(f"  boot area: blocks 1..{boot_blocks} of {BOOTBLK} bytes, "
                f"{boot_blocks * BOOTBLK:,} bytes")
        print("  empty -- run tools/installbasic.py to make it bootable")
    else:
        print("  no boot area (data disc)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
