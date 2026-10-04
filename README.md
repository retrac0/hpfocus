# hpfocus

An emulator of the **HP 9000 Series 500**, the machines built on HP's FOCUS
processor, written as a [MAME](https://www.mamedev.org/) driver. FOCUS
(1982) was the first commercial 32-bit microprocessor: a microcoded,
segmented, stack-oriented machine with no programmer-visible
general-purpose registers. Its architecture is the HP 3000's widened to
32 bits. No other emulation of it exists.

**Status: first alpha.** HP-UX 5.30 boots on the emulated 9000/540 and
runs multiuser, with a root login on the console and on all six ports of an
emulated HP 27140A multiplexer. It is an alpha: what is known to be wrong
is in [KNOWN-ISSUES.md](KNOWN-ISSUES.md), and what it was tested with is in
[docs/VALIDATION.md](docs/VALIDATION.md#8-the-hp-ux-release-tests).

## Quick start (Linux)

    git clone https://github.com/retrac0/hpfocus.git && cd hpfocus
    tools/setup.sh                    # check, fetch MAME, build (long), fetch HP's software, prepare the discs
    tools/smoketest.sh                # one-minute check: PASS or FAIL
    tools/runhpux.sh --multiuser      # HP-UX 5.30; log in as root, no password

To stop, type `cd /` and `/etc/stopsys` at a root shell, wait for `The
system is stopped.`, and close the window.

[INSTALL.md](INSTALL.md) covers the rest: prerequisites for Debian/Ubuntu,
Fedora, Arch and macOS, logging in, more terminals (`tools/hpuxterm.py 1`
.. `6`), stopping cleanly, and what persists between runs and how to
reset it. The first build compiles MAME's core: it took 1 hour 9 minutes
at `-j4` on a busy 8-core machine (expect 20 to 40 minutes on an idle
one), and the finished install is 2.7 GB.

**HP's software and manuals are not in this repository.** They are
Hewlett-Packard's, and third-party archives host them:
[MEDIA.txt](MEDIA.txt) lists each file with its source and SHA-256.
`tools/setup.sh` downloads the software the emulator boots (46 MB, from
bitsavers.org) and checks it; `tools/fetch-media.sh --manuals` fetches the
manuals the documentation cites (230 MB) and extracts their text. The
520's software (the BASIC, demo and diagnostic floppies and the HP-UX 5.01
tape) is at the HP Computer Museum, whose downloads sit behind a CAPTCHA:
you download four items in a browser and `tools/fetch-media.sh --from
<folder or .zip>` puts them in place ([INSTALL.md](INSTALL.md) section 8).
HP-UX 5.30 on the 540 needs none of that.

## What works

- **HP-UX 5.30 on the 9000/540**, from a unit save of a real 1991
  installation. It boots single-user or multiuser, runs `/etc/rc`, and
  takes logins on the console (a window, a TCP port or a pty) and on six
  terminal ports you can leave and come back to. It compiles and runs C
  programs, and what it writes is kept on a working copy of the disc.
  The release tests are a 30-case matrix of boots and sessions (one of
  them four logins with twelve background jobs in 2 MB), seven users
  logging in at once at 30 different timings, and a
  compile-copy-sort-checksum workload on three terminals that is checked
  again after a reboot. What they are, and how this release scored, is in
  [docs/VALIDATION.md](docs/VALIDATION.md#8-the-hp-ux-release-tests).
- **BASIC 2.0 on the 9000/520**, on its own rendered display with a live
  keyboard. It boots from HP's floppies or from an installed fixed
  disc, and runs typed and stored programs (`tools/runbasic.sh`,
  `tools/runbasic-hd.sh`; `src/test/basictest.sh` checks 25 cases).
- **HP's 9020 factory diagnostics** boot and run: `LOAD "TEST"` and `RUN`
  on the diagnostics disc bring up HP's Test Supervisor, which the release
  tests check at four typing times. The power-up self test of the display
  passes, the graphics controller draws and the thermal printer prints
  (`software/hd/diag.img`, which `tools/setup.sh` builds;
  docs/DIAGNOSTICS.md).
- **HP-UX 5.01** boots on the 520 from the damaged release tape, as far as
  its copyright banner (`tools/runhpux.sh --release 5.01 --headless`).

## What does not work, yet

- **Closing the window is a power cut to HP-UX.** Stop HP-UX first, with
  `/etc/stopsys`. Without it your synced files are still kept, but each
  power cut leaves up to 0.9 MB of swap space marked in use on the disc,
  and nothing checks the file system at boot. `tools/runhpux.sh --reset`
  starts again from the pristine disc.
- **The default 2 MB is small.** HP-UX swaps hard under load, and a
  program that filled a 700 KB array never finished there: run with
  `--ram 4` for anything large.
- **Root has no password, and the terminal ports are open** to every user
  of your computer (they listen on 127.0.0.1 only). The disc is a real
  site's 1991 system, with its accounts and mail.
- **No networking**, and the terminals are generic, not HP 262x: screen
  programs such as `vi` need a `TERM` your terminal matches. No Break key,
  and ^S/^Q do not pause output.
- **HP-UX 5.01 has no userland.** The tape lost it, so it stops at its
  banner.
- **Floating-point traps and rounding modes** are not modelled, and a few
  overflow and condition-code details are wrong. Two instructions known
  only from HP's assembler (`CTCB`, `GMPY`) are not implemented. Of the
  200 instructions HP-UX uses, 81 have no unit test yet: expect more
  instruction-level defects.
- **The 520** is still flagged "not working" in MAME, so its window opens
  on a warning screen: tap a key to dismiss it, because a key still held
  when the machine powers on stops the boot. In its window BASIC wants
  capitals (press Caps Lock), and Return is BASIC's ENTER key, not
  EXECUTE, which is Ctrl-J. Most of HP's BASIC demo programs do not run
  yet, and what BASIC writes to a flexible disc is not saved.
- **No save states, and MAME's debugger is untried.**
- **No loader ROM.** No dump of the SCM loader ROM exists, so the emulator
  performs the loader's job itself.
- **Linux only so far, and only Arch has been tried.** The scripts are
  written for macOS too, but nothing has been built or run there. The
  window has been driven with synthetic keys under a virtual display, not
  yet by a person on a
  real desktop.

The details, and what to do about each, are in
[KNOWN-ISSUES.md](KNOWN-ISSUES.md).

## Where things are

| | |
|---|---|
| [INSTALL.md](INSTALL.md) | build, run, terminals, state, troubleshooting |
| [KNOWN-ISSUES.md](KNOWN-ISSUES.md) | what the alpha gets wrong |
| [docs/FOCUS.md](docs/FOCUS.md) | the CPU: architecture and full opcode specification |
| [docs/HARDWARE.md](docs/HARDWARE.md) | the 520/540 hardware: memory, I/O, cards, boot media |
| [docs/HPUX.md](docs/HPUX.md) | HP-UX on the emulator: kernel map, debugging tools, how each milestone was reached |
| [docs/SOFTWARE.md](docs/SOFTWARE.md), [software/README.md](software/README.md) | the software images, where they came from, what is on them |
| [docs/VALIDATION.md](docs/VALIDATION.md), [src/test/](src/test/) | the evidence: unit and differential tests (`make -C src/test test`), the BASIC suite (`src/test/basictest.sh`), the HP-UX and diagnostics matrix, stress and soak (`src/test/hpuxtest.py`) |
| `src/cpu/focus/`, `src/iop/`, `src/mame/` | the CPU core, the I/O processor and cards, the hp9020/hp9040 driver |
| `tools/` | run scripts, disc and media tools, disassembler (`tools/focusdis.py`) |

## Licence

| what | licence |
|---|---|
| the emulator: `src/cpu/focus/`, `src/iop/`, `src/mame/hp9000_5xx.cpp` and the C++ in `src/test/` | GNU GPL, version 2 or any later version ([COPYING](COPYING)). The file headers say `license:GPL-2.0+`, which is how MAME spells SPDX `GPL-2.0-or-later`. |
| everything else that is the project's: `tools/`, the scripts and `Makefile` in `src/test/`, `docs/opcodes.csv`, and the documentation (this file, `INSTALL.md`, `KNOWN-ISSUES.md`, `MEDIA.txt`, the `.md` files under `docs/` and `software/`) | MIT ([LICENSE-MIT](LICENSE-MIT)) |
| `src/iop/focusfont.h` and `tools/mame/focus-mame.patch` | BSD-3-Clause ([LICENSE-BSD-3-Clause](LICENSE-BSD-3-Clause)). They are MAME's: its terminal font, and an edit to two of its files. |

Copyright (c) 2026 Joel Heikkila, the two MAME files apart. Each source
file names its own licence on its first lines, and that line is what
counts where this table and a file disagree.

MAME is not part of this repository. `tools/setup.sh` fetches it and the
emulator is built into it. MAME's own terms (`mame/COPYING` and
`mame/README.md`: the GPL, version 2 or later, for the program as a whole)
are what a built `mame/focus` is distributed under.

Hewlett-Packard's software and manuals are HP's and nothing here licenses
them. The repository does not carry them: [MEDIA.txt](MEDIA.txt) points at
where they are published, and [software/README.md](software/README.md)
says where each image came from. The same holds for what is quoted or
transcribed from HP's documents, which is the six files in `docs/notes/`
and the passages cited in the documentation.

---

# Research log

Everything below is the project's working log, kept as it was written.
Sections are dated, and where one says "the current frontier", read it as
the frontier at the time. Recipes in it have been brought up to date only
where they would otherwise fail, and are marked where they were.

## Introduction

This is an ongoing quest to attempt to emulate the HP 9000 Series Model 520
with the HP FOCUS microprocessor.

FOCUS, introduced in 1982, was the first commercial 32-bit microprocessor --
a microcoded, segmented, stack-oriented machine with no programmer-visible 
general-purpose registers, implementing something similar to the HP 3000
architecture widened to 32 bits.

The goal is to run HP-UX and BASIC.

## Current state

**HP-UX 5.30 runs multiuser** on the emulated 9000/540: `tools/runhpux.sh
--multiuser` boots to run level 2 with a getty on the console and on all
six ports of an emulated HP 27140A multiplexer, and a root login typed into
any port reaches a shell (`docs/HPUX.md` section 10; the card is
`docs/HARDWARE.md` 2.6.6). Without the flag the disc boots as imaged, to
init's single-user shell (section 9). BASIC 2.0 and the 9020 diagnostics
environment boot on the 520 to "BASIC Ready for use." on a rendered screen
with a live command line. The console-open work, the drain, the shell and
the multiuser step each cost emulator defects, recorded in `docs/HPUX.md`
sections 6, 8, 9 and 10. **HP-UX 5.01, off the release tape, reaches its
copyright banner on the emulated 520** — `tools/runhpux.sh --release 5.01`,
section 11.

Much of what follows is a running log and is dated; where it says "the
current frontier", read it as the frontier at the time it was written.

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

The string-descriptor instructions (`SLD`, `SVAL`, `SCON`, `SAS`) went the
same way and are covered by five `exectest` cases; the note that used to
name them here was stale. What is left in the gap is **`BTD` and `DTB`**,
the binary-to-decimal pair on ERS sheets 84–85 — and they are not academic:
`DISP 8/2` computes 4.0 correctly and then traps 020 converting it for
display, so they sit directly on the path that shows a computed number.
`DLAD` and `DLSB`, the rest of the decimal group, are implemented.
Everything else left is an opcode Appendix B does not assign — data that a
linear scan misreads as code, since a run of `0xff` decodes as format 6
opcode 37.

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
the test disc and runs, up to the Test Supervisor's menu. It used to stop in
its first subprogram with `ERROR 702, No graphics driver/CRT graphics hw
failure`; **the graphics display is modelled now** and the display draws --
`PLOTTER IS 1,"INTERNAL"` then `FRAME` puts a rectangle round the raster.
Its command set is undocumented everywhere, and was read off the machine's
own power-up self test, `PWP SELF TEST`, which turned out to be the smallest
segment in the boot image (HARDWARE.md 2.7.4, DIAGNOSTICS.md 3). Five emulator defects had to go first, none of which anything else in
the project exercised -- a loader stack that sat inside any boot image over
half a megabyte, `PSHN` running past `SL` instead of trapping at it, the
variable forms of `ADDS`/`SUBS` being silent no-ops, a byte lost in
`PSHR`/`SETR`'s S form, and `IXIT` mistaking the bottom of the interrupt
stack for an empty one. See [docs/DIAGNOSTICS.md](docs/DIAGNOSTICS.md).

**A program typed at the keyboard runs.** `10 A=3` / `20 B=4` /
`30 DISP A*B+2` / `40 END` / `RUN` prints ` 14`, and immediate-mode
arithmetic, `LET`, `NEW` and consecutive statements all work.

Getting there took one fix, not the five the notes below had expected.
`LRAS P+n,X` with a P-relative base and direct addressing was dropping its
index, so it pushed a pointer to element **zero** of its table. BASIC's
compiler selects the code template for every statement and every operator
with exactly that instruction, so every statement was compiled from template
0 — `LDIS #9; PCLS STT[9]`. That is the ` 9` every operator printed, the
`TRY,B` prologue whose absence trapped the second statement and starved
`NEW`'s frame, and the `PCLS STT[9]` that `RUN` trapped on. `src/test/basictest.sh`
went from 11 passing to 22 of 24; the account is below and in
[docs/SUNOS.md](docs/SUNOS.md) section 16.

(Note which key ends a line: in `FOCUS_TYPE`, `\n` is EXECUTE and `\r`
STORE, and a numbered line needs `\r` or BASIC answers `IMPROPER COMMAND`.)

Two cases still fail, and neither is that bug: `DISP "A"&"B"` halts on
string concatenation, and `DISP 4.5+1` answers `5.49999999907` — a wrong
answer where it used to be a halt.

> **The rest of this subsection is a superseded log.** Everything from here
> to "Recovered software" is the investigation that chased the symptoms of
> the `LRAS P+n,X` defect above -- short transfer tables, empty container
> slots, composed scaffolds, the `LDIS #9` that every operator printed. The
> measurements in it are sound and worth keeping; the diagnoses are not.
> It is left in place because it is the record of how the bug was cornered.

**A cheaper bench, and what it shows.** A stored program off the demo discs
fails in exactly the same instruction, which makes HP's `MASTERMIND` — 3,780
bytes against the diagnostics driver's 31,652 — the smallest reproducer the
project has (docs/SOFTWARE.md). Watched with `FOCUS_WWATCH` on the length
word and `FOCUS_TRAPLOG`, the whole sequence takes six milliseconds and
names its own participants:

```
t=32.489  BASIC COMPILER  (PB=0x130dc) composes  LDIS #9; PCLS 9   at PB+0x288
t=32.491  BASIC PARSER    (PB=0x35cf8) stamps    length = 0x18     by MVB 7,B
t=32.495  the PCLS traps  021, parameter 0x24
```

The compose and the stamp are two *different* BASIC segments writing the
same container two milliseconds apart, and the stamp lands after the code.

The container's table, read out at the moment of the trap (`PL` = 0x77d6b):

```
entry 1   0000027c   local, PB+0x27c -- the composed program's entry point
entry 2-6 80008c10 802e8006 8022c006 803a0003 80394003
entry 7-9 00000000 00000000 00000000
length    00000018
```

**Raising the length alone is not enough, and that is the new fact.**
Poking `0x24` into the length word immediately before the call
(`FOCUS_POKEPC=00077ce2:00077d68:00000024`) clears the STT violation — and
the call lands on **zeros**, trapping 020 with CIR = 0. Entries 7, 8 and 9
of this container are empty, not merely out of bounds.

So the length mismatch was hiding the real gap. It is not enough to know
which of the two containers the scaffold belongs in: whichever it is, the
step that *fills* slots 7-9 never runs for a freshly compiled program.

**Entries 5-9 are fixed runtime hooks.** `FOCUS_FIND` locates the three-word
pattern in five other places in the same run, so populated nine-entry
containers exist alongside the broken one. Reading a working one out
(`0x77864`) against the broken one puts the difference beyond doubt:

| entry | broken | working |
|---|---|---|
| 1-4 | program-specific | program-specific |
| 5, 6 | `803a0003 80394003` | `803a0003 80394003` |
| 7, 8, 9 | `00000000` x3 | `80340006 802f4006 80360006` |
| length | `00000018` | `00000024` |

Entries 5 to 9 are the same constants in every working container — hooks
into segment 6, BASIC — and the broken one received two of the five.

**Supplying the missing three gets the call through.** Poking entries 7-9
and the length in immediately before the call (`FOCUS_POKEPC`) lets
`PCLS STT[9]` resolve: it calls into segment 6 at offset 0x6344, runs, and
returns cleanly. The failure moves two instructions on, to **trap 030**, in
exactly the place the earlier `MAIN`-table attempt reached — now from a
third independent direction, and on a 3.7 KB program rather than a 31 KB
one.

**And the scaffold disassembles.** The ten words the compiler composes are:

```
LDS     P-16          a word out of the code segment
LDIS    #9
PCLS    STT[9]        <- was trapping 021
ADDI    #145
INCM    S-3,I         <- now traps 030
DEL
```

**What the trap 030 actually is.** The same instruction appears in BASIC's
DISP path (`LPP STT[11]; INCM S-3,I`), which is what the external-program-
label fallback in `focusaddr.h` was written for, so the first guess was that
the scaffold had produced a second test case for that hypothesis. **It has
not**, and the measurement says so plainly. Watching the operand
(`FOCUS_RWATCH` on the stack top) catches the read:

```
[rwatch 0008250c -> 000000ad P=00077ce8]     the pointer INCM dereferences
```

`0x000000ad` has tag `(0:2) = 00`, which is the *external data segment*
path, and its segment number `(2:11)` is zero — that is the trap. The label
fallback needs bit 0 set and lives inside the `tag == 2` branch, so this
pointer never reaches it. `FOCUS_NOLABELDATA` makes no difference here.

The pointer has **no tag at all**. It is a bare offset, and the two
instructions that build it are visible:

```
LDS  P-16     loads 0x0000001c from the code segment at 0x77cd0
ADDI #145     0x1c + 145 = 0xad
INCM S-3,I    dereferences 0xad -> external DST segment 0 -> trap 030
```

**The emulator is faithful here, and the whole block says why it matters.**
ERS sheet 56 gives the pointer evaluation as pseudocode, and it contains
`If PTR (2:11) = 0 then EDS Pointer Violation Trap` — literally what the
core does, so `0xad` really is an invalid pointer. `CPTR`, which is what
builds tagged pointers two instructions before the compiler plants that
word, also checks out: sheet 121 assigns `(S)(2:30) := Offset`, preserving
the tag, and the core writes `ers32::set(ptr, 2, 30, offset)`.

Dumping the whole 48-byte block rather than the 16 bytes first watched
shows the stack discipline, and that is the thing:

```
PCLS STT[4]
DEL
LDS  P-8        pushes 0xc000002c   -- a STACK pointer, tag 11
LDS  SB+8       pushes ...
LDS  P-16       pushes 0x0000001c
LDIS #9
PCLS STT[9]     the callee's EXIT N=1 drops one word, the 9
ADDI #145       0x1c + 145 = 0xad
INCM S-3,I      traps 030
```

`0xc000002c + 145` is `0xc00000bd`, a perfectly good stack pointer. So the
`ADDI` is meant to land on the pointer that `LDS P-8` pushed, which means
the hook at STT[9] should consume **three** words and leave that pointer on
top — not one.

**Which makes the trap 030 above an artifact of the poke, not a second
blocker.** The entries poked into 7-9 were copied from a different
container on the assumption that 5-9 are common constants; entries 5 and 6
do match, but the routine actually reached takes one argument where this
call site supplies three. The earlier claim that this reproduced the trap
030 from the `MAIN`-table attempt is therefore too strong and is withdrawn.

What the block does give is a hard constraint on the answer: whatever STT[9]
resolves to for a freshly compiled program, it has to consume the three
pushed words and leave `0xc000002c` on the stack. That is a much sharper
test than "does it stop trapping", and `MASTERMIND` is small enough to walk
instruction by instruction.

**The destination is not inverted, so the stamp is the wrong side.** The
compose site in the compiler reads directly:

```
LRA  DB+4564      the source buffer
LDS  DB+384,I     the object pointer
LDXI #60
LDS  DB+384,I,X   object[+60], the offset
CPTR,B            build the destination pointer
MVB  7            compose
```

Breaking on that `CPTR` and reading its operands gives base `0x40880000`
and delta `0x274`. The base is an external pointer naming **user DST
segment 17, offset 0**, and 0x40880000 + 628 lands at `0x77cd0` — which
makes segment 17's base `0x77a5c`, exactly the `PB` of the container that
then traps. BASIC is not composing into the wrong container; it is
composing into the one it means to.

And that container has room for nine entries. It is 784 bytes
(`PL - PB + 1`), the composed block ends near `0x77d07`, and a nine-entry
table would occupy `0x77d44` upward — clear of the code with room to spare.
Five other containers in the same run carry nine entries with hooks 5-9
identical to this one's. So of the two alternatives the older note left
open — the boot stamping the pair the wrong way round, or the destination
pairing being inverted — **the destination is right, and the 0x18 the
parser stamps two milliseconds after the compose is the wrong number.**

(One correction to the note above: `CPTR`'s external branch, where the core
departs from sheet 121 by folding the delta into the offset field instead of
re-evaluating the pointer, *is* on this path — the earlier claim that it was
not was wrong. It does not bite here, because 628 fits inside segment 17 and
no chain walk is needed, so the two readings agree. It would bite on a
container whose offset ran past its segment.)

**How the six-entry table gets there, and a walk-back.** Watching every byte
of the table region shows the length word and all six entries arriving in a
*single* 28-byte `MVB 7,B` — the "28-byte move" of the older note, installing
`0x18` and six entries in one go. Breaking on it puts the machine in the
parser segment, privileged, with `X = 783` (the container's size less one)
and a caller chain lying entirely in the SUN OS KERNEL at `0x44xxx-0x49xxx`.
This is the kernel's segment-creation path, handed a six-entry table to
install. The nine-entry containers, by contrast, are never written after
`t = 25`: they pre-exist, and the six-entry one is built fresh for this
program at RUN.

That means the conclusion just above — "the destination is not inverted" —
is stated too strongly and should be read as: *the emulator faithfully
computes the destination that BASIC's own object fields specify*. It does
not establish that those fields are right, and there is now positive
evidence against them. The composed code calls `STT[9]` and expects that
call to consume three stack words, but entry 9 is the same constant
`0x80360006` in every container examined, and the routine it names exits
`N=1`. A template whose call site disagrees about arity with the universal
hook it names is a template built for a different container's conventions.
So the destination pairing is back on the table alongside the stamp, and
the arity mismatch is the sharpest evidence either way.

**A positive control, and the failing site is the odd one out.** Searching
memory for the two halfwords `2c09 3209` — `LDIS #9; PCLS STT[9]` — finds
five sites: one in the compiler at `+0x209e`, which is the template source
the older note located at `+0x209a`; two inside containers; the failing one;
and one further out. The two container copies are byte-identical to each
other and *unlike* the failing one:

```
0x77d18, 0x7ce98:   LDIS #1; LDIS #4; STD Q+217; LDIS #9; PCLS STT[9]; LDIS #1
0x77ce2 (failing):  LDS P-16;                    LDIS #9; PCLS STT[9]; ADDI #145; INCM S-3,I
```

The common form calls `STT[9]` having pushed exactly one argument, which is
what the universal hook's `EXIT N=1` consumes. It is the shape the hook was
written for. The failing site is the only one that pushes a pointer and two
more words first and then dereferences what the call leaves behind.

Note that `0x77d18` is in the *same* container as the failing `0x77ce2`. If
the template really does emit the pair twice, then one copy landed in
surroundings that match the hook and the other did not, which is a sharper
thing to explain than "the container is short".

*Not* established: an attempt to unblock the dereference by planting a
DB-tagged word at `P-16` in place of the compiler's `0x1c` moved the failure
to a bounds violation rather than past it. That poke invented a value rather
than measuring one and should not be read as evidence either way; it is
recorded only so the next person does not repeat it.

**The six-entry table traced back to where it is born.** Breaking on the
block move and reading its three stack operands — source, target, count —
at each of its two firings maps the whole chain, and it turns out to be
short:

```
t=32.488  SUN OS KERNEL builds a table image at 0x77cd0..0x77ceb
t=32.489  MVB  copies 28 bytes  0x77ceb -> 0x77d2b   (staging)
t=32.489  the compiler composes code over 0x77cd0..  (reusing the scratch)
t=32.491  MVB  copies 28 bytes  0x77d2b -> 0x77d6b   (into PL)
t=32.495  the composed code calls STT[9] and traps 021
```

Both moves carry count 28 and both are faithful: dumping the source at the
moment of the first one shows

```
0x77cc0: 75650000 ffffffff 30005450 6c6f7400   <- unrelated data
0x77cd0: 80394003 803a0003 8022c006 802e8006   <- entries 6,5,4,3
0x77ce0: 80008c10 00000086 00000018            <- entries 2,1, length 0x18
```

Entries grow downward from a table's top, so entries 7, 8 and 9 would sit at
`0x77cc4`, `0x77cc8`, `0x77ccc` — and what is there is `"Plot\0"` and
`0xffffffff`, not pointers. **The image is born with six entries.** Neither
copy truncates anything; the 28 is correct for what it is copying.

**The same instruction is called with 40 at LOAD and 28 at RUN.** Logging
inter-segment transfers (`FOCUS_SEGCALL`, docs/SUNOS.md) shows the program
is started by one user container calling into another — `3088 -> 3089
entry 1` — and reading both out of the user CST separates them cleanly.
The user CST base is word 0 of the CPU's dedicated block, and user segment
*N* is CST 3072+*N* in the single table SUNOS uses as both CST and DST, so
"user DST segment 17" and "CST segment 3089" are one entry.

| | container 3088 | container 3089 |
|---|---|---|
| built at | `t=21.880`, during LOAD | `t=32.488`, during RUN |
| built by | segment 6, BASIC itself | the kernel/parser copy path |
| table | **9 entries** | **6 entries** |

Both are runtime containers for the same program, and the composed code went
into the six-entry one. Better still, the *same* block-move instruction at
parser `PB+0x5c32` builds both, so its arguments can be compared directly:

```
LOAD  t=21.825   source 4020018f  target 4020020f  count 0x28 = 40   (seg 3076)
RUN   t=32.489   source 4088028f  target 408802cf  count 0x1c = 28   (seg 3089)
```

Forty bytes is nine entries and a length word; twenty-eight is six. One
instruction, one run, two callers, and the RUN caller asks for six. That is
as far down as measurement reaches without disassembling the caller: the
count is a parameter, the routine is faithful to it, and what computes it at
RUN is the last unknown.

That moves the question one hop further back and makes it much narrower than
"which container": the SUN OS kernel builds a six-entry image at `t=32.488`
for a program whose compiled code calls `STT[9]`, while five other
containers alive in the same run carry nine with the last three being the
fixed hooks `80340006 802f4006 80360006`. The instruction doing the writing
is a `PMVA 3,B` inside a kernel copy loop at `PB+0xb98e`, so the count is an
argument from further up, and what decides it is the thing to find.

**What the two tables actually hold.** Decoding both entry by entry — bit 0
clear is a local pointer, set is an external one with the segment in
`(20:12)` and four times the entry number in `(1:19)` — shows the tail is
not one block of five but two groups:

| entry | broken (6 entries) | working (9 entries) |
|---|---|---|
| 1 | local `PB+0x27c` | local `PB+0x86` |
| 2 | extern seg 3088 entry 2 | local `PB+0x72` |
| 3 | extern seg 6 entry 186 | extern seg 4 entry 83 |
| 4 | extern seg 6 entry 139 | extern seg 3091 entry 1 |
| 5 | **extern seg 3 entry 232** | **extern seg 3 entry 232** |
| 6 | **extern seg 3 entry 229** | **extern seg 3 entry 229** |
| 7 | *empty* | extern seg 6 entry 208 |
| 8 | *empty* | extern seg 6 entry 189 |
| 9 | *empty* | extern seg 6 entry 216 |

Entries 1-4 are the program's own and differ in shape between the two, which
is expected. Entries 5 and 6 are byte-identical in both and point into
segment 3, BASIC IO/MM RTS. Entries 7, 8 and 9 point into segment 6, BASIC
itself — and those are the three the broken container never receives.

So the missing piece is specifically **the link group into the BASIC runtime
segment**, not a generic shortfall, and `STT[9]` — the entry the composed
code calls — is the last member of it. The container gets its IO/MM links
and not its BASIC links.

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

**HP-UX 5.01 reaches its copyright banner on the 520.**
`tools/runhpux.sh --release 5.01` builds a boot image out of the damaged
release tape and boots it; `docs/HPUX.md` section 11 has the whole
account. Three things had to be got right, and each is now a tool rather
than a recipe:

- **Which kernel.** The tape carries two, and it is the *520's* that
  survives whole. `S97080C` is "HP-UX Model 520 Release 05.01" and all
  sixteen of its segments verify; `S97089C` is the 530/540/550 build, and
  although its `os.req` was extracted intact by length, its `CORE SYSTEM`
  segment does not checksum. So the machine this release boots on is the
  520, whose console is the built-in display and keyboard — which is
  exactly what its `9020 HUMAN INT` segment is for.
- **Terminating the chain.** The stored `os.req` is the *installable*
  file, not a boot image: the segment chain ends without the negative word
  the powerup module stops on, so the walk runs off the end and checksums
  garbage for ninety million instructions. Cutting at the chain end and
  appending `ffffffff` makes it bootable.
- **Giving it hardware.** On a bare 520 the kernel raises escape 116 —
  a request dispatched to the NULL driver — from `CORE SYSTEM +0x7b82`,
  and its outermost TRY handler prints "System halted due to system
  error." Fit an HP-IB card with a disc on it and the same kernel walks
  past that, brings up its console and prints the HP-UX banner on the
  screen.

`tools/hpuxboot.py` does the first two (and will splice in the `.opt`
driver modules, which are chain modules in the boot image's own format —
the customize step HP's installer did). Where it stops now is a loop, not
a halt: the kernel reads and writes the file system it is given, and cycles
on escapes 319/154/320/244 out of `HP-UX KERNEL` and `IOS and DDMs`
without ever starting `init`. That it has no 5.01 userland to start is the
likeliest reason — `SCOMMANDS`, the whole of `/bin`, is gone from the tape.

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

Both HP-UX versions once stopped on the same thing: format 4 opcodes
020 and 021, which the Dec-82 Appendix B leaves unassigned — and so,
it turns out, does HP's own toolchain. 5.01 uses
them 1,192 times and 5.30 1,514 times; BASIC and the diagnostics never
execute either. 5.01 reached its first `021` after 1.26M instructions, in
the `CORE SYSTEM` segment. **They are `UP` and `DOWN`, and they are
implemented** — the hunt below is kept because it is how they were named,
and because the reasoning generalises to the slots still unassigned.

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
one that stops. *(Superseded: UP and DOWN are implemented, and use HP-UX's
own wait-list format, which is what got multiuser HP-UX past its stall;
see docs/FOCUS.md and the alpha's commit a26b01c.)*

## Layout

```
docs/
  FOCUS.md             CPU architecture + full opcode spec  <- start here
  HARDWARE.md          520/540 machine hardware: MPB, I/O, boot media
  SOFTWARE.md          target media, HP-UX versions, boot ordering
  opcodes.csv          full FOCUS opcode map, from ERS Appendix B
  bitsavers/           HP's manuals: fetched by tools/fetch-media.sh --manuals
  hpjournal/           3 HP Journal issues on the Series 500: fetched likewise
  notes/               their text (extracted when they are fetched), the
                       project's own OCR passes, Appendix B + loader flowchart
software/
  README.md            provenance and per-image analysis
  97089C.hpi           HP-UX 5.01 tape (damaged; the 520 kernel survives)
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
  hpuxboot.py          build a bootable 5.01 kernel out of the release tape
  lib/focus.sh         the run scripts' shared MAME launch
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
$ gunzip -k 7914_unitSave_hpux4.0.bin.gz
$ gunzip -k 7914_unitSave_hpux4.0_tape2.bin.gz
$ python3 ../../tools/sdf7914.py 7914_unitSave_hpux4.0.bin 7914_unitSave_hpux4.0_tape2.bin --extract out
```

*(Updated for the alpha: `-k` keeps the tracked `.gz` files, which plain
`gunzip` deleted. The assembled volume that tools/runhpux.sh boots,
`var/pristine/hpux530.bin`, can be listed in the same way with `--list`.)*

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
tools/setup.sh --no-build    # MAME at the pinned commit, wired in (INSTALL.md)
cd src/test && make test
```

*(Updated for the alpha: the submodule commit this recipe fetched existed
only on the maintainer's disc. The unit tests need no MAME at all.)*

Two harnesses sit above the unit tests, because unit tests keep passing
through a great many real regressions:

- **`src/test/golden.sh`** — behaviour preservation. Seventeen probes,
  including two multi-million-instruction traces and three real boots,
  compared byte for byte against a recorded baseline. It answers "did
  anything change?", which is the question a refactor has to answer.
- **`src/test/basictest.sh`** — behaviour *correctness*. A table of
  (keystrokes, expected screen line) cases, each booted headless, typed at
  with `FOCUS_TYPE` and read back through `FOCUS_DIMDUMP` and
  [tools/screen.py](tools/screen.py). It answers "does BASIC give the right
  answer?", which golden.sh cannot. Cases that pass are regression guards;
  cases that fail are the work list, and the score is the measure of
  progress. `--list` shows the table, `--only <name>` runs one.

*(Updated for the alpha: `golden.sh` is a tool for whoever is changing the
emulator, not a release test. It compares a tree with a baseline you
captured yourself from the tree before your change, and there is no
baseline in the repository. It has nineteen probes now. Ten need only a
compiler and the 520's floppies (`--quick`); six more need the emulator
and the two BASIC discs `tools/setup.sh` builds; the three HP-UX ones need
`software/cct/7914_unitSave_hpux4.0.bin`, which a fresh clone does not
have, and are skipped there. Its header says how to get them. The tests
the release was checked with are in
[docs/VALIDATION.md](docs/VALIDATION.md#8-the-hp-ux-release-tests).)*

`tools/screen.py` rebuilds the display from a `FOCUS_DIMDUMP` log. That is
not a formality: the dump prints the frame buffer in vram order, while the
6845 scrolls by moving its start address, so past the first screenful the
dumped row numbers no longer name displayed lines.

`tools/gfxscreen.py` does the same for the graphics plane and a
`FOCUS_GFXDUMP` log -- 560 x 455 pixels, a digit per pixel holding its three
plane bits, with `--plane` for one plane and `--ppm` for a picture.

## Documentation

The primary documents are HP's manuals on bitsavers.org
(`pdf/hp/9000_500/`, `9000_cio/` and `disc/`). The repository points at
them ([MEDIA.txt](MEDIA.txt)) rather than carrying them:
`tools/fetch-media.sh --manuals` puts them in
[docs/bitsavers/](docs/bitsavers/) under the names the notes cite. The two
that matter most:

- **FOCUS Instruction Set ERS** (Dec 1982, 150 sheets) — the CPU reference.
  Registers, addressing, segmentation, interrupts, every instruction, plus
  appendices covering the opcode bit patterns, dedicated memory locations,
  IEEE floating-point conformance, and known silicon bugs.
- **FOCUS I/O ERS** (Dec 1982, 80 sheets) — the IOP, DMA, interrupts, and the
  interval timer.

Both have usable OCR text layers, which the fetch extracts into
[docs/notes/](docs/notes/).

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
   *(Superseded: the HP-UX 5.30 unit save from the Crisis Computer Tapes
   is that better image, and it runs multiuser.)*
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
5. Full HP-UX — blocked on a better tape *(superseded: HP-UX 5.30 from the
   7914 unit save runs multiuser)*

See [docs/SOFTWARE.md](docs/SOFTWARE.md) for detail.

## Interactive BASIC and `RUN` both work: `LRAS P+n,X` had lost its index

*2026-08-25.* Everything below this section about scaffolds, short transfer
tables, missing arithmetic and shallow frames describes the symptoms of **one
emulator bug**, now fixed. Read this first; the sections that follow are kept
because their measurements are sound and their conclusions are not.

`LRA`/`LRAS` with a **P-relative base, direct addressing and the index bit
set** pushes `E - PB`, and `E` is the *indexed* effective address (ERS sheet
61: "a pointer to the computed address, using byte indexing"). The
implementation formed the address without `X`, so every such instruction
pointed at element **zero** of its table.

BASIC's compiler picks the code template for each statement and each operator
with exactly that instruction — `LRAS P+14,X` over a pool in its own code
segment, at COMPILER +0x2090 — so it compiled **every statement from template
0**, which is `LDIS #9; PCLS STT[9]`. That single sequence is the "spurious
literal 9" every operator printed, the missing `TRY,B` statement prologue
behind the second statement's trap 1 and `NEW`'s sixteen-byte-short frame,
`LET`'s absent pointer, and the `PCLS STT[9]` that `RUN` trapped on.

The fix is one expression in `do_lra`. `LRA` and `LRAS` had no test coverage
at all, which is how it survived a full audit of the instruction set.

```
10 A=3
20 B=4
30 DISP A*B+2
40 END
RUN      ->   14
```

`src/test/basictest.sh` goes from 11 passing to **22 of 24** — arithmetic,
`LET`, `NEW`, consecutive statements, `LIST` twice, and `RUN` of a typed-in
program. `src/test/golden.sh` reports 16 of 17 probes byte-identical,
including two multi-million-line boot traces and all three MAME boots, so
nothing on the boot path was touched. Full account in
[docs/SUNOS.md](docs/SUNOS.md) section 16.

Still failing, and unrelated: `DISP "A"&"B"` halts (string concatenation),
and `DISP 4.5+1` answers `5.49999999907` — a wrong answer where it used to be
a halt, which is a far sharper frontier than anything below.

## `RUN` of a typed program: the short table, confirmed by experiment

*Superseded by the section above: the short table is a symptom, not the cause.*

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

*Superseded: the container map is accurate, the diagnosis is not.*

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

*Superseded by the section above: the short table is a symptom, not the cause.*

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

*Superseded: the container map is accurate, the diagnosis is not.*

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

*Superseded: the missing per-statement create is the statement template's
own `TRY,B`, never copied.*

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
