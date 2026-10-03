#!/usr/bin/env python3
"""Report how much of the real FOCUS code the execution core can run.

Parses the case labels out of src/cpu/focus/focusexec.h to learn which
(format, opcode) pairs are implemented, then disassembles the HP-UX kernel and
the BASIC boot disc and weights coverage by how often each opcode actually
occurs. Static frequency, not a dynamic trace, but it is the right guide for
deciding what to implement next.
"""
import collections
import importlib.util
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

# The core is split by subject across focusexec.h and the fragments it
# includes, so the dispatchers no longer live in one file. Scanning all of
# them keeps this honest: a dispatcher that moved would otherwise report as
# implementing nothing, and the coverage figure would quietly collapse.
CORE = sorted((ROOT / "src/cpu/focus").glob("focusexec*"))

spec = importlib.util.spec_from_file_location("fd", ROOT / "tools/focusdis.py")
fd = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fd)

# Each dispatch function handles one format; find its case labels.
FUNCS = {
    "exec_fmt1": 1,
    "exec_fmt2": 2,
    "exec_fmt3": 3,
    "exec_fmt4": 4,
    "exec_fmt5": 5,
    "exec_fmt6": 6,
}


def implemented() -> set:
    src = "\n".join(f.read_text() for f in CORE)
    out = set()
    found = set()
    for name, fmt in FUNCS.items():
        m = re.search(rf"exec_trap {name}\(", src)
        if not m:
            continue
        found.add(name)
        # Scan forward to the next function definition at the same level.
        rest = src[m.end():]
        nxt = re.search(r"\n\t(?:exec_trap|void|bool|u32) \w+\(", rest)
        body = rest[:nxt.start()] if nxt else rest
        for c in re.finditer(r"case (\d+):", body):
            lit = c.group(1)
            # Format 1 opcodes are written in decimal, the rest in octal.
            out.add((fmt, int(lit, 8) if lit.startswith("0") else int(lit)))

    missing = set(FUNCS) - found
    if missing:
        raise SystemExit(
            "isa_coverage: no definition found for "
            + ", ".join(sorted(missing))
            + f" in {', '.join(f.name for f in CORE)}.\n"
            "The dispatchers have moved; teach CORE where they went rather "
            "than reading a coverage figure that is silently too low.")
    return out


def main() -> int:
    impl = implemented()
    corpora = sys.argv[1:] or []
    if not corpora:
        print("usage: isa_coverage.py <image>...", file=sys.stderr)
        return 2

    hist = collections.Counter()
    for path in corpora:
        data = Path(path).read_bytes()
        p = 0
        while p < len(data):
            r = fd.decode(data, p)
            if r is None:
                break
            key = classify(data, p)
            if key:
                hist[key] += 1
            p += r[0]

    total = sum(hist.values())
    covered = sum(n for k, n in hist.items() if k in impl)
    print(f"{total:,} instructions decoded from {len(corpora)} image(s)")
    print(f"{covered:,} would execute  ({100 * covered / total:.1f}%)")
    print(f"{total - covered:,} would trap as unimplemented\n")

    missing = [(k, n) for k, n in hist.most_common() if k not in impl]
    if not missing:
        print("no gaps")
        return 0

    print(f"{'fmt':>3} {'op':>5}  {'count':>9}  {'cum%':>6}  mnemonic")
    print("-" * 44)
    cum = covered
    for (fmt, op), n in missing[:25]:
        cum += n
        ent = fd.OPS.get((fmt, op))
        print(f"{fmt:>3} {op:>5o}  {n:>9,}  {100 * cum / total:>5.1f}%  "
              f"{ent[0] if ent else '?'}")
    return 0


def classify(data: bytes, p: int):
    """(format, opcode) of the instruction at p, matching the core's dispatch."""
    import struct
    if p + 2 > len(data):
        return None
    w = struct.unpack(">H", data[p:p + 2])[0]
    op1 = (w >> 13) & 7
    if op1 == 7:
        return (6, (struct.unpack(">I", data[p:p + 4])[0] >> 24) & 0x1f) \
            if p + 4 <= len(data) else None
    if op1 == 1:
        return (5, (w >> 8) & 0x1f)
    if op1 == 0:
        op2 = (w >> 6) & 0x3f
        if op2 == 0:
            return (3, w & 0x3f)
        if op2 == 1:
            return (4, w & 0x3f)
        return (2, op2)
    return (1, op1)


if __name__ == "__main__":
    raise SystemExit(main())
