# HP's own diagnostics, and what they can tell this emulator

The museum collection includes **9020 System Functional Tests 3.00** — six
discs of HP's factory test suite — and a seven-disc BASIC demo set. Both are
worth more to this project than their contents suggest, for two reasons.

First, the tests are a hardware conformance suite written by the people who
built the machine. Each one drives a specific card through its documented
registers and says whether the answers were right. Anything the emulator gets
wrong in a card model should surface as a named failure rather than as a
mysterious hang three layers up.

Second, and less obviously: **these files are the only place a complete,
HP-built BASIC program can be read at rest.** A stored program carries its
statements as compiled FOCUS code with the transfer-table indices HP's own
compiler assigned — which is exactly the thing the RUN investigation has been
reconstructing by experiment. See [SOFTWARE.md](SOFTWARE.md) for that thread.

**What the suite says about transfer tables.** Reading all twenty-two test
programs gives a rule, and it is a clean one: the transfer table holds
**one entry per subprogram, plus one**. Twenty-one of the twenty-two obey
it exactly — `(7)KEYBOARD` 17 subprograms and 18 entries, `(6)STATUS` 19 and
20, `TEST` itself 19 and 20, `(17)PRINTER` 24 and 25. The single exception
is `(12)DISPLAY`, 37 subprograms against a 25-entry table, and `BUILD_BOOT`
on the BASIC boot disc breaks it the same way at 36 against 25 — the same
number twice, so 25 reads as a cap rather than a miscount.

On the demo discs the picture splits: `introduction.c` has 4 subprograms and
5 entries, the small launcher stubs have 1 and 2 — and `MASTERMIND`,
`MAIN_MENU`, `BREAKOUT`, `CUBIE` and `bplot_control` have **no table at
all**. `MAIN_MENU` is the one BASIC itself calls `NOT STORABLE`, because it
refers to options the machine that stored it did not have.

**A correction.** An earlier reading of this — that a zero-entry table is
what makes `RUN` fail, and that the table-bearing demo programs run — was
wrong, and wrong in the more embarrassing direction: the programs it rested
on never loaded. `LOAD "DEMO/music"` and
`LOAD "DEMO/DIR/intro/introduction.c"` both answer

    ERROR 62: File is protected or wrong PROTECT code

and the `ERROR 3` seen afterwards is `RUN` finding nothing in memory, not a
program reaching its own error. The first screen dumps showed only the last
line and the `ERROR 62` above it was missed.

What the corrected observation gives is more interesting than what it
replaced, because the correlation inverts:

| file | subprograms | table | `LOAD` |
|---|---:|---:|---|
| `music` | 1 | 2 | **ERROR 62** |
| `introduction.c` | 4 | 5 | **ERROR 62** |
| `MASTERMIND` | 2 | **0** | loads, then trap 021 at `RUN` |
| `MAIN_MENU` | 15 | **0** | loads, then trap 021 at `RUN` |

Files that ship *with* a transfer table refuse to load; files that ship
without one load and then fail at `RUN`.

> **Answered, 2026-08-26, and the correlation above is a coincidence.** The
> refusal is a **PROTECT field in the i-node**, at word +16, which the
> fs(5) page does not name and `sdfread.py` did not decode: top byte `0x03`
> protected, `0x53` not. It has nothing to do with the transfer table —
> the two just happen to line up on `BASDEM1`, where the fourteen protected
> files are all launcher stubs and the stubs all carry tables. §3 has the
> evidence and what it unblocked; `sdfread.py` now prints a `P` column, so
> this is visible at a glance rather than needing a hex dump.

Read any of them with [`tools/progfile.py`](../tools/progfile.py), which
documents the file format in its header comment:

```
python3 tools/td0.py --raw /tmp/d.img software/test/500MFTST.TD0
python3 tools/sdfread.py --extract /tmp/mftst /tmp/d.img
python3 tools/progfile.py '/tmp/mftst/(7)KEYBOARD' --strings
```

---

## 1. What is on the discs

Every disc holds a `TEST` driver — the same 19-subprogram menu program on all
of them — plus one file per device test. The parenthesised number in each
name is the test's menu number, not a select code.

### 500MFTST — the mainframe and its built-in devices

| Test | Subs | HP's date |
|---|---|---|
| `(6)STATUS` | 19 | Feb 24 1983 |
| `(7)KEYBOARD` | 17 | Feb 24 1983 |
| `(8)AGRAPHICS` | 13 | May 12 1983 |
| `(10)FLOPPY` | 14 | Aug 9 1982 |
| `(12)DISPLAY` | 37 | Apr 15 1983 |
| `(13)BGRAPHICS` | 22 | May 12 1982 |
| `(15)WINCHESTER` | 13 | Feb 24 1983 |
| `(17)PRINTER` | 24 | Feb 15 1983 |
| `(18)CGRAPHICS` | 14 | Sep 3 1982 |
| `(22)LTPEN` | 5 | May 13 1982 |

### 500MSTST — mass storage

`(6)CS80`, `(7)TAPE`, `(8)8290X`, `(11)SS80`, `(13)9885`, `(18)9895`,
`(23)9121`. All four-subprogram tests except TAPE (8), dated 1982–1984.

### 500EDTST — external devices

`(6)EXPRINTER`, `(7)97060`, `(8)SRM`, `(9)HPIB`, `(11)PLOTTER`, `(12)97062`,
`(14)HPIO`, `(16)DIGITIZER`, `(17)RGB_ALIGN`, `(19)GPIO`, `(24)ASI`.

### 500ENV1–3 — the environment the tests run in

These three read as valid Teledisk images but carry no SDF volume, because
they are not a filesystem at all: they are **a three-volume boot image**,
labelled `BASIC LANGUAGE SYSTEM  REV 2.00, BINARIES FOR SYS. FUNC. TEST`,
680,668 bytes across the set, whose twenty-nine segments all verify. This is
the system the test discs are meant to be booted on, and it is not
interchangeable with BASIC 2.0: alongside the thirteen segments of the
ordinary boot it carries **`GRAPHICS` (92,908 bytes), `SRM`, `HP98770`,
`HP97060` and `HP97062`** — five binaries that appear on no option disc in
the collection.

That is what the "missing options" message is about. Booted on plain BASIC
2.0 with every shipped option installed, `LOAD "TEST"` still answers

    SUBPROGRAM Clear_screen  REFERS TO MISSING OPTIONS; IT IS NOT STORABLE

because `Clear_screen` needs `GRAPHICS`, which BASIC 2.0 cannot be given.
Booted on this environment it loads clean.

Build a fixed disc from the set with
[`tools/mkdiagdisc.sh`](../tools/mkdiagdisc.sh). The five diagnostics-only
modules are extracted and disassembled under
[`disasm/`](../disasm/README.md).

---

## 2. The tests worth running first

Ranked by how much of what they check this emulator actually implements:

**`(7)KEYBOARD`** is the one to start with. Its subprograms are `Nvm_test`
("Nonvolatile memory test"), `Rtc_update`, `Reset_time`, `Id_test`,
`Bus_reg_test` and `Beeper_test`, built on `FNRead_reg` / `Write_reg` /
`FNBits` / `FNGetbits` helpers. That is precisely the select-code-6
keyboard-board hardware modelled in [HARDWARE.md](HARDWARE.md) §2.4 — the
real-time clock and the 2 KB of battery-backed NVM — exercised register by
register against HP's expectations. If the complement-decode of the RTC
nibbles or the NVM addressing is wrong anywhere, this test names it.

It is the arbiter for two things the manuals leave open (HARDWARE.md §2.4.1):
whether an RTC location is four bits or eight, and whether the 520's `R12`
latches 8 bits or 16. It also grades the select-code-6 clock WRITE, which was
dead code until an early return above the register switch was removed. Its
escapes come from the 520 Service manual's table C-1, reported as
`Self test error 1: IO address 006,0002`:

| Escape | Meaning |
|---|---|
| 1 | Failed loopback to FR15 |
| 2 | Keyboard not responding to poll |
| 3 | Keyboard failed self test |
| 4 | **Battery low condition detected** |
| 5 | Keyboard not responding with SRQ bit |
| 6 | Keyboard not forcing status false |
| 7 | Reading FR10 did not clear keyboard poll response |
| 8 | Power fail warning bit set |

**`(6)STATUS`** walks the machine inventory: what it reports about the card
cage is directly comparable with what the driver's fake cards claim.

**`(10)FLOPPY`**, **`(15)WINCHESTER`** and **`(6)CS80`** cover the disc
paths — the ICEI command lists and the CS/80 command set in
[HARDWARE.md](HARDWARE.md) §2.8, which the boot path exercises only along
the one route it needs.

**`(24)ASI`** and **`(9)HPIB`** cover the two CIO cards implemented from the
tech references; they would be the first independent check of the FR-register
protocol reconstructed in §2.6.

**`(12)DISPLAY`** (37 subprograms) is the largest test on any disc and would
be a thorough workout of the 6845 and the frame buffer.

---

## 3. Where the tests stop now

**2026-08-25: the graphics blocker is gone.** With the display graphics
interface modelled at select code 1 (`src/iop/focusgfx.h`, HARDWARE.md
§2.7.4), `Clear_screen` succeeds and HP's driver runs:

```
                   Welcome to the HP 9000 Test Supervisor
This program allows you to verify the operation of your computer and its
peripherial devices.  The test prompts and results may be displayed on
the CRT display, printed on the internal thermal printer, or printed to
an external printer.  Type the name of your choice and press RETURN, or
if you just press RETURN, the CRT display is chosen as a Default.
Please choose either  DISPLAY, INTERNAL, or EXTERNAL.  (Default=DISPLAY)
```

Answering `DISPLAY` (with `FOCUS_TYPERATE=250`, or the keystrokes outrun the
display driver) gets the next error, and it is a **new** one and nothing to
do with graphics:

    SYMBOL TABLE ENTRIES NOT ALLOWED IN COMPILED ONLY CONTEXT

so the frontier has moved from the display into the interpreter's handling
of a compiled program's `INPUT`.

**2026-08-26: the command set is modelled and the display draws.** The
specification turned out to be on the disc all along, in the one segment
`tools/mkdisasm.sh` was not extracting: **`PWP SELF TEST`**, segment 13 of
the boot image, 3,712 bytes, the smallest segment there is. Its local
entries 17–32 are HP's own power-up self test for the display and they name
every command, every parameter count and both display memory sizes. That
listing, plus the traffic `HP98770` generates when BASIC is told to draw,
is the whole of HARDWARE.md §2.7.4 — which is now the only description of
this interface that exists anywhere.

Reading a module for its register protocol is a method worth repeating, and
it beats sweeping values at a card: extract with `tools/mkdisasm.sh`, find
the entry points with `tools/stt.py --segment N`, and read the `LDI`
constant that sits in front of each `PCLS` to the register primitive.

What works now, on the diagnostics environment:

    PLOTTER IS 1,"INTERNAL"
    FRAME

puts a rectangle round the whole 560 × 455 raster, and `MOVE`/`DRAW` place
lines where BASIC's coordinate system says they belong — `MOVE 0,0` lands
at raster (0, 454), the bottom left. `FOCUS_GFXDUMP=<n>` prints the raster
as text and `tools/gfxscreen.py` turns it back into a picture or a PPM; the
screen composites the graphics plane under the alphanumeric one, which is
dominant over it.

**Three emulator defects were found on the way there**, none of them in the
display, and all three are fixed — after which HP's power-up self test for
the whole display subsystem passes clean for the first time:

- the IOP read F1 alone as the DMA direction, so Table 5.1's "Input,
  Verify" ran as an output and the memory test verified nothing;
- the alphanumeric card had no read-back path, though its frame buffer is
  documented as readable back for diagnostics;
- `MVB` moved overlapping regions like `memmove`, which breaks the
  self-propagating buffer fill HP's test depends on. That workaround was on
  a diagnosis that named the wrong instruction — the real cause was `LRAS
  P+n,X`, fixed separately — and `FOCUS_MVBLOG` shows the only overlapping
  moves in a whole BASIC boot are the four the display self test makes.
  HARDWARE.md §2.7.4 item 11 has the argument; `FOCUS_MVBMEMMOVE` is the
  A/B control.

**2026-08-26: the demonstration discs load and run.** `ERROR 62` and the
`ERROR 56` behind it are both answered, and neither was a graphics problem.

**`ERROR 62` is real protection, not an emulator fault.** The i-node's word
at **+16** is a PROTECT field the fs(5) page does not name: its top byte is
`0x03` on a protected file and `0x53` on an unprotected one, and a volume
that never used PROTECT leaves the whole word `0xffffffff`. On `BASDEM1`
every file that answers `ERROR 62` has `0x03` and every file that loads has
`0x53` or `0xff`, with no exceptions; clearing the byte on a copy of the
image makes the same `LOAD` succeed. `tools/sdfread.py` now prints a `P`
column for it. The 9020 test discs are `0xff` throughout, which is why
`TEST` never had the problem.

**The type field explains `ERROR 58`.** `uftype` is BASIC's user file type
— `-5680` a stored program, `-5687` a data file, `-5693` a directory — and
`LOAD` on a data file is `ERROR 58: Improper file type`.
`/DEMO/DIR/others/SIERPINSKI` is data that the `SP` programs plot, not a
program. `sdfread.py --long` names the type.

**The real programs are on disc 4.** The fourteen protected files in
`/DEMO` are launcher stubs: each sets `/DEMO/DIR/others` and loads a
program from there by name. Those directories are empty on discs 1, 2, 3, 5
and 6; `BASDEM4` carries the real set — `QIX` and `SPIRAL_CONTROL` as
programs, `SIERPINSKI`, `SP1`–`SP3` and `BPLOT1` as their data — and every
one of them is **unprotected**. Nothing has to be patched to reach them:

    FOCUS_TYPE='LOAD "/DEMO/DIR/others/QIX"\nRUN\n;\x7040\n;\x7050\n;\x700\n' \
    FOCUS_TYPEAT=28,52,60,68 tools/runbasic-hd.sh software/hd/diag.img -- \
        -mpb3 1m -mpb4 1m -flop1 "$PWD/software/demos/BASDEM4.TD0" -str 110

`QIX` loads, runs, prints its partition table and takes all three of its
answers; `SPIRAL_CONTROL` loads and runs. Both then stop on something that
is neither the file system nor the display.

**The prompts want the CONTINUE key, and it is keycode `0x1d`.** `CONT` as
a command answers `CONT NOT ALLOWED WHILE WAITING FOR INPUT`, so these are
`INPUT` statements whose "(CONT = 100)" means the key. It has no ASCII of
its own; `FOCUS_TYPE`'s `\x1d` presses it. Found by bisecting the keycodes
`0x00`–`0x7f` against "does the next question appear", which is what the
`\xNN` escape is for.

**`SPIRAL_CONTROL`** reports `ERROR 504 IN 110: Partition(s) must be in the
STOP state` — it wants to `GET "SP1",1,1` into partitions, so partitions
are its frontier and nothing to do with graphics.

**`QIX` halts with `TRAP:00000021`, an STT Violation**, and the rule behind
it is clean. A stored program's file carries a transfer table, and the
count is normally one per subprogram plus one — but some files ship with
**zero** entries, and those trap the moment they call a subprogram:

| file | subprograms | table | what happens |
|---|---:|---:|---|
| `TEST` | 19 | 20 | runs; the Test Supervisor is this program |
| `func_3d` | 1 | 2 | runs, reaches its own `ERROR 56` |
| `SPIRAL_CONTROL` | 1 | **0** | runs, reaches its own `ERROR 504` |
| `QIX` | 2 | **0** | **trap 021** when `MAIN` calls `Picknewpt` |
| `MAIN_MENU` | 15 | **0** | runs and produces nothing |

A zero-entry table is only fatal when the program actually makes a
cross-subprogram call, which is why `SPIRAL_CONTROL` gets past it and `QIX`
does not. Localised: the trap fires at **`BASIC` (segment 6) `+0x1702`**,
which is `PCLS STT[0]` — an indirect call through a routine pointer that
BASIC has just loaded from a linked list with `LDS Q+4,I,X`, X = 8. The
pointer is zero, so the transfer lands at the segment's own base and the
hardware calls it what it is. HP shipped these programs, so BASIC must
build that list at `LOAD` time for a file that carries no table, and it is
that path — not the graphics, not the file system — that is wrong here.

A locally typed program with a subprogram (`10 CALL Foo / 30 SUB Foo /
40 DISP 7`) runs and prints ` 7`, so `CALL` itself is sound; this is
specific to a program loaded from disc.

> **A correction.** An earlier note here said a program that does not
> issue `PLOTTER IS` draws nothing, and guessed at a missing default
> plotter. That was measured before the keyboard was fixed, when typed
> lines were being mangled. It is wrong: `GINIT` then `FRAME`, with no
> `PLOTTER IS` at all, draws the same seven vectors as `PLOTTER IS
> 1,"INTERNAL"` then `FRAME`. The default plotter works. `QIX` draws
> nothing because it never reaches its draw loop.

**Getting there needed two things in the emulator**, both fixed:

- **lower-case `e` could not be typed.** It came out `{`, and had been
  written up as a key this model has no way to press. It was the wrong
  keycode: `E` was mapped to `0x2a` because a sweep of the unshifted half
  found `0x2a` echoing `E` and stopped, and `0x2a`'s shifted partner
  `0xaa` is `{`. The E key is `0x6c` — `0x6c` echoes `E`, `0xec` echoes
  `e`. Until this was right, `/DEMO/DIR/others/QIX` could not be typed at
  all. A sweep that validates one half of a shifted pair has validated
  half a key.
- **`FOCUS_TYPE` could only send characters.** `\xNN` now presses keycode
  NN, which is how a script reaches a key that puts nothing on the screen
  — `\x70` clears the input line, without which an answer typed at a
  prompt lands after whatever the echo left there.

The account below is what the situation was before that, kept because the
measurements in it stand.

They boot, load and run. The environment comes up to `BASIC Ready for use.`,
`LOAD "TEST"` reads the 31,652-byte driver off the flexible volume with no
complaint, and `RUN` executes it:

    tools/mkdiagdisc.sh
    tools/runbasic-hd.sh software/hd/diag.img -- -mpb3 1m -mpb4 1m \
        -flop1 "$PWD/software/test/500MFTST.TD0"

then `LOAD "TEST"` and `RUN` — or, for a batch run,

    FOCUS_TYPE='LOAD "TEST"\n;RUN\n' FOCUS_TYPEAT=20,36 FOCUS_DIMDUMP=600 \
    tools/runbasic-hd.sh software/hd/diag.img -- -mpb3 1m -mpb4 1m \
        -flop1 "$PWD/software/test/500MFTST.TD0" -seconds_to_run 48

The driver's first act is `Clear_screen`, and that is where it stops:

    ERROR 702 IN Clear_screen: No graphics driver/CRT graphics hw failure

which is the program working correctly and the machine being incomplete.
**The remaining blocker is the graphics display on select code 1**, which
this emulator does not model at all (HARDWARE.md §2.7). It is not avoidable
by running a single test instead of the driver: each test file's `MAIN` is
an empty stub — `PROGRAM MAIN !KEYBOARD    FEB 24, 1983` and nothing else —
and the real body is a `SUB Program` the driver calls, so `TEST` is the only
entry point there is. Confirmed from the other side too: `LOAD "(6)STATUS"`
then `RUN` answers `ERROR 5 IN MAIN: Improper context terminator`.

### What the driver actually wants from select code 1

Parking a logging card there says more than "not modelled" does:

    FOCUS_PROBE=1 FOCUS_PROBELOG=1 FOCUS_TYPE='LOAD "TEST"\n;RUN\n' \
    FOCUS_TYPEAT=30,44 tools/runbasic-hd.sh software/hd/diag.img -- \
        -mpb3 1m -mpb4 1m -flop1 "$PWD/software/test/500MFTST.TD0" -str 56

The scan **succeeds**. It reads FR1, reads FR3 three times and gets zero,
writes `1<<1` = 2 to FR0, and reads the presence echo back from FR1 — the
same sequence every other card goes through, and a plain echoing interface
satisfies all of it. The card is registered.

What follows is the interesting part: something then reads FR1 **598,730
times** and gives up. So `ERROR 702` is not "no card found", it is a
**timeout waiting on a status bit in FR1** from a card that has already
been accepted.

**Both halves of that are now pinned down** (2026-08-25). Giving the probe
card a hook that reports the processor's segment and PC on every FR access
attributes all 598,720 of the reads to one instruction — `SUN OS KERNEL`
+0x28fa — and the bit it waits on is **FR1 bit 6**. It is the kernel's
interface bring-up, not the display driver. Answering with bit 6 set clears
the timeout on the first read; bit 7 has to be set as well, or the kernel
takes a reset-and-escape path (`FR1 = 6`, `FR1 = 1`, `ESCP`) instead.

That is not enough to bind the driver, and the reason is now legible too.
`HP98770`, the 9020C display driver in this very environment (segment 43),
binds only if the kernel types the device **16 or 17**, and the type comes
from **FR1 bits 8-11** — a device class — not from the `FR3` identity that
the earlier sweep of 0-15 was varying. HARDWARE.md §2.7.4 carries the whole
map. What is still missing is why the kernel's typing pass never visits
select codes 1-3 at all.

A short sweep of what FR1 answers — 0x0002, 0x0042, 0x0082, 0x00c2, 0x0102
through `FOCUS_PROBEFR1`, all keeping bit 1 set so the presence echo still
passes — leaves `ERROR 702` unchanged. (It would: none of those five is the
class value bits 8-11 have to carry.) That is the expected outcome rather
than a discouraging one: §2.7 of HARDWARE.md records that no document in
the collection gives the meaning of the individual FRs for this interface,
so the bit is not going to be found by guessing five of them. Reading it
off HP's own driver, which is on the option disc as `GRAPHICS`, is the
route that does not depend on a manual nobody has.

Five defects in the emulator had to go first, and all five were found by
these tests rather than by anything else the project runs:

- **The loader's stack sat inside the boot image.** The handoff put SB at
  `top - 0x80000`, which only clears the image while the image is under half
  a megabyte. BASIC 2.0's is 373 KB and fits; this environment's is 680 KB,
  and the loader's Q landed at image offset `0x80203` — inside `HP97062`,
  which powerup then relocated with the stack marker written through it.
  The machine said so precisely: `Self test error 2: CHECKSUM for
  segment0014`.
- **`PSHN` ran past `SL`.** The ERS is explicit that `ADDS`, `PSHN`, `SLD`
  and `SCAN` test the stack limit themselves and "clear STAT[9:1] before
  trapping" (sheet 51), rather than leaving S past `SL` for the
  end-of-instruction check. BASIC starts a program by allocating its whole
  variable area in one `PSHN` — 3,330 words for this driver — so run to
  completion it put S 6 KB outside the stack segment, the handler grew the
  stack by just enough to cover where S had already landed, and the next
  call over-ran the fresh limit fatally.
- **`ADDS` and `SUBS` had only their fixed form.** A zero operand means the
  count is on the stack instead, which is how the kernel probes for room:
  `STAX; ADDS #0; LDXA; SUBS #0` reserves N words and gives them straight
  back, purely to make the overflow trap fire. Both halves were silent
  no-ops, so the probe probed nothing.
- **`PSHR`/`SETR` were a byte out on the S form.** The ERS writes the push
  as `S:=S+1; (S):=S-SB-1`, both S's being the incremented one, so the `-1`
  undoes the increment; in byte addressing it took a byte off the offset
  instead. It cancelled against the same error in `SETR` whenever a pair
  carried S back to itself — but `AP`'s procedure prologue pushes the S form
  and restores it as the **Q** form (`PSHR 1` / `SETR 2`, opening a frame at
  the current top), and there the missing byte came through: Q landed one
  byte below the word grid and the `EXIT` that unwound it read its delta
  from a misaligned marker. That is the one that was actually killing the
  test driver.
- **`IXIT` read "bottom of the ICS" as "no dispatcher to return to".** The
  delta word of the frame at `QI` is zero for the bottom frame, which is as
  true of a dispatcher that really was interrupted as of the empty frame the
  boot handoff leaves; taking the boot's door out of a live one threw the
  interrupted context away and dispatched through a TCB that had never been
  filled in. The pseudo-disable count separates them, exactly as it already
  did in the sibling deferred-`DISP` case. This one showed up as a *phase*
  dependence — `LOAD` typed at 35 emulated seconds died in a stack underflow
  inside the `IXIT`, typed at 20 or 33 it did not — which is worth
  remembering as a shape: a fault that moves with timing is usually in the
  interrupt path, not in the code that appears to be running.

---

## 4. Provenance

Everything here came from the HP Computer Museum's Series 500 collection,
donated by Charlie Brett of Fort Collins, Colorado in November 2010. See
[software/README.md](../software/README.md).
