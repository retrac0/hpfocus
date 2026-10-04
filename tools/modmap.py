#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Joel Heikkila
"""Map the named modules in a Series 500 boot image.

Both the HP-UX kernel (`os.req`) and the BASIC system boot discs are built from
modules introduced by a 16-byte space-padded ASCII name field. Listing them
gives a table of contents for the image and, critically, locates the
`SYSTEM POWERUP` module -- the entry point the SCM loader ROM jumps to.

Usage: modmap.py <image> [--min-gap N]
"""
import argparse
import re
from pathlib import Path

# A module name: starts with a letter, then letters/digits/space/punctuation,
# exactly 16 bytes, and must contain at least one letter run of 3+.
NAME = re.compile(rb"[A-Z][A-Z0-9 _/&.\-]{15}")
SUBSTANTIVE = re.compile(rb"[A-Z]{3}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("image", type=Path)
    ap.add_argument("--min-gap", type=int, default=64,
                    help="suppress hits closer than this to the previous one "
                         "(filters text that merely looks like a name)")
    args = ap.parse_args()

    data = args.image.read_bytes()
    hits, last = [], -10**9
    for m in NAME.finditer(data):
        raw = m.group(0)
        if not SUBSTANTIVE.search(raw):
            continue
        name = raw.decode().rstrip()
        # Reject fragments of running prose: a real module name is followed by
        # a non-text byte or another aligned field, not lowercase.
        if m.start() - last < args.min_gap:
            continue
        tail = data[m.end():m.end() + 4]
        if any(97 <= b <= 122 for b in tail):
            continue
        hits.append((m.start(), name))
        last = m.start()

    print(f"{args.image.name}: {len(data):,} bytes, {len(hits)} modules\n")
    print(f"{'offset':>10}  {'gap':>9}  name")
    print("-" * 46)
    for i, (off, name) in enumerate(hits):
        gap = hits[i + 1][0] - off if i + 1 < len(hits) else len(data) - off
        print(f"0x{off:08x}  {gap:>9,}  {name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
