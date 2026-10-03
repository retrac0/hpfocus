#!/usr/bin/env python3
"""Read a stored BASIC 2.0 program file from a Series 500 disc.

BASIC's stored programs are not source listings and not pure object code:
they are both at once. Each subprogram carries its statements as compiled
FOCUS instructions with the operands the compiler resolved, and carries the
names and comment text those statements came from as plain ASCII alongside.
That is why LIST can reproduce a program it never re-parses, and it is why
these files are the only place a *complete* HP-built program segment can be
read at rest -- the transfer table indices in the code are the ones HP's own
compiler assigned.

Layout, as read off the 9020 System Functional Tests 3.00 and the BASIC demo
discs (all big-endian, all offsets from the start of the file):

    +0   u32  number of subprograms
    +4   u32  the same count again (purpose unknown; equal in every file seen)
    +8   u32  entries in the fixed table that follows the directory
    +12  u32  format version, 1 everywhere

    directory, one 36-byte entry per subprogram:
      +0   18 bytes  name, space padded
      +18  2 bytes   never initialised -- the two bytes past a short name
                     hold whatever was in the buffer, and in these files
                     that is stale compiler output (0x2c04 is LDIS #4,
                     0x3200 a PCLS). Ignore them; they are not a field.
      +20  u32       offset of the body
      +24  u32       size field (not the distance to the next body)
      +28  i32       a signed delta, zero for most subprograms
      +32  u32       1

    then `table entries` x 8 bytes, nearly all zero -- one slot is used in
    the files seen, holding {offset, length}, so this is a side table
    (COMMON or the file/device list) rather than anything per-statement.

    each body opens with twelve u32 of its own before the statements begin;
    words 3-6 are offsets within the body and word 1 is its total length.

Inside a body, identifiers and quoted text are length-prefixed ASCII and the
statements are FOCUS code, so a plain hex dump reads as alternating runs.
`--strings` pulls the readable runs out, which is enough to recover what a
diagnostic does; `--extract` writes each body to its own file for
tools/focusdis.py to disassemble.

    tools/progfile.py TEST                 header + directory
    tools/progfile.py TEST --strings       readable runs per subprogram
    tools/progfile.py TEST --extract DIR   one file per subprogram
"""

import argparse
import re
import struct
from pathlib import Path

HEADER = 16
DIR_ENTRY = 36
NAME_LEN = 18
NAME_SLACK = 2   # the uninitialised bytes described above
BODY_HEADER = 48


def u32(data, off):
    return struct.unpack(">I", data[off:off + 4])[0]


def i32(data, off):
    return struct.unpack(">i", data[off:off + 4])[0]


class Subprogram:
    def __init__(self, name, offset, size, delta, flag):
        self.name = name
        self.offset = offset
        self.size = size
        self.delta = delta
        self.flag = flag


class ProgramFile:
    def __init__(self, data):
        self.data = data
        self.count = u32(data, 0)
        self.count2 = u32(data, 4)
        self.table_entries = u32(data, 8)
        self.version = u32(data, 12)
        self.subs = []
        off = HEADER
        for _ in range(self.count):
            name = data[off:off + NAME_LEN].decode("latin1").rstrip()
            fields = off + NAME_LEN + NAME_SLACK
            self.subs.append(Subprogram(name, u32(data, fields),
                    u32(data, fields + 4), i32(data, fields + 8),
                    u32(data, fields + 12)))
            off += DIR_ENTRY
        self.table_off = off
        self.table = [(u32(data, off + 8 * i), u32(data, off + 8 * i + 4))
                for i in range(self.table_entries)]

    def body(self, n):
        """Bytes of subprogram n, ending where the next one starts."""
        start = self.subs[n].offset
        end = (self.subs[n + 1].offset if n + 1 < len(self.subs)
                else len(self.data))
        return self.data[start:end]


# A run is worth printing when it looks like something a person wrote: the
# compiled code between the runs is full of incidental printable bytes, and
# a length of four keeps those out without losing short identifiers.
RUN = re.compile(rb"[ -~]{4,}")


def strings(body):
    return [m.group().decode("latin1") for m in RUN.finditer(body)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--strings", action="store_true",
            help="print the readable runs inside each subprogram")
    ap.add_argument("--extract", type=Path,
            help="write each subprogram body to this directory")
    args = ap.parse_args()

    data = Path(args.image).read_bytes()
    prog = ProgramFile(data)

    print(f"stored BASIC program: {prog.count} subprograms, "
            f"{prog.table_entries}-entry table, version {prog.version}, "
            f"{len(data):,} bytes")
    used = [(o, l) for o, l in prog.table if o or l]
    if used:
        print("  side table:", ", ".join(f"{{{o:#x}, {l:#x}}}" for o, l in used))
    print()
    print("   #  name                   offset      size    delta")
    for n, sub in enumerate(prog.subs):
        print(f"  {n:2}  {sub.name:<20}  {sub.offset:#8x}  {sub.size:#8x}"
                f"  {sub.delta:6}")

    if args.strings:
        for n, sub in enumerate(prog.subs):
            print(f"\n=== {sub.name} ({sub.offset:#x})")
            for s in strings(prog.body(n)):
                print("   ", s)

    if args.extract:
        args.extract.mkdir(parents=True, exist_ok=True)
        for n, sub in enumerate(prog.subs):
            safe = re.sub(r"[^A-Za-z0-9_.-]", "_", sub.name) or f"sub{n}"
            (args.extract / f"{n:02d}_{safe}.bin").write_bytes(prog.body(n))
        print(f"\nextracted {len(prog.subs)} subprograms to {args.extract}")


if __name__ == "__main__":
    main()
