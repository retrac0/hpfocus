#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Joel Heikkila
"""Read the SDF filesystem on the HP 7914 unit-save tapes.

The two tapes hold one 132 MB SDF volume. Block 0 -- the superblock -- was
never imaged, so the volume has to be entered through its File Attributes
file instead. That turns out to need no guesswork at all: the FA file
describes itself, and finding it pins every remaining unknown.

WHY THE SUPERBLOCK IS NOT NEEDED

fs(5) says block 0 carries s_blksz, s_fa and s_maxblk. All three are
recoverable from the FA file, and the FA file is findable directly because
of what fs(5) says lives at its front:

    record 0   the i-node of the FA file itself
    record 1   the i-node of the root directory

So the volume's entry point is two adjacent 128-byte records at the start of
some block, the first describing a completely full file (di_size ==
di_blksz * blocksize, the FA file having no partial last block) and the
second a directory that is its own parent -- which is what "/" is. That pair
occurs once in 133 MB, at image block 69,084:

    record 0  di_blksz 2016  di_size 2,064,384 = 2016 * 1024
              extents (69087,17) (5011,124) (13005,124) (20999,124)
              di_exmap 129
    record 1  ftype 1 (directory)  di_parent 1  extent (1076,1)  408 bytes

Record 129 lies inside the first extent, so the i-node's own four extents
bootstrap the lookup of the extent map that supplies the other thirteen:

    (28993,124) (36987,124) (44981,124) (52975,124) (60969,124) (68963,124)
    (76974,124) (84968,124) (92962,124) (100956,124) (108950,124)
    (116944,124) (124938,139)

389 blocks in the i-node plus 1,627 in the map is 2,016, exactly di_blksz.
The file closes on itself, so the extent list is right rather than merely
plausible.

THE TWO SKEWS

Those seventeen extents are also the geometry check, and they are the reason
an earlier single-skew reading could not work. Each extent must land on
128-byte records that are typed as SDF FA records; ordinary file data does
not satisfy that. Sixteen of the seventeen are 992/992 records typed -- but
only if the two tapes are given different origins:

    tape 1   disc blocks 1..65,406        offset = (block - 1) * 1024
    tape 2   disc blocks 65,409..130,814  offset = (block - 3) * 1024

so disc blocks 65,407 and 65,408 fall between the tapes and were never
imaged, along with block 0. Both tapes report reading 65,406 blocks, which
is why they look like the same range and were read as one; they are
consecutive spans of one disc with a two-block hole at the join.

i-node number is the FA record index directly -- root is i-node 1 and sits
at record 1. No base offset exists to solve for. Earlier attempts to fit one
were fitting an artifact.

DATES

Restored too, from the two encodings inode(5) gives (see JDN_EPOCH below).
They are coherent with the system's history: a 1987-06-01 bulk mtime across
/bin from the HP-UX install, per-file access times running 1989-1991, and a
last-touched date of 1991-08-20 on /usr/adm/pacct, the process accounting
file -- which dates the shutdown of the machine these tapes came off.

HARD LINKS

421 i-nodes carry more than one name, so a reader that dedupes by i-node
loses 881 names, /bin/sh (a link to /bin/rsh) among them. The walk is guarded
by its ancestor stack rather than a visited set, so every name is emitted.

    sdf7914.py t1.bin t2.bin --list
    sdf7914.py t1.bin t2.bin --extract OUTDIR
"""
import argparse
import datetime
import os
import struct
from collections import Counter
from pathlib import Path

BLK = 1024          # s_blksz, confirmed by (di_blksz-1)*1024 < di_size <= di_blksz*1024
FA_REC = 128        # every FA file entry is exactly 128 bytes, per fs(5)
DIRENT = 24         # 16-byte name, 4 reserved, 4-byte i-node
FT_DIR = 1

# inode(5) gives an i-node three times in two different encodings:
#   di_atime  time_t    4 bytes, an ordinary big-endian Unix epoch count
#   di_ctime  time_ios  8 bytes, HP's IOS time
#   di_mtime  time_ios  8 bytes
#
# time_ios is two big-endian 32-bit words: a Julian Day Number, then seconds
# since midnight. Both halves are checkable and both check out across the
# whole volume -- all 12,994 populated fields have a seconds word inside
# [0, 86400), which arbitrary data would satisfy about 0.002% of the time,
# and the smallest day number present is 2,440,588, which is exactly
# 1970-01-01. Unset times sit on that epoch rather than on zero.
JDN_EPOCH = 2440588  # Julian Day Number of 1970-01-01


def ios_time(rec, off):
    """time_ios -> Unix timestamp, or None if unset."""
    jdn, secs = struct.unpack(">ii", rec[off:off + 8])
    if jdn <= 0 or not 0 <= secs < 86400:
        return None
    return (jdn - JDN_EPOCH) * 86400 + secs


def unix_time(rec, off):
    """time_t -> Unix timestamp, or None if unset."""
    t = struct.unpack(">I", rec[off:off + 4])[0]
    return t or None

T_INODE, T_EXTMAP = 1, 2
FA_TYPES = {1, 2, 3, 4, 5, 6, 7}


class Disc:
    """The volume, as spans of disc blocks contributed by each tape."""

    def __init__(self, spans):
        # spans: list of (data, first_disc_block)
        self.spans = spans
        self.holes = 0

    def blkoff(self, blk):
        for data, first in self.spans:
            n = len(data) // BLK
            if first <= blk < first + n:
                return data, (blk - first) * BLK
        return None, None

    def read(self, start, count=1):
        """Blocks [start, start+count). Unimaged blocks read as zeros."""
        out = bytearray()
        for b in range(start, start + count):
            data, off = self.blkoff(b)
            if data is None:
                self.holes += 1
                out += bytes(BLK)
            else:
                out += data[off:off + BLK]
        return bytes(out)


def u16(b, o):
    return struct.unpack(">H", b[o:o + 2])[0]


def i32(b, o):
    return struct.unpack(">i", b[o:o + 4])[0]


def parse_inode(r, ino=None):
    """struct dinode, inode(5). Offsets verified field-by-field against it."""
    if len(r) < FA_REC or u16(r, 0) != T_INODE:
        return None
    exnum = u16(r, 38)
    return {
        "ino": ino,
        "ftype": u16(r, 2),
        "count": u16(r, 4),
        "blksz": i32(r, 28),
        "exnum": exnum,
        "extents": [struct.unpack(">ii", r[40 + 8 * i:48 + 8 * i])
                    for i in range(min(max(exnum, 0), 4))],
        "exmap": i32(r, 72),
        "size": i32(r, 76),
        "parent": i32(r, 80),
        "name": r[84:100].decode("ascii", "replace").rstrip("\x00 "),
        "ctime": ios_time(r, 8),
        "atime": unix_time(r, 100),
        "mtime": ios_time(r, 104),
        "uid": u16(r, 116),
        "gid": u16(r, 118),
        "mode": u16(r, 120),
    }


def parse_extmap(r):
    """struct em_rec, fs(5)."""
    if len(r) < FA_REC or u16(r, 0) != T_EXTMAP:
        return None
    exnum = u16(r, 2)
    if not 1 <= exnum <= 13:
        return None
    return {"exnum": exnum, "next": i32(r, 8), "last": i32(r, 12),
            "inode": i32(r, 16), "boffset": i32(r, 20),
            "extents": [struct.unpack(">ii", r[24 + 8 * i:32 + 8 * i])
                        for i in range(exnum)]}


def find_fa_root(spans):
    """Locate the FA file i-node and the root i-node beside it.

    The signature is structural, not a magic number: record 0 describes a
    file with no partial last block, record 1 is a self-parented directory.
    """
    out = []
    for data, first in spans:
        for off in range(0, len(data) - 2 * FA_REC, BLK):
            r0 = data[off:off + FA_REC]
            r1 = data[off + FA_REC:off + 2 * FA_REC]
            a = parse_inode(r0)
            b = parse_inode(r1)
            if not a or not b:
                continue
            if a["blksz"] <= 0 or a["size"] != a["blksz"] * BLK:
                continue
            if not 1 <= a["exnum"] <= 4 or a["extents"][0][1] <= 0:
                continue
            if b["ftype"] != FT_DIR or b["parent"] != 1 or b["exmap"] != -1:
                continue
            out.append((a, b, first + off // BLK))
    return out


def fa_extents(disc, fa_ino):
    """The FA file's full extent list, grown until it explains di_blksz.

    The i-node's own extents cover the front of the file, which is enough to
    resolve the extent-map record they point at, which extends the list, and
    so on. No circularity: each map record always lies in the part already
    mapped.
    """
    ext = [e for e in fa_ino["extents"] if e[1] > 0]

    def rec_at(n):
        want, seen = n * FA_REC, 0
        for s, cnt in ext:
            if seen + cnt * BLK > want:
                blk = s + (want - seen) // BLK
                return disc.read(blk)[(want - seen) % BLK:][:FA_REC]
            seen += cnt * BLK
        return None

    nxt, guard = fa_ino["exmap"], 0
    while nxt is not None and nxt >= 0 and guard < 64:
        guard += 1
        r = rec_at(nxt)
        if r is None:
            break
        m = parse_extmap(r)
        if m is None:
            break
        ext += [e for e in m["extents"] if e[1] > 0]
        nxt = m["next"]
        if sum(c for _, c in ext) >= fa_ino["blksz"]:
            break
    return ext


class SDF:
    def __init__(self, disc, fa_ino):
        self.disc = disc
        self.fa_ino = fa_ino
        self.ext = fa_extents(disc, fa_ino)
        self.fa = bytearray()
        for s, c in self.ext:
            self.fa += disc.read(s, c)
        self.nrec = len(self.fa) // FA_REC

    def record(self, n):
        if not 0 <= n < self.nrec:
            return None
        return bytes(self.fa[n * FA_REC:(n + 1) * FA_REC])

    def inode(self, ino):
        r = self.record(ino)
        return parse_inode(r, ino) if r else None

    def all_extents(self, ino):
        """i-node extents, then any chained through extent-map records.

        di_exmap is an FA record number, not a block number.
        """
        ext = [e for e in ino["extents"] if e[1] > 0]
        nxt, guard = ino["exmap"], 0
        while nxt is not None and nxt >= 0 and guard < 512:
            guard += 1
            r = self.record(nxt)
            if r is None:
                break
            m = parse_extmap(r)
            if m is None:
                break
            ext += [e for e in m["extents"] if e[1] > 0]
            nxt = m["next"]
        return ext

    def read_file(self, ino):
        buf = bytearray()
        for s, c in self.all_extents(ino):
            if s <= 0 or c <= 0:
                continue
            buf += self.disc.read(s, c)
            if ino["size"] > 0 and len(buf) >= ino["size"]:
                break
        return bytes(buf[:ino["size"]]) if ino["size"] >= 0 else bytes(buf)

    def readdir(self, ino):
        raw = self.read_file(ino)
        out = []
        for i in range(0, len(raw) - DIRENT + 1, DIRENT):
            e = raw[i:i + DIRENT]
            child = i32(e, 20)
            nm = e[0:16].rstrip(b"\x00 ")
            if not nm or child <= 0:
                continue
            out.append((nm.decode("ascii", "replace"), child))
        return out

    def walk(self, ino=None, path="", depth=0, stack=()):
        """Yield (path, i-node) for every name in the tree.

        The guard is the ancestor stack, not a global visited set: 421
        i-nodes here carry more than one name -- /bin/sh is a link to
        /bin/rsh, /dev/console to /dev/syscon -- and a global set would
        silently drop whichever name it met second.
        """
        if ino is None:
            ino = self.inode(1)
        if ino is None or depth > 32:
            return
        yield (path or "/"), ino
        if ino["ftype"] != FT_DIR or ino["ino"] in stack:
            return
        for name, child in self.readdir(ino):
            ci = self.inode(child)
            if ci is None:
                continue
            yield from self.walk(ci, f"{path}/{name}", depth + 1,
                                 stack + (ino["ino"],))


def build_disc(paths):
    """Give each tape its disc-block origin, chosen by structural validation.

    Tape 1 begins at disc block 1. Tape 2's origin is whichever value makes
    the FA extents that fall on it land on typed FA records -- an unambiguous
    test, since ordinary data does not decode as FA records.
    """
    datas = [p.read_bytes() for p in paths]
    if len(datas) == 1:
        # A lone tape 1 begins at disc block 1 -- its first sector is the
        # boot descriptor, 0xe9c28206. The volume tools/mkhpuxdisc.py
        # assembles, the file HP-UX actually boots and writes, begins at
        # block 0 with an SDF header (s_format 0x0700); mapping it from
        # block 1 put every block one off, "found" the FA file a block late
        # with 4 of its 17 extents, and listed 0 names without complaint.
        first = 0 if datas[0][:2] == b"\x07\x00" else 1
        return Disc([(datas[0], first)])

    n1 = len(datas[0]) // BLK
    best, best_score = None, -1
    for skew in range(0, 9):
        first2 = n1 + 1 + skew
        disc = Disc([(datas[0], 1), (datas[1], first2)])
        cands = find_fa_root([(datas[0], 1), (datas[1], first2)])
        score = 0
        for fa_ino, _root, _at in cands:
            for s, c in fa_extents(disc, fa_ino):
                blk = disc.read(s, c)
                score += sum(1 for i in range(0, len(blk), FA_REC)
                             if u16(blk, i) in FA_TYPES)
        if score > best_score:
            best, best_score = first2, score
    return Disc([(datas[0], 1), (datas[1], best)]), best


def stamp(t):
    """A timestamp for listings. UTC: the volume records no zone."""
    if t is None:
        return " " * 16
    return datetime.datetime.fromtimestamp(t, datetime.timezone.utc) \
                   .strftime("%Y-%m-%d %H:%M")


def set_times(dest, ino, stats):
    """Restore mtime and atime onto an extracted file or directory."""
    mt, at = ino["mtime"], ino["atime"]
    if mt is None and at is None:
        return
    try:
        os.utime(dest, (at if at is not None else mt,
                        mt if mt is not None else at))
        stats["dated"] += 1
    except (OSError, OverflowError, ValueError):
        stats["undated"] += 1


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("tapes", nargs="+", type=Path, help="tape 1, then tape 2")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--extract", type=Path)
    a = ap.parse_args()

    built = build_disc(a.tapes)
    disc, first2 = built if isinstance(built, tuple) else (built, None)
    total = sum(len(d) for d, _ in disc.spans)
    print(f"{len(disc.spans)} tape(s), {total:,} bytes, {total // BLK:,} blocks")
    if first2:
        print(f"tape 2 begins at disc block {first2:,} "
              f"(blocks {len(disc.spans[0][0]) // BLK + 1:,}"
              f"..{first2 - 1:,} were never imaged)")

    cands = find_fa_root(disc.spans)
    if not cands:
        print("no FA file found")
        return 1
    fa_ino, root_ino, at = cands[0]
    print(f"FA file i-node at disc block {at:,}: {fa_ino['blksz']:,} blocks, "
          f"{fa_ino['size']:,} bytes")

    fs = SDF(disc, fa_ino)
    print(f"FA extents: {len(fs.ext)} covering "
          f"{sum(c for _, c in fs.ext):,} blocks -> {fs.nrec:,} records")

    files = dirs = nbytes = 0
    stats = Counter()
    seen_ino = {}          # i-node -> first path written, so links stay links
    distinct = set()       # counted whether or not we are extracting
    dir_times = []         # applied last: writing into a dir resets its mtime
    for path, ino in fs.walk():
        isdir = ino["ftype"] == FT_DIR
        dirs += isdir
        files += not isdir
        if not isdir and ino["ino"] not in distinct:
            distinct.add(ino["ino"])
            nbytes += max(ino["size"], 0)
        if a.list:
            print(f"  {'d' if isdir else '-'} {ino['mode']:06o} "
                  f"{max(ino['size'], 0):>10,} {stamp(ino['mtime'])}  {path}")
        if not a.extract:
            continue
        dest = a.extract / path.lstrip("/")
        try:
            if isdir:
                dest.mkdir(parents=True, exist_ok=True)
                dir_times.append((dest, ino))
                continue
            dest.parent.mkdir(parents=True, exist_ok=True)
            first = seen_ino.get(ino["ino"])
            if first is None:
                dest.write_bytes(fs.read_file(ino))
                seen_ino[ino["ino"]] = dest
                set_times(dest, ino, stats)
            else:
                if dest.exists():
                    dest.unlink()
                os.link(first, dest)
                stats["links"] += 1
        except (OSError, ValueError):
            stats["failed"] += 1

    for dest, ino in reversed(dir_times):
        set_times(dest, ino, stats)

    print(f"\n{files:,} names ({len(distinct):,} distinct files), "
          f"{dirs:,} directories, {nbytes:,} bytes")
    if files - len(distinct):
        print(f"{files - len(distinct):,} of those names are hard links"
              + (" (reproduced with link(2))" if a.extract else ""))
    if stats["dated"]:
        print(f"{stats['dated']:,} timestamps restored"
              + (f", {stats['undated']:,} out of range" if stats["undated"] else ""))
    if stats["failed"]:
        print(f"{stats['failed']:,} failed to write")
    if disc.holes:
        print(f"{disc.holes:,} block reads fell in the unimaged gap")
    if a.extract:
        print(f"extracted to {a.extract}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
