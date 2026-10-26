#!/usr/bin/env python3
"""Generate the C++ opcode tables for the MAME FOCUS disassembler.

docs/opcodes.csv is the single source of truth for the opcode map. Keeping a
second, hand-maintained copy in C++ would guarantee the two drift apart, so the
C++ table is generated from the CSV instead.

Rewrites the tables in place in the MAME tree's focusdasm.cpp, between the
BEGIN/END GENERATED TABLES markers.
"""
import csv
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "docs/opcodes.csv"
DST = ROOT / "mame/src/devices/cpu/focus/focusdasm.cpp"
BEGIN = "// BEGIN GENERATED TABLES"
END = "// END GENERATED TABLES"

# Operand-kind strings in the CSV -> the C++ enumerator that renders them.
# ORDER MATTERS: these are substring tests, and several keys are suffixes of
# others -- "halfword offset" contains "word offset", "immediate operand 2"
# contains "immediate operand". Most specific first.
KINDS = [
    ("halfword offset",             "OK_BRANCH_HW"),
    ("word offset",                 "OK_MEM_WORD"),
    ("byte offset",                 "OK_MEM_BYTE"),
    ("four times STT entry number", "OK_STT4"),
    ("STT entry number",            "OK_STT"),
    ("immediate operand 2",         "OK_BITFIELD"),
    ("32-#bits, start bit",         "OK_BITFIELD"),
    ("immediate operand",           "OK_IMM"),
    ("shift count",                 "OK_SHIFT"),
    ("I/O opcode",                  "OK_IO"),
    ("N field",                     "OK_NFIELD"),
    ("level + byte offset",         "OK_LEVEL"),
    ("parameter",                   "OK_PARAM"),
    ("",                            "OK_NONE"),
]


def kind_of(operand: str) -> str:
    for text, enum in KINDS:
        if text and text in operand:
            return enum
    return "OK_NONE"


def main() -> int:
    rows = [l for l in SRC.read_text().splitlines() if not l.startswith("#")]
    entries = list(csv.DictReader(rows))

    # (format, octal) must be unique; the CSV is keyed that way.
    by_fmt: dict[int, dict[int, tuple[str, str, str]]] = {}
    for r in entries:
        f = int(r["format"])
        v = int(r["octal"], 8)
        by_fmt.setdefault(f, {})[v] = (r["mnemonic"], kind_of(r["operand"]),
                                       r["operand"])

    sizes = {1: 8, 2: 64, 3: 64, 4: 64, 5: 32, 6: 32}
    out = []
    out.append(BEGIN)
    out.append("// The opcode map, from Appendix B of the Focus Machine Instruction")
    out.append("// Set ERS, A-1FE1-3020-B, 12/06/82, sheet 137.")
    out.append("")

    total = 0
    for fmt in sorted(sizes):
        n = sizes[fmt]
        tbl = by_fmt.get(fmt, {})
        out.append(f"const focus_disassembler::op "
                   f"focus_disassembler::s_fmt{fmt}[{n}] = {{")
        for v in range(n):
            e = tbl.get(v)
            if e:
                mn, kind, raw = e
                total += 1
                comment = f"  // {v:03o}" + (f"  {raw}" if raw else "")
                out.append(f'\t{{ "{mn}", {kind} }},{comment}')
            else:
                out.append(f'\t{{ nullptr, OK_NONE }},  // {v:03o}')
        out.append("};")
        out.append("")

    out.append(END)
    text = DST.read_text()
    head, rest = text.split(BEGIN, 1)
    _, tail = rest.split(END, 1)
    DST.write_text(head + "\n".join(out) + tail)
    print(f"wrote {DST.relative_to(ROOT)}  ({total} opcodes across "
          f"{len(sizes)} formats)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
