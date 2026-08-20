# Target software and media

The end goal is booting original Series 500 software. This tracks what exists,
where it is, and what still has to be obtained.

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
as does BASIC 2.0.

### Availability of other versions

A survey of bitsavers (documentation *and* the `bits/` software tree),
archive.org and the HP Computer Museum found **no other Series 500 HP-UX
version online in any form**. Bitsavers' `HP_9000/HPUX_5/` is Series 200/300
media, not Series 500. The museum's item 534 is the only Series 500 HP-UX image
known to be public, and it is the damaged tape we already have.

If a Series 500 HP-UX other than 5.01 turns up, it will almost certainly come
from a private collection rather than an existing archive.

## Status summary

| Item | Acquired | State |
|---|---|---|
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
   state, jump. Standard emulator practice, and the cheapest route.
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
   userland, is unrecoverable from the copy we have.

Note the shape of this: **the damaged tape is not on the critical path.** The
undamaged BASIC and diagnostic discs carry the project through its first three
milestones, and they target the 520 — which is the machine the media was
written for.

## What is still missing

1. **SCM loader ROM** — see above. Soft blocker, late in the schedule.
2. **Memory Controller ERS.** Referenced by both ERSes but not on bitsavers.
   Narrow in practice: the software-visible MPB semantics are covered by the
   I/O ERS and the Instruction Set ERS (HARDWARE.md §4); what is lost is
   bus-protocol detail and the MC's own channel-0 command set.
3. **A clean HP-UX 5.0 image.**
