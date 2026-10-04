#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Joel Heikkila
"""Read the Segment Transfer Tables out of a Series 500 boot image.

A FOCUS code segment ends with its STT, and every transfer between segments
goes through one: `PCL`/`PCLS` and `LPP`/`LPPS` name an entry by number, and
an entry is either a local program pointer into this segment or an external
one naming another segment and an entry in *its* table (ERS section 3.1.2).

So the STTs *are* the inter-segment interface. A segment's own table is the
list of entry points it publishes; the external entries in everyone else's
tables are the calls they make into it. For a boot image whose segments are
an operating system and a language system -- SUN OS KERNEL, BASIC, BASIC
COMPILER, IO/MM RTS -- that is the system call interface, and it can be read
off the image without running anything.

    tools/stt.py image.bin                     one line per segment
    tools/stt.py image.bin --segment 2         one segment's table in full
    tools/stt.py image.bin --callers 2         who calls into segment 2
    tools/stt.py image.bin --graph             the whole call graph

Segment numbers are the CST numbers the running machine uses, which for a
boot image are the segments' positions in the chain. The layout of a segment
header is documented in tools/segdump.py: a 28-byte header whose word at +4
is the length, then a 16-byte name, with the code beginning at header+8 as
far as PB is concerned.
"""
import argparse
import struct
import sys
from pathlib import Path


def u32(d, off):
    return struct.unpack_from(">I", d, off)[0]


def ers(v, pos, width):
    """A field in the ERS's bit numbering, where bit 0 is the MSB."""
    return (v >> (32 - pos - width)) & ((1 << width) - 1)


def segments(data):
    """Walk the segment chain. Yields (number, header offset, length, name)."""
    # Segment 0 is preceded by a four-byte marker word (segdump.py); the
    # chain proper starts after it.
    off = 4
    n = 0
    out = []
    while off + 44 < len(data):
        length = u32(data, off + 4)
        if length == 0 or length > len(data):
            break
        name = data[off + 28:off + 44].split(b"\0")[0].decode("latin1").strip()
        # The header's first halfword is the segment's CST number, which is
        # what every external pointer in every table names -- and it is not
        # the position in the chain. Checked against a running machine: the
        # kernel declares 2, IO/MM RTS 3, the compiler 4, the parser 5,
        # BASIC 6, and all of them match the CST the powerup module builds.
        cst = u32(data, off) >> 16
        if n == 0:
            cst = 0                      # segment 0's word is a marker
        out.append((cst, off, length, name))
        # Segment 0 carries its checksum at the length, the rest eight bytes
        # past it, so the first step through the chain is shorter by twelve
        # (tools/segdump.py documents the two shapes).
        nxt = off + length + (0 if n == 0 else 12)
        if nxt <= off or nxt + 44 >= len(data) or u32(data, nxt) == 0xffffffff:
            break
        off = nxt
        n += 1
    return out


class Table:
    """One segment's STT, read from the image."""

    def __init__(self, data, hdr, length, number, name):
        self.number, self.name = number, name
        self.pb = hdr + 8                     # PB, as a file offset
        # PL addresses the segment's last byte and always has its low two
        # bits set, so the word it points into begins three bytes earlier.
        # (The CST carries length-1 for the same reason.)
        self.pl = self.pb + length - 4
        self.ok = 0 <= self.pl < len(data) - 4
        self.entries = []
        if not self.ok:
            return
        lw = u32(data, self.pl)
        self.bytes = ers(lw, 13, 19)
        self.uncallable = ers(lw, 1, 1)
        # Bits 2-12 must be zero (sheet 28); anything else is not a table.
        self.plausible = ers(lw, 2, 11) == 0 and 0 < self.bytes <= length
        if not self.plausible:
            return
        for i in range(1, self.bytes // 4 + 1):
            off = self.pl - 4 * i
            if off < self.pb:
                break
            self.entries.append(u32(data, off))

    def classify(self, v):
        if v == 0:
            return ("empty", None, None)
        # All-ones is the fill for a slot the linker reserved and never
        # used. Read as a pointer it would name segment 4095, which is the
        # largest a 12-bit field can hold and not a segment any of these
        # images defines.
        if v == 0xffffffff:
            return ("unused", None, None)
        if ers(v, 0, 1) == 0:
            return ("local", None, ers(v, 2, 30))
        return ("extern", ers(v, 20, 12), ers(v, 1, 19) // 4)


def describe(t):
    if not t.ok or not getattr(t, "plausible", False):
        return f"seg {t.number:<3} {t.name:<20} (no readable table)"
    kinds = [t.classify(v)[0] for v in t.entries]
    calls = {}
    for v in t.entries:
        k, seg, _ = t.classify(v)
        if k == "extern":
            calls[seg] = calls.get(seg, 0) + 1
    tail = ", ".join(f"{n}->seg{s}" for s, n in sorted(calls.items()))
    return (f"seg {t.number:<3} {t.name:<20} {len(t.entries):>4} entries  "
            f"{kinds.count('local'):>4} local {kinds.count('extern'):>4} extern "
            f"{kinds.count('unused'):>4} unused {kinds.count('empty'):>4} empty  "
            f"{tail}")


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image")
    ap.add_argument("--segment", type=int, help="print this segment's table")
    ap.add_argument("--callers", type=int, help="who calls into this segment")
    ap.add_argument("--graph", action="store_true", help="the whole call graph")
    a = ap.parse_args()

    data = Path(a.image).read_bytes()
    segs = segments(data)
    tables = [Table(data, hdr, length, n, name)
              for n, hdr, length, name in segs]
    tables.sort(key=lambda t: t.number)
    by_num = {t.number: t for t in tables}

    if a.segment is not None:
        t = by_num.get(a.segment)
        if t is None or not getattr(t, "plausible", False):
            print(f"segment {a.segment} has no readable table", file=sys.stderr)
            return 1
        print(f"{t.name} -- segment {t.number}, {len(t.entries)} entries, "
              f"STT length {t.bytes} bytes\n")
        for i, v in enumerate(t.entries, 1):
            kind, seg, val = t.classify(v)
            if kind == "empty":
                print(f"  {i:>4}  {v:08x}  --")
            elif kind == "unused":
                print(f"  {i:>4}  {v:08x}  unused")
            elif kind == "local":
                print(f"  {i:>4}  {v:08x}  local   PB+0x{val:x}")
            else:
                who = by_num.get(seg)
                print(f"  {i:>4}  {v:08x}  extern  seg {seg} entry {val}"
                      f"{'  (' + who.name + ')' if who else ''}")
        return 0

    if a.callers is not None:
        target = a.callers
        who = by_num.get(target)
        print(f"calls into segment {target}"
              f"{' (' + who.name + ')' if who else ''}\n")
        total = 0
        for t in tables:
            if not getattr(t, "plausible", False):
                continue
            hits = [(i, t.classify(v)[2])
                    for i, v in enumerate(t.entries, 1)
                    if t.classify(v)[1] == target]
            if not hits:
                continue
            total += len(hits)
            called = sorted({e for _, e in hits})
            print(f"  seg {t.number:<3} {t.name:<20} {len(hits):>3} entries -> "
                  f"{', '.join(str(e) for e in called)}")
        print(f"\n{total} references")
        return 0

    if a.graph:
        print("segment                    calls")
        for t in tables:
            if not getattr(t, "plausible", False):
                continue
            calls = {}
            for v in t.entries:
                k, seg, _ = t.classify(v)
                if k == "extern":
                    calls.setdefault(seg, set()).add(t.classify(v)[2])
            if not calls:
                continue
            named = ", ".join(
                f"seg{s}({len(e)})" for s, e in sorted(calls.items()))
            print(f"  {t.number:<3} {t.name:<22} {named}")
        return 0

    for t in tables:
        print(describe(t))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
