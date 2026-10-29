#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Joel Heikkila
"""Read the Instruction Set ERS: one sheet, one instruction, or the code's
citations of it.

The ERS is a scanned HP drawing set with a text layer, and its sheet numbers
are its PDF page numbers -- so "ERS sheet 90", as the source cites it, is
page 90 of the PDF. Section 6.2 (sheets 54-130) describes each instruction
in turn under a numbered heading:

    107.    ANDI     (And Immediate)        Format 5 Opcode 2=37
                    (S):= (S) AND CIR(24:8)
            The immediate quantity CIR(24:8) is expanded to 32 bits with
            leading zeros and ANDed with the TOS. The result replaces
            the TOS.
            Indicators = CCA

The pseudocode and the `Indicators =` line are what an audit compares the
implementation against, and they survive the text layer intact.

    tools/ers.py --sheet 90              one sheet, frame stripped
    tools/ers.py --insn ANDI             that instruction's description
    tools/ers.py --insn ANDI --raw       ... without the OCR clean-up
    tools/ers.py --cites                 every "ERS sheet N" in src/,
                                         with what sits at that sheet
    tools/ers.py --list                  every instruction the ERS
                                         describes, and its sheet

Falls back to the tesseract layer in docs/notes when a page's embedded text
is missing; neither layer is clean, so `--insn` reports which it used.
"""
import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PDF = ROOT / "docs/bitsavers/FOCUS_Instruction_Set_ERS_Dec82.pdf"


def chart_mnemonics() -> set:
    """The opcode chart, which is what the ERS is being checked against."""
    import csv as _csv
    rows = [l for l in (ROOT / "docs/opcodes.csv").read_text().splitlines()
            if not l.startswith("#")]
    return {r["mnemonic"] for r in _csv.DictReader(rows)}

TESS = ROOT / "docs/notes/FOCUS_Instruction_Set_ERS_Dec82.tesseract.txt"

if not PDF.exists():
    sys.exit(f"{PDF.relative_to(ROOT)} is not here: HP's manuals are fetched, not carried.\n"
             "Run tools/fetch-media.sh --manuals (MEDIA.txt lists them).")

# Filled on first use; a heading that reads as another chart mnemonic is
# that instruction, not a misreading of this one.
CHART = chart_mnemonics()

# Section 6.2, the per-instruction descriptions.
FIRST, LAST = 54, 130

# Boilerplate from the scanned HP drawing frame that wraps every page. Same
# list tools/extract_opcodes.py uses; kept in step with it deliberately.
FRAME = re.compile(
    r"HEW ?LET ?T|PAC ?K ?A ?R ?D|REVISIONS|SUPERSEDES|SHEET|DWG|Fiasconaro"
    r"|Focus Machine Instruction Set ERS|MISERS|/ ?hp ?/|P\.C\. #|STK"
)

# The drawing's ruled border and the title block under it survive as runs of
# punctuation and stray single characters in the margin. Both sit between
# the paragraphs they interrupt, so dropping them joins a description back
# together rather than losing any of it.
RULE = re.compile(r"^[\s|:+\-_=!lI\.]*$")


def is_frame(line: str) -> bool:
    if FRAME.search(line):
        return True
    if RULE.match(line):
        return True
    # The title block's fields are separated by wide gaps; prose and
    # pseudocode are indented on the left but not internally columnated.
    # Headings are the exception -- "19.  DEL   (Delete   A)   Format 3" is
    # as widely spaced as any title block row -- and every one of them
    # carries the instruction's name in brackets, which the title block
    # never does. Dropping that test cost DEL and DUP their descriptions.
    if "(" not in line and "[" not in line \
            and len(re.findall(r"\S {5,}\S", line)) >= 3:
        return True
    # A line that is nearly all punctuation is the border, not prose.
    body = line.strip()
    if len(body) > 20:
        punct = sum(1 for c in body if not c.isalnum() and not c.isspace())
        if punct > len(body) * 0.6:
            return True
    return False

# "107.  ANDI  (And Immediate)  Format 5 Opcode 2=37", with the item number
# and the mnemonic both frequently damaged.
# The trailing dot is optional: HP numbered later insertions "107.1 XORI"
# with no dot, and those are real headings -- XORI and ORI are only ever
# reachable through one.
HEADING = re.compile(
    r"^\s*(\d+(?:\.\d+)?)\.?\s+([A-Z][A-Z0-9]{1,5})\b\s*\(([^)]*)\)"
    r"(.*?Format\s*(\d).*?)?$", re.M)


def sheet_text(n: int) -> str:
    out = subprocess.run(
        ["pdftotext", "-layout", "-f", str(n), "-l", str(n), str(PDF), "-"],
        capture_output=True, text=True).stdout
    return "\n".join(l for l in out.splitlines() if not is_frame(l))


def clean(text: str) -> str:
    """Undo the OCR damage that is mechanical rather than a judgement call.

    Only substitutions that cannot change the meaning: the assignment
    operator is variously ":=", "::", ":: ", ":" and "; the ERS never uses a
    bare colon as anything else in pseudocode. Everything ambiguous is left
    alone for a reader to adjudicate -- this is an aid to reading the page,
    not a transcription anyone should cite.
    """
    out = []
    for line in text.splitlines():
        if re.search(r"\bBegin\b|\bEnd\b|:=|::|\bIf\b|\bthen\b|\bGo to\b", line):
            line = re.sub(r"\s*::\s*", " := ", line)
            line = re.sub(r"(\w|\))\s+:\s+(?=[A-Z0-9(])", r"\1 := ", line)
        out.append(line.rstrip())
    # The := substitution can make a surviving scrap of the title block read
    # as prose, so the frame filter runs once more over the result.
    out = [l for l in out if not is_frame(l)]
    # Collapse the runs of blank lines the frame strip leaves behind.
    return re.sub(r"\n{3,}", "\n\n", "\n".join(out)).strip("\n")


# A heading is frequently broken across an OCR line end, so the search runs
# over the sheet flattened to one line -- the same thing extract_opcodes.py
# does with the Format/Opcode clause, and for the same reason.
# The item number's dot floats: the scan renders "21." as "21 ." about as
# often as not, so whitespace is allowed on both sides of it.
# The name is parenthesised. The scan sometimes reads the parentheses as
# square brackets -- "4.6 LBIT [Load Bit]" -- so there is a second pattern
# for that, run as a separate pass: allowing either bracket in one pattern
# lets a match span further right than it should and displaces neighbouring
# headings (DEL and DUP both vanished that way).
# The item number is the weakest part of a heading to anchor on: the scan
# renders 110 as "llO" and 141 as "l~1", so requiring digits loses XGDS and
# ASL outright. The rest of the shape -- a short all-caps mnemonic followed
# by a bracketed name -- carries the match, so the number is allowed to be
# any short run of digits and the glyphs commonly confused with them.
# ...and the digits may be spaced apart ("1 6 l~ ." for 161) or read as
# letters ("1A9" for 149), so the run tolerates both.
ITEM = (r"(?:[\dlLiIoOSsGgAaBb~]\s{0,2}){1,4}"
        r"(?:\.\s*(?:[\dlLiIoOSsGgAaBb~]\s{0,2}){1,2})?")


def _heading(open_, close):
    return re.compile(
        r"(?:^|\s)(" + ITEM + r")\s*\.?\s+([A-Z][A-Z0-9]{1,5})\s*"
        + open_ + r"([^" + close + r"]{3,60})" + close
        + r"\s*(?:For\S*\s*(\d))?")


FLAT_HEADING = _heading(r"\(", r"\)")
FLAT_HEADING_SQ = _heading(r"\[", r"\]")

_INDEX = None


def index() -> dict:
    """mnemonic -> (sheet, item number, full name, format). Cached: it costs
    77 pdftotext runs to build and every mode below wants it."""
    global _INDEX
    if _INDEX is not None:
        return _INDEX
    found = {}
    pages = {n: re.sub(r"[ \t\n]+", " ", sheet_text(n))
             for n in range(FIRST, LAST + 1)}
    # Parenthesised headings first, so a bracketed match can only add.
    for pattern in (FLAT_HEADING, FLAT_HEADING_SQ):
        for n, flat in pages.items():
            for m in pattern.finditer(flat):
                item, mnem = m.group(1), m.group(2)
                name, fmt = m.group(3), m.group(4)
                found.setdefault(mnem, (n, item, name.strip(), fmt))
    # Second pass over the tesseract layer, which reads some headings the
    # embedded text loses outright -- ZERO and DZRO among them. It carries
    # no page structure, so an instruction found only here has no sheet
    # number; the audit still gets the text, which is what it needs.
    if TESS.exists():
        flat = re.sub(r"[ \t\n]+", " ", TESS.read_text(errors="replace"))
        for pattern in (FLAT_HEADING, FLAT_HEADING_SQ):
            for m in pattern.finditer(flat):
                item, mnem = m.group(1), m.group(2)
                name, fmt = m.group(3), m.group(4)
                if mnem not in found:
                    found[mnem] = (None, item, name.strip(), fmt)
    # The hand-read headings, which no pattern reaches.
    for mnem, (sheet, item, how) in ADJUDICATED.items():
        found.setdefault(mnem, (sheet, item, f"adjudicated: {how}", None))
    _INDEX = found
    return found


# The scan confuses a small, stable set of glyph pairs, and it does so in
# the headings as much as the bodies -- "S8IT" for SBIT, "TOR" for IOR,
# "IXTT" for IXIT, "DR" for BR. Rather than hand-correct a list that would
# go stale, generate the plausible readings of a mnemonic and try each.
OCR_PAIRS = [("B", "8"), ("I", "T"), ("I", "1"), ("I", "L"), ("O", "0"),
             ("O", "D"), ("S", "5"), ("B", "D"), ("C", "G"), ("U", "V")]


# Headings the scan destroys past any pattern's reach, read off the page by
# hand. Each entry records the sheet, the item number, and how the line
# actually reads, so the claim can be rechecked rather than taken on trust
# -- the same discipline as FOCUS.md Appendix B.
#
# Two kinds. Most are a mangled mnemonic over an intact name and opcode
# clause, which is enough to identify them outright. CSL and CTCB have no
# legible heading at all and are placed by their item number: section 6.2
# numbers its descriptions in sequence, so a gap between two known items
# has exactly one instruction in it, and the opcode confirms which.
ADJUDICATED = {
    #          sheet  item     how the heading actually reads
    "TSTB":  (70,  "34",    'TSTS   (Test Byte)  Format 3 Opcode 3=27'),
    "CSL":   (73,  "45",    'heading destroyed; the gap between LSR (44) and '
                            'CSR (46), and format 2 opcode 2=4 is CSL'),
    "BRE":   (93,  "114.1", 'SRE (Branch on TOS Even) Format 2 Opcode 2=71'),
    "BRO":   (93,  "114.2", 'BRa (Branch on TOS Odd) Format 2 Opcode 2=72'),
    "DISP":  (98,  "124",   'DrS?    (Dispatch)    Format 4 Opcode 3=16'),
    "CMPB":  (105, "137",   'Ct"l P B  ( Com par e By t e s )  For:n a t 2 '
                            'CP cod e 2 =5 4'),
    "IOW":   (115, "142",   'lOW (1/0 Write) Format 5 Opcode 2=17'),
    "CNTO":  (125, "161",   'CNT0  ( Co un tOn e s )  For In a t 4 a p cod '
                            'e 3 =5 4'),
    "CTCB":  (127, "168",   'heading destroyed; the gap between ESCP (167) '
                            'and GTSP (169), with (CTCBP):=(S) on sheet 128'),
}

# Still unplaced: UP, DOWN and GMPY. Neither their headings nor their
# opcode clauses survive anywhere in section 6.2's text layer, so no amount
# of pattern work will find them -- they need the page images read at 400
# dpi, the method FOCUS.md 3.1 used for the STATUS field diagram. All three
# were named from HP's own toolchain instead (tools/astable.py), which is
# why the project has them at all.
UNPLACED = ("UP", "DOWN", "GMPY")


def ocr_variants(mnem: str):
    seen = {mnem}
    out = [mnem]
    # One substitution at a time is enough for every case seen so far, and
    # two would generate more false matches than it resolved.
    for i, ch in enumerate(mnem):
        for a, b in OCR_PAIRS:
            for frm, to in ((a, b), (b, a)):
                if ch != frm:
                    continue
                v = mnem[:i] + to + mnem[i + 1:]
                if v not in seen:
                    seen.add(v)
                    out.append(v)
    return out


def resolve(mnem: str, idx: dict):
    """Find the heading that describes an instruction.

    Three ways, in decreasing confidence: the heading is there as written;
    the heading is there under an OCR misreading; or this is a variant form
    -- LDS, LPPS, PCLS -- described inside its base instruction's text,
    which is how the ERS writes them.
    """
    # The hand-read table first: index() folds those entries in so --insn
    # can print them, which would otherwise make them indistinguishable
    # from a heading the scan read cleanly. They are weaker evidence and
    # the report says so.
    if mnem in ADJUDICATED:
        return mnem, "adjudicated"
    if mnem in idx:
        return mnem, "exact"
    for v in ocr_variants(mnem)[1:]:
        if v in idx and v not in CHART:
            return v, f"ocr:{v}"
    # The S suffix is "off the stack", the I suffix "immediate"; both are
    # usually a paragraph under the base rather than a heading of their own.
    for suffix in ("S", "IS", "I"):
        if mnem.endswith(suffix) and len(mnem) > len(suffix) + 1:
            base = mnem[:-len(suffix)]
            if base in idx:
                return base, f"variant of {base}"
            for v in ocr_variants(base)[1:]:
                if v in idx and v not in CHART:
                    return v, f"variant of {base}, ocr:{v}"
    return None, "not found"


def describe(mnem: str, raw: bool) -> int:
    idx = index()
    head, how = resolve(mnem, idx)
    if head:
        if how != "exact":
            print(f"[{mnem}: {how}]", file=sys.stderr)
        mnem = head
    if mnem not in idx:
        near = [k for k in idx if k.startswith(mnem[:2])]
        print(f"{mnem} has no heading in section 6.2 of the text layer.",
              file=sys.stderr)
        if near:
            print("near it: " + ", ".join(sorted(near)), file=sys.stderr)
        return 1
    n, item, name, fmt = idx[mnem]
    if n is None:
        # Only the tesseract layer has this heading. That layer carries no
        # page structure, so there is no sheet to name -- but the text is
        # there, and it is what the audit actually needs. Slice it from the
        # heading to the next one, the same way a sheet is sliced.
        return describe_tesseract(mnem, name, item, raw)

    lines = sheet_text(n).splitlines()
    # Heading lines on the page itself, found the same tolerant way the
    # index finds them, so the slice lands where the index said it would.
    line_head = re.compile(r"^\s*" + ITEM + r"\s*\.?\s+"
                           r"([A-Z][A-Z0-9]{1,5})\s*[(\[]")
    marks = [(i, m.group(1)) for i, l in enumerate(lines)
             if (m := line_head.match(l))]

    body = "\n".join(lines)
    for k, (i, who) in enumerate(marks):
        if who != mnem:
            continue
        if k + 1 < len(marks):
            body = "\n".join(lines[i:marks[k + 1][0]])
        else:
            # The last description on a sheet runs onto the next one, and
            # what it carries over is often the `Indicators =` line -- ADD's
            # condition codes are on sheet 75, its opcode on 74. Follow it
            # to the next heading rather than stopping at the page edge.
            body = "\n".join(lines[i:])
            nxt = sheet_text(n + 1).splitlines()
            over = next((j for j, l in enumerate(nxt) if line_head.match(l)),
                        len(nxt))
            if over:
                body += "\n" + "\n".join(nxt[:over])
        break
    else:
        # The heading is on this sheet but its line is too damaged to
        # anchor on; give the whole sheet rather than nothing, and say so.
        print(f"[{mnem}: heading line unreadable, printing all of sheet {n}]",
              file=sys.stderr)

    print(f"=== {mnem} ({name}) -- ERS sheet {n}, item {item} ===")
    print(body if raw else clean(body))
    return 0


def describe_tesseract(mnem: str, name: str, item: str, raw: bool) -> int:
    lines = TESS.read_text(errors="replace").splitlines()
    head = re.compile(r"^\s*" + ITEM + r"\s*\.?\s+([A-Z][A-Z0-9]{1,5})\s*[(\[]")
    marks = [(i, m.group(1)) for i, l in enumerate(lines)
             if (m := head.match(l))]
    body = None
    for k, (i, who) in enumerate(marks):
        if who != mnem:
            continue
        end = marks[k + 1][0] if k + 1 < len(marks) else min(i + 40, len(lines))
        body = "\n".join(lines[i:end])
        break
    print(f"=== {mnem} ({name}) -- item {item}, tesseract layer "
          f"(no sheet number) ===")
    if body is None:
        print(f"heading indexed but its line could not be re-found; try\n"
              f"  grep -n -A20 '{mnem} ' {TESS.relative_to(ROOT)}")
        return 1
    print(body if raw else clean(body))
    return 0


def cites() -> int:
    """Check the source's ERS citations against what the ERS sheets hold.

    Two kinds of citation appear in src/. Most name a sheet in the
    architecture chapters -- registers, addressing, the trap sequence --
    which this cannot check, and it says so rather than pretending. The
    rest name a sheet in section 6.2 alongside an instruction, and those
    are checkable: the sheet either describes that instruction or it does
    not, and a citation that has drifted a page is worth knowing about.
    """
    idx = index()
    by_sheet = {}
    for mnem, (n, _, _, _) in idx.items():
        if n is not None:
            by_sheet.setdefault(n, set()).add(mnem)

    pat = re.compile(r"ERS sheets? (\d+)(?:\s*[-\u2013]\s*(\d+))?")
    mnem_tok = re.compile(r"\b([A-Z][A-Z0-9]{1,5})\b")

    checked = confirmed = mismatched = outside = unplaceable = 0
    findings = []
    for d in ("mame/src/devices/cpu/focus", "mame/src/devices/bus/hp_cio", "mame/src/mame/hp"):
        for f in sorted((ROOT / d).rglob("*")):
            if f.suffix not in (".h", ".cpp", ".ipp"):
                continue
            lines = f.read_text().splitlines()
            for i, line in enumerate(lines, 1):
                for m in pat.finditer(line):
                    lo = int(m.group(1))
                    hi = int(m.group(2)) if m.group(2) else lo
                    span = range(lo, hi + 1)
                    if not any(FIRST <= x <= LAST for x in span):
                        outside += 1
                        continue
                    on = set().union(*(by_sheet.get(x, set()) for x in span)) \
                            if span else set()
                    # Which instruction is the citing line about? These
                    # comments put their subject first and the citation
                    # after it -- "SCON (ERS sheets 111-112). Built on a
                    # real temp: SLD makes one..." is about SCON, and SLD
                    # is only mentioned. Taking every mnemonic on the line
                    # reported those mentions as mismatches; taking the
                    # first takes the subject.
                    all_named = [t for t in mnem_tok.findall(line)
                                 if t in CHART and t != "ERS"]
                    named = set(all_named[:1])
                    if not named:
                        checked += 1
                        continue
                    # An instruction the index cannot place -- one the
                    # embedded layer lost and only the page-less tesseract
                    # layer carries -- cannot be checked either way, and
                    # reporting it as a mismatch is just noise.
                    placeable = {t for t in named
                                 if (h := resolve(t, idx)[0])
                                 and idx[h][0] is not None}
                    unplaceable += len(named) - len(placeable)
                    if not placeable:
                        checked += 1
                        continue
                    missing = {t for t in placeable
                               if resolve(t, idx)[0] not in on}
                    checked += 1
                    if missing:
                        mismatched += 1
                        where = f"sheet {lo}" if lo == hi else f"sheets {lo}-{hi}"
                        findings.append((
                            f"{f.relative_to(ROOT).as_posix()}:{i}", where,
                            sorted(missing), sorted(on)[:8], line.strip()[:88]))
                    else:
                        confirmed += 1

    print(f"{outside:>4}  citations of sheets outside section 6.2 "
          f"(architecture; not checkable here)")
    print(f"{checked:>4}  citations of a section 6.2 sheet")
    print(f"{confirmed:>4}    ... naming an instruction that sheet describes")
    print(f"{mismatched:>4}    ... naming one it does not")
    print(f"{unplaceable:>4}  instructions named but not placeable "
          f"(heading lost; see --coverage)\n")

    for where, sheet, missing, on, text in findings:
        print(f"{where}  cites {sheet}")
        print(f"    names:     {', '.join(missing)}")
        print(f"    sheet has: {', '.join(on) if on else '(nothing in 6.2)'}")
        print(f"    line:      {text}")
    return 1 if mismatched else 0


def listing() -> int:
    idx = index()
    print(f"{len(idx)} instructions with a section 6.2 heading\n")
    print(f"{'mnem':<8}{'sheet':>6}  {'item':<7}{'fmt':<5}name")
    for mnem in sorted(idx):
        n, item, name, fmt = idx[mnem]
        print(f"{mnem:<8}{n:>6}  {item:<7}{fmt or '?':<5}{name}")
    return 0


def indicators() -> int:
    """Every instruction's `Indicators =` line against what the code sets.

    The condition code is the easiest thing in this architecture to leave
    out: it is a side effect, nothing tests it in the instruction's own
    unit test, and an omission only shows up when some later branch reads a
    stale code. The ERS states the effect for every instruction, so the two
    can be compared wholesale.

    This reads the code, so it reports candidates, not defects: a body that
    sets the code through a helper looks like one that does not, and the
    OCR renders "CCA" as "eCA" and "Unaffected" as "Un~ffected". Each
    survivor still needs a person.
    """
    import csv as _csv
    import io
    import contextlib

    core = sorted((ROOT / "mame/src/devices/cpu/focus").glob("focus_*.cpp"))
    src = "\n".join(f.read_text() for f in core)

    funcs = {f"exec_fmt{n}": n for n in range(1, 7)}
    bodies = {}
    for name, fmt in funcs.items():
        m = re.search(rf"exec_trap {name}\(", src)
        if not m:
            continue
        rest = src[m.end():]
        nxt = re.search(r"\n\t(?:exec_trap|void|bool|u32) \w+\(", rest)
        fn = rest[:nxt.start()] if nxt else rest
        cases = [(mm.start(), int(mm.group(1), 8) if mm.group(1).startswith("0")
                  else int(mm.group(1)))
                 for mm in re.finditer(r"case (\d+):", fn)]
        for i, (pos, op) in enumerate(cases):
            # A fallthrough label slices to an empty body; the code belongs
            # to the last label of the run and both opcodes share it.
            j = i
            while j + 1 < len(cases) and not re.sub(
                    r"case \d+:|//[^\n]*|\s", "",
                    fn[cases[j][0]:cases[j + 1][0]]):
                j += 1
            end = cases[j + 1][0] if j + 1 < len(cases) else len(fn)
            bodies[(fmt, op)] = fn[pos:end]

    # Only the four STATUS fields the ERS calls indicators: the condition
    # code, Carry, Overflow and Inexact Result (focusstate.h). Writing any
    # other field -- the interrupt enable, the rounding mode -- is not an
    # indicator effect, and counting it reported INT and SRM as violations
    # of an "Indicators Unaffected" they honour perfectly well.
    sets = re.compile(r"\bd?cca\(|set_cca|set_fp_cc|set_overflow"
                      r"|r\.status\s*=|put\(sf::(?:CC|C|O|IR)\b")
    # A body that delegates sets the code inside the helper it calls, so
    # the helper is inspected too -- otherwise every LSI-style instruction
    # reports as setting nothing.
    call = re.compile(r"\b(do_\w+|stack_indirect|exec_fmt\d)\s*\(")

    def body_sets(body, depth=1):
        if sets.search(body):
            return True
        if depth <= 0:
            return False
        for m in call.finditer(body):
            h = re.search(rf"\n\t(?:exec_trap|void|bool|u32) {m.group(1)}\(",
                          src)
            if not h:
                continue
            rest = src[h.end():]
            nxt = re.search(r"\n\t(?:exec_trap|void|bool|u32) \w+\(", rest)
            if body_sets(rest[:nxt.start()] if nxt else rest, depth - 1):
                return True
        return False

    rows = [l for l in (ROOT / "docs/opcodes.csv").read_text().splitlines()
            if not l.startswith("#")]
    chart = [(r["mnemonic"], int(r["format"]), int(r["octal"], 8))
             for r in _csv.DictReader(rows)
             if not r["mnemonic"].startswith("ESCAPE_") and r["mnemonic"] != "-"]

    line = re.compile(r"Indicator[s]?\s*[=:]?\s*(.{0,70})", re.I)
    unaffected = re.compile(r"un.{0,2}ffected", re.I)

    def stated(mnem):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            try:
                describe(mnem, False)
            except SystemExit:
                pass
        # An unsliceable description prints the whole sheet, and the first
        # Indicators line on it belongs to some other instruction.
        if "printing all of sheet" in err.getvalue():
            return None
        m = line.search(out.getvalue())
        return m.group(1).strip() if m else None

    silent, noisy, seen = [], [], set()
    for mnem, fmt, op in chart:
        if mnem in seen:
            continue
        body = bodies.get((fmt, op))
        if body is None:
            continue
        say = stated(mnem)
        if not say:
            continue
        seen.add(mnem)
        if unaffected.match(say):
            if body_sets(body):
                noisy.append((mnem, fmt, op, say))
        elif not body_sets(body):
            silent.append((mnem, fmt, op, say))

    print(f"{len(silent)} instructions: the ERS names an indicator, the code "
          f"appears to set none\n")
    for mnem, fmt, op, say in sorted(silent):
        print(f"  {mnem:<7} fmt{fmt} op{op:<4o} ERS: Indicators = {say[:50]}")
    print(f"\n{len(noisy)} instructions: the ERS says Unaffected, the code "
          f"sets something\n")
    print("  (advisory: a body that delegates is judged by the whole helper,\n"
          "   so a store form sharing a helper with a load form reports the\n"
          "   load's condition code as its own)\n")
    for mnem, fmt, op, say in sorted(noisy):
        print(f"  {mnem:<7} fmt{fmt} op{op:<4o} ERS: Indicators {say[:50]}")
    return 0


def coverage() -> int:
    """Every mnemonic in the chart, and whether the ERS describes it."""
    import csv as _csv
    rows = [l for l in (ROOT / "docs/opcodes.csv").read_text().splitlines()
            if not l.startswith("#")]
    # ESCAPE_* are the chart's names for the encoding escapes that select a
    # format, not instructions -- section 6.2 has nothing to describe.
    chart = [(r["mnemonic"], r["format"], r["octal"])
             for r in _csv.DictReader(rows)
             if not r["mnemonic"].startswith("ESCAPE_")]
    idx = index()

    buckets = {}
    for mnem, fmt, octal in chart:
        head, how = resolve(mnem, idx)
        kind = how.split(":")[0].split(" ")[0] if head else "unreadable"
        if kind not in ("exact", "ocr", "variant", "adjudicated"):
            kind = "unreadable"
        sheet = idx[head][0] if head else None
        buckets.setdefault(kind, []).append((mnem, fmt, octal, sheet, how))

    total = len(chart)
    print(f"{total} mnemonics in docs/opcodes.csv\n")
    for kind, title in (("exact", "described under their own heading"),
                        ("ocr", "heading found under an OCR misreading"),
                        ("variant", "described inside a base instruction"),
                        ("adjudicated", "heading read off the page by hand"),
                        ("unreadable", "not located at all")):
        rows_ = buckets.get(kind, [])
        print(f"{len(rows_):>4}  {title}")
    print()
    for kind in ("ocr", "variant", "adjudicated", "unreadable"):
        rows_ = buckets.get(kind, [])
        if not rows_:
            continue
        print(f"--- {kind} ---")
        for mnem, fmt, octal, sheet, how in sorted(rows_):
            where = f"sheet {sheet}" if sheet else "-"
            print(f"  {mnem:<8} fmt{fmt} op{octal:<4} {where:<10} {how}")
        print()
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sheet", type=int, help="print one sheet")
    ap.add_argument("--insn", help="print one instruction's description")
    ap.add_argument("--cites", action="store_true", help="check src/ citations")
    ap.add_argument("--list", action="store_true", help="index of section 6.2")
    ap.add_argument("--coverage", action="store_true",
                    help="the chart against the ERS, ahead of an audit")
    ap.add_argument("--indicators", action="store_true",
                    help="every Indicators line against what the code sets")
    ap.add_argument("--raw", action="store_true", help="skip the OCR clean-up")
    a = ap.parse_args()

    if not PDF.exists():
        print(f"{PDF} is missing", file=sys.stderr)
        return 2
    if a.sheet:
        t = sheet_text(a.sheet)
        print(t if a.raw else clean(t))
        return 0
    if a.insn:
        return describe(a.insn.upper(), a.raw)
    if a.cites:
        return cites()
    if a.list:
        return listing()
    if a.coverage:
        return coverage()
    if a.indicators:
        return indicators()
    ap.print_help()
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
