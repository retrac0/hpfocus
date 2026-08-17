#!/usr/bin/env python3
"""Extract the FOCUS opcode map from the Instruction Set ERS text layer.

Section 6.2 of the ERS describes each instruction with a line of the form

    12.  ADD (Add)   Format 3 Opcode 3=10

The mnemonic and the "Format N Opcode M=v" clause are frequently split
across OCR line breaks, so we flatten the section to a single stream and
match across whitespace.  Output is CSV: mnemonic,format,opcode_field,octal.
"""
import re
import subprocess
import sys
from pathlib import Path

PDF = Path(__file__).resolve().parents[1] / "docs/bitsavers/FOCUS_Instruction_Set_ERS_Dec82.pdf"
FIRST, LAST = 54, 130  # section 6.2, detailed instruction descriptions

# Boilerplate from the scanned HP drawing frame that wraps every page.
FRAME = re.compile(
    r"HEW ?LET ?T|PAC ?K ?A ?R ?D|REVISIONS|SUPERSEDES|SHEET|DWG|Fiasconaro"
    r"|Focus Machine Instruction Set ERS|MISERS|/ ?hp ?/|P\.C\. #|STK"
)

# "Format 6 Opcode 2=14", "Format 4 Opcode 3 = 2", "Format 3 Opcode 3=44"
CLAUSE = re.compile(
    r"Format\s*(\d)\s*[,;]?\s*Opcode\s*(\d)\s*=?\s*=\s*([0-7]{1,3})"
    r"|Format\s*(\d)\s*[,;]?\s*Opcode\s*(\d)\s*=\s*([0-7]{1,3})",
    re.IGNORECASE,
)

# A mnemonic is 2-6 uppercase chars, optionally with a trailing digit.
MNEMONIC = re.compile(r"\b([A-Z][A-Z0-9]{1,5})\b\s*(?:\([^)]*\)|\[[^\]]*\])?\s*$")


def page_text() -> str:
    out = subprocess.run(
        ["pdftotext", "-layout", "-f", str(FIRST), "-l", str(LAST), str(PDF), "-"],
        capture_output=True, text=True, check=True,
    ).stdout
    keep = [ln for ln in out.splitlines() if not FRAME.search(ln)]
    return "\n".join(keep)


def main() -> int:
    text = page_text()
    flat = re.sub(r"[ \t]+", " ", text)
    rows, seen = [], set()

    for m in CLAUSE.finditer(flat):
        g = [x for x in m.groups() if x is not None]
        fmt, field, octal = g[0], g[1], g[2]
        # Look back over the preceding text (may span a line break) for the
        # mnemonic that heads this instruction's description.
        before = flat[max(0, m.start() - 120):m.start()]
        before = before.replace("\n", " ").rstrip()
        mn = MNEMONIC.search(before)
        if not mn:
            # Fall back: first all-caps token after the item number.
            toks = re.findall(r"\b[A-Z][A-Z0-9]{1,5}\b", before)
            if not toks:
                continue
            name = toks[-1]
        else:
            name = mn.group(1)
        key = (name, fmt, field, octal)
        if key in seen:
            continue
        seen.add(key)
        rows.append(key)

    rows.sort(key=lambda r: (int(r[1]), int(r[3], 8), r[0]))
    print("mnemonic,format,opcode_field,octal,decimal")
    for name, fmt, field, octal in rows:
        print(f"{name},{fmt},OP{field},{octal},{int(octal, 8)}")
    print(f"# {len(rows)} entries", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
