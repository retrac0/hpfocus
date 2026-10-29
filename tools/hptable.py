#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Joel Heikkila
"""Read HP's own FOCUS instruction table out of the Series 500 toolchain.

`/usr/lib/pascomp`, the HP-UX 5.30 Pascal compiler recovered from the 7914
unit-save tapes, carries the instruction set as an array of 16-byte
records:

    +0   4 bytes   mnemonic, space padded
    +4   halfword  the opcode bits, positioned as they sit in the
                   instruction word (see below)
    +6   halfword  operand syntax group
    +8   halfword  operand/format class
    +10  halfword  zero
    +12  word      attribute flags

The opcode field holds the byte that carries the opcode for that format,
which makes the format recoverable from the value alone:

    format 3   op3               (0x00-0x3f, no operand)
    format 2   op2               (0x00-0x3f, 6-bit operand)
    format 4   0x40 | op3
    format 5   0x20 | op2
    format 6   0xe0 | op2

Formats 2 and 3 share a range and are told apart by the operand class.

This is HP's table, not a transcription of the printed chart, so it is the
best available check on docs/opcodes.csv -- it confirmed 196 assignments,
corrected `SRSI` to `BRSI` and `RPC` to `RFC`, and showed that HP's own
1987 tools leave format 4 opcodes 020-024 unassigned, exactly as the
Dec-82 ERS does, even though the same table lists every documented
privileged instruction.

    python3 tools/hptable.py <pascomp> [--csv docs/opcodes.csv]
"""
import argparse
import csv
import re
import struct
import sys

RECORD = 16
NAME = re.compile(rb"[a-z][a-z0-9 ]{3}")


def records(data, lo=0x400, hi=0x2000):
    """Yield (address, mnemonic, opcode byte, group, class, flags)."""
    for a in range(lo, min(hi, len(data) - RECORD), RECORD):
        if not NAME.fullmatch(data[a:a + 4]):
            continue
        enc, group, cls, _z, flags = struct.unpack(">HHHHI", data[a + 4:a + 16])
        yield a, data[a:a + 4].decode().strip(), enc, group, cls, flags


def formats(enc, cls):
    """The (format, opcode) an encoding byte stands for, or None."""
    if enc & 0xe0 == 0xe0:
        return 6, enc & 0x1f
    if enc & 0xc0 == 0x40:
        return 4, enc & 0x3f
    if enc & 0xe0 == 0x20 and cls in (8, 9, 10):
        return 5, enc & 0x1f
    return None, enc


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pascomp")
    ap.add_argument("--csv", help="compare against this opcode map")
    args = ap.parse_args()

    data = open(args.pascomp, "rb").read()
    table = list(records(data))
    if not table:
        print("no instruction table found", file=sys.stderr)
        return 1
    print(f"{len(table)} records, {table[0][0]:#x}..{table[-1][0]:#x}")

    if not args.csv:
        for _a, mn, enc, group, cls, flags in table:
            fmt, op = formats(enc, cls)
            where = f"F{fmt}:{op:03o}" if fmt else f"enc {enc:#04x}"
            print(f"  {mn:<6} {where:<10} group={group} class={cls} "
                  f"flags={flags:#010x}")
        return 0

    hp = {}
    for _a, mn, enc, _g, _c, _f in table:
        hp.setdefault(mn, enc)
    rows = [r for r in csv.reader(
        l for l in open(args.csv) if not l.startswith("#"))]
    head = rows[0]
    fi, oi, mi = (head.index("format"), head.index("octal"),
                  head.index("mnemonic"))
    expect = {2: lambda op: op, 3: lambda op: op, 4: lambda op: 0x40 | op,
              5: lambda op: 0x20 | op, 6: lambda op: 0xe0 | op}

    agree = 0
    for r in rows[1:]:
        if len(r) <= mi or not r[mi].strip():
            continue
        mn = r[mi].strip().lower()
        if mn.startswith("escape") or mn not in hp:
            continue
        try:
            fmt, op = int(r[fi]), int(r[oi], 8)
        except ValueError:
            continue
        if fmt not in expect:
            continue
        want = expect[fmt](op)
        if want == hp[mn]:
            agree += 1
        elif fmt == 2 and want - hp[mn] == 4:
            # Short branches: the chart lists the sense variant, HP's table
            # the base form. Bit 3 of the operand field carries the sign.
            agree += 1
        else:
            print(f"  MISMATCH {mn}: ours F{fmt}:{op:03o} (enc {want:#04x}) "
                  f"vs HP {hp[mn]:#04x}")
    print(f"{agree} assignments agree with HP's table")

    holes = [op for op in range(64)
             if op not in {e & 0x3f for e in hp.values() if e & 0xc0 == 0x40}]
    print("format 4 slots HP leaves unassigned: "
          + " ".join(f"{op:03o}" for op in holes))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
