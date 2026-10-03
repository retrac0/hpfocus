#!/usr/bin/env python3
"""Assemble the whole HP-UX system disc from the two 7914 unit-save tapes.

The tapes are two consecutive spans of one SDF volume, imaged from disc
block 1 with a two-block hole at the join (tools/sdf7914.py has the
measurement):

    block 0                 the volume header -- never imaged
    blocks 1 .. 65,406      tape 1, byte for byte
    blocks 65,407 .. 65,408 never imaged (both are free in the volume's
                            own free map, so nothing was lost there)
    blocks 65,409 .. 129,023  tape 2, byte for byte
    past 129,023            not disc at all: the rest of tape 2 is one zero
                            block and then 1,790 blocks of 0xff filler

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
    s_maxblk   129,023  the largest addressable block (fs(5)). Three
                        things agree on it: the free map is FA records
                        3..128 -- 126 records of 1,024 bits, 129,024
                        blocks, its last bit set -- and record 129 is the
                        FA file's own extent map; a 7914 is 1,152
                        cylinders of 7 heads of 64 256-byte sectors,
                        which is 129,024 such blocks; and tape 2 holds
                        nothing but filler past it.

s_maxblk is not cosmetic. HP-UX sizes the free map from it. This tool
used to write 130,814 (the tapes' imaged length, by analogy with 500BT1,
which is 264 blocks and says 263), and the kernel then took FA records 129
and 130 -- the FA file's own extent map -- for free-map bits: 217 + 35 =
252 phantom free blocks. With the real free space used up it printed
"Less than 252 blocks left on disc" and allocated them, clearing bits in
the extent map that locates FA records 3,112 onward, which is nearly
every i-node on the volume: the next boot finds no /etc. --repair puts an
older disc's header right while that map is still intact.

The label is read off the root directory's i-node, which carries the
volume name at +84 (tools/sdf7914.py's record layout).

    python3 tools/mkhpuxdisc.py tape1.bin tape2.bin out.bin
    python3 tools/mkhpuxdisc.py --repair out.bin

The new volume is written beside the target and renamed into place, and
checked before it is: every block a file, the boot area or the FA file
occupies must be in use in the free map and no later than s_maxblk.
--repair changes nothing on a disc that is already right, and refuses one
whose extent map HP-UX has already overwritten.
"""
import os
import struct
import sys
from pathlib import Path

BLK = 1024
TAPE_BLOCKS = 65406
GAP = 2
S_FA = 69087
VERSION = 0x42285
MAXBLK = 129023                                  # 1,152 x 7 x 64 sectors - 1
TOTAL = MAXBLK + 1                               # 129,024 blocks
TAPE2_FIRST = 1 + TAPE_BLOCKS + GAP              # disc block 65,409
OLD_MAXBLK = 130814                              # what this tool once wrote
FA_EXMAP = 129                                   # the FA file's own extent map


def fa_exmap_intact(rec):
    """True if FA record 129 is still the FA file's extent map: type 2,
    13 extents, owner i-node 0. The kernel's first allocation from the
    phantom free blocks rewrites it, mostly to zeros."""
    etype, exnum = struct.unpack(">HH", rec[0:4])
    owner, = struct.unpack(">i", rec[16:20])
    return etype == 2 and exnum == 13 and owner == 0


def header_state(h, exmap):
    """(ok, message) for a volume header and its FA record 129.

    ok is True when the header is right, False when it carries the old
    s_maxblk and can be repaired, None when it cannot be (not this
    volume, or the extent map already overwritten)."""
    if struct.unpack(">H", h[0:2])[0] != 0x0700:
        return None, "not an SDF volume (no s_format 0x0700 in block 0)"
    s_fa, = struct.unpack(">I", h[0x28:0x2c])
    maxblk, = struct.unpack(">I", h[0x30:0x34])
    if s_fa != S_FA:
        return None, (f"FA file at block {s_fa:,}, not {S_FA:,}: not the 7914 "
                      "volume tools/mkhpuxdisc.py assembles")
    if maxblk == MAXBLK:
        return True, f"s_maxblk {MAXBLK:,}"
    if maxblk != OLD_MAXBLK:
        return None, f"s_maxblk {maxblk:,} is neither {OLD_MAXBLK:,} nor {MAXBLK:,}"
    if not fa_exmap_intact(exmap):
        return None, (f"s_maxblk {OLD_MAXBLK:,} and FA record {FA_EXMAP} is no "
                      "longer the FA file's extent map -- HP-UX has allocated "
                      "from it and the volume is damaged beyond this tool; "
                      "delete it and assemble a new one from the tapes")
    return False, f"s_maxblk {OLD_MAXBLK:,}, from an older mkhpuxdisc.py"


def repair(path):
    """Put an older disc's s_maxblk right, in place: one 4-byte write."""
    try:
        f = path.open("r+b")
    except OSError as e:
        print(f"{path}: {e.strerror}", file=sys.stderr)
        return 1
    with f:
        h = f.read(BLK)
        f.seek(S_FA * BLK + FA_EXMAP * 128)
        ok, msg = header_state(h, f.read(128))
        if ok is None:
            print(f"{path}: {msg}; left alone", file=sys.stderr)
            return 1
        if ok:
            return 0                    # right already: say nothing
        f.seek(0x30)
        f.write(struct.pack(">I", MAXBLK))
    print(f"{path}: s_maxblk {OLD_MAXBLK:,} -> {MAXBLK:,}")
    return 0


def check_volume(path):
    """Problems with an assembled volume's free map, or [] if none.

    Every block the boot area, the FA file or any file's extents occupy
    must be in use (bit clear) in the free map, and none may lie past
    s_maxblk."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from sdfread import SDF, FA_REC
    fs = SDF(path.read_bytes())
    fa0 = fs.inode(0)
    nrec = sum(n for _, n in fs.all_extents(fa0)) * fs.blksz // FA_REC
    nmap = (fs.maxblk + 1 + 8 * FA_REC - 1) // (8 * FA_REC)
    bm = b"".join(fs.fa_record(3 + i) for i in range(nmap))
    problems = []

    def claim(start, n, who):
        for b in range(start, start + n):
            if b > fs.maxblk:
                problems.append(f"{who}: block {b:,} is past s_maxblk {fs.maxblk:,}")
                return
            if (bm[b >> 3] >> (7 - (b & 7))) & 1:
                problems.append(f"{who}: block {b:,} is in use but free in the map")
                return

    claim(0, 1 + fs.bootsz, "volume header and boot area")
    for i in range(nrec):
        r = fs.fa_record(i)
        if struct.unpack(">H", r[0:2])[0] != 1 or i in range(3, 3 + nmap):
            continue
        for s, n in fs.all_extents(fs.inode(i)):
            if s > 0 and n > 0:
                claim(s, n, f"i-node {i}")
    return problems


def main() -> int:
    if len(sys.argv) == 3 and sys.argv[1] == "--repair":
        return repair(Path(sys.argv[2]))
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        return 2
    try:
        t1 = Path(sys.argv[1]).read_bytes()
        t2 = Path(sys.argv[2]).read_bytes()
    except OSError as e:
        print(f"{e.filename}: {e.strerror}", file=sys.stderr)
        return 1
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
    fa_off = (S_FA - TAPE2_FIRST) * BLK
    root = t2[fa_off + 128:fa_off + 256]
    fa0 = t2[fa_off:fa_off + 128]
    size, = struct.unpack(">i", fa0[76:80])
    if size != 2016 * BLK:
        print(f"the FA file's i-node is not at block {S_FA} (size {size})",
                file=sys.stderr)
        return 1
    if not fa_exmap_intact(t2[fa_off + FA_EXMAP * 128:fa_off + (FA_EXMAP + 1) * 128]):
        print(f"FA record {FA_EXMAP} is not the FA file's extent map, so the "
              f"free map does not end at block {MAXBLK:,} as this tool expects",
              file=sys.stderr)
        return 1
    # Everything past the volume's last block is the tape's own filler.
    keep = (TOTAL - TAPE2_FIRST) * BLK
    if set(t2[keep:]) - {0x00, 0xff}:
        print(f"tape 2 carries data past block {MAXBLK:,}, the end of a 7914; "
              "this is not the volume this tool knows", file=sys.stderr)
        return 1
    label = root[84:100].decode("ascii", "replace").rstrip("\0 ") or "HP-UX"

    h = bytearray(BLK)
    struct.pack_into(">HH", h, 0, 0x0700, 0)
    h[4:20] = label.encode("ascii", "replace").ljust(16)[:16]
    struct.pack_into(">6I", h, 0x1c, BLK, 1, boot_blocks, S_FA, VERSION, MAXBLK)

    # The emulated 7914 describes itself as 7 heads of 64 sectors, and
    # HP-UX sizes the volume from that geometry, so the image is whole
    # cylinders -- and 129,024 blocks is exactly a 7914's 1,152.
    assert (TOTAL * BLK // 256) % (7 * 64) == 0
    # Written beside the target, checked, and renamed into place:
    # tools/runhpux.sh assembles the disc only when the file is missing,
    # so a half-written one left by an interrupted run would otherwise be
    # booted from then on.
    out = Path(sys.argv[3])
    part = out.with_name(out.name + ".part")
    with part.open("wb") as f:
        f.write(h)
        f.write(t1)
        f.write(bytes(GAP * BLK))
        f.write(t2[:keep])
    problems = check_volume(part)
    if problems:
        part.unlink()
        print("the assembled volume's free map disagrees with its files:\n  "
              + "\n  ".join(problems[:10]), file=sys.stderr)
        return 1
    os.replace(part, out)
    print(f"{out}: {TOTAL:,} blocks of {BLK}, label {label!r}, boot 1+{boot_blocks}, "
            f"FA at {S_FA:,}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
