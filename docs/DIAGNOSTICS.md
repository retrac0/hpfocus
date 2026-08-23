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
entry point there is.

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
