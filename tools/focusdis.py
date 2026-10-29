#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Joel Heikkila
"""A disassembler for the HP FOCUS machine instruction set.

Table-driven from docs/opcodes.csv, which is the hand transcription of
Appendix B of the Focus Machine Instruction Set ERS (A-1FE1-3020-B, 12/06/82).

Bit numbering follows the ERS throughout: **bit 0 is the most significant**,
and a field written (N:M) is M bits starting at bit N. Instruction layouts
(ERS section 6.1, field widths confirmed against Appendix B):

  Fmt 1 (16b, OP1=2..6)   OP1(0:3) I(3) X(4) BASE(5:3) OFFSET(8:8)
  Fmt 2 (16b, OP1=0)      000 b(3) OP2(4:6) OPERAND(10:6)
  Fmt 3 (OP1=0, OP2=0)    000 b(3) 000000   OP3(10:6)
  Fmt 4 (OP1=0, OP2=1)    000 b(3) 000001   OP3(10:6)
  Fmt 5 (16b, OP1=1)      001 OP2(3:5) OPERAND(8:8)      -- no I or X bit
  Fmt 6 (32b, OP1=7)      111 OP2(3:5) I(8) X(9) BASE(10:3) OFFSET(13:19)

Usage:
  focusdis.py <image> [--start N] [--count N] [--end N] [--stats]
"""
from __future__ import annotations  # PEP 604/585 hints on Python 3.8
import argparse
import csv
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TABLE = ROOT / "docs/opcodes.csv"

# BASE field: the offset is always positive; the base selects direction.
BASES = ["P+", "P-", "DB+", "DL-", "Q+", "Q-", "SB+", "S-"]


def load_table() -> dict:
    """(format, opcode_value) -> (mnemonic, operand_kind, flags)"""
    rows = [l for l in TABLE.read_text().splitlines() if not l.startswith("#")]
    out = {}
    for r in csv.DictReader(rows):
        out[(int(r["format"]), int(r["octal"], 8))] = (
            r["mnemonic"], r["operand"], r["flags"])
    return out


OPS = load_table()


def _flags(i: int, x: int) -> str:
    s = ""
    if i:
        s += ",I"
    if x:
        s += ",X"
    return s


def _mem(base: int, off: int, i: int, x: int) -> str:
    return f"{BASES[base]}{off}{_flags(i, x)}"


def _ccf(bits: int) -> str:
    """Condition-code mask: bit 0 = branch if CCG, 1 = CCE, 2 = CCL.

    ERS sheet 90: `If CC=CCG and CCF(0:1)=1 then P:=E`, and likewise for CCE
    via CCF(1:1) and CCL via CCF(2:1).
    """
    names = [n for n, b in (("G", 4), ("E", 2), ("L", 1)) if bits & b]
    if len(names) == 3:
        return "ALW"
    return "".join(names) or "NVR"


# Operand kinds, mirroring tools/gen_dasm_tables.py so the Python and C++
# disassemblers dispatch identically. ORDER MATTERS: several keys are
# suffixes of others ("halfword offset" contains "word offset").
KINDS = [
    ("halfword offset",             "BRANCH_HW"),
    ("word offset",                 "MEM_WORD"),
    ("byte offset",                 "MEM_BYTE"),
    ("four times STT entry number", "STT4"),
    ("STT entry number",            "STT"),
    ("immediate operand 2",         "BITFIELD"),
    ("32-#bits, start bit",         "BITFIELD"),
    ("immediate operand",           "IMM"),
    ("shift count",                 "SHIFT"),
    ("I/O opcode",                  "IO"),
    ("N field",                     "NFIELD"),
    ("level + byte offset",         "LEVEL"),
    ("parameter",                   "PARAM"),
]


def kind_of(operand: str) -> str:
    for text, k in KINDS:
        if text in operand:
            return k
    return "NONE"


def decode(data: bytes, p: int, base_addr: int = 0):
    """Return (length, text, branch_target_or_None).

    Output text is byte-identical to the MAME C++ disassembler in
    mame/src/devices/cpu/focus/focusdasm.cpp, so the two can be compared with plain diff.
    """
    if p + 2 > len(data):
        return None
    w = struct.unpack(">H", data[p:p + 2])[0]
    op1 = (w >> 13) & 7

    # ---- Format 6: 32-bit, always word aligned --------------------------
    if op1 == 7:
        if p + 4 > len(data):
            return (2, f"{'.half':<8}{w:#06x}", None)
        d = struct.unpack(">I", data[p:p + 4])[0]
        op2 = (d >> 24) & 0x1F
        ent = OPS.get((6, op2))
        if not ent:
            return (4, f"{'.word':<8}{d:#010x}", None)
        mn, operand, _ = ent
        kind = kind_of(operand)

        if mn == "BCC":
            # ERS sheet 90: CCF := CIR(9:3); sign := CIR(12:1), 0 = +, 1 = -.
            ccf = (d >> 20) & 7
            off = d & 0x7FFFF
            tgt = (base_addr + p + (-off if (d >> 19) & 1 else off)) & 0xFFFFFFFF
            return (4, f"{'BCC.' + _ccf(ccf):<8}{tgt:#010x}", tgt)

        if mn in ("EXF", "DPF"):
            # J = CIR(27:5) start bit, K = CIR(22:5) = 32 - width,
            # bit 8 sign-extends (EXF), bit 9 indexes (ERS sheet 116).
            j, k = d & 0x1F, (d >> 5) & 0x1F
            t = f"{mn:<8}start={j},width={32 - k}"
            if mn == "EXF" and (d >> 23) & 1:
                t += ",S"
            if (d >> 22) & 1:
                t += ",X"
            return (4, t, None)

        i, x = (d >> 23) & 1, (d >> 22) & 1
        base, off = (d >> 19) & 7, d & 0x7FFFF
        if kind in ("MEM_BYTE", "MEM_WORD"):
            arg = _mem(base, off, i, x)
        elif kind == "IMM":
            # LDI/LNI immediates are CIR(8:24) -- the full 24 bits after
            # the opcode (ERS sheet 62), not the 19-bit offset field.
            arg = f"#{d & 0xFFFFFF}"
        elif kind == "STT4":
            arg = f"STT[{off // 4}]"
        elif kind == "LEVEL":
            arg = f"lev={base},{off}"
        else:
            arg = f"{off}"
        return (4, f"{mn:<8}{arg}", None)

    # ---- Format 5: 8-bit operand, no index or indirect bit --------------
    if op1 == 1:
        op2 = (w >> 8) & 0x1F
        operand_val = w & 0xFF
        ent = OPS.get((5, op2))
        if not ent:
            return (2, f"{'.half':<8}{w:#06x}", None)
        mn, operand, _ = ent
        kind = kind_of(operand)
        if kind == "STT":
            arg = f"STT[{operand_val}]"
        elif kind == "IO":
            arg = f"io={operand_val:#04x}"
        elif kind == "NFIELD":
            arg = f"N={operand_val}"
        elif kind == "LEVEL":
            arg = f"lev={operand_val >> 5},{operand_val & 0x1F}"
        else:
            arg = f"#{operand_val}"
        return (2, f"{mn:<8}{arg}", None)

    # ---- Formats 2, 3, 4: OP1 == 0 --------------------------------------
    if op1 == 0:
        b3 = (w >> 12) & 1
        op2 = (w >> 6) & 0x3F
        operand_val = w & 0x3F

        if op2 in (0, 1):
            ent = OPS.get((3 if op2 == 0 else 4, operand_val))
            if not ent:
                return (2, f"{'.half':<8}{w:#06x}", None)
            return (2, ent[0] + (",B" if b3 else ""), None)

        ent = OPS.get((2, op2))
        if not ent:
            return (2, f"{'.half':<8}{w:#06x}", None)
        mn, operand, _ = ent
        kind = kind_of(operand)

        if kind == "BRANCH_HW":
            # Backward variants have bit 2 of the opcode set (Appendix B).
            sign = -1 if (op2 >> 2) & 1 else 1
            tgt = (base_addr + p + sign * operand_val * 2) & 0xFFFFFFFF
            return (2, f"{mn:<8}{tgt:#010x}" + (",I" if b3 else ""), tgt)

        suffix = (",X" if b3 else "") if kind == "SHIFT" else (
            ",B" if b3 else "")
        return (2, f"{mn:<8}{operand_val}{suffix}", None)

    # ---- Format 1: OP1 == 2..6 -------------------------------------------
    ent = OPS.get((1, op1))
    if not ent:
        return (2, f"{'.half':<8}{w:#06x}", None)
    mn, operand, _ = ent
    kind = kind_of(operand)

    if kind == "BRANCH_HW":
        # BCCS: CCF = CIR(20:3) = bits 4..6; sign = CIR(23:1) = bit 7,
        # 0 is + and 1 is -; offset counts halfwords (ERS sheet 90).
        ccf = (w >> 9) & 7
        off = w & 0xFF
        tgt = (base_addr + p + (-off if (w >> 8) & 1 else off) * 2) & 0xFFFFFFFF
        ind = ",I" if (w >> 12) & 1 else ""
        return (2, f"{'BCCS.' + _ccf(ccf):<8}{tgt:#010x}{ind}", tgt)

    i, x = (w >> 12) & 1, (w >> 11) & 1
    base = (w >> 8) & 7
    off = (w & 0xFF) * (4 if kind == "MEM_WORD" else 1)
    return (2, f"{mn:<8}{_mem(base, off, i, x)}", None)


def validate(data: bytes, start: int, end: int | None) -> int:
    """Branch-target coherence test.

    Raw "did it decode" rates prove nothing here: 254 of 256 encodings are
    assigned, so arbitrary bytes decode to *something* at ~99%. Running this
    over English text scores as highly as over real code.

    A test that does discriminate: linear-sweep the region, collect every
    computed branch target, and ask what fraction land on an address that the
    sweep itself identified as an instruction boundary. Real code branches to
    instruction boundaries almost always; in non-code the targets are noise and
    hit boundaries only at the chance rate.
    """
    end = end if end is not None else len(data)

    boundaries, targets = set(), []
    p = start
    while p < end:
        r = decode(data, p)
        if r is None:
            break
        boundaries.add(p)
        if r[2] is not None:
            targets.append((p, r[2]))
        p += r[0]

    inrange = [(s, t) for s, t in targets if start <= t < end]
    hits = [(s, t) for s, t in inrange if t in boundaries]
    n = len(inrange)
    ninstr = len(boundaries)
    # Chance baseline: a target lands on a boundary by luck this often, since
    # most instructions are 2 bytes and so most even addresses are boundaries.
    even = max((end - start) // 2, 1)
    chance = 100 * ninstr / even
    density = 100 * len(targets) / max(ninstr, 1)

    print(f"region         : {start:#x}..{end:#x}  ({ninstr:,} instructions)")
    print(f"branch density : {density:.1f}% of instructions are branches")
    print(f"targets inside : {n:,} of {len(targets):,}")
    if n:
        print(f"on a boundary  : {len(hits):,}  ({100 * len(hits) / n:.1f}%, "
              f"chance baseline {chance:.1f}%)")
        odd = [t for _, t in inrange if t % 2]
        print(f"odd addresses  : {len(odd):,}  (must be 0 -- P's LSB is always 0)")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("image", type=Path)
    ap.add_argument("--start", type=lambda s: int(s, 0), default=0)
    ap.add_argument("--count", type=int, default=64,
                    help="number of instructions (0 = to --end/EOF)")
    ap.add_argument("--end", type=lambda s: int(s, 0))
    ap.add_argument("--stats", action="store_true",
                    help="report decode coverage instead of listing")
    ap.add_argument("--validate", action="store_true",
                    help="branch-target coherence test (see validate())")
    args = ap.parse_args()

    if args.validate:
        return validate(args.image.read_bytes(), args.start,
                        args.end if args.end is not None else None)

    data = args.image.read_bytes()
    p, n = args.start, 0
    limit = args.end if args.end is not None else len(data)
    known = unknown = 0

    while p < limit and (args.count == 0 or n < args.count):
        r = decode(data, p)
        if r is None:
            break
        ln, text, _ = r
        raw = data[p:p + ln].hex()
        if text.startswith("."):
            unknown += 1
        else:
            known += 1
        # A 32-bit instruction must be word-aligned (ERS sheet 53).
        warn = "  <<< misaligned" if ln == 4 and p % 4 else ""
        if not args.stats:
            print(f"{p:08x}  {raw:<8}  {text}{warn}")
        p += ln
        n += 1

    total = known + unknown
    if total:
        pct = 100 * known / total
        print(f"\n{known:,} decoded, {unknown:,} unknown "
              f"({pct:.1f}% recognised) over {total:,} instructions",
              file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
