#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Joel Heikkila
"""A cursory structural pass over a focusdis.py listing.

Adds two things a plain listing doesn't have: a label at every address a
statically-computed branch (BCCS/BCC/the format-2 halfword branches) lands
on, so the listing shows where control flow actually joins and splits, and
a header block summarising mnemonic frequency, the busiest STT call targets
(PCL/PCLS/LPP/LPPS -- FOCUS's procedure-call instructions, ERS sheet 34-ish;
see docs/opcodes.csv), and the EXIT depths seen. It does not attempt basic
-block boundaries, data/code separation, or anything using the segment
header -- see disasm/README.md for what "cursory" means here and what would
need real analysis to firm up.

Output is byte-for-byte the same instruction lines focusdis.py prints
(`{addr:08x}  {raw:<8}  {text}`), with `; ...` comment lines interleaved, so
it diffs cleanly against a plain `focusdis.py --count 0` run of the same
file.

Usage: structdis.py <bin> > out.dis
"""
import argparse
import re
import sys
from collections import Counter
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import focusdis  # noqa: E402

STT_CALL = re.compile(r"^(PCLS?|LPPS?)\s+STT\[(\d+)\]")
EXIT_RE = re.compile(r"^EXIT\s+N=(\d+)")


def analyze(data: bytes):
    instrs = []
    incoming = Counter()
    mnemonics = Counter()
    stt_calls = Counter()
    exits = Counter()
    p = 0
    while p < len(data):
        r = focusdis.decode(data, p)
        if r is None:
            break
        ln, text, target = r
        instrs.append((p, ln, text))
        mnemonics[text.split(None, 1)[0]] += 1
        if target is not None and 0 <= target < len(data):
            incoming[target] += 1
        m = STT_CALL.match(text)
        if m:
            stt_calls[int(m.group(2))] += 1
        m = EXIT_RE.match(text)
        if m:
            exits[int(m.group(1))] += 1
        p += ln
    return instrs, incoming, mnemonics, stt_calls, exits


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("image", type=Path)
    args = ap.parse_args()

    data = args.image.read_bytes()
    instrs, incoming, mnemonics, stt_calls, exits = analyze(data)

    total = len(instrs)
    known = sum(1 for _, _, t in instrs if not t.startswith("."))
    boundaries = {addr for addr, _, _ in instrs}
    labels = sorted(a for a in incoming if a in boundaries)

    print(f"; {args.image.stem} -- {len(data):,} bytes, {total:,} instructions, "
          f"{100 * known / max(total, 1):.1f}% recognised")
    print(f"; structural pass (tools/structdis.py): {len(labels):,} distinct "
          f"in-range branch targets, labelled L_<addr> below")
    top_mn = ", ".join(f"{m}×{n}" for m, n in mnemonics.most_common(8))
    print(f"; top mnemonics: {top_mn}")
    if stt_calls:
        top_calls = ", ".join(f"STT[{s}]×{n}"
                               for s, n in stt_calls.most_common(10))
        print(f"; busiest STT calls (PCL/PCLS/LPP/LPPS): {top_calls}")
    if exits:
        top_exits = ", ".join(f"N={n}×{c}" for n, c in exits.most_common(6))
        print(f"; EXIT depths seen: {top_exits}")
    print(";")

    for addr, ln, text in instrs:
        if addr in incoming and addr in boundaries:
            n = incoming[addr]
            print(f"; ---- L_{addr:08x}  ({n} ref{'s' if n != 1 else ''}) ----")
        raw = data[addr:addr + ln].hex()
        print(f"{addr:08x}  {raw:<8}  {text}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
