#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Joel Heikkila
"""List and extract the named segments in an assembled Series 500 boot image.

Feed it the output of tools/bootimage.py. The segment chain it walks is the
same one bootimage.py's verify() checks, but here for extraction rather than
a checksum: each segment is a 7-word header, a 16-byte name, then code, and
the layout differs slightly for segment 0 (it has a 4-byte marker word ahead
of the header, and its checksum sits at an absolute offset rather than one
relative to the header):

    segment 0:   marker(4) header(28) name(16) code...   checksum @ length
    segment N>0: header(28) name(16) code...             checksum @ hdr+length+8

`length` (header word 1) measures from the header's start for segment 0,
and from 8 bytes into the header for every later segment -- so it always
includes the trailing header words and the name alongside the code. Strip
those 44 bytes (36 for segment 0, since its marker/checksum offsets already
account for the other 8) to get code-only bytes:

    code_start  = hdr + 44
    code_length = length - 36              (segment 0: length - 48)

Usage:
  segdump.py <image>                            list segments
  segdump.py <image> --extract DIR [NAME ...]   write DIR/<name>.bin (code only,
                                                 header and name stripped);
                                                 with no NAMEs, extract all
"""
import argparse
import struct
from pathlib import Path


def segments(d: bytes):
    """Yield dicts for each segment: name, hdr, length, code_start, code_end."""
    prev = 0
    hdr = 4
    seg = 0
    while True:
        ln = struct.unpack(">I", d[hdr + 4:hdr + 8])[0]
        name = d[hdr + 28:hdr + 44].decode("ascii", "replace").rstrip()
        ckpos = ln if seg == 0 else hdr + ln + 8
        code_start = hdr + 44
        code_len = ln - (48 if seg == 0 else 36)
        yield {
            "index": seg, "name": name, "hdr": hdr, "length": ln,
            "code_start": code_start, "code_end": code_start + code_len,
            "ckpos": ckpos,
        }
        prev = ckpos + 4
        hdr = ckpos + 4
        seg += 1
        if hdr + 4 > len(d) or struct.unpack(">i", d[hdr:hdr + 4])[0] < 0 \
                or d[hdr:hdr + 2] == b"\x00\x00":
            return


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("image", type=Path)
    ap.add_argument("--extract", type=Path, metavar="DIR",
                     help="write each segment's code to DIR/<name>.bin")
    ap.add_argument("names", nargs="*",
                     help="restrict to these segment names (default: all)")
    args = ap.parse_args()

    data = args.image.read_bytes()
    wanted = {n.upper() for n in args.names} if args.names else None

    if not args.extract:
        print(f"{'#':>3}  {'hdr':>8}  {'code':>8}..{'end':>8}  {'bytes':>8}  name")
        for s in segments(data):
            if wanted and s["name"].upper() not in wanted:
                continue
            print(f"{s['index']:3d}  {s['hdr']:#08x}  {s['code_start']:#08x}"
                  f"..{s['code_end']:#08x}  {s['code_end'] - s['code_start']:8,}"
                  f"  {s['name']}")
        return 0

    args.extract.mkdir(parents=True, exist_ok=True)
    found = set()
    for s in segments(data):
        if wanted and s["name"].upper() not in wanted:
            continue
        found.add(s["name"].upper())
        out = args.extract / f"{s['name']}.bin"
        out.write_bytes(data[s["code_start"]:s["code_end"]])
        print(f"wrote {out}  ({s['code_end'] - s['code_start']:,} bytes)")

    missing = (wanted or set()) - found
    if missing:
        print(f"not found: {', '.join(sorted(missing))}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
