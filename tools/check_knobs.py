#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Joel Heikkila
"""Check that the knob catalogue and the source agree.

src/cpu/focus/focusknobs.h carries the one index of the emulator's FOCUS_*
environment knobs, and FOCUS_HELP prints it. An index is only worth having
if it is complete, so this reports both directions:

  * a knob the source names that the catalogue does not list -- unfindable
    by anyone who did not write it;
  * a knob the catalogue lists that no source names -- a knob that has been
    removed, or a typo in one of the two.

A knob is named by a "FOCUS_..." string literal, wherever it appears and
whichever reader consumes it, so this does not have to know the shape of
every call site and cannot drift as new ones are added.
"""
from __future__ import annotations  # PEP 604/585 hints on Python 3.8
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CATALOGUE = ROOT / "src/cpu/focus/focusknobs.h"
SOURCES = ["src/cpu/focus", "src/iop", "src/mame", "src/test"]

LITERAL = re.compile(r'"(FOCUS_[A-Z0-9_]+)"')

# FOCUS_HELP prints the catalogue; it is not an entry in it.
EXEMPT = {"FOCUS_HELP"}


def split_catalogue(text: str):
    """The catalogue block, and everything else in the file."""
    head, rest = text.split("inline constexpr spec CATALOGUE[]", 1)
    block, tail = rest.split("\n};", 1)
    return block, head + tail


def main() -> int:
    block, elsewhere = split_catalogue(CATALOGUE.read_text())
    cat = set(LITERAL.findall(block))

    use: dict[str, set[str]] = {}
    for name in LITERAL.findall(elsewhere):
        use.setdefault(name, set()).add(CATALOGUE.relative_to(ROOT).as_posix())
    for d in SOURCES:
        for f in sorted((ROOT / d).rglob("*")):
            if f.suffix not in (".h", ".cpp", ".ipp") or f == CATALOGUE:
                continue
            for name in LITERAL.findall(f.read_text()):
                use.setdefault(name, set()).add(f.relative_to(ROOT).as_posix())

    for n in EXEMPT:
        use.pop(n, None)
        cat.discard(n)

    missing = sorted(set(use) - cat)
    stale = sorted(cat - set(use))

    for n in missing:
        print(f"NOT CATALOGUED  {n:<22} named in {', '.join(sorted(use[n]))}")
    for n in stale:
        print(f"NOT USED        {n:<22} catalogued but no source names it")

    print(f"\n{len(cat)} catalogued, {len(use)} used, "
          f"{len(missing)} missing, {len(stale)} stale")
    return 1 if (missing or stale) else 0


if __name__ == "__main__":
    sys.exit(main())
