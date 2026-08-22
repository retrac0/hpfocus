#!/usr/bin/env python3
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
                   directory's, 2 the head of the free-block chain, then one
                   per file
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
  * The free-block chain is records of 31 block numbers: +0 type (6 head,
    5 continuation), +2 the next record, then the numbers, -1 ending the
    used part. 500BT1, which has no free space at all, carries a head with
    link 0 and a single -1, and that is the shape an empty volume takes.
"""

import struct

FA_REC = 128
DIRENT = 24
FREE_PER_REC = 31

# i-node type codes, as the records carry them at +0.
T_INODE = 1
T_FREE_CONT = 5
T_FREE_HEAD = 6

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

    def _plan(self):
        """Where everything goes. The FA file's own size depends on how many
        free-chain records the free space needs, which depends on where the
        FA file ends, so the size is settled by iterating -- two rounds are
        always enough, and the loop is bounded anyway."""
        fa_start = 1 + self.boot_blocks
        dir_bytes = DIRENT * len(self.files)
        fa_blocks = 1
        for _ in range(8):
            data_start = fa_start + fa_blocks
            at = data_start + self._nblocks(dir_bytes)
            extents = []
            for _name, data, _t in self.files:
                n = self._nblocks(len(data)) or 1
                extents.append((at, n))
                at += n
            free = list(range(at, self.blocks))
            chain = max(1, (len(free) + FREE_PER_REC - 1) // FREE_PER_REC)
            need = self._nblocks((3 + len(self.files) + chain - 1) * FA_REC)
            if need == fa_blocks:
                return fa_start, fa_blocks, data_start, extents, free
            fa_blocks = need
        raise RuntimeError("FA size did not settle")

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
    def _free_record(kind, link, blocks):
        r = bytearray(FA_REC)
        struct.pack_into(">HH", r, 0x00, kind, link)
        for i in range(FREE_PER_REC):
            n = blocks[i] if i < len(blocks) else -1
            struct.pack_into(">i", r, 0x04 + 4 * i, n)
            if i >= len(blocks):
                break
        return bytes(r)

    def _fa(self, fa_start, fa_blocks, data_start, extents, free):
        recs = [b""] * (3 + len(self.files))
        recs[0] = self._inode(FT_FILE, fa_blocks, MX_FA, fa_blocks,
                [(fa_start, fa_blocks)], fa_blocks * self.blksz, 0, "", PERM_FA)
        dir_bytes = DIRENT * len(self.files)
        dir_ext = ([(data_start, self._nblocks(dir_bytes))] if dir_bytes else [])
        recs[1] = self._inode(FT_DIR, self._nblocks(dir_bytes), MX_DIR, 2,
                dir_ext, dir_bytes, 1, self.label, PERM_DIR)
        for i, ((name, data, uftype), ext) in enumerate(zip(self.files, extents)):
            recs[3 + i] = self._inode(FT_FILE, ext[1], MX_FILE, 0xffff,
                    [ext], len(data), 0, "", PERM_FILE, uftype)

        # The free chain: the head is record 2, continuations are appended
        # after the files. A volume with nothing free carries a head whose
        # link is 0 and whose first entry is -1, exactly as 500BT1 does.
        chunks = [free[i:i + FREE_PER_REC]
                  for i in range(0, len(free), FREE_PER_REC)] or [[]]
        first = len(recs)
        for i, chunk in enumerate(chunks):
            last = i == len(chunks) - 1
            link = 0 if last and i == 0 else (0xffff if last else first + i)
            kind = T_FREE_HEAD if i == 0 else T_FREE_CONT
            rec = self._free_record(kind, link, chunk)
            if i == 0:
                recs[2] = rec
            else:
                recs.append(rec)

        fa = bytearray(b"".join(recs))
        fa += bytes(fa_blocks * self.blksz - len(fa))
        return bytes(fa)

    def _header(self):
        h = bytearray(self.blksz)
        struct.pack_into(">HH", h, 0x00, 0x0700, 0)       # s_format, corrupt
        h[0x04:0x14] = self.label.encode("ascii").ljust(16)[:16]
        struct.pack_into(">6I", h, 0x1c, self.blksz, 1, self.boot_blocks,
                1 + self.boot_blocks, self.version, self.blocks)
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
            struct.pack_into(">i", e, 20, 3 + i)
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
