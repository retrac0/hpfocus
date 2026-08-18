#!/usr/bin/env python3
"""Generate a MAME debugger script that runs HP's factorial on the emulated CPU.

The program is the recursive factorial from section 7 of the Instruction Set
ERS (sheet 129), assembled from the published listing -- the same one the
standalone core test uses. Running it inside MAME exercises the whole device:
the address space, the scheduler hook, the disassembler and the debugger.

    python3 tools/gen_mame_smoke.py > fact.dbg
    cd mame && ./focus hp9020 -debug -debugscript ../fact.dbg \
        -video none -sound none -nothrottle -window -str 8
    # then check RESULT.bin

NOTE: the MAME debugger parses numeric literals as HEX. Every constant below
is written with an explicit 0x prefix for that reason -- an unprefixed "12"
silently means eighteen.
"""
import sys

def f1(op1, i, x, base, off): return (op1 << 13) | (i << 12) | (x << 11) | (base << 8) | off
def f3(op3, b3=False):        return (0x1000 if b3 else 0) | op3
def f5(op2, operand):         return 0x2000 | (op2 << 8) | operand
def bccs(ccf, back, hw):      return (6 << 13) | (ccf << 9) | (0x100 if back else 0) | hw

CODE_BASE, CALLER, PROC, CODE_END = 0x0800, 0x1000, 0x2000, 0x3000
PL = (CODE_END - 1) & ~3
SB = 0x10000
DB, DL = 0x20000, 0x21000 - 1
PARAM, RESULT = DB + 32, DB + 40          # word offsets 8 and 10

CALLER_CODE = [
    f3(0o006),              # ZERO       allocate space for the result
    f1(2, 0, 0, 2, 8),      # LDS  DB+8  load the actual parameter
    f5(0o022, 2),           # PCLS 2     call the factorial procedure
    f1(5, 0, 0, 2, 10),     # STS  DB+10 store the result
    f3(0),                  # NOP        "rest of program"
]

PROC_CODE = [
    f1(2, 0, 0, 5, 4),      # LDS  Q-4   load the passed parameter
    f5(0o003, 0),           # CMPI 0     test for zero, popping the stack
    bccs(5, 0, 3),          # BCCS GL P+3
    f5(0o014, 1),           # LDIS 1     if zero, return 1
    bccs(7, 0, 0o10),       # BCCS GEL P+10
    f3(0o006),              # ZERO       space for the intermediate result
    f1(2, 0, 0, 5, 4),      # LDS  Q-4
    f5(0o005, 1),           # SUBI 1     decrement the parameter
    f5(0o022, 2),           # PCLS 2     recursive call
    f1(2, 0, 0, 5, 4),      # LDS  Q-4
    f3(0o022, True),        # MPY        doubleword product
    f3(0o001),              # DELB       drop the upper half
    f1(5, 0, 0, 5, 5),      # STS  Q-5   store the intermediate product
    f5(0o023, 1),           # EXIT 1
]


def emit(origin, insns):
    buf = list(insns)
    if len(buf) % 2:
        buf.append(0)
    return [f"d@{origin + k * 2:#x} = {(buf[k] << 16) | buf[k + 1]:#x}"
            for k in range(0, len(buf), 2)]


def main() -> int:
    n = int(sys.argv[1]) if len(sys.argv) > 1 else 12
    out = ["// HP recursive factorial, ERS section 7 sheet 129, inside MAME"]
    out += emit(CALLER, CALLER_CODE)
    out += emit(PROC, PROC_CODE)
    out += [
        f"d@{PL - 8:#x} = {PROC - CODE_BASE:#x}   // STT entry 2 -> procedure",
        f"d@{PARAM:#x} = {n:#x}",
        f"d@{RESULT:#x} = 0",
        f"PB = {CODE_BASE:#x}", f"PL = {PL:#x}", f"P = {CALLER:#x}",
        f"SB = {SB:#x}", f"SL = {SB + 0x8000 - 1:#x}",
        f"S = {SB + 3:#x}", f"Q = {SB + 3:#x}",
        f"DB = {DB:#x}", f"DL = {DL:#x}", "STATUS = 0",
        f"bp {CALLER + 8:#x}",
        "go",
        f"save RESULT.bin,{DB:#x},0x40",
        "quit",
    ]
    print("\n".join(out))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
