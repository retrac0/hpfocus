# hpfocus

## Introduction

This is an ongoing quest to attempt to emulate the HP 9000 Series Model 520
with the HP FOCUS microprocessor.

FOCUS, introduced in 1982, was the first commercial 32-bit microprocessor --
a microcoded, segmented, stack-oriented machine with no programmer-visible 
general-purpose registers, implementing something similar to the HP 3000
architecture widened to 32 bits.

The goal is to run HP-UX and BASIC.

## Current state

### Emulator

There's a sample factorial program in section 7 of the Instruction Set ERS
which runs correctly on the emulated CPU for n = 0 to 12 which means
procedure calls, stack markers, twelve-deep recursion, doubleword multiply
are working correctly.

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

That kernel is now an 5.30 image recovered from the 7914
unit-save tapes, along with its userland -- see [software/README.md](software/README.md). 
It replaces the truncated 5.01 tape as the target, and it raises the 
stretch goal from "bring up a kernel" to boot this system.

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

**BASIC runs HP's own programs, from an installed fixed disc.** Build the
disc with `tools/mkharddisc.py` and `tools/installbasic.py`, boot it with
`tools/runbasic-hd.sh`, and BASIC reaches its banner, mounts the disc as
`:INTERNAL`, catalogues it, and **loads and runs a stored program** — HP's
22 KB `CAT_BOOT` utility prints its banner and prompts for a device.

Two defects had to go first, and they are worth naming because neither was
where the earlier notes had been looking:

- **The fixed disc had no file system.** `mkharddisc.py` wrote a volume
  header over a zeroed file area: no File Attributes file, no root
  directory, no free list. BASIC mounts the device it booted from as its
  system volume, so the boot ended in `ERROR 455` — and with the `IO` and
  `MS` options installed it died before the banner, on a trap 012 in the
  kernel as the first task was dispatched. `tools/sdfwrite.py` now builds a
  real volume, and `installbasic.py --copy` can put files in it.
- **Stack Overflow (cause 024) was never raised.** The ERS is explicit that
  pushing past `SL` traps, that every stack keeps a 64-word reserve above
  `SL` so the condition can be handled at the *end* of an instruction, and
  that the handler runs on the ICS. The core checked `SL` in three string
  instructions and nowhere else, so BASIC's stack silently outgrew its
  segment and the first instruction that does test against `SL` — `TRY`
  deleting its frame — reported a bounds violation tens of thousands of
  instructions later. That is what stopped `RUN`, and it is what the
  "one TRY frame consumed per statement" note was really seeing. BASIC's
  own cause-024 handler grows the stack and returns through `IXIT`.

**HP's own factory diagnostics boot, load and run.** The 9020 System
Functional Tests come with their own environment -- `500ENV1-3` are a
three-volume boot image, not a filesystem, and they carry a `GRAPHICS`
binary that appears on no option disc -- so `tools/mkdiagdisc.sh` installs
that instead of BASIC 2.0, and the 31,652-byte `TEST` driver then loads off
the test disc and runs. It stops in its first subprogram with `ERROR 702,
No graphics driver/CRT graphics hw failure`, which is HP's program working
correctly on a machine that has no graphics display modelled (HARDWARE.md
2.7). Five emulator defects had to go first, none of which anything else in
the project exercised -- a loader stack that sat inside any boot image over
half a megabyte, `PSHN` running past `SL` instead of trapping at it, the
variable forms of `ADDS`/`SUBS` being silent no-ops, a byte lost in
`PSHR`/`SETR`'s S form, and `IXIT` mistaking the bottom of the interrupt
stack for an empty one. See [docs/DIAGNOSTICS.md](docs/DIAGNOSTICS.md).

**What still fails: a program typed at the keyboard.** `10 DISP 5` /
`20 END` / `RUN` traps 021, STT Violation, in the program scaffold. A
statement typed *without* a line number is a useful control: `DISP 5` and
EXECUTE prints ` 5`, so compiling and running one statement works — it is
specifically the stored-program scaffold that fails. (Note which key ends
the line: in `FOCUS_TYPE`, `\n` is EXECUTE and `\r` STORE, and a numbered
line needs `\r` or BASIC answers `IMPROPER COMMAND`.)

Every trap used to return past the instruction that caused it. The ERS
splits its causes into two groups (sheet 52, notes 1 and 2): note 1 is
detected "at the end of an instruction and the offset in the stack marker
points to the next instruction", note 2 "during an instruction ... points
to this instruction", and only the debug and arithmetic traps are note 1.
`take_trap` used `m_next` for all of them, so no note-2 trap was ever
resumable. That is fixed; it is a prerequisite for the scaffold rather than
a cure for it, and the trap is still fatal.

What the scaffold path actually does, read off a run:

- The boot builds **two** scaffold code segments from one ten-word image on
  the stack. Segment 3076 gets all ten words — nine STT entries plus the
  length word `0x24` — by a 40-byte `MVB`; segment 3077 gets only the top
  seven (six entries, length `0x18`) by a 28-byte one from a source twelve
  bytes higher. Both counts are literal immediates in BASIC's own code.
- At `RUN` the compiler *composes* the 40-byte scaffold into a buffer at
  `DB+4564` — it is not stored in the boot image — emitting `LDIS #9;
  PCLS STT[9]` twice, then copies it into segment **3077** and sets that
  segment's `STT[1]` to `0x80`. Six entries, and the code calls the ninth.
- The trap is not a linking mechanism here: BASIC's CS1 sends causes
  021, 022 and 023 to one common internal-error reporter (the vector stub
  pushes the cause and branches to the routine the panic printer sits in),
  so it halts.
- Segment 3077 is 208 bytes and its code sits at `0x7c`, which leaves room
  for a nine-entry table with four bytes to spare — it is sized for nine.
  Segment 3076's entries 7/8/9 are `80340006`, `802f4006`, `80360006`
  (segment 6, entries 208/189/216).

Stamping those three into 3077 and raising its length word to `0x24` does
get the call through, and the failure moves on to a trap 030 two
instructions later — the same place the earlier attempt with `MAIN`'s
table reached, now from a second, independent source. So slots 7-9 have to
be linked *for this program*, and the open question is which of the two
segments the composed scaffold belongs in: the destination comes from field
`+60` of the object at `DB+384`.

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

### Recovered software

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


### Current holds

Both HP-UX versions then stop on the same thing: format 4 opcodes
020 and 021, which the Dec-82 Appendix B leaves unassigned — and so,
it turns out, does HP's own toolchain. 5.01 uses
them 1,192 times and 5.30 1,514 times; BASIC and the diagnostics never
execute either. 5.01 reaches its first `021` after 1.26M instructions, in
the `CORE SYSTEM` segment.

What the recovered toolchain says: The 5.30 tapes carry HP's own
assembler (`/bin/as`) and Pascal compiler (`/usr/lib/pascomp`), and the
compiler holds the instruction set as a table of 16-byte records — mnemonic,
opcode bits in place, operand class, flags. [`tools/hptable.py`](tools/hptable.py)
reads it. Two results:

- It is the strongest check yet on the opcode map: **208 assignments agree,
  none disagree**, and it settles two glyphs the chart had damaged —
  `SRSI` is really `BRSI`, `RPC` is really `RFC` (both now fixed in
  `docs/opcodes.csv`).
- It leaves the same fourteen format 4 slots empty as the 1982
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

## Layout

```
docs/
  FOCUS.md             CPU architecture + full opcode spec  <- start here
  HARDWARE.md          520/540 machine hardware: MPB, I/O, boot media
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
  sdfread.py           read an SDF volume; sdfwrite.py builds one
  mkharddisc.py        make a fixed disc, installbasic.py installs BASIC
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
[docs/FOCUS.md](docs/FOCUS.md)

## The opcode map

[docs/opcodes.csv](docs/opcodes.csv) is a hand transcription of Appendix B,
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
is 25 conflicts, all of them OCR damage in the prose (`S8IT` for `SBIT`,
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
   diagnostics are undamaged, so this is not on the critical path.
3. **Memory Controller ERS is missing** from bitsavers. Less of a hole than
   the references to it imply: the software-visible MPB semantics — slave
   registers, message bits, semaphores, the interrupt lists, all 46 MPB
   commands — are in the I/O ERS and the Instruction Set ERS. What the MC ERS
   withholds is bus-protocol detail and the memory controller's own channel-0
   command set (HARDWARE.md §4).

## Boot order

1. 9020 diagnostics — best CPU workout, fewest dependencies
2. **BASIC 2.0** — first realistic interactive milestone, undamaged media
3. HP-UX kernel bring-up — complete kernel, but stops at `init`
4. BASIC demos — graphics and I/O
5. Full HP-UX — blocked on a better tape

See [docs/SOFTWARE.md](docs/SOFTWARE.md) for detail.

## `RUN` of a typed program: the short table, confirmed by experiment

`10 DISP 5` / `20 END` / `RUN` halts with `TRAP:00000021`. The causes are
numbered in octal, so that is **`021` = `stt_violation`**, and
`FOCUS_TRAPLOG` gives its parameter as **`0x24`** — the code executed
`PCLS STT[9]`, offset 36.

It faults at `PB=0x8233c`, a **bare composed segment**: length `0xd0`,
and no module name in its header, unlike the two `MAIN` copies. Its
`PL` is `0x8240b`, and the STT length word there reads **`0x18`** —
six entries against the nine the code asks for. Three short.

That word is written exactly **once**, at t=1.075, by BASIC `+0x668c`:

    LDS   Q+12,I,X      X = 4
    SUBI  #28
    CPTR,B
    LDIS  #28
    MVB   7

The `7` is not a word count — in `MVB` the operand is flags: `operand&3`
is how many words to pop (3) and bit 2 says the source is a data segment
pointer. The size is the `28` pushed above it. (The older note reading
this as "seven words" is wrong.)

**The decisive experiment.** `FOCUS_POKE=82408:24:14` forces the limit to
nine entries before `RUN`. The STT violation disappears — and the machine
halts instead with **`TRAP:00000020` = `unimplemented_instruction`**. So
the bound is not the problem by itself: entries 7, 8 and 9 are simply not
there, and `PCLS` through them calls into whatever follows. The scaffold
really is composed into the short segment, and no adjustment to the bound
can fix that.

The builder's own frame holds both sizes at the moment it runs — `0x24`
below `Q` and `0x18` above it — and `LDIS #24; STS Q+52` at builder
`+0x1e` plants the small one unconditionally. Which of those the
destination should be chosen from is the open question; the notes point
at field `+60` of the object at `DB+384`.

**Not yet fixed.** A keyboard-entered program still does not run.

## The scaffold machinery, mapped

Watching the containers, their tables and the user DST turns the two
BASIC failures into one mechanism. Two things previously written here
were wrong and are corrected below.

- **User code segments are DST entries.** A CSN of 3072+n is user DST
  entry n; the CSN in the RUN trap's STATUS is `0xc05` = 3077 = user
  entry **5**. (The DST register itself is top of memory − 0x2b00, so
  it moves with the fitted RAM: `0x0fd500` on the 1 MB default,
  `0x1fd500` on a 2 MB machine. Watching a stale DST address is a good
  way to measure nothing at all.)
- **CORRECTION — "A and B" are one segment, not two copies.** They are
  user entry 4 before and after a **growth**:

  | | length field | base |
  |---|---|---|
  | before (A) | `0x18f` → 0x190 bytes | `0x8219c` |
  | after (B) | `0x1ef` → 0x1f0 bytes | `0x7655c` |

  Same DST entry, same module name, 86 of 120 words identical — the
  grow could not extend in place, so it relocated. Everything the notes
  said about "two scaffold segments, one three entries short" is this:
  the short table is the segment's state *before* it grows.
- **CORRECTION — trap 021 is not the activation trigger.** An earlier
  version of this section claimed the kernel had a scaffold-activation
  arm ahead of the internal-error reporter, driven by the fault.
  `FOCUS_TRAPLOG` from t=15 over the immediate flow shows exactly **one
  trap in the whole session** — trap 1 at 19.568, the TRY bounds
  violation — and none at 17.118 when the table is stamped. The
  stamping is proactive, not fault-driven. The lesson is the same one
  as the semaphore and the CLC: a plausible mechanism, believed because
  the timing fitted, and disproved by the log that counts.
- **What actually stamps the table: a conditional segment extend in
  PARSER/RTS.**

      +0x5c0e   LDS Q-16,I ; LDS Q-20 ; CMP,B ; BCCS.EL   skip if
                                                  needed <= current
      +0x5c1c   PCL STT[306]  ->  kernel +0x8734           grow it

  The current size it compares against is `0x190` — the segment's
  pre-growth length — and the grow is what restamps the STT with its
  **nine** entries (length `0x24`, entries `0x86`, `0x72`, external
  labels into seg 6). So the nine-entry table is not something the
  scaffold is born with; it is earned by growing.
- **The RUN flow never enters that routine.** Zero hits at the guard
  (+0x5c0e), at the call (+0x5c1c), and at three successive callers up
  the chain — COMPILER +0x2eb0, COMPILER +0x602, BASIC +0x5128 —
  against one hit each in the immediate flow. RUN never asks for a
  scaffold extend. The kernel's grow entry is reached twice in the RUN
  flow (from TRAP HANDLERS +0x758, before the trap, so the machinery is
  alive) but never for the scaffold's table.
- So the composed program is planted in user segment **5** — a
  different, 0xd0-byte container carrying the six-entry table stamped
  at boot — and nothing ever grows *it*. `PCLS STT[4]` succeeds against
  the stale table, `PCLS STT[9]` exceeds its limit, trap 021.
  Immediate mode works because segment 4 gets grown on its first
  statement and gains nine entries; segment 5 never does.

- **The two flows place code through different routines**, measured
  0 against 1 hits each way: COMPILER +0x966c (the copier that plants
  the composed program) runs only under `RUN`, and PARSER +0x5bf8 /
  +0x5cac (the sizing and grow) only in immediate mode.
- **`RUN` does size-check — and the check passes.** COMPILER +0x95d2
  computes `[DB+488]*4 + 125 + [Q+12]` = `0`·4 + 125 + 16 = **141** and
  calls `STT[195]` with it. Container 5 is `0xd0` = 208 bytes, so the
  request fits, nothing grows, and the table is never reconsidered.
- **One instruction stream writes a complete table, and only one.**
  PARSER +0x5c32 writes the length word `0x24` and entries 1..9 in a
  single descending byte burst at t=17.118 — the whole table at once,
  as an image copy. Nothing else in either flow produces more than a
  single entry: at `RUN` time exactly **one** entry (2, value `0x80`,
  from BASIC +0x516a) is written into container 5, and entries 3-9 are
  boot residue.
- **Killed hypothesis: an empty table does not trigger activation.**
  Poking container 5's table length to 0 before `RUN` does not summon
  any builder; the trap simply moves earlier, parameter `0x10` —
  `PCLS STT[4]` — instead of `0x24`. There is no fault-driven table
  producer in any direction, which is the second and final nail in the
  "trap 021 activates the scaffold" idea.

So the defect is exact: `RUN` plants a scaffold body that references
`STT[9]` — an index that comes from the template, not from compiling
`DISP 5` — into a container that never receives the block stamp,
because the stamp happens only inside the PARSER grow, and RUN's size
request fits the container it was given.

### The chain, end to end

Following the block stamp back reaches a hard-coded constant, and the
picture changes once more.

- **The grow does not create the nine-entry table — it preserves one.**
  Segment 4 *pre*-grow already has `0x24` at its PL. Both tables are
  stamped at boot, ~1 ms apart, by two block moves in one BASIC
  routine, from an image the routine builds in its own locals:

  | site | `LDIS` | bytes | target |
  |---|---|---|---|
  | BASIC +0x668c | `#24` (`0x18`) | 28 = 7 words | container 5 |
  | BASIC +0x66d4 | `#36` (`0x24`) | 40 = 10 words | segment 4 |

  Both sizes are **constants in BASIC's own code**. Container 5 is
  *designed* to hold six entries. (This is the "ten words versus the
  top seven" in the oldest notes, finally located: not two copies of
  one scaffold, but two containers deliberately built to different
  sizes.)
- **The composition destination is user segment 5, and it is chosen
  deliberately.** The `MVB` operands read directly: source
  `0x800011d4` = DB+0x11d4 = the compose buffer at DB+4564; target
  `0x4028007c` = **user segment 5, offset 124**; count `0x28` = 40
  bytes. The segment comes from word 0 of the segment-4 header object
  (`0x40280000`, a segment-5 pointer) and the offset from field `+60`
  (`0x7c`), which advances as composition proceeds. The two headers
  name each other — segment 5's word 0 is `0x40200000`, a segment-4
  pointer — so the pair is deliberate. The long-standing note "the
  destination comes from field +60" is half right: `+60` is the
  offset; word 0 is the segment.
- **`STT[9]` is not computed — it is canned.** COMPILER +0x209a
  assembles the compose buffer by `MVB`-ing a template out of the
  compiler's **own code segment** (`LRAS P+14,X`, with X = `[Q-16]*2`
  = `0x12a` and the length from halfword tables at P+580/P+586). The
  template literally reads `LDIS #9; PCLS STT[9]` — twice — and the
  bytes land in the buffer verbatim.
- **And the constant is calibrated to nine entries exactly.** The STT
  check is `offset > limit`, so `PCLS STT[9]` (offset 36) passes
  against segment 4's limit of 36 as *the last valid entry*, and traps
  against container 5's limit of 24.

So the whole failure is one mismatch: a template hard-wired for a
nine-entry segment, composed into a container BASIC deliberately built
with six.

**NEXT.** Only two things can be wrong, and they are now sharply
separable. Either the boot stamps the two containers the wrong way
round — the 40-byte image belongs on segment 5, the code destination —
or the destination pairing is inverted and composition should target
segment 4. Reading which object each stamp takes as its target
(`[Q+12]` indexed for the 28-byte move, `[Q-16]` indexed for the
40-byte one) against what the RUN pairing expects should settle it in
one measurement.

## `RUN` of a typed program: the short table, confirmed by experiment

`10 DISP 5` / `20 END` / `RUN` halts with `TRAP:00000021`. The causes are
numbered in octal, so that is **`021` = `stt_violation`**, and
`FOCUS_TRAPLOG` gives its parameter as **`0x24`** — the code executed
`PCLS STT[9]`, offset 36.

It faults at `PB=0x8233c`, a **bare composed segment**: length `0xd0`,
and no module name in its header, unlike the two `MAIN` copies. Its
`PL` is `0x8240b`, and the STT length word there reads **`0x18`** —
six entries against the nine the code asks for. Three short.

That word is written exactly **once**, at t=1.075, by BASIC `+0x668c`:

    LDS   Q+12,I,X      X = 4
    SUBI  #28
    CPTR,B
    LDIS  #28
    MVB   7

The `7` is not a word count — in `MVB` the operand is flags: `operand&3`
is how many words to pop (3) and bit 2 says the source is a data segment
pointer. The size is the `28` pushed above it. (The older note reading
this as "seven words" is wrong.)

**The decisive experiment.** `FOCUS_POKE=82408:24:14` forces the limit to
nine entries before `RUN`. The STT violation disappears — and the machine
halts instead with **`TRAP:00000020` = `unimplemented_instruction`**. So
the bound is not the problem by itself: entries 7, 8 and 9 are simply not
there, and `PCLS` through them calls into whatever follows. The scaffold
really is composed into the short segment, and no adjustment to the bound
can fix that.

The builder's own frame holds both sizes at the moment it runs — `0x24`
below `Q` and `0x18` above it — and `LDIS #24; STS Q+52` at builder
`+0x1e` plants the small one unconditionally. Which of those the
destination should be chosen from is the open question; the notes point
at field `+60` of the object at `DB+384`.

**Not yet fixed.** A keyboard-entered program still does not run.

## The scaffold machinery, mapped (2026-08-23)

A day of watching the containers, their tables and the user DST turns
the two BASIC failures from mysteries into one mechanism with one
poisoned state. The pieces:

- **User code segments are DST entries.** A CSN of 3072+n (0xc00+n) is
  user DST entry n; the CSN in the RUN trap's STATUS is **0xc05 = 3077**
  = user entry 5. The scaffold world at the crashes:
  entry 4 = `0x7655c` (B, len 0x1f0) in immediate mode but `0x8219c`
  (A, len 0x190) at RUN; entry 5 = `0x8233c` (len 0xd0) throughout.
  B is the immediate-mode **all-in-one** (prologue + statement area);
  A + entry 5 are the RUN-mode **split pair** (prologue + composed
  code). A and container 5 are also the **startup** context's pair —
  A's prologue ran at t=1.076, and container 5's six-entry table was
  stamped at t=1.075.
- **Scaffold tables are stamped lazily, and trap 021 is the trigger.**
  Measured in the immediate flow: B's table is all zeros until the
  first statement's `PCLS` faults into it; the kernel (SUN OS KERNEL
  0x48b7c..0x49144) takes the fault, briefly marks the DST entry
  absent (`w0=0x00070011`), and PARSER/RTS +0x5c32 stamps the full
  **nine-entry** table (length 0x24, entries 0x86, 0x72, external
  labels into seg 6...) at the container's PL; the statement then
  runs. The old note "CS1 routes 021/022/023 to one common
  internal-error reporter, so it always halts" is **wrong** — 021 has
  a scaffold-activation arm ahead of the reporter.
- **The RUN composition, instruction by instruction** (COMPILER
  +0x9654): copy `[DB+468]*2` bytes from the compose buffer at
  `DB+4564` to the cursor held in the descriptor's word 0 — the
  descriptor being `resolve(DB+384)` = A, whose `+0` holds a user-seg-5
  pointer advancing as composition proceeds. The composed code calls
  `PCLS STT[4]` and `PCLS STT[9]`.
- **The poisoned state.** Container 5 still carries its **stale
  six-entry table from boot** (stamped by BASIC +0x668c at t=1.075 for
  the startup context; 28-byte MVB — the operand `7` is flags, not a
  size; the size is the pushed 28). The composed program's
  `PCLS STT[4]` **succeeds against the wrong table** (startup's entry
  4 = seg6+0x22c, where the nine-entry table has a self-label into the
  container), and `PCLS STT[9]` exceeds the six-entry limit → trap 021
  — which the kernel this time routes to the **panic reporter**, not
  the activation arm. The plausible discriminator: a stale-but-nonzero
  table reads as "already activated", so the fault looks genuine.
- So both failures are the same shape: **a container is entered with
  the wrong table state**. RUN reuses container 5 without resetting
  its boot table (an empty table would have triggered the measured
  activation path and stamped the nine entries). Immediate mode enters
  B at +0x130 with A's session TRY frame and no per-statement
  re-create — the same "wrong container state at entry" family.

**NEXT.** Two threads, in order of sharpness: (1) find the reset that
should clear container 5's table when the RUN context claims it —
watch the DST entry-5 rewrite path (`0x48a5a/0x48a88` at boot) for a
RUN-time equivalent that skips the table, and the kernel's 021 arm for
the exact discriminator it applies before choosing panic over
activation; (2) check whether the boot-time stamp itself is the
divergence — whether on real hardware the startup context uses a
different container and 3077 stays virgin for RUN.

## Interactive BASIC: why only the first statement runs

`DISP 5` alone prints ` 5`. `DISP 5` then `DISP 6` halts with
`TRAP:00000001`. Measured, the cause is a split scaffold, and it is the
same defect that stops `RUN`.

Both statements execute an instruction-for-instruction identical `TRY`
sequence (`FOCUS_TRYLOG`); the only difference is the resting chain head
at `(SB+2)`:

| | statement 1 | statement 2 |
|---|---|---|
| head at MAIN's delete | `0xbf` — a real frame | `1` — the empty sentinel |
| outcome | unwinds, prints ` 5` | bounds violation, trap 1 |

Following that frame's whole life explains it. It is created **once per
session**, at `p=0x821fe`, and destroyed **once per statement**, at
`p=0x766be` — and those two PCs are in *different segments*
(`PB=0x8219c` and `PB=0x7655c`) whose module headers both read `MAIN`.
So the scaffold's entry half and its exit half live in two different
copies: statement 1 consumes the session's only frame, and statement 2
has nothing left to unwind.

The `TRY` delete itself is not the bug. ERS `F4:40` with `b`=0 unwinds
the newest frame bounds-checked against `Q < frame < S`, which is what
the emulator does; with the head reading `1` there is no frame to find
and a trap is correct. The missing piece is the per-statement create.

Dumping both copies settles what "two scaffold segments" means in
practice. They are near-identical — 86 of 120 words the same, the same
module name, and the same `TRY,B` create at `+0x62` — and they differ
in exactly the way that matters:

| | A = `0x8219c` | B = `0x7655c` |
|---|---|---|
| length at `+0x04` | `0x190` | `0x1f0` |
| `TRY,B` create at `+0x62` | present, **runs once per session** | present, **never entered** |
| body at `+0x130` | **all NOPs** | the compiled statement, ending `TRY` (delete) at `+0x162` |

`FOCUS_WATCH` shows BASIC entering B at **`+0x130`** — straight into the
body, past B's own prologue. So the live prologue is A's and the live
body is B's: the two halves of one scaffold, executing out of two
different copies. A's create fires once at session start, B's delete
fires once per statement, and the second statement finds nothing to
unwind.

Neither `MAIN` appears in the system CST at `0x480` even with all 300
entries dumped, which fits: the composed program scaffold lives in the
**user** segment table (the 3076/3077 pair), not the system one. That
is the same two-segment split already known to break `RUN`, so one fix
should recover both. The open question is unchanged — which segment the
composed scaffold belongs in; the destination comes from field `+60` of
the object at `DB+384`.

`FOCUS_CSTMAX` raises the CST dump's cap; it was a silent 128 against
BASIC's 300 entries, and both scaffold segments sit above the cut. The
dump now reports how many live entries it dropped.

## Sources

- [bitsavers.org — HP 9000/500](http://www.bitsavers.org/pdf/hp/9000_500/)
- [HP Computer Museum — 500 Series](https://www.hpmuseum.net/exhibit.php?class=1&cat=12)
- [OpenPA — HP FOCUS Processor](https://www.openpa.net/focus_processor_hp.html)
- [The HPDir Project](https://www.hp9845.net/9845/projects/hpdir/)
