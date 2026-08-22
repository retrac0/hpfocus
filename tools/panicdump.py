#!/usr/bin/env python3
"""Decode the HP-UX Series 500 panic report back into symbols.

When this kernel cannot go on it prints "System halted due to system
error." followed by its whole state, through the raw-FR console writer
-- registers, a stack window, I/O state and a segment table. The report
is a single long line of 80-column records; this tool parses it back
apart and, given the boot image, resolves every code reference to a
module name and a file offset ready for tools/focusdis.py.

Layout, established by matching a live report against FOCUS_BREAK state:

  <message>  ERROR RET STATUS ERROR          the panic frame: the escape
                                             number, the PB-relative
                                             return offset and STATUS
                                             (CSN in its low 12 bits)
  MSG MASK PSDB DST SL SB DL DB              the register file
  8 words                                    unidentified (word 0 looks
                                             like FLAGS)
  STK:  OFFSET:w w w w w w w w ...           stack rows. The label is the
        SB-relative BYTE offset of the first word; words follow at
        DESCENDING addresses, rows descending too. The first word of the
        first row is the panic frame's own stack marker (delta at Q+0),
        so the call chain walks exactly: status at Q-4, return at Q-8,
        next Q = Q - delta.
  IO:   8 words                              unidentified
  SEGS: 56 words                             per-segment words, meaning
                                             unestablished

Usage:
  tools/runhpux.sh ... | tools/panicdump.py --image software/cct/7914_unitSave_hpux4.0.bin
  tools/panicdump.py crash.txt --image ...

The CSN -> module map is the runtime CST of the current HP-UX 5.30
configuration (printed by FOCUS_STATEAT); segment numbers are stable for
a given image and boot order but are NOT a property of the image alone.
"""
import argparse
import re
import sys
from pathlib import Path

from escmap import load_boot_image
from segdump import segments

# The runtime CST, as booted from the 7914 unit save on the hp9040
# driver (FOCUS_STATEAT dump, 2026-08-22). Data segments are omitted.
CSN_MODULE = {
    1: "TRAP HANDLERS", 2: "CORE SYSTEM", 4: "HP-UX ENTRYPTS",
    6: "HP-UX KERNEL", 8: "IOS and DDMs", 10: "SDF_DISC",
    11: "TTY SUBSYSTEM", 21: "HP-UX DIRECTORY", 26: "MATH LIBRARY",
    29: "HP-UX IO DRIVERS", 32: "HP27128/30", 33: "HP27125A",
    35: "HPIB_FLEX", 38: "SERIAL", 40: "HP7970", 42: "HP268X",
    43: "INTERNAL_HP-IB", 46: "MEMORY_VOL", 47: "CIPERLP",
    49: "CONFIGURATION", 50: "MEMORY INTRINSIC", 51: "HP27110",
    53: "HP27140", 61: "HP98700", 70: "WIN/H9000",
    100: "9030/40 FR PANEL",
}

REGS = ["MSG", "MASK", "PSDB", "DST", "SL", "SB", "DL", "DB"]


def parse_report(text: str):
    i = text.find("System halted due to system error.")
    if i < 0:
        sys.exit("no 'System halted due to system error.' in the input")
    toks = re.findall(r"[0-9A-F]{6}:[0-9A-F]{8}|[0-9A-F]{8}|STK:|IO:|SEGS:",
                      text[i:])
    head, stack, io, segs = [], {}, [], []
    section = "head"
    for t in toks:
        if t in ("STK:", "IO:", "SEGS:"):
            section = t
            continue
        if ":" in t:
            label, w = t.split(":")
            off = int(label, 16)
            stack[off] = int(w, 16)
            cursor = off - 4
            continue
        v = int(t, 16)
        if section == "head":
            head.append(v)
        elif section == "STK:":
            stack[cursor] = v
            cursor -= 4
        elif section == "IO:":
            io.append(v)
        else:
            segs.append(v)
    return head, stack, io, segs


def module_files(image: bytes):
    """name -> (pb_file, code_start, code_end); first segment wins."""
    out = {}
    for s in segments(image):
        out.setdefault(s["name"], (s["code_start"] - 36,
                                   s["code_start"], s["code_end"]))
    return out


def where(csn: int, ret: int, mods) -> str:
    name = CSN_MODULE.get(csn)
    if not name:
        return f"seg {csn} +{ret:#x}"
    loc = f"{name} +{ret:#x}"
    if mods and name in mods:
        pb, lo, hi = mods[name]
        if lo <= pb + ret < hi:
            loc += f"  (file {pb + ret:#08x})"
    return loc


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("input", nargs="?", type=Path,
                    help="captured output (default: stdin)")
    ap.add_argument("--image", type=Path,
                    help="boot image or 7914 unit save, for file offsets")
    args = ap.parse_args()

    text = (args.input.read_text(errors="replace") if args.input
            else sys.stdin.read())
    head, stack, io, segs = parse_report(text)
    mods = module_files(load_boot_image(args.image)) if args.image else None

    if len(head) < 12:
        sys.exit(f"short header ({len(head)} words) -- truncated report?")
    err, ret, status, param = head[0:4]
    regs = dict(zip(REGS, head[4:12]))
    extra = head[12:]

    csn = status & 0xfff
    print(f"error {err} (0x{err:x})  parameter {param} (0x{param:x})")
    print(f"raised from {where(csn, ret, mods)}   STATUS={status:08x}")
    print("  " + "  ".join(f"{n}={v:08x}" for n, v in regs.items()))
    if extra:
        print("  unident: " + " ".join(f"{v:08x}" for v in extra))

    # The call chain: the first (highest) stack word is the panic frame's
    # delta word at Q+0.
    if stack:
        sb = regs["SB"]
        top = max(stack)
        print("\ncall chain (walked from the report's own stack):")
        q = top
        for _ in range(16):
            delta = stack.get(q)
            st = stack.get(q - 4)
            rt = stack.get(q - 8)
            if delta is None or st is None:
                break
            fcsn = st & 0xfff
            print(f"  Q=SB+{q:#05x} ({sb + q:#08x})  delta={delta:<5} "
                  f"ret {where(fcsn, rt or 0, mods)}")
            if delta == 0 or delta > q:
                break
            q -= delta

        # Text the stack carries is often the loudest clue.
        lo = min(stack)
        raw = bytearray()
        for off in range(lo, top + 4, 4):
            raw += (stack.get(off, 0)).to_bytes(4, "big")
        text_runs = re.findall(rb"[ -~]{5,}", bytes(raw))
        if text_runs:
            print("\nstack text: " +
                  " | ".join(r.decode() for r in text_runs))

    if io:
        print("\nIO:   " + " ".join(f"{v:08x}" for v in io))
    if segs:
        nz = [(i + 1, v) for i, v in enumerate(segs) if v]
        print("SEGS: " + " ".join(f"[{i}]={v:04x}" for i, v in nz))
    return 0


if __name__ == "__main__":
    sys.exit(main())
