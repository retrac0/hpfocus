# Software images

**Provenance: two sources.** `cct/` -- the HP-UX 5.30 disc unit save and
four other Series 500 images -- comes from bitsavers' *Crisis Computer
Tapes* collection (see [`cct/`](#cct--hp-7914-disc-unit-save-hp-ux-530)
below and `cct/readme`). **Everything else in this directory came from the
HP Computer Museum, <https://www.hpmuseum.net/>.** Their 500 Series
software collection is at
<https://www.hpmuseum.net/exhibit.php?swc=21>; downloads are CAPTCHA-gated and
were fetched by hand. Most of the collection was donated to the museum by
**Charlie Brett of Fort Collins, Colorado** (November 2010) — without that
donation none of this would exist.

The museum asks to be contacted by anyone holding 500 Series hardware or
software. If any gap below gets filled, send the result back to them.

A first survey of other sources found nothing: bitsavers hosts Series 500
*documentation*, and its `HP_9000/HPUX_5/` is Series 200/300 media. It
missed `bits/HP/Crisis_Computer_Tapes/`, which is where `cct/` came from.
The museum and that collection are the only public sources of Series 500
software known to this project.

```
software/
  97089C.hpi        HP-UX 5.01 system tape (damaged — see below)
  basic/            BASIC Language System 2.0, 7 discs
  demos/            BASIC demo suite, 7 discs
  test/             9020 System Functional Tests 3.00, 6 discs
  cct/              HP-UX 5.30 7914 unit save (2 tapes) + 4 more images,
                    from bitsavers' Crisis Computer Tapes
```

**On what terms.** These images are Hewlett-Packard's copyrighted
software; HP-UX also carries AT&T and University of California code, and
its boot banner prints HP's restricted-rights legend. They are here so the
emulator has something to run. This project has no licence for them from
HP and states no terms of its own for them. **The HP-UX 5.30 unit save is a
real site's working system from 1991**, and it holds that site's user
accounts, home directories and mailboxes as they were. Please treat them
accordingly: run the system, but do not republish what you find there.

**Derived files.** Nothing here is ever written by the tools. What they
derive is gitignored: the HP-UX machines in `var/` (tools/lib/hpux.sh),
the 5.01 kernel `cct/hpux501.bin`, raw decodes in `basic/img/`, and the
fixed discs in `hd/` (tools/setup.sh builds `hd/basic.img`,
tools/mkdiagdisc.sh `hd/diag.img`).

All disc images are Teledisk 2.1, 5.25″ DS/DD, **35 cyl × 2 heads × 16 sectors
× 256 bytes = 286,720 bytes** each, signature `TD` (no LZH stage). Every one
decodes cleanly with `tools/td0.py` — **1120/1120 sectors recovered, zero
errors, on all 20 discs**. Only the tape is damaged.

---

## The boot module format (the important structural finding)

Both the HP-UX kernel and the BASIC system are built from modules introduced by
a **16-byte space-padded ASCII name field**. `tools/modmap.py` lists them.

The two systems open the same way:

| HP-UX `os.req` | BASIC `500BT1` |
|---|---|
| `0x000020` **SYSTEM POWERUP** | `0x000820` **SYSTEM POWERUP** |
| `0x001084` CONFIGURATION | — |
| `0x003140` **TRAP HANDLERS** | `0x001860` **TRAP HANDLERS** |
| `0x003e25` **STKPSTK IOSEGS** | `0x00248d` **STKPSTK IOSEGS** |

**`SYSTEM POWERUP` is the entry point the SCM loader ROM jumps to.** Having it
in two independent, unrelated system images is the single most useful thing
here: the loader→OS handoff contract — previously the project's main unknown —
can be cross-referenced between them rather than guessed at. Whatever both
images assume on entry *is* the contract.

Full HP-UX kernel module map (35 modules) via
`python3 tools/modmap.py os.req --min-gap 256`; highlights:

```
SYSTEM POWERUP   CONFIGURATION    TRAP HANDLERS    STKPSTK IOSEGS
CORE SYSTEM      HP-UX ENTRYPTS   HP-UX KERNEL     HP-UX DIRECTORY
SDF / SDF_DISC   MEMORY           MEMORY INTRINSICS
INTERNAL_DISC    CS80             HP27110 (HP-IB)  TTY SUBSYSTEM
POWERUP SELFTEST MATH LIBRARY     HP-UX IO DRIVERS THERMAL_PRINTER
```

---

## `97089C.hpi` — HP-UX 5.01 system tape

16,744,448 bytes = 65,408 LIF blocks of 256 bytes, imaged from a 150 ft tape.

- **Product `16-USER-05.01`, tape sequence 1** (from `CONTENTS`)
- LIF volume `HPUX1`, created **1985-08-08 16:09:17**
- 15 files — `python3 tools/lifdir.py software/97089C.hpi`

On real hardware: `hpdrive -9144-150 97089C.hpi`, presenting a virtual 9144
cartridge tape at HP-IB address 0.

`GETTOOLS` is a Bourne script (`@(#)gettools 1.2 85/04/22`) that uses `lifcp`
to pull `TOOLS2` and unpacks it with `cpio -idmuv`. The `S*` filesets are
therefore **cpio archives** — old-binary format (magic 070707) with
**big-endian** 16-bit fields, because FOCUS is big-endian. GNU cpio assumes
host byte order and cannot read them; use `tools/cpiolist.py`.

### Damage: far worse than the museum's note

The museum records **47 read errors** and says the image is "probably not
entirely intact". That understates it, and the damage is not 47 scattered bad
sectors.

**The image is truncated.** The last block with any nonzero byte is **16,315**;
everything from 16,316 to the end — **12.57 MB, 77% of the tape** — is zero
fill. The imaging run stopped rather than skipping bad spots.

| Fileset | Blocks | Intact | Status |
|---|---|---|---|
| `GETTOOLS` | 6+2 | 100% | OK |
| `TOOLS1` | 8+185 | 98.9% | truncated tail |
| `TOOLS2` | 193+824 | 98.2% | truncated tail |
| `CONTENTS` | 1017+6 | 100% | OK |
| **`S97080C`** | 1023+1892 | **100%** | **complete** — the 520 kernel |
| `S97089C` | 2915+1974 | 99.5% | cpio sync lost at 99.1%; `os.req` extracts but does not verify |
| `SUPDT_TLS` | 4889+978 | 94.4% | cpio unreadable from offset 0 |
| `SHPUX_MAN` | 5867+8080 | 100% | OK — 2 MB of manuals |
| `SSTARBASE` | 13947+7106 | 31.6% | truncated |
| `SPAM` | 21053+162 | **0%** | gone |
| `SKERNTOOLS` | 21215+46 | **0%** | gone |
| `SFTN_CORE` | 21261+784 | **0%** | gone (Fortran) |
| `SPAS_CORE` | 22045+496 | **0%** | gone (Pascal) |
| `S97084A` | 22541+6486 | **0%** | gone |
| **`SCOMMANDS`** | 29027+32814 | **0%** | **gone — 8.4 MB, the entire userland** |

### What survived, and why it matters

**There are two kernels on this tape, and only one of them is usable.**
`S97080C` is the **520** build — its `os.rev` reads `HP-UX Model 520
Release 05.01 (97080C)` — and all sixteen of its segments verify.
`S97089C` is the **530/540/550** build, and although the cpio sync does not
break until 99.1%, well past `os.req`, so the file extracts at its full
353,510 bytes, its `CORE SYSTEM` segment does not checksum and no other
word position in the segment does either: the damage is inside the segment
body. The two are different builds (the 520's `CORE SYSTEM` is four bytes
shorter), so the good one cannot substitute. **HP-UX 5.01 therefore runs
here on the 520, not the 540.**

`S97080C` is whole, and contains:

```
system/97080C/os.req        363,566 bytes   <- the HP-UX kernel
system/97080C/*.opt         13 driver modules
system/97080C/customize     3,124 bytes
system/97080C/os.rev, revlist
```

The `.opt` driver modules name the supported peripherals: `HP27128`,
`HP27140`, `HP27112`, `HPIB_FLEX`, `SERIAL`, `CIPERLP`, `HP268X`, `HP7970`,
`HP7974`, `HP9885`, `HP97062_GR`, `HP98770_GR`, `MEMORY_VOL`. (`S97089C` is the
same product for a different display, carrying `HP98700` instead of
`HP98770_GR`.)

`os.req` carries the kernel tunables in its `CONFIGURATION` module:
`root_device`, `vm_device`, `page_size`, `cache_buf_size`, `cache_buf_num`,
`max_processes`, `max_proc_per_usr`, `stack_size`, `swap_time`,
`page_swap_time`, `work_set_ratio`, `interactive_time`, `read_ahead_level`,
`vm_pool_size`, `gpcyl_size_bits`, `blks_per_phys`, `num_init_groups`,
`segment_num`, `local_time_zone`, `running_secured`, `keep_stats`,
`zero_fill_inhibt`, `scroll_pages`.

**The loss that matters:** `SCOMMANDS` is entirely gone — `/bin` and `/usr/bin`,
the shell and every command. A kernel with no userland boots as far as trying
to start `init`, then stops. This image supports **kernel bring-up and
validation, not a usable HP-UX session.**

### Making `os.req` bootable — and booting it

All sixteen segments verify, so the 520 kernel itself is undamaged. But the
stored file is what `osmgr` *installs*, not what the loader hands to the
CPU: its segment chain simply ends, with no negative terminator word, so
`SYSTEM POWERUP` walks straight past the last segment and checksums
whatever follows in memory. The chain ends at `0x58c2c` (the file's last
two bytes are padding), so a bootable image is the file truncated there
with `ffffffff` appended — and the `.opt` driver modules can be spliced in
before the terminator, because each is a chain module whose own checksum
already covers exactly its own bytes. `tools/hpuxboot.py` does both:

```sh
python3 tools/hpuxboot.py software/97089C.hpi --list
python3 tools/hpuxboot.py software/97089C.hpi -o hpux501.bin --option HPIB_FLEX
tools/runhpux.sh --release 5.01 -str 100 -video none | python3 tools/screen.py
```

**That reaches the HP-UX copyright banner on the emulated 520**, with the
`9020 HUMAN INT` console driver running the display as a terminal, once
the machine is given an HP-IB card with a disc on it — without one the
kernel raises escape 116 against the NULL driver and halts. It then loops
without starting `init`, which is unsurprising given that there is no 5.01
userland for it to start. `docs/HPUX.md` section 11 is the full account.

To close the remaining gap: ask the museum whether the original tape can be
re-read (the 2013 file date suggests a single pass long ago), or find
another copy of the 97089C tape. A better read would give back both
`SCOMMANDS` and an undamaged 530/540 kernel.

---

## `basic/` — BASIC Language System 2.0 (7 discs)

Museum item **432**, "97050B 9020 BASIC System Discs". Product line **97050**.
Every disc carries an internal volume header `97050-<part>R0200` matching its
label, so revision **2.00** throughout.

| File | Contents | Part | Internal header |
|---|---|---|---|
| `500BT1.TD0` | BASIC LANGUAGE SYSTEM 2.00, boot disc 1/2 | 97050-11024 | `97050-11024R0200` |
| `500BT2.TD0` | BASIC LANGUAGE SYSTEM 2.00, boot disc 2/2 | 97050-11025 | `97050-11025R0200` |
| `500OPT1.TD0` | Optional binaries / boot utilities 1/2 | 97050-11026 | `97050-11026R0200` |
| `500OPT2.TD0` | Optional binaries / boot utilities 2/2 | 97050-11034 | `97050-11034R0200` |
| `500UT1.TD0` | Utility programs 1/3 | 97050-11027 | `97050-11027R0200` |
| `500UT2.TD0` | Utility programs 2/3 | 97050-11028 | `97050-11028R0200` |
| `500UT3.TD0` | Utility programs 3/3 | 97050-11029 | `97050-11029R0200` |

(The Teledisk comment on `500OPT1` says 97050-11034, but its internal header
says 97050-11026 — a typo by whoever imaged it. Trust the internal header.)

**Revision 2.00 is exactly what the Rev B boot loader ROM demands** — the
service manual states 09020-80001 requires "UNIX 4.0 or Basic 2.0". So this
BASIC and a Rev B loader are a matched, bootable pair.

`500BT1` sector 0 is a boot header: a 4-byte field, the ASCII part number
`97050-11024R0200`, then geometry/pointer words. Volume label reads
`BASIC LANGUAGE SYSTEM  REV 2.00 ... Vol 1 of 2`.

Modules identified in `500BT1` include `SYSTEM POWERUP`, `TRAP HANDLERS`,
`STKPSTK IOSEGS`, `BASIC IO/MM RTS`, `MEMORY`, `SDF`, `CS80 / INTERNAL`,
`AVAILABLE MEMORY`, `AUTOBN`, `BASIC COMPILER`, and `CONSOLE`.

Recovered runtime strings show the halt/diagnostic paths a simulator will hit
first, which makes them useful bring-up checkpoints:

```
System Halted-> MC trap
System halted due to double bit memory error on MC #
System halted due to system error. TRAP:
Self test error 1: IO address AAA,UUUU STATUS:
```

There is also an SDF directory-listing template — `FILE NAME / LEV / TYPE /
RECORDS / LENGTH / DATE / TIME / ACC / STAT`, `AVAILABLE SPACE:`, `FORMAT:`,
`LABEL:` — and the SDF file-type and device-type tables:

```
types:   BIN  PROG DSET ROOT ASCII BDAT DIR  BCD  DATA
devices: 9825 9845 250  9831 9835  98X6 9845 9000 HPUX
```

Named utility programs found on the other discs:

| Disc | Program | Rev |
|---|---|---|
| `500UT1` | `DISC_BACKUP` | 2.19 |
| `500UT3` | `BOOT_TO_FILE` | 1.00 |
| `500OPT1` | `FIND_PASSWORD` | 1.09 |
| `500OPT2` | `CERTIFY_TAPE` | 1.0 |

**`BOOT_TO_FILE` is worth early attention.** A utility that writes a boot image
to a file is likely to expose the boot image layout from the system's own point
of view — and may offer a route to capturing the loader ROM.

---

## `demos/` — BASIC demo suite (7 discs)

Museum item **467**, "9020 Demonstration Software" (1982), product line 11141.

| File | Contents | Part |
|---|---|---|
| `BASDEM1.TD0` | 9020 BASIC DEMO, main disc | 11141-10703 |
| `BASDEM2.TD0` | 9020 BASIC DEMO, disc 2 | 11141-10709 |
| `BASDEM3.TD0` | **9030** BASIC DEMO, disc 3 | 11141-10704 |
| `BASDEM4.TD0` | 9020 BASIC DEMO, disc 4 | 11141-10705 |
| `BASDEM5.TD0` | 9020 BASIC DEMO, disc 5 | 11141-10706 |
| `BASDEM6.TD0` | 9020 BASIC DEMO, disc 6 | 11141-10707 |
| `BASDEM7.TD0` | 9020 BASIC DEMO, disc 7 | 11141-10708 |

Disc 3 is labelled **9030** rather than 9020 — probably a label error given the
contiguous part numbers, but worth checking once discs can be mounted.

These need BASIC already running, so they are a late-stage target. Their value
is as a graphics and I/O workout once the console and display are emulated.

---

## `test/` — 9020 System Functional Tests, Rev 3.00 (6 discs)

Museum item **433**, "9020 Test Software" (1983). Factory diagnostics.

| File | Contents | Part |
|---|---|---|
| `500MFTST.TD0` | Mainframe tests | 09020-11011 Rev 3.00 |
| `500MSTST.TD0` | Mass storage tests | 09020-11011 Rev 3.0 |
| `500EDTST.TD0` | External devices tests | 09020-11013 Rev 3.0 |
| `500ENV1.TD0` | Environment disc 1/3 | 09020-11021 Rev 3.00 |
| `500ENV2.TD0` | Environment disc 2/3 | 09020-11022 Rev 3.00 |
| `500ENV3.TD0` | Environment disc 3/3 | 09020-11023 Rev 3.00 |

**The most valuable asset here for CPU work.** Diagnostics deliberately
exercise architectural corners ordinary software never touches, which makes
them the best conformance suite available for a machine with no surviving
hardware to compare against. They are also plausibly an *easier* first boot
target than either BASIC or HP-UX — a self-booting diagnostic has far fewer
dependencies than a Unix kernel — and unlike the tape they are undamaged.

---

---

## `cct/` — HP 7914 disc unit save, HP-UX 5.30

Two gzipped tape images from the Crisis Computer Tapes collection,
<https://www.bitsavers.org/bits/HP/Crisis_Computer_Tapes/ccc_20050830/S500/>.
Each expands to 66,975,744 bytes; together they are a backup of a 132 MB
HP 7914 disc.

**These are not an install tape.** A "unit save" is a raw dump of a disc unit,
so what they hold is a *complete installed system* — which for our purposes is
considerably better.

**The filename understates them.** They are labelled `hpux4.0`, but the boot
image says:

```
HP-UX Model 530/540/550 Release 05.30 (97089C)
```

Release **5.30** is the last version HP shipped for the Series 500.

### Condition: intact

| | tape 1 | tape 2 |
|---|---|---|
| blocks | 261,624 | 261,624 |
| zero blocks | 15.1% | 13.1% |
| longest zero run | 4,092 | 2,708 |
| last non-zero block | 261,623 (the last) | 261,623 (the last) |

Both carry data to the final block with no trailing zero region, so neither is
truncated. The scattered zero blocks are ordinary filesystem free space. This
is a completely different picture from `97089C.hpi`, which stops dead at 23%.

### What is on them

- **A complete bootable HP-UX 5.30 image**, 611,740 bytes, described by a
  boot descriptor at offset `0` of tape 1 (not `0x400`, where a disc with an
  SDF volume header would put it) and starting at sector 18, mapping to 46+ modules — `SYSTEM POWERUP`, `TRAP HANDLERS`,
  `HP-UX KERNEL`, `SDF`, `CORE SYSTEM` and the rest.
- **The userland.** Around 7,500 Unix paths with a populated `/bin` —
  `awk`, `admin`, `asa`, `aterm` and hundreds more. This is exactly what
  `SCOMMANDS` on the damaged 5.01 tape was supposed to contain.
- Live user data: tape 2 opens with UUCP session logs.

### The rest of the collection

The same bitsavers directory holds four more Series 500 images, all now here.
`labels.txt` one level up transcribes the physical tape labels, which is what
identifies them — and it confirms the mislabel in the imager's own words:
*"7914 unit save #1 4.0 / HP-UX Model 530/540/550 Release 05.30 (97089C)
<< in blk 0 ???"*. They noticed too.

| Image | Size | Label | Format |
|---|---|---|---|
| `97089-10038r5.05` | 3.1 MB | HP-UX customer update, 520/30/40/50, rev 5.05, 1986 | tape; contains the same `gettools` script as `97089C.hpi`, but padded with `0xff` rather than a clean LIF volume |
| `97080-19234` | 16.7 MB | HP 9000 Series 500 **unsupported software / contrib**, Jul 1986 | **ASCII cpio**; GNU cpio reads it, but tape block padding interrupts the stream |
| `dskbkup_bas2.0_500` | 11 MB | BASIC 2.0 **disc backup**, 8/19/84 | disc image, `D# BASIC LANGUAGE SYSTEM REV 2.00` |
| `45524-10034r1.5b` | 1.7 MB | Microtrak for HP-UX, 45524U opt 022 rev 1.5b | cpio, opens `usr/bin/conv15` |

The sibling directories `ccc_9144tapes` and `ccc_9trkTapes_20050826` are
HP 1000 and HP 3000 material with nothing for the Series 500.

### What is actually on the disc

Counting structure markers across both tapes:

| Marker | tape 1 | tape 2 |
|---|---|---|
| module signature `e307fffc` | 55 | 66 |
| binary cpio (`070707`, big-endian) | 105 | 444 |
| ASCII cpio (`"070707"`) | 128 | 38 |
| SDF magic `07000000`, sector-aligned | 1 (spurious) | 0 |

So beyond the boot image there are around 120 loaded modules and several
hundred embedded cpio archives — the installed filesets.

The SDF row is worth reading carefully. Counting `07000000` anywhere in the
images gives 1,911 and 2,413, but those are unaligned coincidences in ordinary
data. Counted only where a volume header could actually begin — on a 256-byte
sector boundary — there is exactly one hit on tape 1, at block 33,130, and its
block-size and FA fields are garbage. **Neither tape has an SDF superblock.**
On tape 1 the reason is visible: the boot descriptor occupies offset 0, where
the volume header would otherwise sit.

One caution on the earlier "7,500 Unix paths" figure: many of those come from
newline-separated **manifest files** listing installed paths, not from
directory entries. The paths are real, but that count is not by itself proof
that every file's contents survive.

### One volume, spanning both tapes

The recurring directory names — `src` on 13 i-nodes, `unfmt` on 6 spread right
across tape 1, `fmt`, `bin` and `lib` in the same bands — look at first like
several volumes. Solving for a single FA base sharpens that into seven
candidates in an exact arithmetic sequence 62,960 records apart, which reads
as eight volumes of 7,870 blocks.

Both readings are wrong. There is one volume, it spans both tapes, and
nothing has to be fitted at all — the piecewise-linear "FA base" that earlier
work solved for does not exist.

**The two tapes are consecutive spans of one disc**, not the same block range
twice. The imaging log reports "blocks 1..65406" for each tape because those
are *tape* block numbers, which restart on every reel; the disc blocks they
carry do not:

| | disc blocks | image offset |
|---|---|---|
| tape 1 | 1 – 65,406 | `(block − 1) × 1024` |
| tape 2 | 65,409 – 130,814 | `(block − 3) × 1024` |

Tape 2 resumes two blocks after tape 1 stops, so disc blocks 65,407 and 65,408
were never imaged, along with block 0. The two skews are not a guess: they are
forced by the FA file's own extent list (below). Getting tape 2's origin wrong
by even one block makes its FA extents land on ordinary file data instead of
on FA records, which is an unambiguous test.

**i-node number is the FA record index, directly.** Root is i-node 1 and sits
at FA record 1. There is no base offset anywhere in the format. The appearance
of one was an artifact of not having the FA file's extent list; the regular
62,960-record stride that earlier work fitted is the spacing of the FA file's
own extents, and once those extents are read from the file itself the stride
stops being something to solve for.

### SDF, decoded

The Series 500 fs(5) and inode(5) manual pages document the format:
<https://www.typewritten.org/Manual/HP/HP-UX/5.00/man5/fs.html>.

```
block 0      superblock: s_format 0x0700, 16-byte volume name,
             s_blksz, s_boot/s_bootsz, s_fa, s_maxblk
s_fa         File Attributes file, 128-byte records
               record 0   i-node of the FA file itself
               record 1   i-node of the root directory
               record 2+  free map, then i-nodes and extent maps
directories  24-byte entries: 16-byte name, 4 reserved, 4-byte i-node
files        up to four extents in the i-node, more chained through
             extent map records (e_type 2)
```

[`tools/sdfread.py`](../tools/sdfread.py) implements this and recovers
**206 files under 215 names, from the 15 floppy volumes** that carry a
filesystem, including nested directories on the demo discs. Spot-checked
against content: `DEGAUSS` extracts to a BASIC program opening
`PROGRAM MAIN !RE-SAVE "DEGAUSS.S " ! TUE 1 AUG 1982 REV1.0` with HP's
copyright notice. Dates come back too, on the same `time_ios` reading the
7914 volume needed — 1982 across the demo suite, 1984 across the 9020 tests,
both matching their labels.

The other five discs — `500BT1`, `500BT2` and the three `500ENV` volumes —
are *pure boot volumes*, not failures to read: their superblocks give
`boot = 1 + 262` against `maxblk 263`, so the boot area is the whole volume
and the root directory is legitimately empty.

Validating this reader against all 20 discs turned up one real defect, since
fixed. *(Corrected during the alpha: what follows was itself a reader bug.
I-node 63 is `/DEMO/DIR/9845c_data/land`, an ordinary 9,980-byte data file;
the reader had not followed the FA file's own extents on multi-extent
volumes, and read the wrong record for it -- tools/sdfread.py now does.)*
`BASDEM7` carries a **stale i-node (63) that duplicates `/DEMO`'s
name, parent and extents** and is reachable as
`/DEMO/DIR/9845c_data/land`. A walk guarded by a global visited set descends
that alias first and claims `MAIN_MENU` and eight other demo programs under
it, then skips them on reaching the real `/DEMO` — so nine files came out
under a path that is not theirs, and `/DEMO` looked half empty. Guarding on
the ancestor stack instead breaks the loop without picking a winner: every
name is reported, `/DEMO` is whole again, and the alias is reproduced as a
hard link rather than a second copy.

The diagnostics disc reads as eleven named tests — `TEST`, `(6)STATUS`,
`(12)DISPLAY`, `(7)KEYBOARD`, `(10)FLOPPY`, `(15)WINCHESTER` and the rest —
which is the conformance suite the CPU work has been aiming at.

This also corrected an earlier misreading of the volume header: the field at
offset `0x1c` is **`s_blksz`**, not a directory pointer. It is 1024 on boot
discs and 256 on data discs, which is why treating it as an offset appeared
to work.

### The 7914 unit saves: raw images, but missing block 0

They *are* raw sector images — an earlier reading here that called them a
backup record format was wrong. The imaging logs settle it:

```
bytes per block: 1024
reading blocks 1..1...reading blocks 2..2... [through] 65406
```

Both tapes read blocks **1 through 65,406**, and 65,406 × 1024 = 66,975,744,
which is the image size to the byte. So each tape is a straight sector dump —
and **block 0, the superblock, was never read**. Not missed by the search:
absent by construction, one block before the start of the image.

Those counts are per-tape and both start at 1, which is what made the tapes
look like the same block range imaged twice. They are not: tape 2's blocks are
disc blocks 65,409–130,814, as the FA extent list proves.

Note that this and the boot descriptor at image offset 0 are consistent, not
in tension: under `offset = (block − 1) × 1024`, image offset 0 *is* disc
block 1. Block 0 is the missing superblock; block 1 is the boot area. A scan
of every 256-byte sector of both tapes for the `07000000` volume-header magic
returns one hit, at block 33,130, with a garbage block size and FA pointer —
confirming there is no second superblock hiding anywhere either.

### Entering the volume without its superblock

The superblock is missing, but nothing in it is actually needed, because the
FA file describes itself and can be found on structure alone. fs(5) fixes what
lives at the front of that file:

```
record 0   the i-node of the FA file itself
record 1   the i-node of the root directory
```

That is a signature, not a magic number, and it is searchable: two adjacent
128-byte records at the start of a block, the first describing a file with no
partial last block (`di_size == di_blksz × 1024`, which is what a file made
only of whole records must satisfy), the second a directory whose parent is
itself — which is what `/` is. The pair occurs **once** in 133 MB, at image
block 69,084:

```
record 0   di_blksz 2016   di_size 2,064,384 = 2016 × 1024   di_exmap 129
           extents (69087,17) (5011,124) (13005,124) (20999,124)
record 1   ftype 1 (directory)   di_parent 1   extent (1076,1)   408 bytes
```

Record 129 lies inside the i-node's own first extent, so those four extents
bootstrap the lookup of the extent map holding the other thirteen —
`(28993,124)` through `(124938,139)`. **389 blocks in the i-node plus 1,627 in
the map is 2,016, exactly `di_blksz`.** The file closes on itself, so the
extent list is proved rather than fitted.

The same seventeen extents settle the geometry. Each must land on 128-byte
records that type as SDF FA records, which ordinary file data does not; that
is what pins 1024-byte blocks and the two per-tape skews. Sixteen of the
seventeen come out 992/992 records typed. (The seventeenth is the file's first
extent, most of which is the free-block bitmap rather than records.) Block size
is independently confirmed by every i-node satisfying
`(di_blksz − 1) × 1024 < di_size ≤ di_blksz × 1024`.

### The full dump

[`tools/sdf7914.py`](../tools/sdf7914.py) does all of the above with no
constants baked in — it locates the FA file by signature and derives tape 2's
origin by validation. Run it over both tapes in order:

```
python3 tools/sdf7914.py tape1.bin tape2.bin --extract out/
```

It recovers **6,943 names — 6,062 distinct files and 881 hard links — in 434
directories, 62.9 MB**, every one under its real path. There is no `.unlinked/`
salvage bucket and no mis-filing risk, because nothing is inferred: each name
comes from the directory entry that holds it.

The walk is provably complete. The FA file holds 6,497 i-node records and the
walk from root reaches **6,496 of them**; the single exception is the FA file
itself, which by construction appears in no directory. No file touches the
two-block gap between the tapes. i-nodes claim 67,161 of 130,814 disc blocks —
the remaining 49% is free space, which is why so much of the image is zeros.

Hard links matter more here than they usually would: 421 i-nodes carry more
than one name, so a reader that dedupes by i-node silently loses **881 names**,
including `/bin/sh` (a link to `/bin/rsh`) and `/dev/console`. The tool emits
every name and reproduces the links with `link(2)`.

### The dates

Timestamps come back too, which matters because they date the machine's life
rather than just its files. inode(5) stores three, in two encodings:

| field | width | encoding |
|---|---|---|
| `di_atime` | 4 | ordinary big-endian Unix `time_t` |
| `di_ctime` | 8 | `time_ios` |
| `di_mtime` | 8 | `time_ios` |

`time_ios` had to be worked out, and turns out to be two big-endian 32-bit
words: **a Julian Day Number, then seconds since midnight**. Both halves are
independently checkable and both check out across the whole volume — all
12,994 populated fields have a seconds word inside `[0, 86400)`, which
arbitrary data would satisfy about 0.002% of the time, and the smallest day
number present is **2,440,588, which is exactly 1970-01-01**. Unset times sit
on that epoch rather than on zero; 13 entries do, and they are exactly the
ones you would expect — `/dev/console`, `/etc/utmp`, `/etc/wtmp`,
`/usr/adm/sulog`.

All 6,496 objects get their dates back, and the result is a coherent history:

```
1983-1986   older source and manual trees carried forward
1987-06-01  16:00:00 exactly, right across /bin -- the HP-UX 5.30 install
1988-05-24  ctime on most of /bin -- when this disc was built
1989-1991   per-file access times: the machine actually in use
1991-08-20  17:56 on /usr/adm/pacct, the process accounting file
```

That last date is the most useful single fact the timestamps give up: process
accounting is written continuously by a running system, so **1991-08-20 is
when the machine these tapes came off was shut down** — four years after the
software on it was installed.

What comes out is unmistakably real: `/etc/passwd` with `root:,..:0:1:The
Super User`, a `/.profile` stamped `@(#).profile 27.2 85/05/21`, 919 C source
files that are 100% printable, AT&T-copyright source, troff manual input,
FORTRAN, `ar` archives and NLS message catalogs — and a complete Series 500
`/usr/include`, whose `a.out.h` documents the HPOFF object format and confirms
the EDSP/GDSP/SSP pointer encodings that
[`docs/FOCUS.md`](../docs/FOCUS.md) §5.2 derives from the ERS.

`/new.kernel` extracts to a 565,522-byte boot image that `tools/modmap.py`
reads as **54 modules opening with `SYSTEM POWERUP` at `0x20`** — an
end-to-end check that the extents are chained correctly, since a single
misplaced block would break the module chain.

| | |
|---|---|
| `/bin` | 117 entries, 2.2 MB — `sh` `csh` `cc` `f77` `ed` `grep` `cpio` … |
| `/usr` | 5,814 files, 53 MB — source, manuals, spool, `/usr/include` |
| `/lib` | 87 files, 3.0 MB |
| `/etc` | 189 files, 2.2 MB |
| `/dev` | 544 device nodes |
| `/graphics` | 112 files, 11 MB — the Sketch workstation software |
| `/system` | 58 files, 2.9 MB — `97089C` and `HPUX` fileset trees |

### Reading it

**Linux cannot mount this.** Series 500 HP-UX uses **SDF**, HP's own
Structured Directory Format — not LIF, and unrelated to any filesystem Linux
has a driver for. There is no `mount -t` that will touch it.

Use [`tools/sdf7914.py`](../tools/sdf7914.py), which reads the filesystem
itself and gives the whole tree under its real paths. Two narrower routes
remain useful for cross-checking a single object:

1. **The embedded cpio archives** extract directly. GNU cpio handles the
   ASCII ones; [`tools/cpiolist.py`](../tools/cpiolist.py) handles the
   big-endian binary ones GNU cpio cannot read. Tape block padding between
   members needs skipping.
2. **The boot image** is extractable straight from the descriptor at offset
   `0` — though `/new.kernel` now also falls out of the filesystem walk.

### Consequences

This supersedes `97089C.hpi` as the HP-UX target. It also supplied a **third
independent `SYSTEM POWERUP` sample**, which confirms the entry point rule
exactly — BASIC 2.0, HP-UX 5.01 and HP-UX 5.30 all begin with the same nine
instructions at terminator + `0x1e`.

And it corrected a misreading: `e9c28206` is **not** a product identifier. It
is the boot image format magic, identical on the BASIC discs and here.

## Tools

| Tool | Purpose |
|---|---|
| `../tools/td0.py` | decode Teledisk images to raw sectors |
| `../tools/lifdir.py` | list/extract the LIF directory of `.hpi` images |
| `../tools/cpiolist.py` | list/extract big-endian old-binary cpio filesets |
| `../tools/modmap.py` | map named modules in a boot image |
| `../tools/sdfread.py` | read and extract an SDF filesystem (floppy volumes) |
| `../tools/sdf7914.py` | read and extract the 7914 unit-save volume |
| `../tools/bootimage.py` | reassemble a boot image from its volumes |

```sh
python3 tools/td0.py      software/basic/500BT1.TD0 --raw /tmp/BT1.img
python3 tools/lifdir.py   software/97089C.hpi
python3 tools/lifdir.py   software/97089C.hpi --extract S97080C --out /tmp/S97080C
python3 tools/cpiolist.py /tmp/S97080C
python3 tools/cpiolist.py /tmp/S97080C --extract system/97080C/os.req --out /tmp/os.req
python3 tools/modmap.py   /tmp/os.req --min-gap 256
```

---

## How the system is meant to be set up

The seven BASIC discs are not seven copies of one thing. They divide into a
bootable system, a set of options, and a set of utilities, and the difference
matters: **booting the two BT discs gives you the bare language system, which
is not how a configured machine runs.**

| Disc | Volume | What it is |
|---|---|---|
| `500BT1`, `500BT2` | 97050-11024/5 | The boot set. 1 KB blocks, a large boot area, no filesystem — a 372,932-byte image of 13 segments split across the two. |
| `500OPT1` | 97050-11026 | Option binaries: `IO`, `MS`, `SERIAL`, `HPIB_FLEX`, `HP27110`, `HP27112`, `LIF_DISC`, `9845_DISC`, `HP9885`, `HP7970`, `CIPERLP`, `MEMORY_VOL`, `XREF`, **`ERRORS_ENG`** (error message text), plus `BUILD_BOOT`, `CAT_BOOT`, `FIND_PASSWORD`. |
| `500OPT2` | 97050-11034 | More options: `HP268X`, `HP7974`, `CERTIFY_TAPE`. |
| `500UT1` | 97050-11027 | `DISC_COPY`, `DISC_BACKUP`, `DEGAUSS`, `CONVERGE`. |
| `500UT2` | 97050-11028 | `MAG_TAPE_COPY`, `BANNER`. |
| `500UT3` | 97050-11029 | `TRANSLATOR`, `CHANGE_SYS`, `MARK_BOOT`, `BOOT_TO_FILE`, `SET_NON_VOL_MEM`. |

### What HP intended

`BUILD_BOOT` on the option disc says it plainly:

> This is the BUILD BOOT utility. It will copy the BASIC boot from the source
> device (usually `:INTERNAL`) to the destination device (usually `:CS80,7`).
> BUILD_BOOT will either initialize the destination disc with a boot area, or
> it will use the existing boot area on the destination disc. After copying
> the boot, it will allow **any number of option binaries to be added to the
> boot area**. The binaries are read from the source device, and may be on any
> number of volumes.

So a working installation is: boot copied onto a hard disc, with the options
that machine needs spliced into its boot area. The option files are chain
modules in exactly the boot image's format — length at `+4`, 16-byte name at
`+28`, byte-sum checksum at `length+8` — verified against every binary on the
disc. They are not loaded at run time; they are part of the boot.

### What this emulator does about it

The emulated media is read-only, so `BUILD_BOOT` cannot write a destination
disc. **`FOCUS_BOOTOPT=<file>[,<file>...]`** stands in for it: the driver walks
the assembled chain to its end, splices the named option modules in before the
terminator, and boots the result. Appending them *after* the terminator instead
— the obvious thing, and the first thing tried — loads them into memory and
leaves them unregistered, with the code segment table unchanged.

```
FOCUS_BOOTOPT=/path/ERRORS_ENG,/path/IO,/path/MS tools/runbasic.sh
```

The chain walk reports what it found (`chain of 13 segments ends at 0x5b0c0`,
matching `tools/bootimage.py`), and the added modules show up as new code
segments — `MS` arrives as a 15,367-byte segment 21.

### Installing to a hard disc (previously not possible)

A real 520 could carry an internal Winchester, and the emulated one now can
too: `-hard1` fits a fixed disc as device 1 of select code 7, answering
identity 5 instead of the bare loader card's 8. With one fitted the machine
boots from it, as real hardware does — the loader circuitry lives on the
Winchester's own controller — and falls back to the flexible drive when the
fixed disc has no boot area.

```
mkdir -p software/basic/img software/hd
for d in 500BT1 500BT2; do
    python3 tools/td0.py --raw software/basic/img/$d.img software/basic/$d.TD0
done
tools/mkharddisc.py software/hd/basic.img
tools/installbasic.py software/hd/basic.img \
    --basic software/basic/img/500BT1.img software/basic/img/500BT2.img \
    --option /path/ERRORS_ENG /path/IO /path/MS
tools/runbasic-hd.sh
```

*(Updated for the alpha: `software/basic/img` and `software/hd` are
gitignored, so the floppies are decoded first; tools/setup.sh does all of
this without the options. And what the next paragraph says about the file
area is history: tools/sdfwrite.py now builds a real volume, and a disc
built by the alpha's tools takes BASIC's CREATE and STORE. A disc built
before that reports ERROR 455; build it again.)*

BASIC comes up from the fixed disc with the options installed. What is **not**
finished is the file area: the disc has a working boot area but no filesystem,
which BASIC reports accurately at startup as *"ERROR 455: Possible corrupt
directory"*. Creating one from inside BASIC needs `INITIALIZE`, and the
command reaches the disc — the CS/80 Initialize Media (0x37) and Set Status
Mask (0x3e) commands are implemented — but answers *"ERROR 458: Unsupported
directory format"* whichever way the directory format has been spelled so far.
HP's own utilities carry `SDF` as a string literal, so the format is a
parameter; the exact syntax has not been established, and no BASIC 2.0
language manual has been found. Until then a fixed disc is a system disc, and
files live on flexible volumes.

### Does the bare configuration explain anything?

It explains why errors print as bare numbers (`ERROR 56` rather than its
text) — that is `ERRORS_ENG` missing. It does **not** explain the RUN failure:
booting with `ERRORS_ENG`, `IO` and `MS` installed reproduces the same
transfer-table trap, byte for byte.