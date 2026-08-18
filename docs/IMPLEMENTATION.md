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

Still open from this phase: locating the true entry point of `SYSTEM POWERUP`.
The module layout is understood — 32-byte binary header, 16-byte name field,
variable-length SCCS version string, then body — which is why offset `0x30` is
the ASCII string `@(#)05.01` rather than code. Dense code begins around
`0x100`, but the header words are still unidentified.

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
cd src/test && make test
```

**1.3 — Memory and addressing** ✅ **done**.
[`src/cpu/focus/focusaddr.h`](../src/cpu/focus/focusaddr.h): effective address
computation for every base register, all four indirect pointer formats,
post-indexing, and the segment bounds checks the microcode applies to every
reference (HARDWARE.md §3.1.1–3.1.3).

Verified against **ERS Figure 2.1** (sheet 19), a worked example HP supplied
with the memory contents and the expected top-of-stack for ten `LOAD`
instructions spanning every addressing mode and pointer format. All ten
reproduce exactly. 35 assertions in total, adding bounds and trap cases.

Memory and external-data-segment lookup are abstract interfaces, so the unit
tests against a flat array and the CPU device will plug in an `address_space`.

The external data segment path is complete in
[`src/cpu/focus/focusdst.h`](../src/cpu/focus/focusdst.h): the DST entry and
page table entry layouts (now transcribed, HARDWARE.md §4.1), the unpaged
path, both chaining modes, the paged path, and every protection trap.
78 further assertions.

One documentation discrepancy surfaced and is recorded in §4.1: sheet 55's
pseudocode tests `OFFSET < LENGTH-1` where sheet 18 gives the valid range as
inclusive of `Length-1`. Read literally the pseudocode would make the last
word of every segment unreachable, so the implementation follows sheet 18.

**1.4 — Instruction execution** ◐ **in progress**.
[`src/cpu/focus/focusexec.h`](../src/cpu/focus/focusexec.h) is an
instruction-at-a-time interpreter over the register model, addressing unit and
DST walk.

**Milestone: the ERS's own sample program runs.** Section 7 (sheet 129) gives
a recursive factorial in FOCUS machine code, and it executes correctly for
n = 0…12, producing 479001600 for 12!. That exercises procedure call and
return with stack markers, recursion twelve deep, the doubleword `MPY` with
`DELB`, Q-relative parameter access, and both `BCCS` targets the listing names.

Implemented: format 1 loads and stores; format 2 shifts, circular shifts and
short branches; format 3 stack, integer arithmetic and logic, and the full
single and double floating-point group; format 4 stack-indirect, bit access
and the privileged absolute-address group; format 5 immediates, debug aids and
`PCLS`/`EXIT`; format 6 loads, stores, branches, bit fields and `CHEK`.

Floating point uses the host's own `float` and `double`. That is sound rather
than lazy: the ERS targets IEEE 754 **Draft 5.11**, whose single and double
basic formats are identical to the ratified binary32 and binary64 (Appendix F).
What differs is the four selectable rounding modes and the inexact-result flag,
neither of which is modelled yet.

`TRY`/`ESCP` exception handling is in, including the pseudo stack marker and
its chain head at `(SB+2)`. `ESCP` unwinds by pointing `Q` at the frame and
performing an `EXIT 0`, exactly as the ERS specifies.

**Internal traps are in.** The sequence on ERS sheet 44 is implemented: form
an external program pointer to code segment 1 with the STT entry number set
from the cause, push a stack marker, push the parameter, load `PB` and `PL`
from CST entry 1, clear `STATUS` except the mode bit and the code segment
number, and take `P` from the STT entry. `trap_cause` carries all 40 causes
from §5.2.2 with their octal STT entry numbers.

Faults the core already detected — unimplemented opcodes, bounds violations,
divide by zero, every addressing trap — now vector through this path once a
Code Segment Table exists. Until one does they are still reported to the
caller, which is what unit tests want and what a cold machine has to do
anyway.

Step 3 of the sequence, adjusting code segment use counts, is not modelled:
the counts exist for the operating system's benefit and nothing in the CPU
depends on them.

Not yet: decimal (BCD), move/scan/string, external interrupts and `IXIT`. Procedure calls take local program
pointers only — external pointers need the Code Segment Table walk and the
U-bit check. Unimplemented opcodes return `trap::unimplemented` rather than
behaving as no-ops, so gaps surface immediately instead of corrupting state.

### Measuring progress

[`tools/isa_coverage.py`](../tools/isa_coverage.py) parses the dispatch tables
out of the core, disassembles the HP-UX kernel and the BASIC boot disc, and
weights coverage by how often each opcode actually occurs — so effort goes
where the real code is rather than where the opcode table looks empty.

```
298,238 instructions decoded
288,430 would execute  (96.7%)
```

The three largest remaining entries — format 6 opcode 37 and format 4 opcodes
20 and 21 — are encodings **Appendix B leaves unassigned**. They cannot be
real instructions, so they are data caught by the linear sweep. Actual code
coverage is therefore higher than 95.9%.

Two caveats on that number. It is a static count over whole images, so data
regions decoded as instructions inflate the denominator — several of the
"missing" entries are encodings Appendix B leaves unassigned, which cannot be
real code. And static frequency is not dynamic frequency: a rare opcode inside
a hot loop matters more than the count suggests.

`MVB` is in — the highest-frequency string instruction, moving bytes in
either direction between a data segment and either another data segment or
the code segment.

Largest genuine gaps by frequency: the rest of the move/scan/string group,
`CRB`, `LPP`/`LPPS`, `LIA`/`LIAS`, and the decimal instructions. The string
group needs the four-word string descriptor machinery of ERS §2.3.4, which is
a subsystem rather than a handful of opcodes.

One open question from the trap sequence: step 4 says the parameter is pushed
"at Q+3", but after the marker `S` equals `Q`, making the next free word
`Q+1`. The implementation pushes normally; the discrepancy is unresolved.

The stack is memory-backed for now. The tested `tos_cache` renamer from step
1.2 is a later optimisation; correctness first.

**1.5 — Validation.** The ERS sample program *(sheet 130)* and the Appendix E
floating-point conversion sequences *(sheet 144)* as unit tests, then the 9020
diagnostics as the real conformance suite.

Target throughout: single CPU, no MPB multiprocessing.

**1.6 — MAME device** ✅ **done**.
[`src/cpu/focus/focus.h`](../src/cpu/focus/focus.h) and
[`focus.cpp`](../src/cpu/focus/focus.cpp) wrap the core as `focus_device`:
`cpu_device` with the execute, memory, disassemble and state interfaces, a
29-bit big-endian program space over 32-bit words, all eighteen registers
exposed to the debugger, and save-state entries.

The device also implements `focus::memory_interface`, so the core's accesses
route straight to the MAME address space. The architectural headers keep no
MAME dependency, which is what lets the same code carry 198 unit assertions
outside a MAME tree.

Registered in `scripts/src/cpu.lua` as `CPUS["FOCUS"]`, with the disassembler
as a separate `opt_tool` entry so `unidasm` can pick it up.

All three sources compile clean against real MAME headers (C++20).

### Phase 2 — machine

**2.1 — CPU side of the I/O path** ✅ **done**. `IOW`, `IOR` and `IOC` are
implemented against an `io_interface` seam, the same pattern used for memory
and external data segments. Per FOCUS I/O ERS §6, the instruction's 8-bit
operand is the I/O operation code and the interface-address word on the stack
supplies IC (bits 22–25), the IOP channel number (26–28) and PA (29–31). A
write pops its data word, a read pushes the reply, a control sends nothing.

With no IOP attached the instructions set Message register bit 7, "illegal
I/O opcode", and raise a message trap — so a machine without one behaves
predictably rather than silently doing nothing.

**2.2 — IOP** ◐ **core done**. [`src/iop/focusiop.h`](../src/iop/focusiop.h)
implements the device behind that seam: eight CIO channels each with its own
DMA register set, interrupt collection with mask and request registers, the
interface poll, and the interval timer.

The 46 MPB command opcodes of I/O ERS §5 are transcribed as an enum, in the
octal the ERS uses. About 35 are implemented — the DMA, interrupt, timer and
interface groups — and an unrecognised opcode is *refused* rather than
silently accepted, so the CPU raises its message trap and gaps surface.

Cards attach through an `iop_interface` with status, flag, service-request
and byte-at-a-time DMA. The IOP reaches memory and the CPU's interrupt line
through an `iop_core::host`, so like the CPU it unit tests standalone: 82
assertions covering the register set, masked and unmasked interrupts, DMA in
both directions, the poll word, timer reload, and the CPU driving it all the
way through `IOW`/`IOR`/`IOC`.

**Remaining:** attention requests, the command-execution-from-interrupt path,
address lockout, the register save and restore commands (`RIRS`/`WIRR`), and
DMA verify and match. None is needed to move bytes.

**2.3 — System Control Module** ◐ **registers and bootstrap done**.
[`src/iop/focusscm.h`](../src/iop/focusscm.h) implements the SCM as a card on
IOP channel 1, peripheral address 7 — a select code being an (IOP channel,
peripheral address) pair, which puts select code 7 there.

Registers per the service manual: `R1` arms the ROM read on `84` hex, `R2`
reports power-fail, self-test and battery-fail status, `R3` bit 14 announces
the loader, `R4` and `R6` read ROM data with the address counter decrementing
from `FFFF`, `R10` carries service-processor traffic, and `R12` latches an
address in complemented form. `find_loader()` performs the documented
downward select-code scan.

Note the bit numbering: the service manual counts these hardware registers
conventionally with bit 0 least significant, the opposite of the Instruction
Set ERS. Both conventions now live in the codebase, each documented where it
applies.

**Remaining:** the real-time clock and the 2 KB of battery-backed memory are
stubbed, and the high-level loader itself is not written — the mechanism will
serve a ROM image the moment one is dumped, but with none available the
loader's *effect* still has to be synthesised. That depends on the
`SYSTEM POWERUP` handoff contract, which is the next thing to reverse
engineer.

**Finding: the handoff can be determined empirically.** Disassembling both
`SYSTEM POWERUP` modules shows them running the *same* I/O sequence at a
constant offset — `RCS` to read the CPU revision, `WAIT`, `PSHR`, `IXIT`,
then a block of IOC/IOW device initialisation. BASIC 2.0 and HP-UX 5.01 do
the same thing in the same order.

That reframes the loader problem. Rather than deduce the handoff contract by
reading, the module can be *run*: load it, set plausible register state, and
single-step in MAME's debugger. Where it faults says what it expected. The
CPU, IOP and SCM now exist precisely to make that possible, and the two
independent samples cross-check each other.

**2.4 — Mass storage.** HP-IB and the 9895 floppy path, to mount the BASIC
and diagnostic discs.

**2.5 — Memory map.** MPB layout and the dedicated locations of
HARDWARE.md §6 — the Device Reference Table, the interrupt priority head and
tail pointers, and the per-CPU blocks.

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
