#!/usr/bin/env python3
"""Read the FOCUS instruction table out of the HP-UX Series 500 assembler.

`/bin/as` (HP-UX 5.30, recovered from the 7914 unit-save tapes with
tools/sdf7914.py) carries its instruction set as an array of 28-byte
records in the data segment, each pointing into a string pool of
mnemonics:

    +0   word      data-space address of the mnemonic (nul terminated)
    +4   byte      handler class (an ascii character; selects the
                   assembler's per-format encoding routine)
    +5   3 bytes   encoding; bit 12 is a flag (set only on lbit/sbit),
                   the low byte holds the opcode bits
    +8   bytes     operand-syntax string, nul terminated, zero padded
                   to the 28-byte record

The handler class fixes the instruction format, so (format, opcode) is
recoverable from (class, encoding):

    R Z S          format 1, op = enc          (lras/lds/ldds/sts/bccs)
    b U W          format 5, op = enc - 0x20
    [ \\ ^ _        format 2, op = enc          (branches store the
                   forward form; +4 in the chart is the backward bit)
    ]              format 4, op = enc & 0x3f, when enc bit 6 is set;
                   format 3, op = enc, otherwise
    a ` T V X Y d  format 6, op = enc & 0x1f   (enc is 0xe0 | op)
    c              format 1 BCCS with an implied condition (beqs...)
    d              format 6 BCC with an implied condition (beq...)
    Q              assembler pseudo-op, not an instruction

This is the second HP-authored copy of the instruction set (the first is
the Pascal compiler's, tools/hptable.py), and the two disagree in
exactly one place: the assembler spells format 5 opcode 31 `sloc` where
the compiler and the ERS prose (SOLC, "Start Of Line Check") spell it
SOLC.  Beyond that the assembler names four opcodes that the ERS chart,
the compiler and the whole documentation record leave unassigned:

    up     format 4 opcode 020    (operand class 'a', like pshn/srm)
    down   format 4 opcode 021    (operand class 'a')
    ctcb   format 4 opcode 024    (no operand)
    gmpy   format 6 opcode 021    (operand syntax 'ds')

020/021 are the kernel-only instructions both HP-UX kernels execute:
UP/DOWN are the classic semaphore signal/wait pair, and CTCB matches the
ERS's "Current TCB Pointer" (CTCBP) machinery.

The table is found by scanning for the record stride and then fitting
the single displacement that maps every mnemonic pointer onto a string
start, so no file offsets are hardcoded.

    python3 tools/astable.py <as-binary> [--csv docs/opcodes.csv]
"""
import argparse
import csv
import struct
import sys
from collections import Counter

STRIDE = 28
PSEUDO = "Q"
SYNTH = {"beq", "bge", "bgt", "ble", "blt", "bne",
         "beqs", "bges", "bgts", "bles", "blts", "bnes", "brs"}


def scan(data):
    """All plausible records: (offset, ptr, class char, enc, operands)."""
    recs = []
    for a in range(0, len(data) - STRIDE, 4):
        ptr, = struct.unpack(">I", data[a:a + 4])
        if not 0x1000 <= ptr < len(data):
            continue
        cls = data[a + 4]
        if not (0x14 <= cls <= 0x7f) or data[a + 5] & 0xef:
            continue
        enc, = struct.unpack(">H", data[a + 6:a + 8])
        s = data[a + 8:a + 8 + 20]
        ops = s[:s.index(0)] if 0 in s else None
        if ops is None or not all(0x61 <= c <= 0x7a for c in ops):
            continue
        if any(s[len(ops):]):
            continue
        recs.append((a, ptr, chr(cls), enc, ops.decode()))
    return recs


def table(data):
    """The instruction records, with mnemonics resolved."""
    recs = scan(data)
    runs, cur = [], [recs[0]]
    for r in recs[1:]:
        if r[0] - cur[-1][0] == STRIDE:
            cur.append(r)
        else:
            runs.append(cur)
            cur = [r]
    runs.append(cur)
    run = max(runs, key=len)

    # One displacement maps the pointers onto nul-terminated pool strings.
    starts = {a for a in range(4, len(data) - 4, 4)
              if chr(data[a]).isalpha() and data[a - 1] == 0}
    deltas = Counter()
    for _a, ptr, *_ in run:
        for st in starts:
            d = st - ptr
            if 0 < d < len(data):
                deltas[d] += 1
    delta, hits = deltas.most_common(1)[0]
    if hits < len(run) * 0.9:
        raise SystemExit(f"pointer fit failed ({hits}/{len(run)})")

    # The run breaks on records the scan filter rejects (register names,
    # punctuation operands); the delta re-admits them from either end.
    def rec(a):
        ptr, = struct.unpack(">I", data[a:a + 4])
        if not 0x1000 <= ptr < len(data) or ptr + delta not in starts:
            return None
        enc, = struct.unpack(">H", data[a + 6:a + 8])
        s = data[a + 8:a + 8 + 20]
        ops = s[:s.index(0)] if 0 in s else b""
        return a, ptr, chr(data[a + 4]), enc, ops.decode("latin1")

    lo, hi = run[0][0], run[-1][0]
    while lo >= STRIDE and rec(lo - STRIDE):
        lo -= STRIDE
    while hi + 2 * STRIDE <= len(data) and rec(hi + STRIDE):
        hi += STRIDE
    out = []
    for a in range(lo, hi + 1, STRIDE):
        r = rec(a)
        if not r:
            continue
        _a, ptr, cls, enc, ops = r
        e = data.index(b"\0", ptr + delta)
        out.append((data[ptr + delta:e].decode("latin1"), cls, enc, ops))
    return out, delta


def formats(cls, enc):
    """The (format, opcode) a record encodes, or None for pseudo-ops."""
    enc &= 0xfff                       # bit 12 flags lbit/sbit
    if cls in "RZSc":
        return 1, enc
    if cls in "bUW":
        return 5, enc - 0x20
    if cls in "[\\^_":
        return 2, enc
    if cls == "]":
        return (4, enc & 0x3f) if enc & 0x40 else (3, enc)
    if cls in "a`TVXYd" and enc >= 0xe0:
        return 6, enc & 0x1f
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("assembler", help="/bin/as from the recovered filesystem")
    ap.add_argument("--csv", help="compare against this opcode map")
    args = ap.parse_args()

    data = open(args.assembler, "rb").read()
    recs, delta = table(data)
    print(f"{len(recs)} records, mnemonic pool displacement {delta:#x}")

    if not args.csv:
        for mn, cls, enc, ops in recs:
            fo = formats(cls, enc) if cls != PSEUDO and mn.isalpha() else None
            where = f"F{fo[0]}:{fo[1]:03o}" if fo else "pseudo/reg"
            print(f"  {mn:<12} {where:<10} class={cls!r} enc={enc:#06x} "
                  f"ops='{ops}'")
        return 0

    ours = {}
    for r in csv.reader(l for l in open(args.csv) if not l.startswith("#")):
        if r[0] == "format" or r[3].startswith("ESCAPE"):
            continue
        ours.setdefault(r[3].strip().lower(), []).append(
            (int(r[0]), int(r[2], 8)))

    agree = 0
    seen = set()
    for mn, cls, enc, _ops in recs:
        if cls == PSEUDO or not mn.isalpha() or mn in SYNTH:
            continue
        fo = formats(cls, enc)
        seen.add(mn)
        if mn not in ours:
            print(f"  assembler only: {mn} F{fo[0]}:{fo[1]:03o}")
        elif fo in ours[mn] or (fo[0] == 2 and (2, fo[1] | 4) in ours[mn]):
            agree += 1
        else:
            print(f"  MISMATCH {mn}: asm F{fo[0]}:{fo[1]:03o} vs csv "
                  + " ".join(f"F{f}:{o:03o}" for f, o in ours[mn]))
    print(f"{agree} assignments agree with the assembler")
    only = sorted(set(ours) - seen)
    if only:
        print("csv only: " + " ".join(only))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
