#!/usr/bin/env python3
"""List and extract files from an SDF volume.

SDF is the Series 500's own filesystem. A data volume carries a 48-byte
header and then a directory of 36-byte entries beginning at the offset held
in the header at 0x1c:

    +0x00  18 bytes  name, space padded
    +0x12   2 bytes  type
    +0x14   4 bytes  start offset in bytes
    +0x18   4 bytes  length in bytes
    +0x1c   4 bytes  date
    +0x20   4 bytes  flags

Boot volumes have no directory -- they carry a raw slice of a boot image
instead, described by the record at 0x400 (see tools/bootimage.py).

    sdfdir.py <image>
    sdfdir.py <image> --extract NAME --out FILE
"""
import argparse
import struct
import sys
from pathlib import Path

ENTRY = 36


def volume(data: bytes) -> dict:
    return {
        "magic": struct.unpack(">I", data[0:4])[0],
        "part": data[4:20].decode("ascii", "replace"),
        "dir": struct.unpack(">I", data[0x1c:0x20])[0],
    }


def entries(data: bytes, start: int):
    off = start + 0x10          # the directory proper follows a 16-byte head
    while off + ENTRY <= len(data):
        e = data[off:off + ENTRY]
        name = e[0:18].decode("ascii", "replace").rstrip()
        if not name or not name[0].isalnum():
            break
        typ = struct.unpack(">H", e[18:20])[0]
        pos, length, date, flags = struct.unpack(">4I", e[20:36])
        yield {"name": name, "type": typ, "pos": pos, "len": length,
               "date": date, "flags": flags, "at": off}
        off += ENTRY


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("image", type=Path)
    ap.add_argument("--extract")
    ap.add_argument("--out", type=Path)
    args = ap.parse_args()

    data = args.image.read_bytes()
    v = volume(data)

    if v["magic"] != 0x07000000:
        print(f"not an SDF volume (magic {v['magic']:#010x})", file=sys.stderr)
        return 1
    if v["dir"] == 0x400:
        print("this is a boot volume, not a filesystem -- "
              "use tools/bootimage.py", file=sys.stderr)
        return 1

    if args.extract:
        for e in entries(data, v["dir"]):
            if e["name"].lower() == args.extract.lower():
                blob = data[e["pos"]:e["pos"] + e["len"]]
                dest = args.out or Path(e["name"])
                dest.write_bytes(blob)
                print(f"wrote {len(blob)} bytes to {dest}")
                return 0
        print(f"no such file: {args.extract}", file=sys.stderr)
        return 1

    print(f"volume    {args.image.name}")
    print(f"part      {v['part']}")
    print(f"directory at {v['dir']:#x}\n")
    print(f"{'name':<20} {'type':>6} {'start':>9} {'length':>9}")
    print("-" * 48)
    n = 0
    for e in entries(data, v["dir"]):
        print(f"{e['name']:<20} {e['type']:6x} {e['pos']:9x} {e['len']:9,}")
        n += 1
    print(f"\n{n} entries")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
