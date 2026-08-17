# Software images

**Provenance: everything in this directory came from the HP Computer Museum,
<https://www.hpmuseum.net/>.** Their 500 Series software collection is at
<https://www.hpmuseum.net/exhibit.php?swc=21>; downloads are CAPTCHA-gated and
were fetched by hand. Most of the collection was donated to the museum by
**Charlie Brett of Fort Collins, Colorado** (November 2010) — without that
donation none of this would exist.

The museum asks to be contacted by anyone holding 500 Series hardware or
software. If any gap below gets filled, send the result back to them.

A survey of other sources found nothing: bitsavers hosts Series 500
*documentation* but its `bits/` software tree has no Series 500 material
(`HP_9000/HPUX_5/` is Series 200/300 media). **The museum appears to be the
only public source of Series 500 software.**

```
software/
  97089C.hpi        HP-UX 5.01 system tape (damaged — see below)
  basic/            BASIC Language System 2.0, 7 discs
  demos/            BASIC demo suite, 7 discs
  test/             9020 System Functional Tests 3.00, 6 discs
```

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
| **`S97080C`** | 1023+1892 | **100%** | **complete** |
| `S97089C` | 2915+1974 | 99.5% | cpio sync lost at 99.1% |
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

`S97080C` is **whole**, and contains:

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

To close that gap: ask the museum whether the original tape can be re-read (the
2013 file date suggests a single pass long ago), or find another copy of the
97089C tape.

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

## Tools

| Tool | Purpose |
|---|---|
| `../tools/td0.py` | decode Teledisk images to raw sectors |
| `../tools/lifdir.py` | list/extract the LIF directory of `.hpi` images |
| `../tools/cpiolist.py` | list/extract big-endian old-binary cpio filesets |
| `../tools/modmap.py` | map named modules in a boot image |

```sh
python3 tools/td0.py      software/basic/500BT1.TD0 --raw /tmp/BT1.img
python3 tools/lifdir.py   software/97089C.hpi
python3 tools/lifdir.py   software/97089C.hpi --extract S97080C --out /tmp/S97080C
python3 tools/cpiolist.py /tmp/S97080C
python3 tools/cpiolist.py /tmp/S97080C --extract system/97080C/os.req --out /tmp/os.req
python3 tools/modmap.py   /tmp/os.req --min-gap 256
```
