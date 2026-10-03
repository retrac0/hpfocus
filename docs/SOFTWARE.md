# Target software and media

The end goal is booting original Series 500 software. This tracks what exists,
where it is, and what still has to be obtained.

> **Status at the first alpha.** Much of this page predates the HP-UX 5.30
> disc. That image is a 7914 unit save from bitsavers' *Crisis Computer
> Tapes* (`software/cct`, provenance in `software/README.md`), and it is
> now the main target: it boots to multiuser on the emulated 540
> (`tools/runhpux.sh --multiuser`, INSTALL.md). Where this page calls full
> HP-UX "blocked", or says no other Series 500 HP-UX exists online, read it
> as the state before that disc was found. The sections are corrected in
> place below.

## HP-UX versions on the Series 500

**Earliest: 1.0. Latest: 5.3.** The 520 spans the whole range.

The critical thing to understand is that **Series 500 HP-UX is a separate
product line from Series 200/300 HP-UX, with its own version numbering.** The
same number means different software on different hardware. Series 500 HP-UX
was layered on **SUNOS** — a Series 500-specific kernel, no relation to Sun
Microsystems' SunOS — whereas the Series 200/300 line was not.

This trips up the usual references. Bitsavers'
[`HPUX_Versions.txt`](https://www.bitsavers.org/pdf/hp/9000_hpux/HPUX_Versions.txt)
tracks the mainstream numbering only: it lists 1.0 (1984) as Integral PC and
5.0 (1985) as "Series 200, 300 and 500", which makes it look as though the
Series 500 got exactly one release. That is an artefact of the document's
scope, not history.

Confidence by source:

| Claim | Evidence | Confidence |
|---|---|---|
| Range is 1.0 – 5.3 | [OpenPA](https://www.openpa.net/systems/hp-9000_520.html), corroborated by HandWiki/Wikipedia | good, but secondary |
| 4.0 existed | **Primary** — 530/540 service manual and 9020 CE handbook both gate boot-loader ROM revisions on "UNIX 4.0" | certain |
| 5.01 existed | **Primary** — the acquired tape is `16-USER-05.01` | certain |
| 5.3 was the last | secondary only | probable, unverified |

So the range is well attested at the top end and by secondary sources at the
bottom, but no primary document in hand names 1.0, 2.x, 3.x, 5.2 or 5.3 for
the Series 500 specifically.

**Practical target: 5.01**, since that is the version actually in hand. It also
satisfies the Rev B boot loader ROM constraint (which requires 4.0 or later),
as does BASIC 2.0. *(Superseded: 5.30, from the 7914 unit save, is in hand
and is the target that runs. 5.01 reaches its banner on the 520.)*

### Availability of other versions

A survey of bitsavers (documentation *and* the `bits/` software tree),
archive.org and the HP Computer Museum found **no other Series 500 HP-UX
version online in any form**. Bitsavers' `HP_9000/HPUX_5/` is Series 200/300
media, not Series 500. The museum's item 534 is the only Series 500 HP-UX image
known to be public, and it is the damaged tape we already have.

If a Series 500 HP-UX other than 5.01 turns up, it will almost certainly come
from a private collection rather than an existing archive.

*(Corrected: one did turn up in an existing archive. Bitsavers'
`bits/HP/Crisis_Computer_Tapes/ccc_20050830/S500/` holds a two-tape unit
save of an HP 7914 disc with an installed HP-UX 5.30 (97089C) system, the
last release for the Series 500, plus a 5.05 customer update, a contrib
tape, a BASIC 2.0 disc backup and Microtrak. This survey had looked at
bitsavers' documentation and `HP_9000/HPUX_5/`, not at that collection.
All of it is in `software/cct`.)*

## Status summary

| Item | Acquired | State |
|---|---|---|
| HP-UX 5.30 installed system (7914 unit save, `cct/`) | **yes** | **intact**; boots multiuser on the emulated 540 |
| HP-UX 5.01 system tape (`97089C.hpi`) | **yes** | **77% lost** — kernel intact, userland gone |
| HP-UX kernel (`os.req`, incl. `SYSTEM POWERUP`) | **yes** | complete, from the tape |
| BASIC Language System 2.0 (7 discs) | **yes** | **undamaged** |
| 9020 System Functional Tests 3.00 (6 discs) | **yes** | **undamaged** |
| 9020 BASIC demo suite (7 discs) | **yes** | **undamaged** |
| SCM loader ROM | no | mechanism documented, contents missing |
| HP-UX Technical BASIC | no | runs under HP-UX; low priority |

Everything acquired is in [../software/](../software/), with provenance and a
detailed damage analysis in [../software/README.md](../software/README.md).

## Target machines: 520 and 540

**Decision: target both the 520 and the 540.** The 540 is the primary goal; the
520 comes along nearly free and unlocks BASIC.

BASIC is not a valid target for the 540 on its own. The 520 shipped as an
integrated workstation and could boot either HP BASIC or HP-UX; the 530, 540
and 550 were sold as HP-UX-only, and the 540 has no built-in display or
keyboard for the BASIC system to use.

The two machines share the entire processor stack — same FOCUS CPU, same MPB,
same IOP, same select-code layout. The deltas are:

| | 520 (9020) | 540 (9040) |
|---|---|---|
| Packaging | integrated workstation | free-standing cabinet |
| Display | built-in (9020A 12″ colour 512×390, 9020B 12″ mono 560×455, 9020C 13″ colour 560×455) | external terminal |
| Keyboard | integrated | external |
| Boot loader | may be on a **Bootstrap Loader card** rather than the SCM | SCM |
| OS | BASIC or HP-UX | HP-UX only |

Note the loader difference: the 520 CE handbook describes a *Bootstrap Loader
card* whose ROM is replaced separately, whereas on the 530/540 the loader ROM
sits on the SCM. The select-code scan that finds the loader is the same in both
cases, so a simulator can treat this as one mechanism with two placements.

The 520 also happens to be the machine the acquired diagnostics were written
for — the TD0 images are all 9020 part numbers — which makes the 520 the
natural **first** boot target even though the 540 is the headline goal.

There is also **HP-UX Technical BASIC** (1986), a different product from the
520's System BASIC, which runs as an application under HP-UX. Not a substitute,
but worth having later.

## HP-UX 5.0 for the 500 Series

- HP Computer Museum, software item **534**:
  <https://www.hpmuseum.net/display_item.php?sw=534>
- Format: `.hpi` tape image (`97089C.hpi`), ~1.417 MB, imaged from a 150 ft tape.
- **Known damaged**: the museum recorded **47 read errors** during imaging and
  states it is probably not entirely intact. Contents remain listable via the
  BASIC `CAT` command from a 300 Series machine.
- Intended use on real hardware: `hpdrive -9144-150 97089C.hpi`, which presents
  a virtual **9144 cartridge tape drive** at HP-IB address 0.

**Blocker:** the download goes through
`https://www.hpmuseum.net/capcha/freecap_wrap.php?s=534`, which is a CAPTCHA
wrapper — a scripted fetch returns the HTML challenge page, not the image. This
one needs a browser.

Given the 47 read errors, it is worth asking the museum whether a cleaner
image has surfaced since, and worth looking for a second copy before building
tooling around this one.

## Other 500 Series software at the museum

Exhibit: <https://www.hpmuseum.net/exhibit.php?swc=21>

| ID | Title | Year |
|---|---|---|
| 467 | 9020 Demonstration Software | 1982 |
| 432 | 9020 System BASIC Software | 1983 |
| 433 | 9020 Test Software | 1983 |
| 434 | 97052B 9020 3D Graphics Binary | 1983 |
| 534 | HP-UX 5.0 Operating System | 1984 |
| 570 | HP-UX Technical BASIC | 1986 |

**433, 9020 Test Software**, is disproportionately valuable: diagnostics
exercise architectural corners that ordinary software never touches, which
makes them the best available conformance suite for a CPU simulator.

## Media formats

- **`.hpi`** — the HPDir project's image format. A 1:1 dump of the medium's
  records, exactly as `dd` would produce: a linear array of 256-byte records,
  with no drive geometry encoded. Straightforward to consume.
- **LIF** (Logical Interchange Format) — HP's device-independent filesystem,
  used across Series 80/200/300 and the calculators.
- **SDF** (Structured Directory Format) — what the Series 500 itself uses.

Reference: [The HPDir Project](https://www.hp9845.net/9845/projects/hpdir/),
which documents both the format and the `hpdrive` drive emulator. Its drive
emulation model is a good guide for the simulator's mass-storage layer.

## The loader ROM question

No dump of the SCM's 8 KB bootstrap loader ROM (09020-80000 Rev A /
09020-80001 Rev B) is known to exist. A search turned up only manual
references to the part numbers, no image.

This is **less of a blocker than it first appears**, because the service manual
documents the mechanism completely — the select-code scan, the `84` hex reset
to `R1`, the DMA from `R4`/`R6` with auto-decrementing address, and the full
loader flowchart. See
[HARDWARE.md §2.5](HARDWARE.md#25-bootstrap-protocol-530540-service-sheet-2-302-31).

What the ROM *does* is also modest: scan IOP 1–3 at peripheral addresses 0–7,
find an OS, load it, jump to it. Three ways to get past it:

1. **High-level emulation — skip the ROM.** Implement the loader's *effect* in
   host code: parse the media image, place the OS in memory, set up register
   state, jump. Standard emulator practice, and the cheapest route. **This is
   what the MAME driver now does**, reading the volume header and boot
   descriptor off whatever discs are in the drive and pulling every byte
   through the disc controller's own Locate-and-Read path — so the machine
   boots from its media rather than from an image handed in around the side.
2. **Reimplement in FOCUS assembly** from Figure 2-17. More faithful; needs an
   assembler first, and still needs the handoff contract below.
3. **Dump from real hardware.** Definitive. Needs a machine, or someone who has
   one.

All three converge on the same genuine unknown, which the documents do not
answer:

- what on-media signature the loader recognises as "an OS"
- what address the OS image is loaded at
- what machine state — registers, CST/DST, privilege mode — exists at *Jump to OS*

These are recoverable by disassembling the HP-UX boot image, because the image
necessarily conforms to whatever the loader expects. That work needs a
functioning CPU core and disassembler — which is exactly what the first phases
of the project produce. **The dependency ordering is favourable: by the time
the ROM matters, the tools to reverse-engineer around it already exist.**

**This got substantially easier — twice over.** Both acquired systems begin
with a module named **`SYSTEM POWERUP`**: the HP-UX kernel (`os.req`, at
offset `0x20`) and the BASIC 2.0 boot disc (`500BT1`, at `0x820`). Both then
proceed through `TRAP HANDLERS` and `STKPSTK IOSEGS`.

That gives **two samples of the same entry convention**. Whatever both assume on
entry *is* the handoff contract; it can be cross-referenced rather than guessed.
The ROM's own instructions remain missing, but the OS side — the half that
actually defines the interface — is fully in hand.

They are more than merely similar: disassembly turned up a **395-byte
byte-identical block** shared between the two powerup modules —
`os.req 0xe0..0x26b` and `500BT1 0x8d8..0xa63` — containing a common dispatch
routine. The two systems are built from shared code at this level, which is
consistent with both sitting on the Series 500's SUNOS layer, and it makes the
cross-reference argument stronger than two independent implementations would.

The rest of the modules diverge (roughly 10% incidental byte agreement outside
that block), so this is a shared routine rather than a shared module.

See [../software/README.md](../software/README.md#the-boot-module-format-the-important-structural-finding)
for the module maps.

The BASIC utility disc also carries a program called **`BOOT_TO_FILE`** (Rev
1.00, on `500UT3`), which writes a boot image to a file. That is worth
disassembling early: it should expose the boot image layout from the system's
own perspective, and may suggest a route to capturing the loader ROM itself.

Nothing about the CPU core, memory and segmentation, traps, or the IOP depends
on the ROM at all.

## Booting order

Given what survived, the sensible sequence is:

1. **9020 diagnostics** (undamaged, self-booting, 520 target) — exercises the
   CPU hard with minimal dependencies. Best first boot target and the best
   conformance suite available.
2. **BASIC 2.0** (undamaged, complete, 520 target) — a full, self-contained
   system that actually reaches an interactive prompt. **This is the first
   realistic "it boots and I can use it" milestone**, and it does not depend on
   the damaged tape at all.
3. **HP-UX kernel bring-up** — `os.req` is complete, so the kernel can be
   loaded and single-stepped, though it will stop when it reaches `init`.
4. **BASIC demos** — graphics and I/O workout once console and display exist.
5. **Full HP-UX** — blocked on a better tape image; `SCOMMANDS`, the entire
   userland, is unrecoverable from the copy we have. *(Superseded: the
   HP-UX 5.30 unit save is that image, and it runs multiuser.)*

Note the shape of this: **the damaged tape is not on the critical path.** The
undamaged BASIC and diagnostic discs carry the project through its first three
milestones, and they target the 520 — which is the machine the media was
written for.

## The BASIC demonstration discs

`software/demos/BASDEM1-7.TD0` are HP's seven-volume BASIC 2.0 demonstration
set, October 1982. All seven decode and mount, and the emulator reads their
file system correctly -- `CAT` inside BASIC agrees with `tools/sdfread.py`
run over the same image offline, directory for directory.

Each disc carries a `/DEMO` directory with an uppercase `MAIN_MENU` and a
crowd of lowercase-named programs; disc 2 also has `/DEMO/DIR/games/` with
`BREAKOUT`, `MASTERMIND` and `CUBIE`.

**Where they get to.** `LOAD` works and `LIST` works -- a demo's source
prints on the console, line numbers and all:

```
1510    Y=45*SIN(Ang)
1520*   MISSING OPTION # 48
```

`RUN` does not. Every stored program tried so far stops at the same place:

```
System halted due to system error. TRAP:00000021 00001578 00064001 00000099
```

The trap parameters are **identical** for a 3.7 KB text game (`MASTERMIND`)
and a 12.5 KB menu program (`MAIN_MENU`), and identical again whether the
machine boots plain BASIC 2.0 or the option-rich diagnostics disc built by
`tools/mkdiagdisc.sh`. This is the typed-program scaffold failure the README
describes, reached by a second, independent route: not a demo-specific
problem and not a missing option.

The control matters as much: **`CAT_BOOT` still runs** -- banner, copyright
and its interactive device prompt -- so the failure is not "stored programs
do not run", it is narrower than that.

**Two things get in the way before that, both worth knowing.**

*The letter `e` cannot be typed in lower case.* Everything else can:
typing `abcdefghijklmnopqrstuvwxyz` at the BASIC prompt produces
`abcd{fghijklmnopqrstuvwxyz`. The keyboard translation table in the
diagnostics image (`diag.bin` 0x23614) says why -- unshifted keycode 0x2a is
`E`, and the shifted half maps 0xaa, which is what `code | 0x80` produces,
to `{`. There is no shifted slot that yields lower-case `e`.

This matters because most demo file names are lower case, and `games` has an
`e` in it. Two ways round it, both tried:

- Build the name in BASIC: `LOAD "DEMO/DIR/gam"&CHR$(101)&"s/..."`. The
  string concatenation itself stops at `TRAP:00000030`, with
  `2F67616D 2F444952 44454D4F` -- "DEMO/DIR/gam" -- on the stack.
- Rename the directory in the image. Uppercasing the six occurrences of
  `games` in `BASDEM2` leaves the file system valid (`sdfread.py` reports
  `corrupt=0`) and makes the whole path typeable. This is what got
  `MASTERMIND` as far as its trap.

The source comment in `hp9000_5xx.cpp` still says "LOWER CASE IS NOT
REACHABLE YET" and that shifted letters "type unrelated characters". That
was true when it was written and is not any more: twenty-five of the
twenty-six work. Only `e` is left.

*The graphical demos cannot work yet regardless.* `MAIN_MENU` and most of
the lowercase programs draw, and no graphics display is modelled
(HARDWARE.md 2.7) -- the same wall HP's own diagnostics hit at `ERROR 702`.
`MASTERMIND` was chosen for the attempt because it is the one likely to be
pure text.

**What separates the programs that run from the ones that trap.** Reading
the stored files directly with `tools/progfile.py` -- extract a disc with
`tools/sdfread.py <image> --extract <dir>` first -- shows every program's
subprogram count and the size of its transfer table:

| program | subprograms | table | bytes | RUN |
|---|---:|---:|---:|---|
| `CAT_BOOT` (fixed disc) | 20 | **21** | 22,100 | runs, prompts |
| `BUILD_BOOT` (fixed disc) | 36 | **25** | 56,238 | not tried |
| `introduction.c` | 4 | **5** | 13,874 | runs, `ERROR 3` |
| the ~380-byte stubs | 1 | **2** | ~380 | runs, `ERROR 3` |
| `bplot_control` | 2 | 0 | 1,105 | runs, `ERROR 3` |
| `MASTERMIND` | 2 | 0 | 3,780 | **TRAP 021** |
| `BREAKOUT` | 7 | 0 | 8,979 | not tried |
| `CUBIE` | 9 | 0 | 12,882 | not tried |
| `MAIN_MENU` | 15 | 0 | 12,595 | **TRAP 021** |

**Those `ERROR 3` rows are not what they look like, and a later measurement
overturned them.** `music`, `introduction.c` and `bplot_control` do not run
and then report a BASIC error; they never load. `LOAD` answers

    ERROR 62: File is protected or wrong PROTECT code

and the `ERROR 3` that follows is `RUN` finding nothing in memory. The first
screen dumps showed only the last line, so the `ERROR 62` above it was
missed. Any conclusion drawn from those three "running" is void.

Corrected, the correlation runs the other way: the two files that load are
the two with **no** transfer table, and the two that refuse are the two that
have one. `CAT_BOOT`, with 21 entries, loads and runs from the fixed disc, so
"has a table" is not the whole story — see DIAGNOSTICS.md.

What survives is narrower and more useful: **the trap needs a real
run-time call into a subprogram whose transfer-table entry was never
built.** A program that ships with its table populated (`CAT_BOOT`, 21
entries) resolves its calls without the scaffold and runs. A program that
ships with none has to have them linked as it goes, and that is the
mechanism that fails. A program that errors out before its first call
never exercises either path.

That makes `MASTERMIND` the cheapest reproducer the project has for the
scaffold trap: 3,780 bytes, two subprograms, and it fails in the same
instruction with the same parameters as HP's 22 KB diagnostics driver.

**Iterating LIST works, with a caveat.** `LIST 1,25` lists the range and
prints its `AVAILABLE MEMORY` trailer correctly -- then the machine takes a
bounds violation, `TRAP:00000001`, before another command can be typed. So
a program can be read out of the machine a window at a time, but at one
window per boot. Reading the file offline with `progfile.py` is the faster
route and is what the table above came from.

`FOCUS_DIMCELLS` is the way to capture that output: it logs every character
cell written, so text that scrolled off is still recoverable. Replaying the
log in write order, breaking a line wherever the address is not one past
the last, reconstructs what was printed.

## What is still missing

1. **SCM loader ROM** — see above. Soft blocker, late in the schedule.
2. **Memory Controller ERS.** Referenced by both ERSes but not on bitsavers.
   Narrow in practice: the software-visible MPB semantics are covered by the
   I/O ERS and the Instruction Set ERS (HARDWARE.md §4); what is lost is
   bus-protocol detail and the MC's own channel-0 command set.
3. **A clean HP-UX 5.0 image.** *(Less pressing now: 5.30, the last
   release, is in hand whole. A clean 5.01 would still give the 520 a
   userland.)*
