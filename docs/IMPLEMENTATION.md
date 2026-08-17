# Implementation plan

**Decision: build the simulator inside MAME. Use SIMH's `hp3000` as the
architectural reference. Prototype the disassembler standalone first.**

## Why MAME

There is **no existing FOCUS emulation anywhere** — not in MAME, not in SIMH,
not standalone. The CPU core is greenfield whichever host is chosen, so no
framework saves any ISA work. The real question is what *else* the machine
needs, and for the Series 500 that list is long: HP-IB/CS80 mass storage, an
IOP with DMA on eight channels, Teledisk floppies, and for the 520 a raster
display and keyboard.

MAME supplies all of it:

| Need | MAME provides |
|---|---|
| HP-IB | `bus/ieee488/` — full IEEE-488 bus with many HP devices |
| Teledisk discs | `formats/td0_dsk.cpp` |
| Display, keyboard, input | framework |
| Save states, scheduling | framework |
| Debugger, disassembly, tracing | framework, via `util::disasm_interface` |

There is also a substantial body of adjacent HP work by **F. Ulivi** — accurate
HP 9845B/C, HP 9825A, HP 9831 and HP-85 drivers, plus an HP-IB *remotizer* that
bridges emulated HP-IB to real hardware over TCP. That is the closest existing
work to this project and the natural person to consult.

The debugger deserves particular weight. The dominant early activity is
disassembling `SYSTEM POWERUP` against an unknown loader handoff contract.
Registering a disassembler with the CPU device yields breakpoints, watchpoints,
tracing and memory windows for free — exactly that workload.

## Why not SIMH

Wrong shape. SIMH targets terminal-driven minicomputers: no HP-IB
infrastructure, weak graphics story. The 520 is a bitmapped workstation with an
integrated display, and the 540 needs CIO and HP-IB mass storage.

**But read its HP 3000 source.** FOCUS is explicitly "patterned after the AMIGO
machine instruction set" *(ISET ERS, 4)* — the HP 3000 architecture widened to
32 bits — and Dave Bryan and Bob Supnik's
[HP 3000 Series III simulator](https://opensimh.org/simdocs/hp3000_doc.html)
implements the same programmer's model: stack machine, CST/DST segmentation,
absent-segment traps, condition codes. Two techniques transfer directly:

1. The 3000 keeps **the top four stack locations in hardware registers** —
   these are FOCUS's `A`, `B`, `C`, `D` *(ISET ERS, 57)*.
2. It uses a **register renamer** so pushes and pops need no physical copying.
   Worth adopting; a naively memory-backed stack will be both slow and clumsy.

## Why not standalone

Reimplementing IEEE-488, floppy handling, video and a debugger is where
projects of this kind die. The existing Python tooling stays for media work,
but the machine itself should not be built there.

---

## Staging

### Phase 0 — standalone disassembler ✅ **done**

[`tools/focusdis.py`](../tools/focusdis.py), table-driven from
[opcodes.csv](opcodes.csv).

**Result: the opcode transcription and format decoding are confirmed correct
against real FOCUS code.** Full evidence in
[HARDWARE.md §5.2](HARDWARE.md#52-empirical-validation); the short version is a
cleanly decoded switch dispatch in the HP-UX kernel at `0x100`, with parallel
case bodies and converging default paths, plus branch density 70–97× higher in
code than in a text control region.

Two encoding questions were resolved in the process and folded back into the
spec: format 6's `BASE` is 3 bits (not 4), and the `BCCS`/`BCC` condition-mask
and sign fields are now pinned down (HARDWARE.md §5.1.1).

```sh
python3 tools/focusdis.py os.req --start 0x100 --count 40
python3 tools/focusdis.py os.req --start 0x100 --end 0x8000 --validate
```

Still open from this phase: `EXF`/`DPF` sub-field boundaries, and locating the
true entry point of `SYSTEM POWERUP` (offset `0x30` is the ASCII SCCS string
`@(#)05.01`, not code; dense code begins around `0x100`).

### Phase 1 — MAME CPU device

Broken into five steps, smallest useful artifact first.

**1.1 — Disassembler** ✅ **done**. `util::disasm_interface`, in
[`src/cpu/focus/`](../src/cpu/focus/), ready to drop into MAME at
`src/devices/cpu/focus/`. The natural first MAME artifact: MAME requires a
disassembler for any new CPU, it is a standalone compilable unit with no
dependency on the execution core, and it was fully specifiable today.

To avoid two divergent opcode tables, the C++ table is **generated** from
[opcodes.csv](opcodes.csv) by
[`tools/gen_dasm_tables.py`](../tools/gen_dasm_tables.py) — 218 opcodes across
six formats. `opcodes.csv` remains the single source of truth.

Verified by **differential testing against `focusdis.py`**. A small shim
([`src/test/emu.h`](../src/test/emu.h)) supplies the handful of MAME types
so the same source builds standalone; both implementations then disassemble
the same corpora and their output is diffed line by line:

| Corpus | Instructions | Result |
|---|---|---|
| HP-UX 5.01 kernel `os.req` | 164,720 | identical |
| BASIC 2.0 boot disc | 132,108 | identical |
| BASIC 2.0 utility disc | 122,131 | identical |
| Control: manual text | 131,071 | identical |
| **Total** | **550,030** | **identical** |

The control corpus matters: running over non-code exercises the rare and
malformed encodings that real code never produces, which is where two
implementations of the same spec are most likely to drift apart.

```sh
cd src/test && make test
```

Debugger integration is wired up: `STEP_OVER` on `PCL`/`PCLS`, `STEP_OUT` on
`EXIT`/`SXIT`/`IXIT`.

**1.2 — Register model and state** ✅ **done**.
[`src/cpu/focus/focusstate.h`](../src/cpu/focus/focusstate.h) holds the 18
machine-visible registers (HARDWARE.md §2), the `STATUS` fields, and the
A/B/C/D top-of-stack cache using the SIMH `hp3000` renaming approach — pushing
rotates an index rather than copying four values, and returns the word
displaced off the bottom so the caller can spill it.

Deliberately free of MAME dependencies, so it unit tests on its own and the
CPU device can simply embed it.

Resolving this step required settling the `STATUS` layout, which had been an
open question since the OCR of the ERS field diagram is unusable. Read from
the page image at 400 dpi, and the reading is *forced*: all seventeen named
fields tile the register with only bits 15 and 18 left over (HARDWARE.md §2.5).

The `ers_field` helper converts ERS bit numbering (bit 0 = MSB) exactly once,
so nothing else has to think about it — the single most likely source of
silent bugs in this architecture.

81 assertions, covering bit-numbering conversion, the layout tiling property,
field independence under write, the pointer alignment invariants, and the
renamer's push/pop/`XCH`/`DXCH`/`CAB` semantics.

```sh
cd src/test && make && ./statetest
```

**1.3 — Memory and addressing.** 29-bit address space, big-endian. Effective
address computation for all six formats including indirect and post-indexing,
plus the bounds checks the microcode performs on every reference.

**1.4 — Instruction execution.** Grouped as the ERS groups them: loads/stores,
stack and register, arithmetic and logical, program control, move/scan/string,
I/O, special. Traps and the interrupt sequence last.

**1.5 — Validation.** The ERS sample program *(sheet 130)* and the Appendix E
floating-point conversion sequences *(sheet 144)* as unit tests, then the 9020
diagnostics as the real conformance suite.

Target throughout: single CPU, no MPB multiprocessing.

### Phase 2 — machine

MPB memory map and dedicated locations (HARDWARE.md §6), IOP with DMA, SCM, and
HP-IB/CS80 mass storage. The missing loader ROM is replaced by a high-level
implementation of the documented bootstrap protocol (HARDWARE.md §8.5).

### Phase 3 — 520 display and keyboard

Then the diagnostics, then BASIC 2.0, then the demo discs.

---

## Core design notes

**Bit numbering.** The ERS numbers bits with **0 as the most significant**, and
writes fields as `(N:M)` = M bits starting at bit N. MAME's `BIT()` counts from
the LSB. Convert at the boundary and keep ERS numbering inside the decoder, or
this will cause bugs indefinitely.

**Bounds checking belongs in the CPU.** Every memory reference is bounds-checked
*by the microcode* *(ISET ERS, 10)*, not by an external MMU. Model it in the
core, not as a separate device.

**Instruction length.** 16 bits unless `OP1 == 7`, which selects a 32-bit
instruction; those are always word-aligned *(ISET ERS, 53)*. Per Appendix G, a
16-bit instruction in the right halfword of a word whose left halfword is
arbitrary data cannot be branched to — so a disassembler doing linear sweep
should track halfword alignment and flag a 32-bit opcode found at an odd
halfword.

**Unimplemented-instruction traps.** Format 4 opcode 75, format 5 opcode 30 and
format 6 opcode 37 *should* trap but do not on CPU 4.0 and earlier
(HARDWARE.md §9). Decide early whether to be bug-compatible; contemporary
software was written to avoid these.

**Condition codes** have three interpretations (CCA, CCB, CCC) selected per
instruction, not a single global meaning. The opcode table must record which
applies.

---

## Resolved: format 6 field widths

HARDWARE.md §5 listed the format 6 `BASE` width as an open question — 3 or 4
bits. **It is 3 bits.** The tick marks under the ERS field diagram read
`0 3 8 9 10 13 31`, and Appendix B shows a combined `I X Bas` field spanning
bits 8–12. Offset therefore runs bits 13–31, which is exactly the 19 bits the
text specifies.

Final layouts, ERS bit numbering (bit 0 = MSB):

```
Format 1 (16-bit, OP1 = 2..6)
  0  1  2 | 3 | 4 | 5  6  7 | 8 .............. 15
   OP1    | I | X |  BASE   |     OFFSET

Format 2 (16-bit, OP1 = 0)
  0  1  2 | 3 | 4 ....... 9 | 10 ............ 15
   0 0 0  | b |    OP2      |    OPERAND

Format 3 (OP1 = 0, OP2 = 0)  /  Format 4 (OP1 = 0, OP2 = 1)
  0  1  2 | 3 | 4 ....... 9 | 10 ............ 15
   0 0 0  | b |  000000/1   |      OP3

Format 5 (16-bit, OP1 = 1)        no index or indirect bit
  0  1  2 | 3 ..... 7 | 8 ................... 15
   0 0 1  |   OP2     |       OPERAND

Format 6 (32-bit, OP1 = 7)
  0  1  2 | 3 ..... 7 | 8 | 9 | 10 11 12 | 13 ......... 31
   1 1 1  |   OP2     | I | X |   BASE   |    OFFSET
```

`BASE` decodes identically in formats 1 and 6:

| 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|
| `P+` | `P−` | `DB+` | `DL−` | `Q+` | `Q−` | `SB+` | `S−` |

In formats 2, 3 and 4, bit 3 is the indirect bit, the index bit, or an
instruction-specific indicator, depending on the instruction *(ISET ERS, 54)*.
