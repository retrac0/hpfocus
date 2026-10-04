#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Joel Heikkila
"""List a Series 500 HP-UX cpio archive extracted from the LIF tape.

The tape's S* filesets are old-binary cpio (magic 070707) with **big-endian**
16-bit fields -- FOCUS is a big-endian machine, so GNU cpio, which assumes host
byte order, will not read these.

Header is 26 bytes:
  magic dev ino mode uid gid nlink rdev  (8 x uint16)
  mtime (2 x uint16, high word first)
  namesize (uint16)
  filesize (2 x uint16, high word first)
then the NUL-terminated name padded to an even length, then the file data
padded to an even length.

Usage: cpiolist.py <archive> [--offset N] [--extract PATH --out FILE]
"""
import argparse
import struct
import sys
from pathlib import Path

MAGIC = 0o070707
TRAILER = "TRAILER!!!"

S_IFMT, S_IFREG, S_IFDIR, S_IFLNK = 0o170000, 0o100000, 0o040000, 0o120000


def kind(mode: int) -> str:
    t = mode & S_IFMT
    return {S_IFREG: "-", S_IFDIR: "d", S_IFLNK: "l"}.get(t, "?")


def perms(mode: int) -> str:
    out = ""
    for who in (6, 3, 0):
        bits = (mode >> who) & 7
        out += ("r" if bits & 4 else "-") + ("w" if bits & 2 else "-") + \
               ("x" if bits & 1 else "-")
    return out


def walk(data: bytes, start: int = 0):
    """Yield (name, mode, size, data_offset). Stops at TRAILER or damage."""
    p = start
    while p + 26 <= len(data):
        f = struct.unpack(">13H", data[p:p + 26])
        if f[0] != MAGIC:
            yield ("<lost sync>", 0, 0, p)
            return
        mode, namesize = f[3], f[10]
        size = (f[11] << 16) | f[12]
        p += 26
        raw = data[p:p + namesize]
        name = raw.split(b"\x00")[0].decode("ascii", "replace")
        p += namesize + (namesize & 1)
        if name == TRAILER:
            return
        yield (name, mode, size, p)
        p += size + (size & 1)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("archive", type=Path)
    ap.add_argument("--offset", type=int, default=0)
    ap.add_argument("--extract")
    ap.add_argument("--out", type=Path)
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    data = args.archive.read_bytes()

    if args.extract:
        for name, mode, size, off in walk(data, args.offset):
            if name == args.extract:
                dest = args.out or Path(Path(name).name)
                dest.write_bytes(data[off:off + size])
                print(f"wrote {size} bytes to {dest}")
                return 0
        sys.exit(f"not found: {args.extract}")

    total = nfiles = ndirs = 0
    lost = False
    for name, mode, size, off in walk(data, args.offset):
        if name == "<lost sync>":
            print(f"\n*** lost cpio sync at offset {off:,} "
                  f"({100*off/len(data):.1f}% through the archive) ***")
            lost = True
            break
        if kind(mode) == "d":
            ndirs += 1
        else:
            nfiles += 1
            total += size
        if not args.quiet:
            print(f"{kind(mode)}{perms(mode)} {size:>9,}  {name}")

    print(f"\n{nfiles:,} files ({total:,} bytes), {ndirs:,} directories"
          f"{'  [TRUNCATED]' if lost else ''}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
