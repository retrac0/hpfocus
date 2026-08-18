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

The most recent step closed the system group the kernel needs and the earlier
corpora barely exercised: `LPP`/`LPPS` and `LIA`/`LIAS` (program and
intermediate pointers), `PSLA` (the test-and-set semaphore), `CPAA`, `CRB` and
`MUC` (segment use counts). Everything still named in the remaining gap is
either the string group or an opcode Appendix B does not assign — the latter
being data that a linear scan misreads as code, since a run of `0xff` decodes
as format 6 opcode 37.

Digital archaeology results: very little survives for this machine!  There's
a few disk images preserved by hpmuseum.net but that's it.  This repository
includes copies of: 
 * BASIC Language System 2.0
 * 9020 factory diagnostics,
 * BASIC demo suite (all undamaged), 
 * an HP-UX 5.01 system tape (preliminary analysis suggests this tape image is
   truncated) and most of the length is lost but there does seem to be a complete
   kernel!

## Layout

```
docs/
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
3. **Memory Controller ERS is missing** from bitsavers. The Instruction Set ERS
   defers MPB slave-register semantics to it.

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
