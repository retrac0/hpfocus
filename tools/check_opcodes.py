#!/usr/bin/env python3
"""Cross-check docs/opcodes.csv against the ERS section 6.2 prose.

opcodes.csv is a hand transcription of Appendix B, which is authoritative.
This script re-derives the opcode assignments independently from the OCR of
the per-instruction descriptions and reports every disagreement, so that a
slip in the hand transcription would show up as a conflict that is *not*
explainable as OCR damage.

Last run: 25 conflicts, all attributable to OCR damage in the prose
(S8IT/SBIT, IXTT/IXIT, LOB/LDB, TOR/IOR, DR/BR, ...) plus one spurious
attribution (FCPS to format 1). No conflict indicated an error in the chart
transcription.
"""
import csv
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def load_chart() -> dict:
    rows = [l for l in (ROOT / "docs/opcodes.csv").read_text().splitlines()
            if not l.startswith("#")]
    return {(r["format"], r["octal"]): r["mnemonic"] for r in csv.DictReader(rows)}


def load_prose() -> list:
    out = subprocess.run(
        [sys.executable, str(ROOT / "tools/extract_opcodes.py")],
        capture_output=True, text=True, check=True,
    ).stdout
    return [r for r in csv.DictReader(out.splitlines())
            if not r["mnemonic"].startswith("#")]


def main() -> int:
    chart, prose = load_chart(), load_prose()
    conflicts, unknown = [], []

    for r in prose:
        key = (r["format"], r["octal"])
        if key not in chart:
            unknown.append((key, r["mnemonic"]))
        elif chart[key] != r["mnemonic"]:
            conflicts.append((key, chart[key], r["mnemonic"]))

    print(f"chart entries : {len(chart)}")
    print(f"prose entries : {len(prose)}")
    print(f"\nconflicts ({len(conflicts)}) — chart is authoritative:")
    for (fmt, oc), a, b in conflicts:
        print(f"  fmt{fmt} op{oc:>3}  chart={a:<6} prose={b}")
    print(f"\nin prose but not in chart ({len(unknown)}):")
    for (fmt, oc), m in unknown:
        print(f"  fmt{fmt} op{oc:>3}  {m}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
