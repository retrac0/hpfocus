#!/usr/bin/env python3
"""Assemble the whole HP-UX system disc from the two 7914 unit-save tapes.

The tapes are two consecutive spans of one SDF volume, imaged from disc
block 1 with a two-block hole at the join (tools/sdf7914.py has the
measurement):

    block 0                 the volume header -- never imaged
    blocks 1 .. 65,406      tape 1, byte for byte
    blocks 65,407 .. 65,408 never imaged
    blocks 65,409 .. 130,814  tape 2, byte for byte

Mounting tape 1 alone puts the boot descriptor in sector 0 and the boot
image at 0x400, which the loader copes with, but it leaves HP-UX reading
its volume header out of the boot descriptor: the kernel's device
configuration reads sectors 0..3, finds no SDF volume there, never enters
the disc in its unit table, and the root open fails with escape 91. The
inode table is on tape 2 besides. So the volume is put back together here,
with block 0 rebuilt from what the File Attributes file pins:

    s_format   0x0700
    s_blksz    1024
    s_boot     1        the boot descriptor is block 1, the image follows
    s_bootsz   the blocks the boot image occupies after block 1
    s_fa       69,087   the FA file's first extent, where records 0 and 1 --
                        its own i-node and the root's -- live
    s_version  as tools/sdfwrite.py writes it
    s_maxblk   130,814  the highest block number: the shipped 500BT1 is
                        264 blocks and says 263

The label is read off the root directory's i-node, which carries the
volume name at +84 (tools/sdf7914.py's record layout).

    python3 tools/mkhpuxdisc.py tape1.bin tape2.bin out.bin
"""
import struct
import sys
from pathlib import Path

BLK = 1024
TAPE_BLOCKS = 65406
GAP = 2
S_FA = 69087
VERSION = 0x42285
TOTAL = 1 + TAPE_BLOCKS + GAP + TAPE_BLOCKS      # 130,815


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        return 2
    t1 = Path(sys.argv[1]).read_bytes()
    t2 = Path(sys.argv[2]).read_bytes()
    for name, t in (("tape 1", t1), ("tape 2", t2)):
        if len(t) != TAPE_BLOCKS * BLK:
            print(f"{name}: {len(t)} bytes, expected {TAPE_BLOCKS * BLK}",
                    file=sys.stderr)
            return 1

    # The boot descriptor is tape 1's first sector: product, image size.
    magic, total = struct.unpack(">II", t1[0:8])
    if magic != 0xe9c28206:
        print(f"tape 1 does not begin with the boot descriptor ({magic:#x})",
                file=sys.stderr)
        return 1
    boot_blocks = 1 + (total + BLK - 1) // BLK   # descriptor + image

    # The root directory's i-node is FA record 1, in the FA file's first
    # block; the volume name is its 16-byte name field.
    fa_off = (S_FA - 1 - GAP - TAPE_BLOCKS) * BLK
    root = t2[fa_off + 128:fa_off + 256]
    fa0 = t2[fa_off:fa_off + 128]
    size, = struct.unpack(">i", fa0[76:80])
    if size != 2016 * BLK:
        print(f"the FA file's i-node is not at block {S_FA} (size {size})",
                file=sys.stderr)
        return 1
    label = root[84:100].decode("ascii", "replace").rstrip("\0 ") or "HP-UX"

    h = bytearray(BLK)
    struct.pack_into(">HH", h, 0, 0x0700, 0)
    h[4:20] = label.encode("ascii", "replace").ljust(16)[:16]
    struct.pack_into(">6I", h, 0x1c, BLK, 1, boot_blocks, S_FA, VERSION, TOTAL - 1)

    # The emulated 7914 describes itself as 7 heads of 64 sectors, and
    # HP-UX sizes the volume from that geometry, so the image is padded to
    # whole cylinders; s_maxblk still names the last real block.
    sectors = TOTAL * BLK // 256
    cyl = 7 * 64
    pad = (cyl - sectors % cyl) % cyl
    out = Path(sys.argv[3])
    with out.open("wb") as f:
        f.write(h)
        f.write(t1)
        f.write(bytes(GAP * BLK))
        f.write(t2)
        f.write(bytes(pad * 256))
    print(f"{out}: {TOTAL:,} blocks of {BLK}, label {label!r}, boot 1+{boot_blocks}, "
            f"FA at {S_FA:,}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
