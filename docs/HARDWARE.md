# HP 9020 / HP 9040 — Series 500 Machine Hardware

Working specification of the machine side of the HP 9000 Series 500 — the
Model 520 (product name **HP 9020**) and Model 540 (**HP 9040**) — for a
simulator: the board stack and Memory Processor Bus, the I/O subsystem, the
System Control Module and bootstrap path, the display hardware, and the
on-disc boot media format.

The processor itself — registers, addressing, segmentation, traps and
interrupts, the dedicated memory map, the full instruction set, and the known
silicon bugs — is specified in [FOCUS.md](FOCUS.md); nothing CPU-architectural
is duplicated here.

Everything below is sourced from the primary documents in [bitsavers/](bitsavers/).
Citations are given as *(doc, sheet N)*. Where a fact comes from a secondary
source it is marked **[secondary]** and should be confirmed before it is relied
on for emulation behaviour.

---

## 1. Machine overview

The 540 is the free-standing cabinet member of the Series 500 family; the 520
is the integrated workstation with a built-in display — the machine BASIC was
written for. All four models (520/530/540/550) share the same processor
stack; they differ in packaging, display, and I/O expansion.

| | |
|---|---|
| Introduced | 1982 |
| CPU | 1–3 × FOCUS, 32-bit, 18 MHz — see [FOCUS.md](FOCUS.md) |
| Cache | 16 KB, off-chip **[secondary]** |
| RAM | up to 10 MB |
| Internal disc | 9.896 MB formatted, 85 ms avg access, 205 ms max *(530/540 Service, 4849)* |
| OS | HP-UX 1.0–5.3. **The 540 does not run BASIC** — that is 520-only. |

### 1.1 Chip set

Five NMOS-III VLSI parts: CPU, I/O Processor (IOP), Memory Controller (MC),
RAM, and clock driver **[secondary]**. They are mounted on *finstrate* boards
— circuit boards with a 1 mm copper core for heat spreading.

### 1.2 Backplane: the Memory Processor Bus (MPB)

The card stack has **12 slots**, numbered 1 (bottom) to 12 (top), all
interconnected by the MPB via the stack motherboard. The MPB carries **32
multiplexed data/address lines and 12 control lines**, at 18 MHz
*(530/540 Service, sheet "Stack Configurations")*.

Population rules *(530/540 Service, same section)*:

- Slot 1 **must** contain a CPU board.
- Slot 2 **must** contain an IOP board.
- A second IOP goes in slot 3; a third in slots 3 and 4.
- No empty slot is permitted between two finstrates.
- 1 MB RAM boards must be installed in pairs (2, 4, 6, 8, or 10).
- A *load board* occupies the top used slot when the stack is (a) one CPU +
  one IOP + one 512K RAM, or (b) all-1 MB RAM with fewer than six RAM boards.

| Board | Min | Max |
|---|---|---|
| CPU | 1 | 3 |
| IOP | 1 | 3 |
| RAM | 2 | 10 |

Twelve finstrates total is the hard ceiling. Hardware supports up to six
processors on the MPB; only three are usable under OS control **[secondary]**.
The MPB architecture allows 7 CPU/IOP nodes and 15 memory controllers
**[secondary]**.

> **Simulator note.** A single-CPU, single-IOP configuration is the right
> first target. Multiprocessing touches the MPB slave registers (SAR/SDR), the
> per-CPU dedicated memory blocks (FOCUS.md §7.4), and the `PMPB`/`SMPB`
> instructions; none of that is needed to boot.

---

## 2. I/O subsystem

Source: **FOCUS I/O ERS** (I/O System ERS, F. Gross, 09/20/82, 80 sheets).

The I/O channel is the **IOP**, "a valid implementation of a subset of a CSG
Channel Adapter" *(IO ERS, 4)*.

- **8 I/O channels per IOP**, each with DMA *(Hardware Technical Data)*.
- Bandwidth: 5.1 MB/s burst, 973 KB/s multiplexed **[secondary]**.
- Conforms to the **HP-CIO** (HP Channel I/O) standard.
- Up to three IOPs; each additional IOP reaches the rear panel by 50-conductor
  ribbon cable and can drive a **97098A I/O Expander** (eight more slots).

CPU-side I/O is done with three instructions — `IOW`, `IOR`, `IOC` — each
taking an I/O opcode as its 8-bit operand; see FOCUS.md §10.17 for their
semantics and the interface-word format.

### 2.1 Select codes *(530/540 Service, sheet 2-27)*

| Select code | Assignment |
|---|---|
| 0–6 | I/O backplane |
| 7 | System Control Module |
| 8–15 | first additional IOP |
| 16–23 | second additional IOP |

Backplane slots are physically labelled with their select code numbers.

The **520** divides the same eight select codes differently, because its
peripherals are built in *(520 Service, sheet 2-29)*:

| Select code | Assignment |
|---|---|
| 0 | Display Alphanumeric (§2.7) |
| 1 | Display Graphics (§2.7) |
| 2–5 | I/O backplane slots 2–5 (the only four available to the user) |
| 6 | keyboard, printer, real-time clock, beeper, optional input device — one interface, one RX register or more per device |
| 7 | internal mass storage (fixed and flexible disc share the select code, one at a time, switched by the IOP) |

Additional IOPs take 8–15 and 16–23 as on the 540, and reach the outside world
through a 97098A I/O Expander.

### 2.2 MPB command set *(IO ERS §5, sheets 29–42)*

Forty-eight commands, each opcode indexing one of 128 words of IOP control
store that form a jump table to the command routines. The ERS quotes them in
octal. `(R)`, `(W)` and `(C)` mark reads, writes and controls.

```
003 WDS  W  write DMA status          057 RIRV R  read revision
004 WDC  W  write DMA count           060 RIOB C  reset I/O bus
005 WDA  W  write DMA start address   063 CALM C  clear address lockout
006 WDTF W  write DMA termination     064 SALM C  set address lockout
010 RDCS R  read DMA count/status     070 ICEI C  init cmd exec from interrupt
013 RDMK R  read DMA mask             073 DCE  C  disable IOP cmd execution
014 CDMK C  clear DMA mask            075 LON  C  turn LED on
015 EDMA C  enable DMA                076 LOFF C  turn LED off
016 DDMA C  disable DMA              0103 RIST R  read interface status
023 SD   C  start DMA                0104 RIFG R  read interface flag
025 WIMK W  write interrupt mask     0105 RDTF R  read DMA termination field
026 RIMK R  read interrupt mask      0106 WAMK W  write attention poll mask
030 CIRQ C  clear interrupt request  0107 CAAK C  clear attention acknowledge
033 WIMG W  write interrupt message  0110 RIRS R  read IOP registers, suspend
034 RIRQ R  read interrupt request   0113 WIRR W  write IOP registers, resume
035 SIL  W  set interrupt level      0114 WRIF W  write RIF result to memory
036 EINT C  enable interrupt         0115 WCSM W  write count/status to memory
037 DINT C  disable interrupt        0116 RDEB R  read device end/burst request
040 WTMS W  write timer start        0117 WDB  W  write DMA data buffer
043 WTMC W  write timer count        0120 WDCA W  write DMA current address
044 RTM  R  read timer count         0123 RIMG R  read interrupt message
045 ETMI C  enable timer interrupt   0172 WIF  W  write interface
047 IFPL R  interface poll
054 WDPA W  write DMA peripheral address
055 RDPA R  read DMA peripheral address
056 RDB  R  read data buffer
```

> **Commands the list does not name.** Disassembling the `SYSTEM POWERUP`
> modules of both BASIC 2.0 and HP-UX 5.01 shows real boot code issuing
> opcodes **000, 011, 050, 053 and 065**, none of which appears in the §5
> list. Either the OCR dropped them or the list is incomplete. The IOP refuses
> them rather than guessing, so they will surface as message traps the moment
> a boot attempt reaches that far.

### 2.3 IOP functional areas *(IO ERS, table of contents)*

System communication; interface polling; direct I/O; DMA (single and multiple
transfers per poll response, plus DMA Verify and DMA Match options); interrupt
requests (Interrupt Request Register, Interrupt Mask Register, mapping
peripheral address → interrupt level); MPB I/O commands; the interval timer;
initialisation and self-test.

Every interface exposes sixteen addressable registers, **FR0–FR15**,
corresponding to interface-card addresses `0000`–`1111`. Older HP interfaces
called these R4–R7 (addresses `0000`–`0011`); FOCUS-era documents use the FR
names throughout *(IO ERS, sheet 4)*. The 520 service manual calls the same
things **RX registers**. RAMD-mode transfers — a Greyhound compatibility feature
the ERS advises against using in new designs — always go through **FR8 or FR10**
*(IO ERS, sheet 5)*. Each interface also owns a four-word Device Reference Table
entry; its dedicated-memory address and format reference are in FOCUS.md §7.4.

### 2.4 System Control Module *(530/540 Service, sheet 2-27)*

The SCM is on select code 7 and provides:

- current time at power-up (RTC)
- service panel control
- high-temperature and power-fail warnings to the OS
- **the loader ROM for the power-up routines**
- 2 KB of battery-backed memory

Register detail is in the service manual around sheets 2255–2340 — notably
`R2` bit 3 (self-test enabled) and bit 5 (RTC/NVM battery fail, latched at
power-up and **valid only if read within four seconds of power-up**), and `R12`
which loads a 16-bit address in complemented form.

Other SCM registers: `R10` of SC7 is reserved for communication between the
service processor and main system software (bidirectional via the IOP). Power
Fail Warning sets bit 1 of `R2` and responds to an I/O poll on SC7; as little
as **1 ms** may elapse before shutdown, and the bit self-clears if power is
restored.

### 2.5 Bootstrap protocol *(530/540 Service, sheet 2-30/2-31)*

The 8 KB loader ROM lives on the SCM, addressed through the same locations as
the RTC and NVM. **The mechanism is fully documented**, even though no dump of
the ROM contents is known to exist (§4).

Power-up sequence:

1. All processor stack chips run self-test.
2. CPU microcode has the IOP read **`R3` of SC7**. **Bit 14 set** identifies the
   presence of a loader. If clear, `R3` of SC6 is read, and so on **downward
   through all select codes** until a loader is found.
3. **`84` hex is written to `R1`** of the loader's select code. This resets the
   ROM address counter to `FFFF` hex (`0` hex to the chip) and enables DMA and
   the self-test bit.
4. **2048 bytes** are DMA'd in **from `R4`, with the final byte from `R6`**. The
   ROM address **auto-decrements** after each read.
5. That initial 2 KB of loader code then pulls in the full 8 KB loader.

The loader ROM contains several modules: a kernel module, output modules (e.g.
ASCII terminal), input modules (e.g. HP-IB/CS80), a service panel output
module, self-test supervisory code, and a test module (flowcharted separately
as Figure 3-4).

Loader flow, from **Figure 2-17** — reproduced as
[notes/loader-rom-flowchart.png](notes/loader-rom-flowchart.png):

```
Power On
  → Boot Loader ROM                        (8K-byte DMA)
  → Initialize other output modules        (display, etc.)
  → Initialize service panel output module,
    send "I Am Here" to service processor  (it can now display stack fault info)
  → Send initial "Loader Here" to each output module
  → Call test module for each PA           (hard errors do not return)
  → Look for OS on IOP 1, 2, 3 at PA 0..7  ─┐
  → Report errors                           │
  → OS found? ── no ──→ Wait ───────────────┘
        │ yes
  → Send OS ID to each output module
  → Load OS
  → Any errors? ── yes ──→ hard? ──→ message to output modules / continue
        │ no
  → Send "Loader Done" to all output modules
  → Jump to OS                             (OS turns off SELF TEST LED, turns on RUN LED)
```

Two ROM revisions exist, and they are **not** interchangeable:

| Part | Constraint |
|---|---|
| 09020-80000 Rev A | UNIX 4.0 / BASIC 2.0 or **earlier**; stack must contain **no** 1 MB RAM board |
| 09020-80001 Rev B | any RAM configuration, but requires UNIX 4.0 / BASIC 2.0 — **earlier software will not boot** |

Since HP-UX 5.0 postdates 4.0, a Rev B-equivalent loader is the right target.

> **Simulator note.** The device-scan and DMA handshake above can be implemented
> exactly. What the documents do *not* give is the on-media format the loader
> recognises as "an OS", the load address, or the machine state at *Jump to OS*.
> Those are recoverable by disassembling the HP-UX boot image once the CPU core
> exists — see [SOFTWARE.md](SOFTWARE.md#the-loader-rom-question), and §3.3
> below for what has already been recovered.

### 2.6 Peripherals

| Card | Function |
|---|---|
| 27110A | HP-IB, IEEE-488-1978, up to 14 devices, DMA with two performance modes, controller or slave, hardware self-test |
| 27128A | single-channel asynchronous serial, RS-232C / V.24 / V.28 |
| 27130A | 8-channel asynchronous multiplexer, two 512-byte buffers per channel |
| 27132A | MP-CIO, 8/16-bit parallel with DMA; clocked and transparent modes |
| 98760 | 520 built-in colour graphics display — see §2.7 |
| 97060A | external graphics processor, with built-in HP-IB — see §2.7.5 |

### 2.7 Display subsystem *(Model 520 only)*

The 540 ships with no display; the 520 has one built in, and it is the machine
BASIC was written for. Whatever the option, the display presents to software as
**two ordinary I/O interfaces** on the first IOP:

| Select code | Assignment *(520 Service, sheet 2-29)* |
|---|---|
| 0 | Display Alphanumeric |
| 1 | Display Graphics |

They are addressed through **FR0–FR15** like any other interface (§2.3; *IO ERS,
sheet 4*), and both appear as separately-orderable line items in the
memory/processor module configuration *(520 CE Handbook, sheet 1-2)*. So the
transport is fully specified — what no document in hand gives is the meaning of
the individual FRs (§2.7.4).

#### 2.7.1 The three options

| Model | Display | Interface hardware | Graphics raster | Colours |
|---|---|---|---|---|
| 9020A | **98760A** Standard Color, 12.2 in | alpha/interface + graphics/digital video boards, inside the display casting | 512 × 390 × 4 planes | 16 of 4096 |
| 9020B | **98780A** Monochromatic, 12.2 in | Display Interface Module (DIM) | 560 × 455 | mono |
| 9020C | **98770A** High-Performance Color, 13 in | Display Interface Module (DIM) | 560 × 455 | "8 pure, 4913" |

All three give 26 lines × 80 characters. The 98760A runs at 50 or 60 Hz; the
other two are 60 Hz only *(520 CE Handbook, sheets 1-2/1-3)*.

**Only the 98760A is documented in the collection.** The 98770A and 98780A have
their own service manuals — `98770-90031` and `98780-90030`, referenced as
Appendices I and J of the 520 Service manual — and neither is in
[bitsavers/](bitsavers/) (§4). What *is* documented for those two is the DIM,
the part that faces the IOP bus (§2.7.3).

#### 2.7.2 98760A — the standard colour display (9020A)

Two boards *(520 Service, sheets 2-37…2-40; HP Journal May 1984, "Color Graphics
Display for an Engineering Workstation", D.G. Schmidt)*.

**Alpha/interface board** — mounted under the CRT, faces the IOP bus.

- receives IOD 0–15, buffers, then either stores into **8 KB of alpha memory** or
  passes the data straight on to the graphics board
- controlled by an **MC6845 CRT controller**, which also generates the alpha
  timing consumed by the graphics board
- the 6845 **requires software initialisation** to produce correct sync for 50 or
  60 Hz; there is additionally a physical 50/60 Hz slide switch on the board,
  factory-preset and reachable through a hole in the casting
- connectors: P1 (40-pin) and P2 (50-pin) to the graphics board, P3 (50-pin)
  power, P4 (50-pin) IOP bus

**Graphics/digital video board**

- the graphics controller is a **NEC µPD7220 GDC** with **64K × 16 (128 KB)** of
  dynamic graphics RAM
- graphics raster **512 × 390**, four memory planes, every pixel individually
  addressable; the GDC generates vectors at > 28,000 pixels/s
- colour map is three high-speed 16 × 4 static RAMs → 16 entries × 12 bits, four
  bits per gun: **16 simultaneous colours out of 4096**
- the CPU reaches the colour-map RAM over the IOP bus on **DB12–DB15**, through a
  multiplexer that otherwise feeds the map from the graphics shift registers
  during refresh
- character and attribute ROMs, English/European or Katakana, fitted at the
  factory; alpha is **not** colour-mapped — 1-of-8 direct RGB
- the output gate array is three 16-bit latches plus four 16-bit shift registers,
  shifting four bits at a time at the graphics video rate
- a **51 MHz** crystal oscillator drives both boards; a sync generator supplies
  Hsync, Vsync and Vblank to the video board
- three D-to-A converters produce the R, G, B analogue streams

**Screen geometry.** Alpha raster 720 × 390, 26 lines × 80 characters in a 9 × 15
cell. A graphics pixel is 1.5 alpha pixels wide, so the graphics raster (512 px ≡
768 alpha dots) is 32 graphics pixels wider than the alpha raster, which sits
centred inside it.

**Alpha attributes** are carried as six signals — UL (underline), IV (inverse
video), BL (blink), R, G, B — per character cell, so any character may be any of
eight colours in any combination of underline / blink / inverse. **Alpha pixels
are dominant over graphics**: wherever an alpha pixel is present, the graphics
pixel at the same location is suppressed. Inverse-video characters have the
rising edge of each pixel stretched by half a pixel for legibility; the stretch
circuit is disabled for non-inverse characters. The attribute set is identical on
the 98770A "so that user programs will run on both displays", and the subsystem
emulates the earlier **HP 9845C** display.

> **Conflict to resolve.** The Journal gives a 9 × 15 character cell for the
> 98760A, while the CE Handbook's CRT table gives "7 × 9 font in a 9 × 12 cell"
> for all three displays *(520 CE Handbook, sheet 1-3)*. 26 × 15 = 390 matches
> the stated 390-line alpha raster exactly and 26 × 12 = 312 does not, so the
> Journal is the likelier figure for the 98760A — confirm before relying on it.

#### 2.7.3 Display Interface Module — 98770A and 98780A (9020B/9020C)

The DIM is mounted under the display and translates the general IOP interface
into the specialised alphanumeric bus the display expects *(520 Service, sheets
2-34…2-37)*. Its purpose is alpha refresh; it holds a full frame of characters so
that only changed characters need be written. **Graphics data and control pass
straight through to the display**, so nothing about the graphics side of those
two displays is documented here.

- connectors: J1 (36-pin) alpha data to display, J3 (36-pin) graphics data to
  display, J2 (50-pin) power, J4 (50-pin) IOP bus
- **frame buffer** — four static RAMs, 2480 × 16 bits = 31 rows of 80 characters,
  one full frame. Written from the IOP by DMA or direct I/O; readable back for
  diagnostics. The controller addresses it with 12 address lines
- **line buffer** — an NMOS-II chip: word counter, two 80-character shift
  registers, self-test logic. 16 data paths, of which the display uses 15. It
  shifts register 1 to the display while loading register 2 from the frame
  buffer, then toggles
- display timing drives the transfer: NW (new word), NL (new line), NP (new
  page); NP also resets the controller's frame-buffer address counters
- self-test enters via the TST line, raised either by pushbutton S1 at the bottom
  of the display or by software; the chip emits a test pattern and raises ERR on
  mismatch
- the controller is in display mode ~12% of the time and in IOP mode ~88%,
  servicing the IOP and frame-buffer updates

#### 2.7.4 What the diagnostics reveal about the register interface

The power-up self tests are the best available spec fragment for the software
interface, because they name registers and failure modes *(520 CE Handbook,
sheets 5-3/5-4; same tables in the 520 Service manual, Appendix)*. Device address
0 is alpha and 1 is graphics, matching the two select codes.

98760A (9020A), abridged:

| DA | Code | Meaning |
|---|---|---|
| 0 | 06 / 07 | status false / timeout during DMA transfer (alpha/interface board) |
| 0 | 08 | alpha memory test failed |
| 0 | 09 | channel end does not clear poll response |
| 1 | 01 | **timed out while initialising GDC** |
| 1 | 03 | control register fails readback |
| 1 | 06 | graphics memory test failed |
| 1 | 07 / 08 | timeout waiting for DMA / direct I/O transfer |
| 1 | 09 | GDC not functioning properly |

The DIM tests (9020B/9020C) do loopback on **FR0, FR1, FR8, FR9, FR10, FR12** and
**FR15**, and include a "Weasel chip failed selftest" entry — presumably the
NMOS-II line buffer, the one DIM part with its own self-test, though no document
names it that way **[unconfirmed]**. Further DIM tests: frame buffer memory,
space substitution, display interrupt, synchronous startup, window, reset.
Graphics side: rubber-band memory, line drawing, erase memory.

> **Simulator note.** Both principal parts of the 98760A are standard and already
> modelled in MAME (`upd7220`, `mc6845`), and the geometry, colour map and
> attribute semantics above are enough to drive them. The missing piece is purely
> HP glue: which FR on select codes 0 and 1 maps to the 6845 address/data ports,
> to the GDC command and parameter FIFO, to the colour-map write path and to the
> control register. That is recoverable from the BASIC display driver in the
> acquired media, and the self-test codes above give a graded target — the boot
> diagnostics exercise the GDC before BASIC ever draws anything.

#### 2.7.5 97060A external graphics processor *(optional, not part of the display)*

An outboard box driving an RS-343 monitor (HP 13279B or compatible), connected
through a **27112A** GP-IO 16-bit parallel DMA interface. Bit-slice (2901) and
microcoded: a 1024 × 768 × 8 addressable frame, 576 × 455 × 4 displayed at 60 Hz
non-interlaced, 29.4 kHz horizontal, 16 colours from 4096; area shading
approaching 16M pixels/s. It carries its own HP-IB for a 9111A graphics tablet so
it can take over cursor tracking from the host *(Hardware Technical Data, sheet
9)*.

Its service manual is **the only document in the collection with a real graphics
command set** — Chapter 2 gives the microcode word definition and the full op-code
list: `CMAP` 51 (load colour map), `CRTWR` 46 (write 6845-1 CRT controller
register), `CLEAR` 60, `DRAW` 61, `ARC` 62, `FFILL` 65, `AFILL1/2` 68/69, `CHAR`
6B, `CURS` 71, `COMPDR` 72, `BLANK` 4B, `BLINK` 4D, `GRAFIN` 4A, and so on. It is
a useful model of how HP structured graphics commands of this era, but the
built-in display shares none of it.

---

## 3. Boot media

### 3.1 SDF volume header *(observed)*

Every Series 500 disc opens with the same 48-byte record. Comparing five
volumes — two BASIC boot discs, a BASIC utility disc, a diagnostic and a demo
— separates the constant fields from the variable ones:

| Offset | 500BT1 | 500BT2 | 500UT1 | 500MFTST | BASDEM1 | Reading |
|---|---|---|---|---|---|---|
| `0x00` | `07000000` | `07000000` | `07000000` | `07000000` | `07000000` | format magic |
| `0x04` | `97050-11024R0200` | `97050-11025R0200` | `97050-11027R0200` | `09020-11011R0300` | `11141-10703R0100` | part number and revision |
| `0x14` | `1e3ed0` | `1e3ed0` | `25525a` | `255293` | `254fc1` | per *product*, not per volume |
| `0x18` | `a8c0` | `a8c0` | `7e92` | `13869` | `bd66` | size |
| `0x1c` | **`400`** | **`400`** | `100` | `100` | `100` | — |
| `0x20` | `1` | `1` | `1` | `1` | `1` | |
| `0x24` | `106` | `69` | `0` | `0` | `0` | per volume |
| `0x28` | `107` | `6a` | `202` | `202` | `202` | per volume, `= 0x24 + 1` |
| `0x2c` | **`42285`** | **`42285`** | `0` | `0` | `0` | — |

Two fields separate bootable media from data media exactly: offset `0x1c` is
`0x400` on the two boot discs and `0x100` on the other three, and offset
`0x2c` is `0x42285` on the boot discs and zero elsewhere. Both are identical
across BASIC boot volumes 1 and 2, so they describe the product rather than
the individual disc — as does `0x14`.

Offsets `0x24` and `0x28` are consecutive and vary per volume, which fits a
last-used and first-free block pair.

### 3.2 Boot descriptor *(observed)*

A bootable disc is **not a filesystem**. Where a data disc puts an SDF file
directory at offset `0x100` — 36-byte entries of an 18-byte name, a type
halfword, a start offset, a length and a date — a boot disc puts a descriptor
at `0x400` and then a raw slice of the boot image.

| Offset | 500BT1 | 500BT2 | Meaning |
|---|---|---|---|
| `0x400` | `e9c28206` | `e9c28206` | **boot image format magic** — the same value appears on the HP-UX 5.30 disc, so it identifies the format, not the product |
| `0x404` | `0005b0c4` | `0005b0c4` | **total image size**, 372,932 bytes |
| `0x408` | `2` | `2` | volumes in the set |
| `0x40c` | `1` | `2` | this volume's number |
| `0x410` | `0` | `41400` | byte offset of this volume's slice |
| `0x414` | `8` | `3` | starting sector of the slice |
| `0x418` | `5` | `8` | unidentified |
| `0x430` | \multicolumn | | volume label, `... Vol 1 of 2` |

So **BASIC 2.0 is a single 372,932-byte image split across two discs**:
volume 1 supplies bytes `0`–`0x413ff` from sector 8, volume 2 the remainder
from sector 3. [`tools/bootimage.py`](../tools/bootimage.py) reassembles it,
and the result maps to **57 modules** — `SYSTEM POWERUP`, `TRAP HANDLERS`,
`BASIC COMPILER`, `BASIC`, `SDF`, `CS80`, and the rest of the system.

### 3.3 Module layout and the entry point *(determined)*

Every module begins with a preamble of four parts:

```
+0x00   32 bytes   header: checksum, index, length, signature e307fffc, flags
+0x20   16 bytes   name, space padded
+0x30   variable   SCCS version string, e.g. "@(#)05.01"
        2 bytes    terminator 00c4
        28 bytes   table: size, three pointers, a build date, two words
                   -- 00000494 9f400000 80000000 00a00000 19810828 ...
        then       code
```

The version string is the only variable-length part, which is why the two
`SYSTEM POWERUP` modules sit 8 bytes out of step: BASIC's string is empty and
HP-UX's is `@(#)05.01`. Measured from the `00c4` terminator, both images are
byte-identical for a 395-byte run beginning at exactly **+0xa2**.

**The entry point is terminator + 0x1e** — BASIC image `0x54`, HP-UX `os.req`
`0x5c`. Both begin with the same nine instructions, differing only in branch
targets by the 8-byte offset between them:

```
DUP
CMPN  #2          compare the passed value against -2
BCC.GL <alt>      take the other path unless it is -2
PMPB  4           fetch this CPU's MPB channel number
CMPI  #1          is this the boot CPU?
BCCS.E <skip>
LDS   P-16
LDXI  #6
RCS               read control store 6: the CPU revision date
```

**Three** systems entering identically is what makes this conclusive: BASIC
2.0, HP-UX 5.01 and HP-UX 5.30 all begin with the same nine instructions at
terminator + `0x1e` — `0x54`, `0x5c` and `0x5c` respectively. Executing from there confirms it: `-2` on
the stack takes a long, productive path, while every other value falls
through to the alternate branch.

It also settles two more parts of the handoff.

**The loader leaves −2 on the stack.** The module compares against it and
branches; running from the entry with other values takes the alternate path
instead, so −2 is the cold-start code.

**`MASK` bits 3–5 must say channel 1.** The alternate path does `PMPB 4` then
`CMPI #1` and executes `STOP` if the CPU is not channel 1 — the mechanism by
which secondary processors park themselves at power-up. A CPU whose channel
number is left at zero halts immediately, which is worth knowing before
diagnosing anything else.

**The loader leaves a parameter block above `Q`.** The module reads `Q+64`,
`Q+88` and `Q+92`, and the value at `Q+64` must be at least 2060 — it is
combined with `LNI #2060`, a negative immediate, to form an index into the
global data segment.

`RCS` reading control store location 6 is the documented way to obtain the CPU
release date (FOCUS.md Appendix C), which software uses to decide which silicon
bugs to avoid.

---

## 4. Open questions

1. **SCM loader ROM** — no dump located. The protocol is documented (§2.5) and
   both acquired systems supply the `SYSTEM POWERUP` entry point, so this is a
   soft blocker.
2. **Memory Controller ERS** — referenced by both ERSes and still not located,
   but the gap is narrower than the references suggest. The software-visible
   MPB semantics are documented in the material in hand: I/O ERS §2.1 covers
   the slave address/data registers and what each carries, channel assignment,
   the message-bit map, the read-and-set-to-minus-one semaphore operation and
   the sixteen DRT interrupt lists at octal 2000–2171, and its §5 gives all 46
   MPB commands; the Instruction Set ERS §159–160 gives full instruction-level
   semantics for `RFC`/`WTC`/`RSC`/`SMSG`, including the MSG-bit handshakes.
   What both defer to the MC ERS is bus-protocol detail an ISA-level emulator
   never sees, plus the MC's own slave-register command set as the MPB node on
   channel 0 (memory sizing, ECC error logging, memory healing) — visible only
   if software probes channel 0 directly. For that residue, *HP Journal*
   August 1983 carries a detailed article on the chip set with a block diagram
   of the memory controller, its ECC and memory-healing behaviour, and its
   role as an MPB processor hardwired to channel 0 (see
   [hpjournal/](hpjournal/)), and the probing code itself is the fallback —
   the route that already recovered `WTC`'s leave-operands-on-stack behaviour.
3. **Disc interface** — the internal 9.9 MB drive's controller and protocol are
   not yet pinned down.
4. **Display register map** — nothing in hand assigns meaning to FR0–FR15 on
   select codes 0 and 1: how the 6845 ports, the µPD7220 command/parameter FIFO
   and the colour map are reached from the CPU is unknown (§2.7.4). The BASIC
   display driver in the acquired media is the likely source.
5. **98770A / 98780A display internals** — service manuals `98770-90031` and
   `98780-90030` are cited as Appendices I and J of the 520 Service manual and
   are not in [bitsavers/](bitsavers/). Only the shared Display Interface Module
   is documented (§2.7.3); the graphics side of both displays is undocumented
   here. Blocks 9020B/9020C emulation only — the 9020A is unaffected.
6. **98760A character cell** — 9 × 15 (*HP Journal*) versus 9 × 12 (*520 CE
   Handbook*); see the note in §2.7.2.

Open questions about the CPU itself — the four `/bin/as`-recovered opcodes,
CST entry attribute bits, and the rest — are tracked in
[FOCUS.md](FOCUS.md) Appendix B.

---

## Sources

Primary, in [bitsavers/](bitsavers/):

| File | Notes |
|---|---|
| `FOCUS_Instruction_Set_ERS_Dec82.pdf` | **The** CPU reference — see [FOCUS.md](FOCUS.md). 150 sheets, A-1FE1-3020-B, J. Fiasconaro, 09/23/82, issued 12/06/82 |
| `FOCUS_IO_ERS_Dec82.pdf` | I/O System ERS, F. Gross, 09/20/82, 80 sheets |
| `09040-90038_HP_9030_9040_Models_530_540_Service_Mar85.pdf` | 530/540 service manual, 141 sheets — board stack, select codes, SCM |
| `09020-90037_9000_500_Hardware_Technical_Data_Jan85.pdf` | Models 520/530/540/550 technical data |
| `09040-90035_HP_9030_9040_CE_Handbook_Mar85.pdf` | 530/540 CE handbook |
| `09020-90037_HP_9020_Model_520_Service_Feb85.pdf` | 520 service manual — shares the processor stack |
| `09020-90035_HP_9020_CE_Handbook_Jul85.pdf` | 520 CE handbook |
| `97098-90020_97098_IO_Expander_Service_Apr83.pdf` | I/O expander |
| `98760-90039_HP_98760_CE_Handbook_Mar84.pdf` | graphics |
| `97060-90030_97060A_Graphics_Processor_Service_Aug84.pdf` | graphics processor |
| `MODCAL_6.3_ERS.pdf` | Modular Pascal — the Series 500 systems language. **No text layer; scanned images only.** |

Also in [hpjournal/](hpjournal/), three issues of the *HP Journal* devoted to
the Series 500, retrieved from [hparchive.com](http://www.hparchive.com/):

| Issue | Covers |
|---|---|
| August 1983 | the CPU, IOP, clock, RAM and **memory controller** chips; MPB protocol |
| March 1984 | HP-UX on the Series 500; LAN 9000 |
| May 1984 | the three Series 500 models; the **98760A colour graphics display** (§2.7.2) and the BASIC graphics subsystem; wraps up the design story |

The HP Computer Museum lists the same articles at
<https://hpmuseum.net/exhibit.php?catdoc=12>, but its copies are lower
resolution and carry **no text layer**; the hparchive scans are OCR'd.

Secondary: [OpenPA — HP FOCUS Processor](https://www.openpa.net/focus_processor_hp.html),
[OpenPA — HP 9000 500 FOCUS Computers](https://www.openpa.net/systems/hp-9000_520.html).
