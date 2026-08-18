#!/usr/bin/env python3
"""Assemble a Series 500 boot image from its volumes.

A bootable disc is not a filesystem. It carries a 48-byte SDF volume header,
a boot descriptor at 0x400, and then a raw slice of the boot image. The
descriptor says how the slices fit together:

    0x400  product identifier, identical across the set
    0x404  total image size in bytes, identical across the set
    0x408  number of volumes in the set
    0x40c  this volume's number, counting from 1
    0x410  byte offset of this volume's slice within the image
    0x414  starting sector of the slice on this disc

So BASIC 2.0 is a single 372,932-byte image split across two discs: volume 1
supplies bytes 0..0x413ff from sector 8, volume 2 the remainder from sector 3.

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
        start = d["sector"] * SECTOR
        # A slice runs to the start of the next one, or to the end.
        nxt = [x["offset"] for x in descs if x["offset"] > d["offset"]]
        end_in_image = min(nxt) if nxt else first["total"]
        length = end_in_image - d["offset"]
        chunk = data[start:start + length]
        image[d["offset"]:d["offset"] + len(chunk)] = chunk
        got += len(chunk)
        print(f"  volume {d['volume']}: image {d['offset']:#08x}"
              f"..{d['offset'] + len(chunk):#08x}"
              f"  from sector {d['sector']} ({len(chunk):,} bytes)")

    out.write_bytes(bytes(image))
    print(f"\nwrote {out} -- {got:,} of {first['total']:,} bytes supplied")
    if got < first["total"]:
        print("WARNING: image incomplete, a volume is missing")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
