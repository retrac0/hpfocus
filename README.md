# hpfocus

## Introduction

This is an ongoing quest to attempt to emulate the HP 9000 Series Model 520
with the HP FOCUS microprocessor.

FOCUS, introduced in 1982, was the first commercial 32-bit microprocessor —
a microcoded, segmented, stack-oriented machine with no programmer-visible 
general-purpose registers, implementing something similar to the HP 3000
architecture widened to 32 bits.

The goal is to run HP-UX and BASIC.

## Current state

There's a sample factorial program in section 7 of the Instruction Set ERS
which runs correctly on the emulated CPU for n = 0 to 12 which means
procedure calls, stack markers, twelve-deep recursion, doubleword multiply.
From the listing on ERS sheet 129 (addresses in octal, as printed):

```
calling sequence:
  10000  ZERO           Allocate Space for Result
  10002  LDS DB+10      Load Actual Parameter
  10004  PCLS 2         Call Factorial Procedure
  10006  STS DB+12      Store Result
  10010  ...            Rest of Program

factorial procedure:
  20000  LDS Q-4        Load Passed Parameter
  20002  CMPI 0         Test it for Zero (Pop Stack)
  20004  BCCS GL P+3    If not Zero, Branch to 20012
  20006  LDIS 1         If Zero, Return 1
  20010  BCCS GEL P+10  Branch to 20030
  20012  ZERO           Allocate Space for Intermediate Result
  20014  LDS Q-4        Load Passed Parameter
  20016  SUBI 1         Decrement Parameter
  20020  PCLS 2         Recursive Call
  20022  LDS Q-4        Load Passed Parameter
  20024  MPY            Multiply by Previous TOS
  20026  DELB           Delete Upper Half of Product
  20030  STS Q-5        Store Intermediate Product
  20032  EXIT 1         Exit this Recursion
```

Disassembler, register model, addressing, DST walk and an execution core that
covers **98.3% of the instructions in the real HP-UX 5.30 kernel**, weighted
by frequency. Internal traps vector correctly through code segment 1. Still to
come: the string and decimal groups, external interrupts, then the machine
around the CPU.

That kernel is now a *complete, undamaged* 5.30 image recovered from the 7914
unit-save tapes, along with its entire userland — see
[software/README.md](software/README.md). It replaces the truncated 5.01 tape
as the coverage target, and it raises the stretch goal from "bring up a
kernel" to **boot this system**.

Recent steps closed two groups. First the system group the kernel needs and
the earlier corpora barely exercised: `LPP`/`LPPS` and `LIA`/`LIAS` (program
and intermediate pointers), `PSLA` (the test-and-set semaphore), `CPAA`, `CRB`
and `MUC` (segment use counts). Then the byte scan and compare group —
`SCW`, `SCU`, `CMPB` and `TRAN` — which matters most in userland, where the
kernel-weighted figure understates it.

What is still named in the gap is four string-descriptor instructions
(`SLD`, `SVAL`, `SCON`, `SAS`) and the decimal group. Everything
else left is an opcode Appendix B does not assign — data that a linear scan
misreads as code, since a run of `0xff` decodes as format 6 opcode 37.

**Where the boot actually stops.** `src/test/bootrun` now carries the 9020
diagnostics environment and BASIC 2.0 through their **entire launch chain**:
`SYSTEM POWERUP` verifies and relocates every segment, builds the segment
table, DRT, per-CPU block, Interrupt Control Stack, scheduler lists and the
first task's TCB and stack segment; its final `IXIT` dispatches through the
TCB; the first task enters the trap-handler segment as code segment 1, calls
the SUNOS kernel through the CST, scans and configures the devices, and
**prints the console self-test pattern** — the printable-ASCII ramp — through
the emulated IOP to a console card at select code 6. Both systems then sit in
the same I/O completion wait, which is the current frontier: the console and
mass-storage cards need real device semantics behind their FR registers. The
HP-UX 5.30 kernel gets seven million instructions down the same path before
hitting an opcode the 1982 ERS does not assign — the 1985 kernel uses
post-ERS instructions.

Getting there took a day of digging out what only real code reveals: `LDI`
and `LNI` immediates are 24 bits wide, not the 19-bit offset field (both
disassemblers and the core agreed on the same wrong reading, which is why
550,030 differentially-tested instructions never caught it); the store-side
stack-indirect instructions take their pointer *beneath* the value; a
privileged `EXIT` to code segment 0 restores the pre-CST absolute
environment; `TRY`'s frame bounds test against `SL`, not `S`; and the whole
dispatcher machinery — `IXIT`'s eight cases, `DISP`, `LLSH`, `LINS`/`LDEL`,
`XGDS`, `FLMO`, `GTSP`, the external-interrupt entry — now runs the
flowcharts on ERS sheets 140-142. The one loader-ROM unknown left is marked
by a single documented nudge in the harness (see `bootrun.cpp`).

**The one-table discovery.** SUNOS keeps a *single* segment table serving
as both Code and Data Segment Table, and the two layouts disagree about
what word 0's flag bits mean: the DST's *referenced* bit (29) is the CST's
*absence* bit, and the CST's *reference* bit (31) is the DST's absence bit.
A CPU that maintains either automatically therefore poisons the other view
— the boot died marking the kernel segment absent moments after executing
in it. Neither bit is maintained now, and that is not a shortcut: the
powerup module initialises the flags clear and nothing in the OS ever
clears them again, so the hardware evidently does not write them either.
`LLSH` likewise compares only bits 1-31, because the heap's free-block
sizes carry a flag in bit 0 and the first-fit search must still order them
numerically.

**Where it stands now.** Both systems run their launch chain, print the
console self-test, discover their devices, and then park in the
dispatcher's idle `STOP` with an empty ready list — no faults, no traps, a
correctly idling machine waiting for work. The work never arrives because
the powerup device scan registers nothing: it demands a card whose `RISF`
answers and whose `FR3` identity reads zero, and it is satisfied by no
console. The main task blocks in the kernel's unit-indexed device-request
path with an empty device table. **The next thing this needs is a mass
storage device** — the DDM lives in the `IOS and DDMs` segment and its
protocol can be read out of a run with `bootrun --probe <pa> <id>
--probelog`, which is what that switch exists for.

**HP-UX 5.01 boots the same chain.** The kernel on the damaged tape is
whole (`system/97080C/os.req`, all sixteen segments verify), and once it is
terminated as a boot image it runs powerup, relocation, the TCB dispatch
and the console self-test exactly as BASIC and the diagnostics do. The
stored `os.req` is the *installable* file, not a boot image: the segment
chain ends without the negative word the powerup module stops on, so the
walk runs off the end and checksums garbage for ninety million
instructions. Truncating at the chain end and appending `ffffffff` makes it
bootable:

```sh
python3 tools/lifdir.py software/97089C.hpi --extract S97080C --out s.cpio
python3 tools/cpiolist.py s.cpio --extract system/97080C/os.req --out os.req
python3 -c "d=open('os.req','rb').read(); open('hpux501.bin','wb').write(d[:0x58c2c]+b'\xff\xff\xff\xff')"
src/test/bootrun hpux501.bin --p64 0x1000000
```

**Both HP-UX versions then stop on the same thing**: format 4 opcodes
**020 and 021**, which the Dec-82 Appendix B leaves unassigned — and so,
it turns out, does HP's own toolchain. 5.01 uses
them 1,192 times and 5.30 1,514 times; BASIC and the diagnostics never
execute either. 5.01 reaches its first `021` after 1.26M instructions, in
the `CORE SYSTEM` segment.

**What the recovered toolchain says.** The 5.30 tapes carry HP's own
assembler (`/bin/as`) and Pascal compiler (`/usr/lib/pascomp`), and the
compiler holds the instruction set as a table of 16-byte records — mnemonic,
opcode bits in place, operand class, flags. [`tools/hptable.py`](tools/hptable.py)
reads it. Two results:

- It is the strongest check yet on the opcode map: **208 assignments agree,
  none disagree**, and it settles two glyphs the chart had damaged —
  `SRSI` is really `BRSI`, `RPC` is really `RFC` (both now fixed in
  `docs/opcodes.csv`).
- It leaves **exactly the same fourteen format 4 slots empty** as the 1982
  chart — `020`-`024`, `032`-`036`, `045`, `046`, `061`, `075` — while
  listing every documented privileged instruction. So `020` and `021` are
  implemented in silicon but were never exposed through the shipped tools:
  systems instructions emitted by HP's internal MODCAL compiler, which is
  not on the tapes.

That also matches how the kernels use them. Decoding code segments properly,
`020` (746) and `021` (419) are *more* frequent in the 5.01 kernel than
`SSI`, yet they are absent from every hand-written module (`SYSTEM POWERUP`,
`POWERUP SELFTEST`, `MATH LIBRARY`) and from user programs — where they are
as rare as the privileged absolute-access instructions `PLDA`/`PSTA`. They
are kernel-only, privileged, and compiler-generated.

**What the use sites say about their shape.** Over all 1,165 `UP`/`DOWN`
sites in the 5.01 kernel:

- **The operand is a pointer.** Without the `B` bit they are preceded by
  `LRAS` (267/155), `LRA` (189/114) or `CPTR,B` (53/18) — all of which
  leave one tagged pointer on the stack and nothing else. With `B` they are
  preceded by `ADDI` (141/90) or `ADD` (29), the tail of a
  `LDS Q+n; ADDI #k` sequence: a pointer loaded from a local, advanced to a
  field. Both forms arrive with a pointer on top.
- **They do not report anything the kernel tests.** 76–86% of sites have no
  conditional branch within five instructions, so these are not
  test-and-branch primitives whose result is inspected — the wait is taken
  in microcode, which is what a real P/V pair on this machine would have to
  do, and it implies an interaction with the dispatcher.
- **The `B` bit is not resolved.** It correlates almost perfectly with how
  the pointer was produced (`LRA`/`LRAS` → no `B`, `ADDI` → `B`) rather
  than with anything in the surrounding code, which is what makes the
  obvious reading — the usual "delete the operand" bit — less than certain.

Every `DOWN` sits inside an interrupt-state bracket (`INT,B` … `INT`), as a
kernel semaphore acquisition would.

So the remaining unknown is not what these instructions are for; it is the
layout of the semaphore they point at and what the microcode does to the
running task when the wait blocks. **They are deliberately still
unimplemented**: a guessed P/V that quietly schedules wrong is worse than
one that stops.

Digital archaeology results: very little survives for this machine!  There's
a few disk images preserved by hpmuseum.net but that's it.  This repository
includes copies of: 
 * BASIC Language System 2.0
 * 9020 factory diagnostics,
 * BASIC demo suite (all undamaged), 
 * an HP-UX 5.01 system tape (preliminary analysis suggests this tape image is
   truncated) and most of the length is lost but there does seem to be a complete
   kernel!
 * An HP-UX 5.3 disk image from a 9714 maybe? wrote a tool to dump the filesystem
   appears to contain a live image from early 1991 of HP-UX 5.3 for FOCUS

## Layout

```
docs/
  FOCUS.md             modern CPU architecture description + full opcode spec
  HARDWARE.md          consolidated 520/540 hardware spec  <- start here
  SOFTWARE.md          target media, HP-UX versions, boot ordering
  opcodes.csv          full FOCUS opcode map, from ERS Appendix B
  bitsavers/           11 primary PDFs (~46 MB)
  hpjournal/           3 HP Journal issues on the Series 500
  notes/               pdftotext extractions, Appendix B + loader flowchart
software/
  README.md            provenance and per-image analysis
  97089C.hpi           HP-UX 5.01 tape (damaged)
  basic/ demos/ test/  20 Teledisk discs, all decoding cleanly
src/
  cpu/focus/           MAME CPU device + disassembler, registers,
                       addressing, DST walk, execution core
  iop/                 I/O processor: DMA, interrupts, interval timer
  mame/                the hp9020 / hp9040 machine driver
  test/                standalone builds + unit and differential tests
mame/                  MAME upstream, shallow submodule
tools/
  focusdis.py          FOCUS disassembler + validation harness
  gen_dasm_tables.py   generate the C++ opcode tables from opcodes.csv
  link_into_mame.sh    symlink src/cpu/focus into the MAME tree
  td0.py               decode Teledisk images to raw sectors
  lifdir.py            list/extract LIF volumes
  cpiolist.py          list/extract big-endian old-binary cpio
  modmap.py            map named modules in a boot image
  extract_opcodes.py   scrape opcodes from the ERS text layer
  check_opcodes.py     diff that scrape against the Appendix B transcription
```

## SDF extractor

under tools/sdf7914.py there is a tool to extract the files off the SDF file system
under software/cct/ run:

```
$ cd software/cct/
$ gunzip 7914_unitSave_hpux4.0.bin.gz 
$ gunzip 7914_unitSave_hpux4.0_tape2.bin.gz
$ python ../../tools/sdf7914.py 7914_unitSave_hpux4.0.bin 7914_unitSave_hpux4.0_tape2.bin --extract out 
```

## Plan

Build inside **MAME** — it supplies HP-IB, Teledisk, display, and a debugger,
and no framework saves the ISA work since no FOCUS emulation exists anywhere.
Read SIMH's `hp3000` as the architectural reference: FOCUS is the HP 3000 stack
architecture widened to 32 bits. See [docs/IMPLEMENTATION.md](docs/IMPLEMENTATION.md).

**Phase 0 and steps 1.1-1.2 are complete.**
[tools/focusdis.py](tools/focusdis.py) disassembles FOCUS and confirmed the
opcode map against the real HP-UX kernel and BASIC boot disc. The MAME
disassembler in [src/cpu/focus/](src/cpu/focus/) is differentially tested
against it over **550,030 instructions with byte-identical output**, and the
register model, addressing, DST walk and execution core carry 522 unit
assertions — including all ten worked examples from ERS Figure 2.1 and the
recursive factorial from ERS section 7.

Four encoding questions were resolved along the way and folded back into the
spec: format 6's `BASE` width, the `BCCS`/`BCC` branch fields, the `EXF`/`DPF`
sub-fields, and the full `STATUS` bit layout.

```sh
git submodule update --init --depth 1 mame   # ~930 MB
./tools/link_into_mame.sh
cd src/test && make test
```

## Documentation

Eleven primary documents are in [docs/bitsavers/](docs/bitsavers/), mirrored
from `bitsavers.org/pdf/hp/9000_500/`. The two that matter most:

- **FOCUS Instruction Set ERS** (Dec 1982, 150 sheets) — the CPU reference.
  Registers, addressing, segmentation, interrupts, every instruction, plus
  appendices covering the opcode bit patterns, dedicated memory locations,
  IEEE floating-point conformance, and known silicon bugs.
- **FOCUS I/O ERS** (Dec 1982, 80 sheets) — the IOP, DMA, interrupts, and the
  interval timer.

Both have usable OCR text layers, extracted into [docs/notes/](docs/notes/).

Because that OCR is poor, the CPU is now specified afresh in
**[docs/FOCUS.md](docs/FOCUS.md)** — a modern architecture description and
complete opcode reference reconstructed from the Appendix B chart, the
tested emulator, and the legible parts of the prose, with every contested
reading adjudicated and listed.

## The opcode map

[docs/opcodes.csv](docs/opcodes.csv) is a hand transcription of **Appendix B**,
the fold-out chart on sheet 137 that gives bit patterns for all six instruction
formats. The chart is authoritative — the OCR of the prose descriptions
disagrees with it in several places.

The page is rendered at 400 dpi as
[docs/notes/appendixB-137.png](docs/notes/appendixB-137.png) and is fully
legible.

The transcription is validated against a second, independent derivation:
[tools/extract_opcodes.py](tools/extract_opcodes.py) scrapes opcode
assignments out of the section 6.2 prose, and
[tools/check_opcodes.py](tools/check_opcodes.py) diffs the two. Current result
is 25 conflicts, **all** of them OCR damage in the prose (`S8IT` for `SBIT`,
`IXTT` for `IXIT`, `LOB` for `LDB`, `TOR` for `IOR`, `DR` for `BR`, and so on).
No conflict pointed at an error in the chart transcription.

```
python3 tools/check_opcodes.py
```

## Known gaps

1. **No SCM loader ROM dump** (09020-80000/80001). The *mechanism* is fully
   documented — select-code scan, DMA handshake, complete flowchart — and both
   acquired systems begin with the `SYSTEM POWERUP` module the loader jumps to,
   giving two independent samples of the handoff contract. Nothing in the CPU,
   memory, trap, or IOP work depends on it.
2. **The HP-UX tape is 77% lost.** The kernel survived; the entire userland
   (`SCOMMANDS`) did not. Full HP-UX needs a better image. BASIC and the
   diagnostics are undamaged, so this is **not on the critical path**.
3. **Memory Controller ERS is missing** from bitsavers. Less of a hole than
   the references to it imply: the software-visible MPB semantics — slave
   registers, message bits, semaphores, the interrupt lists, all 46 MPB
   commands — are in the I/O ERS and the Instruction Set ERS. What the MC ERS
   withholds is bus-protocol detail and the memory controller's own channel-0
   command set (HARDWARE.md §10.2).

## Boot order

1. 9020 diagnostics — best CPU workout, fewest dependencies
2. **BASIC 2.0** — first realistic interactive milestone, undamaged media
3. HP-UX kernel bring-up — complete kernel, but stops at `init`
4. BASIC demos — graphics and I/O
5. Full HP-UX — blocked on a better tape

See [docs/SOFTWARE.md](docs/SOFTWARE.md) for detail.

## Sources

- [bitsavers.org — HP 9000/500](http://www.bitsavers.org/pdf/hp/9000_500/)
- [HP Computer Museum — 500 Series](https://www.hpmuseum.net/exhibit.php?class=1&cat=12)
- [OpenPA — HP FOCUS Processor](https://www.openpa.net/focus_processor_hp.html)
- [The HPDir Project](https://www.hp9845.net/9845/projects/hpdir/)
