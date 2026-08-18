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

    sdfread.py <image> [--path /DIR] [--extract OUTDIR] [--long]
"""
import argparse
import struct
from pathlib import Path

FA_REC = 128
DIRENT = 24

FT_DIR = 1          # di_ftype for a directory


class SDF:
    def __init__(self, data: bytes):
        self.d = data
        fmt, corrupt = struct.unpack(">HH", data[0:4])
        if fmt != 0x0700:
            raise ValueError(f"not an SDF volume (s_format {fmt:#06x})")
        self.corrupt = corrupt
        self.name = data[4:20].decode("ascii", "replace").rstrip()
        (self.blksz, self.boot, self.bootsz, self.fa,
         self.version, self.maxblk) = struct.unpack(">6I", data[28:52])

    # -- raw access -------------------------------------------------
    def block(self, n: int, count: int = 1) -> bytes:
        off = n * self.blksz
        return self.d[off:off + count * self.blksz]

    def fa_record(self, ino: int) -> bytes:
        off = self.fa * self.blksz + ino * FA_REC
        return self.d[off:off + FA_REC]

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
            "blocks": blksz, "exnum": exnum, "size": size,
            "extents": extents[:max(0, min(exnum, 4))], "exmap": exmap,
            "parent": parent,
            "name": e[84:100].decode("ascii", "replace").rstrip("\x00 "),
            "uid": struct.unpack(">H", e[116:118])[0],
            "gid": struct.unpack(">H", e[118:120])[0],
            "mode": struct.unpack(">H", e[120:122])[0],
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
        return bytes(buf[:ino["size"]]) if ino["size"] > 0 else bytes(buf)

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

    def walk(self, ino=None, path="/", depth=0, seen=None):
        """Yield (path, inode) for every reachable entry."""
        if seen is None:
            seen = set()
        if ino is None:
            ino = self.root()
        if ino is None or depth > 24:
            return
        yield path, ino
        if ino["ftype"] != FT_DIR:
            return
        for name, child in self.readdir(ino):
            if child in seen:
                continue
            seen.add(child)
            ci = self.inode(child)
            if ci is None or ci["type"] != 1:
                continue
            sub = path.rstrip("/") + "/" + name
            yield from self.walk(ci, sub, depth + 1, seen)


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
    for path, ino in fs.walk():
        isdir = ino["ftype"] == FT_DIR
        dirs += isdir
        files += not isdir
        total += 0 if isdir else max(ino["size"], 0)
        if args.long:
            print(f"  {'d' if isdir else '-'} {ino['mode']:06o} "
                  f"{ino['size']:>9,} ino{ino['ino']:<5} {path}")
        else:
            print(f"  {'d' if isdir else '-'} {ino['size']:>9,}  {path}")
        if args.extract and not isdir:
            dest = args.extract / path.lstrip("/")
            dest.parent.mkdir(parents=True, exist_ok=True)
            try:
                dest.write_bytes(fs.read_file(ino))
            except OSError:
                pass

    print(f"\n{files} files, {dirs} directories, {total:,} bytes")
    if args.extract:
        print(f"extracted to {args.extract}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
