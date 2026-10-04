#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Joel Heikkila
"""List the LIF directory of a Series 500 media image.

LIF (Logical Interchange Format) is HP's device-independent volume format.
Blocks are 256 bytes; the volume header sits in block 0 and points at a
directory of 32-byte entries.

Usage: lifdir.py <image> [--extract NAME --out FILE]
"""
import argparse
import struct
import sys
from pathlib import Path

BLOCK = 256
ENTRY = 32

# Only types we can positively identify are named; everything else prints as
# raw hex rather than guessing.
TYPES = {
    0x0000: "(purged)",
    0x0001: "TEXT",
    0xFFFF: "(end of directory)",
}


def bcd(b: int) -> str:
    """Decode a packed-BCD byte. Non-BCD nibbles are flagged, not silently
    mangled -- a bad nibble here is a signal of media damage."""
    hi, lo = b >> 4, b & 0xF
    if hi > 9 or lo > 9:
        return f"<{b:02x}>"
    return f"{hi}{lo}"


def timestamp(raw: bytes) -> str:
    if raw == b"\x00" * 6:
        return ""
    yy, mm, dd, hh, mi, ss = (bcd(b) for b in raw)
    if any(f.startswith("<") for f in (yy, mm, dd, hh, mi, ss)):
        return f"{yy}-{mm}-{dd} {hh}:{mi}:{ss}  [invalid BCD]"
    year = int(yy)
    century = 19 if year >= 70 else 20
    return f"{century}{yy}-{mm}-{dd} {hh}:{mi}:{ss}"


def read_volume(data: bytes) -> dict:
    magic, label, dir_start, lif_id = struct.unpack(">H6sIH", data[0:14])
    if magic != 0x8000:
        sys.exit(f"not a LIF volume: magic {magic:#06x}, expected 0x8000")
    dir_blocks = struct.unpack(">I", data[16:20])[0]
    version = struct.unpack(">H", data[20:22])[0]
    return {
        "label": label.decode("ascii", "replace").strip(),
        "dir_start": dir_start,
        "dir_blocks": dir_blocks,
        "lif_id": lif_id,
        "version": version,
        "created": timestamp(data[36:42]),
    }


def read_dir(data: bytes, vol: dict) -> list:
    start = vol["dir_start"] * BLOCK
    end = start + vol["dir_blocks"] * BLOCK
    out = []
    for off in range(start, min(end, len(data)), ENTRY):
        e = data[off:off + ENTRY]
        if len(e) < ENTRY:
            break
        ftype = struct.unpack(">H", e[10:12])[0]
        if ftype == 0xFFFF:
            break
        name = e[0:10].decode("ascii", "replace").rstrip(" \x00")
        blk, nblk = struct.unpack(">II", e[12:20])
        out.append({
            "name": name,
            "type": ftype,
            "start": blk,
            "blocks": nblk,
            "bytes": nblk * BLOCK,
            "time": timestamp(e[20:26]),
            "volnum": struct.unpack(">H", e[26:28])[0],
        })
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("image", type=Path)
    ap.add_argument("--extract", metavar="NAME")
    ap.add_argument("--out", type=Path)
    args = ap.parse_args()

    data = args.image.read_bytes()
    vol = read_volume(data)
    entries = read_dir(data, vol)

    if args.extract:
        for e in entries:
            if e["name"] == args.extract:
                blob = data[e["start"] * BLOCK: e["start"] * BLOCK + e["bytes"]]
                dest = args.out or Path(e["name"])
                dest.write_bytes(blob)
                print(f"wrote {len(blob)} bytes to {dest}")
                return 0
        sys.exit(f"no such file: {args.extract}")

    print(f"image      : {args.image}  ({len(data):,} bytes, "
          f"{len(data) // BLOCK:,} blocks)")
    print(f"volume     : {vol['label']!r}")
    print(f"LIF id     : {vol['lif_id']:#06x}   version {vol['version']}")
    print(f"directory  : block {vol['dir_start']}, {vol['dir_blocks']} blocks "
          f"({vol['dir_blocks'] * BLOCK // ENTRY} entry slots)")
    if vol["created"]:
        print(f"created    : {vol['created']}")
    print(f"\n{len(entries)} entries\n")
    print(f"{'name':<12} {'type':<10} {'start':>8} {'blocks':>8} "
          f"{'bytes':>12}  {'timestamp'}")
    print("-" * 72)
    for e in entries:
        t = TYPES.get(e["type"], f"{e['type']:#06x}")
        print(f"{e['name']:<12} {t:<10} {e['start']:>8} {e['blocks']:>8} "
              f"{e['bytes']:>12,}  {e['time']}")

    used = max((e["start"] + e["blocks"] for e in entries), default=0)
    print(f"\nhighest block used: {used:,} of {len(data) // BLOCK:,}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
