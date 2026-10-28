#!/usr/bin/env python3
"""Writer for HP SDF, the Series 500 Structured Directory Format.

The mirror image of tools/sdfread.py: that one takes a shipped volume
apart, this one builds one that reads back the same way. Every structure
here was copied off real media rather than invented -- 500BT2 (1024-byte
blocks, an empty root and free space: the shape of a freshly made volume)
for the volume header, the File Attributes file and its free lists, and
500OPT1 (256-byte blocks, seventeen files) for what a populated directory
and its i-nodes look like.

    block 0        superblock: label, block size, boot area, FA location
    blocks 1..n    the boot area, if there is one (tools/installbasic.py)
    FA file        128-byte records:
                     0     the FA file's own i-node
                     1     the root directory's i-node
                     2     the free-record cache (below)
                     3..m  the free map, one bit per block (fs(5))
                     then  one i-node per file, then spare records
    then           the root directory's data, then each file's data

Why it matters: BASIC creates files by taking an FA record from the cache
and blocks from the free map. A volume that gets either wrong reads and
boots normally and then fails the first CREATE or SAVE with `ERROR 455:
Possible corrupt directory` -- which is how HP's WINCHESTER diagnostic
found that this writer once filled record 2 with block numbers.

Field notes, all from the media:

  * The free map is MSB first, a set bit a free block, and covers blocks
    0 to s_maxblk. On 500MFTST it agrees bit for bit with the extents.
  * Record 2 caches free FA record numbers the way V7 Unix caches free
    blocks: +0 type 6, +2 how many are cached, +4 a link, then the
    numbers in slots 1 up. The link names another free record, itself
    type 6, holding the next thirty; -1 ends the chain. 500MFTST's 42 free
    records are eleven in record 2 and thirty more behind its link.
  * A spare record is type 5; 500BT2 leaves the rest of it zero.
  * s_maxblk is the last block's number, not the count, and s_passwd is
    sixteen spaces: anything else is a password, and BASIC answers
    INITIALIZE with ERROR 62 (protected).
  * A file's name lives only in its directory entry. The 16-byte name field
    in an i-node is used by the root directory (which carries the volume
    name) and is left empty for files.
  * `uftype` at +6 is the BASIC file type -- 0xe9f1, 0xe9d0 and so on. It
    is preserved when a file is copied from one volume to another, because
    it is how BASIC tells a stored program from data.
"""

import struct

FA_REC = 128
DIRENT = 24
CACHED = 30         # free record numbers one cache record holds

# Record type codes, as the records carry them at +0.
T_INODE = 1
T_SPARE = 5
T_CACHE = 6

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
            version=0x42285, sector=256, spare=250):
        self.blksz = blksz
        self.sector = sector
        self.blocks = blocks * sector // blksz    # volume blocks, not sectors
        self.boot_blocks = boot_blocks
        self.label = label
        self.version = version
        self.spare = spare                        # FA records left for new files
        self.files = []                           # (name, data, uftype)

    def add_file(self, name, data, uftype=0):
        if len(name.encode("ascii")) > 16:
            raise ValueError(f"{name!r}: SDF names are at most 16 characters")
        self.files.append((name, bytes(data), uftype))

    # -- layout -----------------------------------------------------
    def _nblocks(self, nbytes):
        return (nbytes + self.blksz - 1) // self.blksz

    def _map_records(self):
        return (self.blocks + FA_REC * 8 - 1) // (FA_REC * 8)

    def _first_inode(self):
        return 3 + self._map_records()

    def _plan(self):
        """Where everything goes: the FA file, sized for the free map, the
        files and the spare records, then the directory, then the data."""
        fa_start = 1 + self.boot_blocks
        records = self._first_inode() + len(self.files) + self.spare
        fa_blocks = self._nblocks(records * FA_REC)
        data_start = fa_start + fa_blocks
        at = data_start + self._nblocks(DIRENT * len(self.files))
        extents = []
        for _name, data, _t in self.files:
            n = self._nblocks(len(data)) or 1
            extents.append((at, n))
            at += n
        if at > self.blocks:
            raise ValueError("the files do not fit on the volume")
        return fa_start, fa_blocks, data_start, extents, at

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

    @staticmethod
    def _cache(free):
        """The cache record for `free`, and the chain records behind it."""
        head, link, rest = free[:CACHED], -1, []
        if len(free) > CACHED:
            link, rest = free[CACHED], free[CACHED + 1:]
        r = bytearray(FA_REC)
        struct.pack_into(">HHi", r, 0x00, T_CACHE, len(head), link)
        for i, n in enumerate(head):
            struct.pack_into(">i", r, 0x08 + 4 * i, n)
        chain = {}
        if link >= 0:
            chain[link], more = Volume._cache(rest)
            chain.update(more)
        return bytes(r), chain

    def _free_map(self, used_end):
        """Blocks from used_end to the end of the volume are free."""
        m = bytearray(self._map_records() * FA_REC)
        for b in range(used_end, self.blocks):
            m[b >> 3] |= 0x80 >> (b & 7)
        return m

    def _fa(self, fa_start, fa_blocks, data_start, extents, used_end):
        records = fa_blocks * self.blksz // FA_REC
        first = self._first_inode()
        recs = [struct.pack(">H", T_SPARE).ljust(FA_REC, b"\0")] * records
        recs[0] = self._inode(FT_FILE, fa_blocks, MX_FA, fa_blocks,
                [(fa_start, fa_blocks)], fa_blocks * self.blksz, 0, "", PERM_FA)
        dir_bytes = DIRENT * len(self.files)
        dir_ext = ([(data_start, self._nblocks(dir_bytes))] if dir_bytes else [])
        recs[1] = self._inode(FT_DIR, self._nblocks(dir_bytes), MX_DIR, 2,
                dir_ext, dir_bytes, 1, self.label, PERM_DIR)
        recs[2], chain = self._cache(list(range(first + len(self.files),
                records)))
        for n, r in chain.items():
            recs[n] = r
        m = self._free_map(used_end)
        for i in range(self._map_records()):
            recs[3 + i] = bytes(m[i * FA_REC:(i + 1) * FA_REC])
        for i, ((name, data, uftype), ext) in enumerate(zip(self.files, extents)):
            recs[first + i] = self._inode(FT_FILE, ext[1], MX_FILE, 0xffff,
                    [ext], len(data), 0, "", PERM_FILE, uftype)
        return b"".join(recs)

    def _header(self):
        h = bytearray(self.blksz)
        struct.pack_into(">HH", h, 0x00, 0x0700, 0)       # s_format, corrupt
        h[0x04:0x14] = self.label.encode("ascii").ljust(16)[:16]
        struct.pack_into(">6I", h, 0x1c, self.blksz, 1, self.boot_blocks,
                1 + self.boot_blocks, self.version, self.blocks - 1)
        h[0x34:0x44] = b" " * 16                          # s_passwd: none
        return bytes(h)

    # -- output -----------------------------------------------------
    def build(self):
        fa_start, fa_blocks, data_start, extents, used_end = self._plan()
        image = bytearray(self.blocks * self.blksz)
        image[0:self.blksz] = self._header()
        fa = self._fa(fa_start, fa_blocks, data_start, extents, used_end)
        image[fa_start * self.blksz:fa_start * self.blksz + len(fa)] = fa

        first = self._first_inode()
        dirdata = bytearray()
        for i, (name, _data, _t) in enumerate(self.files):
            e = bytearray(DIRENT)
            e[0:16] = name.encode("ascii").ljust(16)[:16]
            struct.pack_into(">i", e, 20, first + i)
            dirdata += e
        image[data_start * self.blksz:data_start * self.blksz + len(dirdata)] \
                = dirdata

        for (_name, data, _t), (start, _n) in zip(self.files, extents):
            image[start * self.blksz:start * self.blksz + len(data)] = data
        return bytes(image)

    def describe(self):
        fa_start, fa_blocks, _data_start, _extents, used_end = self._plan()
        return (f"{self.blocks:,} blocks of {self.blksz}, "
                f"boot 1+{self.boot_blocks}, FA at {fa_start} ({fa_blocks} "
                f"block{'s' if fa_blocks != 1 else ''}), "
                f"{len(self.files)} file(s), "
                f"{self.blocks - used_end:,} blocks free")
