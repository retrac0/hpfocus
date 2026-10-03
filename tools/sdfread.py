#!/usr/bin/env python3
"""Reader for HP SDF, the Series 500 Structured Directory Format.

Structures are from the Series 500 fs(5) and inode(5) manual pages,
<https://www.typewritten.org/Manual/HP/HP-UX/5.00/man5/fs.html>.

    block 0        superblock (struct filsys)
    s_fa           the File Attributes file: 128-byte records
                     record 0  i-node of the FA file itself
                     record 1  i-node of the root directory
                     record 2+ free map, then i-nodes and extent maps
    directories    24-byte entries: 16-byte name, 4 reserved, 4-byte i-node

Everything is big-endian. A file's data lives in up to four extents named
directly by its i-node; longer files chain further extents through extent
map records, which this follows.

Two fields the manual pages do not name were read off the discs, because
between them they decide whether BASIC will load a file at all:

    +16  PROTECT.  Its top byte is 0x03 on a protected file and 0x53 on an
         unprotected one; a volume that never used PROTECT leaves the whole
         word 0xffffffff. Every file on the BASIC demo discs that answers
         `ERROR 62: File is protected or wrong PROTECT code` has 0x03 and
         every file that loads has 0x53 or 0xff -- and clearing the byte on
         a copy of the image makes the same LOAD succeed.
    -->  the `uftype` at +6 is the USER file type, and BASIC checks it:
         -5680 is a stored program, -5687 a data file, -5693 a directory.
         LOAD on a data file is `ERROR 58: Improper file type`, which is
         what /DEMO/DIR/others/SIERPINSKI answers -- it is the data that
         the SP programs plot, not a program.

    sdfread.py <image> [--path /DIR] [--extract OUTDIR] [--long]
"""
import argparse
import datetime
import os
import struct
from pathlib import Path

# BASIC's user file types, from the discs: what LOAD will and will not take.
FTYPE = {-5680: 'prog', -5687: 'data', -5693: 'dir'}
FA_REC = 128
DIRENT = 24

FT_DIR = 1          # di_ftype for a directory

# inode(5) times come in two encodings: di_atime is an ordinary big-endian
# Unix time_t, while di_ctime and di_mtime are time_ios -- two big-endian
# 32-bit words holding a Julian Day Number and seconds since midnight.
# 541 of the 551 time_ios fields on the floppies are well formed under that
# reading, and the dates they give are the right ones (the 9020 test discs
# stamp 1984, matching their Rev 3.00 labels).
JDN_EPOCH = 2440588  # Julian Day Number of 1970-01-01


def ios_time(rec, off):
    """time_ios -> Unix timestamp, or None if unset or malformed."""
    if len(rec) < off + 8:
        return None
    jdn, secs = struct.unpack(">ii", rec[off:off + 8])
    if not (2400000 < jdn < 2460000) or not 0 <= secs < 86400:
        return None
    return (jdn - JDN_EPOCH) * 86400 + secs


def unix_time(rec, off):
    """time_t -> Unix timestamp, or None if unset."""
    if len(rec) < off + 4:
        return None
    return struct.unpack(">I", rec[off:off + 4])[0] or None


class SDF:
    def __init__(self, data: bytes, blksz=None, skew=0, fa=None):
        self.d = data
        self._faext = None      # the FA file's extents, read on first use
        if blksz is not None:
            # Salvage mode: geometry supplied because block 0 is missing.
            self.blksz, self.skew = blksz, skew
            self.fa = fa if fa is not None else 0
            self.name, self.corrupt = "(no superblock)", 0
            self.boot = self.bootsz = self.version = 0
            self.maxblk = len(data) // blksz
            return
        self.skew = 0
        fmt, corrupt = struct.unpack(">HH", data[0:4])
        if fmt != 0x0700:
            raise ValueError(f"not an SDF volume (s_format {fmt:#06x})")
        self.corrupt = corrupt
        self.name = data[4:20].decode("ascii", "replace").rstrip()
        (self.blksz, self.boot, self.bootsz, self.fa,
         self.version, self.maxblk) = struct.unpack(">6I", data[28:52])

    # -- raw access -------------------------------------------------
    def block(self, n: int, count: int = 1) -> bytes:
        # An imager that started at block 1 leaves every block one short.
        off = (n - self.skew) * self.blksz
        return self.d[off:off + count * self.blksz]

    def fa_record(self, ino: int) -> bytes:
        """FA record `ino`, found through the FA file's own extents.

        The FA file is one run from s_fa only while it is small. BASDEM7's
        is three extents, (514,29),(799,29),(345,29), and the 7914 HP-UX
        volume's is seventeen -- four in its own i-node (record 0) and
        thirteen in the extent map that i-node names (record 129). Reading
        every record as if the file were contiguous resolves only the first
        extent correctly: the rest are read from whatever blocks follow it,
        and the walk drops their files without a word (the HP-UX volume
        listed no names at all). Salvage mode, which has no trustworthy
        record 0, keeps the contiguous reading.
        """
        if self._faext is None:
            self._faext = self._fa_extents()
        want, seen = ino * FA_REC, 0
        for start, n in self._faext:
            if want < seen + n * self.blksz:
                off = (start - self.skew) * self.blksz + (want - seen)
                return self.d[off:off + FA_REC]
            seen += n * self.blksz
        return b""

    def _fa_extents(self) -> list:
        # Bootstrap: the first extent always holds record 0, and the extent
        # map records are read with whatever extents are known so far.
        whole = [(self.fa, max(0, len(self.d) // self.blksz + self.skew - self.fa))]
        self._faext = whole
        if self.name == "(no superblock)":
            return whole
        r0 = self.inode(0)
        if r0 is None or r0["type"] != 1 or not r0["extents"] \
                or r0["extents"][0][0] != self.fa:
            return whole
        ext = [e for e in r0["extents"] if e[0] > 0 and e[1] > 0]
        nxt, guard = r0["exmap"], 0
        while nxt >= 0 and guard < 256:
            guard += 1
            self._faext = ext
            e = self.fa_record(nxt)
            if len(e) < FA_REC or struct.unpack(">H", e[0:2])[0] != 2:
                break
            cnt = struct.unpack(">H", e[2:4])[0]
            ext = ext + [x for x in (struct.unpack(">ii", e[24 + 8 * i:32 + 8 * i])
                                     for i in range(min(cnt, 13)))
                         if x[0] > 0 and x[1] > 0]
            nxt = struct.unpack(">i", e[8:12])[0]
        return ext

    # -- i-nodes ----------------------------------------------------
    def inode(self, ino: int) -> dict:
        e = self.fa_record(ino)
        if len(e) < FA_REC:
            return None
        typ, ftype, count, uftype = struct.unpack(">HHHh", e[0:8])
        blksz, mx = struct.unpack(">ii", e[28:36])
        exsz, exnum = struct.unpack(">HH", e[36:40])
        extents = [struct.unpack(">ii", e[40 + 8 * i:48 + 8 * i])
                   for i in range(4)]
        exmap, size, parent = struct.unpack(">iii", e[72:84])
        return {
            "ino": ino, "type": typ, "ftype": ftype, "count": count,
            "uftype": uftype,
            "blocks": blksz, "exnum": exnum, "size": size,
            "extents": extents[:max(0, min(exnum, 4))], "exmap": exmap,
            "parent": parent,
            "name": e[84:100].decode("ascii", "replace").rstrip("\x00 "),
            "ctime": ios_time(e, 8),
            "atime": unix_time(e, 100),
            "mtime": ios_time(e, 104),
            "uid": struct.unpack(">H", e[116:118])[0],
            "gid": struct.unpack(">H", e[118:120])[0],
            "mode": struct.unpack(">H", e[120:122])[0],
            "protect": struct.unpack(">I", e[16:20])[0],
        }

    def all_extents(self, ino: dict) -> list:
        """The i-node's own extents, then any chained through extent maps."""
        out = list(ino["extents"])
        nxt, guard = ino["exmap"], 0
        while nxt is not None and nxt >= 0 and guard < 256:
            guard += 1
            e = self.fa_record(nxt)
            if len(e) < FA_REC:
                break
            etype, exnum = struct.unpack(">HH", e[0:4])
            if etype != 2:
                break
            nnext = struct.unpack(">i", e[8:12])[0]
            for i in range(min(exnum, 13)):
                out.append(struct.unpack(">ii", e[24 + 8 * i:32 + 8 * i]))
            nxt = nnext
        return out

    def read_file(self, ino: dict) -> bytes:
        buf = bytearray()
        for start, n in self.all_extents(ino):
            if start <= 0 or n <= 0:
                continue
            buf += self.block(start, n)
        # A size of 0 is an empty file or directory, not "unknown": handing
        # back the whole extent there lists an empty directory's stale
        # blocks as entries (tools/sdf7914.py already reads it so).
        return bytes(buf[:ino["size"]]) if ino["size"] >= 0 else bytes(buf)

    # -- directories ------------------------------------------------
    def readdir(self, ino: dict) -> list:
        raw = self.read_file(ino)
        out = []
        for i in range(0, len(raw) - DIRENT + 1, DIRENT):
            e = raw[i:i + DIRENT]
            name = e[0:16].decode("ascii", "replace").rstrip()
            child = struct.unpack(">i", e[20:24])[0]
            if name and child > 0:
                out.append((name, child))
        return out

    def root(self) -> dict:
        return self.inode(1)

    def scan_inodes(self):
        """Find i-nodes directly, for images whose superblock was not read."""
        out = {}
        d = self.d
        for off in range(0, len(d) - FA_REC, FA_REC):
            if d[off:off + 2] != b"\x00\x01":
                continue
            ftype = struct.unpack(">H", d[off + 2:off + 4])[0]
            exnum = struct.unpack(">H", d[off + 38:off + 40])[0]
            size = struct.unpack(">i", d[off + 76:off + 80])[0]
            if ftype > 4 or not (1 <= exnum <= 4) or not (0 <= size < (1 << 26)):
                continue
            start, nblk = struct.unpack(">ii", d[off + 40:off + 48])
            if not (0 < start < (1 << 22) and 0 < nblk < (1 << 14)):
                continue
            ino = {"ino": off // FA_REC, "at": off, "type": 1, "ftype": ftype,
                   "count": 0, "blocks": nblk, "exnum": exnum, "size": size,
                   "extents": [struct.unpack(">ii", d[off + 40 + 8 * i:
                                                      off + 48 + 8 * i])
                               for i in range(min(exnum, 4))],
                   "exmap": struct.unpack(">i", d[off + 72:off + 76])[0],
                   "parent": struct.unpack(">i", d[off + 80:off + 84])[0],
                   "name": d[off + 84:off + 100].decode("ascii", "replace")
                            .rstrip("\x00 "),
                   "ctime": ios_time(d[off:off + FA_REC], 8),
                   "atime": unix_time(d[off:off + FA_REC], 100),
                   "mtime": ios_time(d[off:off + FA_REC], 104),
                   "uid": 0, "gid": 0,
                   "mode": struct.unpack(">H", d[off + 120:off + 122])[0],
                   "protect": struct.unpack(">I", d[off + 16:off + 20])[0]}
            out[ino["ino"]] = ino
        return out

    def walk(self, ino=None, path="/", depth=0, stack=()):
        """Yield (path, inode) for every name in the tree.

        The recursion guard is the ancestor stack, not a set of everything
        visited: a global set would silently mis-file a name reached twice,
        while the ancestor stack breaks a loop without deciding which name
        wins. (BASDEM7 once seemed to need it -- an "i-node 63" duplicating
        /DEMO, reached as /DEMO/DIR/9845c_data/land -- but that was record
        63 read from the wrong block, before fa_record followed the FA
        file's extents; it is a 9,980-byte data file.)
        """
        if ino is None:
            ino = self.root()
        if ino is None or depth > 24:
            return
        yield path, ino
        if ino["ftype"] != FT_DIR or ino["ino"] in stack:
            return
        for name, child in self.readdir(ino):
            ci = self.inode(child)
            if ci is None or ci["type"] != 1:
                continue
            sub = path.rstrip("/") + "/" + name
            yield from self.walk(ci, sub, depth + 1, stack + (ino["ino"],))


def stamp(t):
    """A timestamp for listings. UTC: the volume records no zone."""
    if t is None:
        return " " * 16
    return datetime.datetime.fromtimestamp(t, datetime.timezone.utc) \
                   .strftime("%Y-%m-%d %H:%M")


def set_times(dest, ino):
    """Restore mtime and atime onto an extracted file or directory."""
    mt, at = ino["mtime"], ino["atime"]
    if mt is None and at is None:
        return
    try:
        os.utime(dest, (at if at is not None else mt,
                        mt if mt is not None else at))
    except (OSError, OverflowError, ValueError):
        pass


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("image", type=Path)
    ap.add_argument("--extract", type=Path)
    ap.add_argument("--long", action="store_true")
    ap.add_argument("--offset", type=lambda s: int(s, 0), default=0,
                    help="byte offset of the volume within the image")
    args = ap.parse_args()

    data = args.image.read_bytes()[args.offset:]
    fs = SDF(data)
    print(f"volume   {fs.name!r}")
    print(f"blksz    {fs.blksz}   boot {fs.boot}+{fs.bootsz}   "
          f"FA at block {fs.fa}   maxblk {fs.maxblk:,}")
    print(f"version  {fs.version:#x}   corrupt={fs.corrupt}\n")

    files = dirs = total = 0
    distinct, written, dir_times = set(), {}, []
    for path, ino in fs.walk():
        isdir = ino["ftype"] == FT_DIR
        dirs += isdir
        files += not isdir
        if not isdir and ino["ino"] not in distinct:
            distinct.add(ino["ino"])
            total += max(ino["size"], 0)
        when = stamp(ino["mtime"])
        prot = "P" if (ino.get("protect", 0) >> 24) == 0x03 else " "
        if args.long:
            print(f"  {'d' if isdir else '-'}{prot} {ino['mode']:06o} "
                  f"{FTYPE.get(ino['uftype'], str(ino['uftype'])):>7} "
                  f"{ino['size']:>9,} ino{ino['ino']:<5} {when}  {path}")
        else:
            print(f"  {'d' if isdir else '-'}{prot} {ino['size']:>9,} {when}  {path}")
        if not args.extract:
            continue
        dest = args.extract / path.lstrip("/")
        try:
            if isdir:
                dest.mkdir(parents=True, exist_ok=True)
                dir_times.append((dest, ino))
                continue
            dest.parent.mkdir(parents=True, exist_ok=True)
            first = written.get(ino["ino"])
            if first is None:
                dest.write_bytes(fs.read_file(ino))
                written[ino["ino"]] = dest
                set_times(dest, ino)
            else:
                if dest.exists():
                    dest.unlink()
                os.link(first, dest)
        except (OSError, ValueError):
            pass
    for dest, ino in reversed(dir_times):
        set_times(dest, ino)

    print(f"\n{files} names ({len(distinct)} distinct files), "
          f"{dirs} directories, {total:,} bytes")
    if files - len(distinct):
        print(f"{files - len(distinct)} of those names are aliases of another")
    if args.extract:
        print(f"extracted to {args.extract}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
