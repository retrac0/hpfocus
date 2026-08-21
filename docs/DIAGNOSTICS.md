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

### 500ENV1–3 — no SDF filesystem

These three read as valid Teledisk images but carry no SDF volume, so they
are not part of the same file world. What they are has not been established.

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

## 3. Running them is blocked on the same thing as everything else

The tests are BASIC programs, so running them needs `LOAD` and `RUN` to work,
and RUN is the open problem tracked in [SOFTWARE.md](SOFTWARE.md). The demo
discs matter here: several demos are **single-subprogram** programs —
`hanoi_towers`, `snow_flake`, `music`, `fft`, `house`, `intro` — which makes
them the smallest possible real-program RUN targets, much simpler than the
19-subprogram test driver.

There is also a hypothesis worth testing directly. Everything learned about
RUN so far comes from a program *typed in* at the keyboard, which BASIC
compiles statement by statement into a prefab scaffold segment. A program
**loaded from disc** arrives already compiled, with its own transfer table,
and may take an entirely different path into execution. If it does, a demo
disc could run before a typed program does.

---

## 4. Provenance

Everything here came from the HP Computer Museum's Series 500 collection,
donated by Charlie Brett of Fort Collins, Colorado in November 2010. See
[software/README.md](../software/README.md).
