#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Joel Heikkila
"""Writer for HP SDF, the Series 500 Structured Directory Format.

The mirror image of tools/sdfread.py: that one takes a shipped volume
apart, this one builds one that reads back the same way. Every structure
here was copied off real media rather than invented -- 500BT1 (a 1024-byte
block boot volume, the closest analogue of a fixed disc) for the volume
header, the File Attributes file and an empty root, and 500OPT1 (256-byte
blocks, seventeen files) for what a populated directory and its i-nodes
look like.

    block 0        superblock: label, block size, boot area, FA location
    blocks 1..n    the boot area, if there is one (tools/installbasic.py)
    FA file        128-byte records: 0 the FA file's own i-node, 1 the root
                   directory's, 2 the free-record cache, 3.. the free-block
                   bitmap, then one per file, then unused (type 5) records
    then           the root directory's data, then each file's data

Why it matters: a disc whose file area is merely zeroed is not an empty
volume, it is a corrupt one. BASIC mounts its boot device as the system
volume, and a fixed disc with no FA file fails that mount -- ERROR 455 at
the end of an otherwise clean boot, and, with the IO and MS options
installed, a trap 012 in the kernel before the banner ever appears.

Field notes, all from the media:

  * A file's name lives only in its directory entry. The 16-byte name field
    in an i-node is used by the root directory (which carries the volume
    name) and is left empty for files.
  * `uftype` at +6 is the BASIC file type -- 0xe9f1, 0xe9d0 and so on. It
    is preserved when a file is copied from one volume to another, because
    it is how BASIC tells a stored program from data.
  * Free space is a BITMAP, FA records 3 onward (fs(5): "a bit for each
    block on the device. If a bit is set, the corresponding block is
    free", zero-padded to a whole record), most significant bit first --
    sized from s_maxblk, which is the largest block number, not the count.
    BASIC reads it so: on a 9,664-block disc it reads records 3..12 and
    reports AVAILABLE SPACE as four 256-byte records per set bit. The
    7914 HP-UX volume and every BASIC floppy are laid out this way.
  * Record 2 is not about blocks at all. It is a type-6 cache of free FA
    RECORD numbers -- +2 a count, then up to 31 record numbers -- that
    CREATE pops an i-node slot from (the floppies carry 35..57, 27..34;
    500BT1 and the 7914 an empty one). Unused records are type 5.
"""

import struct

FA_REC = 128
DIRENT = 24
SPARE_RECS = 64     # unused FA records left for the files BASIC will create

# FA record type codes, as the records carry them at +0.
T_INODE = 1
T_UNUSED = 5        # a free FA record, ready to become an i-node
T_FREE_RECS = 6     # record 2: the cache of free FA record numbers

FT_FILE = 0
FT_DIR = 1

# The two size limits 500BT1 stamps into a 1024-byte-block volume. They are
# per-volume ceilings rather than anything derived, so they are copied.
MX_FA = 0x00c35000
MX_DIR = 0x00249f00
MX_FILE = 0x7fffffff

# The permission-ish half word at +0x72, as each kind of record carries it.
PERM_FA = 0x0080
PERM_DIR = 0x0018
PERM_FILE = 0x0100


class Volume:
    """An SDF volume being built: a boot area, then a flat root directory."""

    def __init__(self, blocks, blksz=1024, boot_blocks=0, label="",
            version=0x42285, sector=256):
        self.blksz = blksz
        self.sector = sector
        self.blocks = blocks * sector // blksz    # volume blocks, not sectors
        self.boot_blocks = boot_blocks
        self.label = label
        self.version = version
        self.files = []                           # (name, data, uftype)

    def add_file(self, name, data, uftype=0):
        if len(name.encode("ascii")) > 16:
            raise ValueError(f"{name!r}: SDF names are at most 16 characters")
        self.files.append((name, bytes(data), uftype))

    # -- layout -----------------------------------------------------
    def _nblocks(self, nbytes):
        return (nbytes + self.blksz - 1) // self.blksz

    def _bitmap_records(self):
        """Free-map records for blocks 0..s_maxblk, one bit each."""
        return (self.blocks + 8 * FA_REC - 1) // (8 * FA_REC)

    def _plan(self):
        """Where everything goes: the FA file right after the boot area,
        holding the bitmap, one i-node per file and some spare records;
        then the root directory's data, then each file's data."""
        fa_start = 1 + self.boot_blocks
        nrec = 3 + self._bitmap_records() + len(self.files) + SPARE_RECS
        fa_blocks = self._nblocks(nrec * FA_REC)
        data_start = fa_start + fa_blocks
        at = data_start + self._nblocks(DIRENT * len(self.files))
        extents = []
        for _name, data, _t in self.files:
            n = self._nblocks(len(data)) or 1
            extents.append((at, n))
            at += n
        if at > self.blocks:
            raise ValueError("the files do not fit on the volume")
        free = list(range(at, self.blocks))
        return fa_start, fa_blocks, data_start, extents, free

    # -- records ----------------------------------------------------
    @staticmethod
    def _inode(ftype, blocks, mx, exsz, extents, size, parent, name, perm,
            uftype=0):
        r = bytearray(FA_REC)
        struct.pack_into(">HHHH", r, 0x00, T_INODE, ftype, 1,
                uftype & 0xffff)
        struct.pack_into(">III", r, 0x10, 0xffffffff, 0xffffffff, 0xffffffff)
        struct.pack_into(">i", r, 0x1c, blocks)
        struct.pack_into(">I", r, 0x20, mx)
        struct.pack_into(">HH", r, 0x24, exsz, len(extents))
        for i, (start, n) in enumerate(extents[:4]):
            struct.pack_into(">ii", r, 0x28 + 8 * i, start, n)
        struct.pack_into(">iii", r, 0x48, -1, size, parent)
        r[0x54:0x54 + len(name)] = name.encode("ascii")[:16]
        struct.pack_into(">H", r, 0x72, perm)
        return bytes(r)

    def _fa(self, fa_start, fa_blocks, data_start, extents, free):
        nrec = fa_blocks * self.blksz // FA_REC
        nbm = self._bitmap_records()
        first_ino = 3 + nbm
        # Unused records as the media carry them: type 5, then 0xff on the
        # 256-byte-block floppies and zeros on the 1024-byte-block volumes
        # (500BT2, 500ENV3, the 7914).
        fill = b"\xff" if self.blksz < 1024 else b"\x00"
        unused = bytes([0, T_UNUSED]) + fill * (FA_REC - 2)
        recs = [unused] * nrec
        recs[0] = self._inode(FT_FILE, fa_blocks, MX_FA, fa_blocks,
                [(fa_start, fa_blocks)], fa_blocks * self.blksz, 0, "", PERM_FA)
        dir_bytes = DIRENT * len(self.files)
        dir_ext = ([(data_start, self._nblocks(dir_bytes))] if dir_bytes else [])
        recs[1] = self._inode(FT_DIR, self._nblocks(dir_bytes), MX_DIR, 2,
                dir_ext, dir_bytes, 1, self.label, PERM_DIR)

        # The free map: a set bit is a free block, MSB first.
        bm = bytearray(nbm * FA_REC)
        for b in free:
            bm[b >> 3] |= 0x80 >> (b & 7)
        for i in range(nbm):
            recs[3 + i] = bytes(bm[i * FA_REC:(i + 1) * FA_REC])

        for i, ((name, data, uftype), ext) in enumerate(zip(self.files, extents)):
            recs[first_ino + i] = self._inode(FT_FILE, ext[1], MX_FILE, 0xffff,
                    [ext], len(data), 0, "", PERM_FILE, uftype)

        # Record 2: the cache of free FA records CREATE takes i-nodes from,
        # left empty the way 500BT1 and the 7914 carry it ("0006 0000
        # ffffffff"); BASIC refills it from the type-5 records itself. (A
        # cache primed with 31 entries made BASIC spill it and then halt
        # with TRAP 030; empty, and primed with 5, CREATE works.)
        r2 = bytearray(FA_REC)
        struct.pack_into(">HHi", r2, 0, T_FREE_RECS, 0, -1)
        recs[2] = bytes(r2)
        return b"".join(recs)

    def _header(self):
        h = bytearray(self.blksz)
        struct.pack_into(">HH", h, 0x00, 0x0700, 0)       # s_format, corrupt
        h[0x04:0x14] = self.label.encode("ascii").ljust(16)[:16]
        # s_maxblk is the largest block number (fs(5); 500BT1 is 264
        # blocks and says 263), and the free map is sized from it.
        struct.pack_into(">6I", h, 0x1c, self.blksz, 1, self.boot_blocks,
                1 + self.boot_blocks, self.version, self.blocks - 1)
        return bytes(h)

    # -- output -----------------------------------------------------
    def build(self):
        fa_start, fa_blocks, data_start, extents, free = self._plan()
        image = bytearray(self.blocks * self.blksz)
        image[0:self.blksz] = self._header()
        fa = self._fa(fa_start, fa_blocks, data_start, extents, free)
        image[fa_start * self.blksz:fa_start * self.blksz + len(fa)] = fa

        dirdata = bytearray()
        for i, (name, _data, _t) in enumerate(self.files):
            e = bytearray(DIRENT)
            e[0:16] = name.encode("ascii").ljust(16)[:16]
            struct.pack_into(">i", e, 20, 3 + self._bitmap_records() + i)
            dirdata += e
        image[data_start * self.blksz:data_start * self.blksz + len(dirdata)] \
                = dirdata

        for (_name, data, _t), (start, _n) in zip(self.files, extents):
            image[start * self.blksz:start * self.blksz + len(data)] = data
        return bytes(image)

    def describe(self):
        fa_start, fa_blocks, data_start, _extents, free = self._plan()
        return (f"{self.blocks:,} blocks of {self.blksz}, "
                f"boot 1+{self.boot_blocks}, FA at {fa_start} ({fa_blocks} "
                f"block{'s' if fa_blocks != 1 else ''}), "
                f"{len(self.files)} file(s), {len(free):,} blocks free")
