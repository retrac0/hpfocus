#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Joel Heikkila
"""Assemble a Series 500 boot image from its volumes.

A bootable disc is not a filesystem. It carries a 48-byte SDF volume header,
a boot descriptor at 0x400, and then a raw slice of the boot image. The
descriptor says how the slices fit together:

    0x400  product identifier, identical across the set
    0x404  total image size in bytes, identical across the set
    0x408  number of volumes in the set
    0x40c  this volume's number, counting from 1
    0x410  byte offset of this volume's slice within the image
    0x414  unknown (8 on first volumes, 3/4 on continuations)
    0x418  unknown (5 on first volumes, 8/15 on continuations)

Every volume's slice starts at sector 8 (byte 0x800). The field at 0x414 was
originally read as the slice's starting sector -- it matches on first volumes
by coincidence, but continuation volumes carry 3 or 4 there and their slices
still start at 0x800. The per-segment checksum chain proves it: assembled
with each slice from 0x800, every segment of both the BASIC 2.0 and the
diagnostics environment images verifies end to end; assembled from the 0x414
field, the chain breaks at the first volume boundary.

So BASIC 2.0 is a single 372,932-byte image split across two discs, and the
diagnostics environment a 680,668-byte image across three.

    python3 tools/bootimage.py out.bin disc1.img disc2.img ...
"""
import struct
import sys
from pathlib import Path

SECTOR = 256
DESC = 0x400


def descriptor(data: bytes) -> dict:
    w = struct.unpack(">8I", data[DESC:DESC + 32])
    return {
        "product": w[0], "total": w[1], "volumes": w[2], "volume": w[3],
        "offset": w[4], "sector": w[5], "unknown": w[6],
        "label": data[DESC + 0x30:DESC + 0x80].decode("ascii", "replace").rstrip(),
    }


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2

    out = Path(sys.argv[1])
    vols = [Path(p).read_bytes() for p in sys.argv[2:]]
    descs = [descriptor(v) for v in vols]

    first = descs[0]
    for d in descs[1:]:
        if d["product"] != first["product"] or d["total"] != first["total"]:
            print("volumes are not from the same set", file=sys.stderr)
            return 1

    print(f"product   {first['product']:#010x}")
    print(f"label     {first['label']}")
    print(f"size      {first['total']:,} bytes across {first['volumes']} volume(s)\n")

    image = bytearray(first["total"])
    got = 0
    for data, d in sorted(zip(vols, descs), key=lambda p: p[1]["offset"]):
        start = 8 * SECTOR
        # A slice runs to the start of the next one, or to the end.
        nxt = [x["offset"] for x in descs if x["offset"] > d["offset"]]
        end_in_image = min(nxt) if nxt else first["total"]
        length = end_in_image - d["offset"]
        chunk = data[start:start + length]
        image[d["offset"]:d["offset"] + len(chunk)] = chunk
        got += len(chunk)
        print(f"  volume {d['volume']}: image {d['offset']:#08x}"
              f"..{d['offset'] + len(chunk):#08x}"
              f"  ({len(chunk):,} bytes)")

    out.write_bytes(bytes(image))
    print(f"\nwrote {out} -- {got:,} of {first['total']:,} bytes supplied")
    if got < first["total"]:
        print("WARNING: image incomplete, a volume is missing")
        return 1
    return verify(bytes(image))


def verify(d: bytes) -> int:
    """Walk the segment chain and check every per-segment byte-sum checksum.

    Segment 0 leads with an extra word (1ffc0000); its 7-word header is at
    +0x4 and its checksum word sits at +len. Later segments' headers follow
    the previous checksum word directly, with the checksum at hdr + len + 8.
    Each checksum covers every byte after the previous checksum word.
    """
    prev = 0
    hdr = 4
    seg = 0
    while True:
        ln = struct.unpack(">I", d[hdr + 4:hdr + 8])[0]
        name = d[hdr + 28:hdr + 44].decode("ascii", "replace").rstrip()
        ckpos = ln if seg == 0 else hdr + ln + 8
        if ckpos + 4 > len(d):
            print(f"  seg {seg} ({name}) checksum beyond the image -- BAD")
            return 1
        ck = struct.unpack(">I", d[ckpos:ckpos + 4])[0]
        if sum(d[prev:ckpos]) & 0xffffffff != ck:
            print(f"  seg {seg:2d} {name:<16} checksum BAD at {ckpos:#x}")
            return 1
        prev = ckpos + 4
        hdr = ckpos + 4
        seg += 1
        if hdr + 4 > len(d) or struct.unpack(">i", d[hdr:hdr + 4])[0] < 0 \
                or d[hdr:hdr + 2] == b"\x00\x00":
            print(f"all {seg} segments verify; chain ends at {hdr:#x}")
            return 0


if __name__ == "__main__":
    raise SystemExit(main())
