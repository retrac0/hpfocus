#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Joel Heikkila
"""Decode Teledisk (.TD0) floppy images to raw sector images.

The Series 500 disks here are all signature "TD" (normal compression), so the
file body is not LZH-packed -- but individual sectors still use Teledisk's
per-sector encodings, which this handles.

Layout:
  12-byte image header, optional 10-byte + N comment block
  then repeating:
    4-byte track header:  nsectors, cylinder, head, crc
                          nsectors == 0xFF marks end of image
    per sector, 6-byte header: cyl, head, snum, size-code, flags, crc
      unless flags say otherwise, a data block follows:
        2-byte length, 1-byte encoding, then `length-1` bytes
      encodings: 0 = literal, 1 = 2-byte pattern repeated, 2 = RLE chunks

Usage:
  td0.py <image>                 summarise geometry
  td0.py <image> --raw OUT.img   write concatenated sector data
  td0.py <image> --strings       dump printable strings from sector data
"""
import argparse
import struct
import sys
from pathlib import Path

SIZES = {0: 128, 1: 256, 2: 512, 3: 1024, 4: 2048, 5: 4096, 6: 8192}
# Sector flag bits that mean "no data block follows".
NODATA = 0x30


def decode_sector(blob: bytes, size: int) -> bytes:
    """Expand one sector's encoded data block."""
    enc, body = blob[0], blob[1:]
    if enc == 0:
        return body[:size].ljust(size, b"\x00")
    if enc == 1:
        if len(body) < 4:
            return b"\x00" * size
        count = struct.unpack("<H", body[0:2])[0]
        return (body[2:4] * count)[:size].ljust(size, b"\x00")
    if enc == 2:
        out, p = bytearray(), 0
        while p + 1 < len(body):
            kind, n = body[p], body[p + 1]
            p += 2
            if kind == 0:                       # n literal bytes
                out += body[p:p + n]
                p += n
            else:                               # n repeats of a 2*kind chunk
                width = 2 * kind
                out += body[p:p + width] * n
                p += width
        return bytes(out[:size]).ljust(size, b"\x00")
    raise ValueError(f"unknown sector encoding {enc}")


def parse(data: bytes):
    """Yield (cyl, head, snum, size, flags, payload)."""
    if data[0:2] not in (b"TD", b"td"):
        sys.exit("not a Teledisk image")
    if data[0:2] == b"td":
        sys.exit("advanced (LZH) compression not supported; none of the "
                 "Series 500 images use it")
    p = 12
    if data[7] & 0x80:                          # comment block present
        ln = struct.unpack("<H", data[14:16])[0]
        p = 22 + ln
    while p + 4 <= len(data):
        nsec, cyl, head = data[p], data[p + 1], data[p + 2]
        p += 4
        if nsec == 0xFF:
            return
        for _ in range(nsec):
            if p + 6 > len(data):
                return
            scyl, shead, snum, scode, flags = data[p:p + 5]
            p += 6
            size = SIZES.get(scode, 0)
            if flags & NODATA:
                yield (scyl, shead, snum, size, flags, None)
                continue
            if p + 2 > len(data):
                return
            blen = struct.unpack("<H", data[p:p + 2])[0]
            p += 2
            # blen counts the encoding byte plus the encoded data.
            payload = decode_sector(data[p:p + blen], size)
            p += blen
            yield (scyl, shead, snum, size, flags, payload)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("image", type=Path)
    ap.add_argument("--raw", type=Path)
    ap.add_argument("--strings", action="store_true")
    args = ap.parse_args()

    data = args.image.read_bytes()
    sectors = list(parse(data))

    cyls = sorted({s[0] for s in sectors})
    heads = sorted({s[1] for s in sectors})
    sizes = sorted({s[3] for s in sectors})
    good = [s for s in sectors if s[5] is not None]
    total = sum(len(s[5]) for s in good)

    print(f"image    : {args.image.name}")
    print(f"cylinders: {len(cyls)} ({min(cyls)}..{max(cyls)})")
    print(f"heads    : {heads}")
    print(f"sectors  : {len(sectors)} total, {len(good)} with data")
    print(f"sec sizes: {sizes}")
    print(f"decoded  : {total:,} bytes")

    if args.raw:
        # Order by cylinder, head, sector number.
        ordered = sorted(good, key=lambda s: (s[0], s[1], s[2]))
        args.raw.write_bytes(b"".join(s[5] for s in ordered))
        print(f"wrote {args.raw}")

    if args.strings:
        blob = b"".join(s[5] for s in sorted(good, key=lambda s: (s[0], s[1], s[2])))
        run = bytearray()
        for b in blob:
            if 32 <= b < 127:
                run.append(b)
            else:
                if len(run) >= 6:
                    print(run.decode())
                run.clear()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
