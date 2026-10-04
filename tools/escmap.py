#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Joel Heikkila
"""Map every constant-operand ESCP site in a Series 500 boot image.

ESCP raises the kernel's software exception -- the "escape" that TRY
handlers catch -- with the error number on top of stack. It is the
kernel's own error channel, distinct from hardware traps, and the number
is what the panic report and FOCUS_ESCLOG print. Nearly every raise site
loads its number as a constant in the instruction immediately before, so
one static scan turns any observed escape into a short list of raising
sites -- and each site's neighbourhood says what condition failed.

Patterns recognised (halfwords, big-endian; ESCP = 0x0061, ESCP,B = 0x1061):

    2cNN 0061           LDIS #N;  ESCP
    2dNN 0061           LNIS #N;  ESCP   (negative)
    e60X XXXX 0061      LDI  #imm; ESCP  (19-bit immediate)

A site whose operand is computed (LDS from a variable, DUP of a caller's
argument) cannot be resolved statically; those are listed separately, as
re-raise sites -- the TRY/re-raise idiom the drivers use to pass an
escape upward.

The image may be a raw assembled boot image (tools/bootimage.py output,
or the slice a loader reads) or a 7914 unit save, which carries the boot
image at +0x400; the difference is detected by where SYSTEM POWERUP sits.

Usage:
  escmap.py <image>                 the full table, sorted by error number
  escmap.py <image> --value 66      sites for one number
  escmap.py <image> --raises        include the non-constant (re-raise) sites
"""
from __future__ import annotations  # PEP 604/585 hints on Python 3.8
import argparse
import struct
import sys
from pathlib import Path

from segdump import segments


def load_boot_image(path: Path) -> bytes:
    d = path.read_bytes()
    if d[0x20:0x2e] == b"SYSTEM POWERUP":
        return d
    if d[0x420:0x42e] == b"SYSTEM POWERUP":
        return d[0x400:]
    sys.exit(f"{path}: no SYSTEM POWERUP module at +0x20 or +0x420 -- "
             "not a boot image or unit save")


def hw(d: bytes, o: int) -> int:
    return struct.unpack_from(">H", d, o)[0]


def scan(d: bytes):
    """Yield (value_or_None, file_offset, b3) for every ESCP in code."""
    for s in segments(d):
        lo, hi = s["code_start"], s["code_end"]
        # PB corresponds to 8 bytes into the segment header, 36 before
        # the code (see segdump.py), so PB-relative = file - pb_file.
        pb_file = s["code_start"] - 36
        for o in range(lo, hi - 1, 2):
            w = hw(d, o)
            if w != 0x0061 and w != 0x1061:
                continue
            value = None
            if o - 2 >= lo:
                p = hw(d, o - 2)
                if (p & 0xff00) == 0x2c00:            # LDIS
                    value = p & 0xff
                elif (p & 0xff00) == 0x2d00:          # LNIS
                    value = -(p & 0xff)
            if value is None and o - 4 >= lo \
                    and d[o - 4] == 0xe6 and d[o - 3] <= 7:
                value = (d[o - 3] << 16) | (d[o - 2] << 8) | d[o - 1]
            yield value, o, o - pb_file, s["name"], w == 0x1061


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("image", type=Path)
    ap.add_argument("--value", type=int,
                    help="report only this error number")
    ap.add_argument("--raises", action="store_true",
                    help="list the non-constant (re-raise) sites too")
    ap.add_argument("--delta", type=lambda v: int(v, 0), default=None,
                    help="runtime = file + delta; adds runtime addresses "
                         "(0x14c98 for the current HP-UX 5.30 load)")
    args = ap.parse_args()

    d = load_boot_image(args.image)
    table: dict[int, list] = {}
    computed = []
    for value, off, rel, name, b3 in scan(d):
        if value is None:
            computed.append((off, rel, name, b3))
        else:
            table.setdefault(value, []).append((off, rel, name, b3))

    def site(off, rel, name, b3):
        run = f"  run {off + args.delta:#x}" if args.delta is not None else ""
        return (f"    {name:<16} +{rel:#07x}  file {off:#08x}{run}"
                f"{'  (,B)' if b3 else ''}")

    for value in sorted(table):
        if args.value is not None and value != args.value:
            continue
        sites = table[value]
        print(f"{value:5d} (0x{value & 0xffffffff:02x})  "
              f"{len(sites)} site{'s' if len(sites) != 1 else ''}")
        for entry in sites:
            print(site(*entry))
    if args.raises and args.value is None:
        print(f"\nnon-constant operand ({len(computed)} sites -- "
              "TRY re-raises and computed errors):")
        for entry in computed:
            print(site(*entry))
    return 0


if __name__ == "__main__":
    sys.exit(main())
