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
| RAM | up to 10 MB, on 512 KB or 1 MB cards (see §1.2) |
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
- Memory shares this stack with the processors: "CPU, IOP, and memory
  boards reside in the stack and communicate via the common memory
  processor bus" *(520 Service, Theory of Operation)*. Boards came as
  **256K finstrates, 512K polystrates, or 1M polystrates**, in "any
  combination of 256K, 512K, and pairs of 1M" *(CE Handbook)*; HP's own
  boards (the 512K is part 97047-69805) were joined by "commercial" boards
  taking third-party DRAM in the same capacities. So the real range is
  well under 1 MB up to 10 MB, and **no 520 ever had 16 MB**.
- The emulator models the boards as MPB slot cards: `-mpb3` .. `-mpb12`
  each take `256k`, `512k` or `1m`, defaulting to the base machine's pair of 512 KB boards.
  The stack's total is reported to the boot as the top of memory, which
  the language system sizes everything from -- with 2 MB fitted BASIC
  reports 1,625,476 bytes free and puts its per-task control block at
  0x1fd500, where the old fixed 16 MB put it at 0xffd500. The pairing
  rule for 1 MB boards is warned about rather than enforced, and a stack
  under 2 MB warns that the emulator's reconstructed boot handoff (image
  at 1 MB) cannot boot it even though a real 520 could run in it.
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
| 6 | keyboard, printer (§2.9), real-time clock, beeper, optional input device — one interface, one RX register or more per device |
| 7 | internal mass storage (fixed and flexible disc share the select code, one at a time, switched by the IOP) |

Additional IOPs take 8–15 and 16–23 as on the 540, and reach the outside world
through a 97098A I/O Expander.

### 2.2 MPB command set *(IO ERS §5, sheets 29–42)*

Fifty-three commands, each opcode indexing one of 128 words of IOP control
store that form a jump table to the command routines. The ERS quotes them in
octal. `(R)`, `(W)` and `(C)` mark reads, writes and controls.

```
003 WDS  W  write DMA status          053 RIOP C  reset the IOP itself
004 WDC  W  write DMA count           054 WDPA W  write DMA peripheral address
005 WDA  W  write DMA start address   055 RDPA R  read DMA peripheral address
006 WDTF W  write DMA termination     056 RDB  R  read data buffer
010 RDCS R  read DMA count/status     057 RIRV R  read revision
011 RIF n R  read interface FRn       060 WMCN W  write MPB channel number
013 RDMK R  read DMA mask             063 CALM C  clear address lockout
014 CDMK C  clear DMA mask            064 SALM C  set address lockout
015 EDMA C  enable DMA                070 ICEI C  init cmd exec from interrupt
016 DDMA C  disable DMA               073 DCE  C  disable IOP cmd execution
023 SD   C  start DMA                 075 LON  C  turn LED on
025 WIMK W  write interrupt mask      076 LOFF C  turn LED off
026 RIMK R  read interrupt mask      0103 RIST R  read interface status
027 RINT C  request interrupt        0104 RIFG R  read interface flag
030 CIRQ C  clear interrupt request  0105 RDTF R  read DMA termination field
033 WIMG W  write interrupt message  0106 WAMK W  write attention poll mask
034 RIRQ R  read interrupt request   0107 CAAK C  clear attention acknowledge
035 SIL  W  set interrupt level      0110 RIRS R  read IOP registers, suspend
036 EINT C  enable interrupt         0113 WIRR W  write IOP registers, resume
037 DINT C  disable interrupt        0114 WRIF W  write RIF result to memory
040 WTMS W  write timer start        0115 WCSM W  write count/status to memory
043 WTMC W  write timer count        0116 RDEB R  read device end/burst request
044 RTM  R  read timer count         0117 WDB  W  write DMA data buffer
045 ETMI C  enable timer interrupt   0120 WDCA W  write DMA current address
046 DTMI C  disable timer interrupt  0123 RIMG R  read interrupt message
047 IFPL R  interface poll           0172 WIF  W  write interface
050 RIOB C  reset I/O bus
```

`RIF n` (read) and `WIF n` (write) are each a **family** of sixteen opcodes,
one per FR register: `RIF n = 8n+1`, `WIF n = 8n+2` octal — so `011` is
`RIF 1` (read FR1) and `0172` is `WIF 15` (write FR15) *(sheet 34)*. Every
other entry above is a single fixed opcode.

> **`RINT` is implemented; `DTMI` still is not.** Request Interrupt sets
> "the bit corresponding to the PA in the channel address ... in the
> Interrupt Request register" (sheet 31) -- the exact mirror of `CIRQ`, and
> two lines beside it. Nothing needed it until the internal thermal printer
> was fitted (§2.9): the keyboard board's register exchange takes the
> interrupt-driven path when the interface flag does not come back on its
> own, and an unimplemented command there is a message trap that kills the
> boot. Disable Timer Interrupt is still unmodelled and still unneeded.

> **The DMA current count is biased by two, and this cost a day.** ERS
> §5.2: "Current count is **two less than the number of transfers
> remaining** once a Start DMA has been issued. If DMA terminates on count
> ... count will be **-2** ... If DMA terminates on Device End, the number
> not transferred is **count+2**." So `RDCS` must hand back `remaining - 2`,
> and a transfer that completed reads back as -2, not 0. The kernel's
> transfer report (entry 14) adds exactly that 2 back to get "how many did
> not go", and a count that reached plain zero therefore told every driver
> that two transfers had failed. The internal thermal printer showed it
> first -- a line that printed perfectly came back as `ERROR 81:
> Device/Interface hardware failure` -- but it was never a printer defect:
> the same wrong count sat in the disc path's transfer reports too, where
> nothing had happened to look at it.

**Five more are list-only.** The ERS's command table has a final group headed
`IOP COMMAND EXECUTION` — opcodes legal only *inside* an ICEI command list,
where they are its control flow (sheets 38–39, 41–42):

```
065  Increment and Branch     067  Skip on Flag False      0115 WCSM W  write count/status
066  Skip on Status False     0114 WRIF W  write RIF result
```

- **065 Increment and Branch** is three words wide, not two: "test the count,
  the contents of the **second** memory location following this command, and
  branch to the absolute address in the **first** memory location following
  this command if the value was not zero. It will also increment and replace
  the count if the count is still negative. If the count is zero, execution
  will continue at the **third** memory location following this command." A
  count of −1 therefore takes the branch once and falls through ever after —
  a list arming itself for a fixed number of passes.
- **066 / 067** test the addressed PA's Status or Flag line; false clears the
  PA's skip-mask bit and *leaves the list pointer on the skip command*, so the
  list retries when the device is next serviced.
- **0114 WRIF** does a Read Interface using the PA and IC of the command word
  and stores the result at the address in the following word.
- **0115 WCSM** stores the Current Count and Status register — the word `RDCS`
  returns — at the address in the following word. It is a **store**, not the
  end of the list; this is where a driver reads its transfer's outcome from.

Two properties of list execution follow from the same sheets and are easy to
miss:

- **The list pointer lives in the DRT and advances.** The ERS says so where it
  describes 066: "The MPB command list pointer in the DRT points to the next
  instruction... The command list pointer remains pointing to the skip
  command." An IOP that only ever *reads* `DRT+12` restarts the list from the
  top on every ICEI.
- **ICEI inside a list is a wait, not an end**: "This command suspends
  execution of commands until the interface responds to a poll" *(sheet 36)*.
  The list resumes at the following entry when the card answers the interface
  poll.
- **Command execution is PER-PA, and DCE does not stop the IOP.** `ICEI`
  "enables Command Execution (bit 20 in the Status register)...
  corresponding to the PA in the channel address" *(sheet 36)*; `DCE`
  "clears a bit in the Interrupt Request register and clears the Command
  Execution bit (bit 20 in the Status register) **both corresponding to
  the PA in the channel address**" *(sheet 38)*; `DCEI` does the same and
  sets the interrupt-mask bit *(sheet 37)*. Modelling `DCE` as a global
  IOP off-switch cost the 540 its console: the driver polls the card for a
  second, arms its real transaction in the command list, and issues `DCE`
  to stop the polling list — and the whole IOP stopped servicing every
  channel, including the suspended list it had just been asked to run.
- **A device already queued on its level's list must not be queued
  again.** The append writes the new entry's address into the old tail's
  link field, so appending a device that *is* the tail links it to
  itself — and the CPU's walk (take the head, follow the link, clear the
  message bit when the link is zero) never terminates. The level's bit
  stays set, every `IXIT` re-enters the service routine, and the machine
  services one interrupt tens of thousands of times a second while the
  card raises a hundred.
- **The host's interrupt line is a level, and only its transitions are
  events.** Re-asserting it re-sets the message-register bit the
  interrupt service has just cleared, which is the same storm by another
  route. Assert only on a change.
- **The Interrupt Request register is set by the termination, not by the
  mask**: "DMA termination or a non-DMA service request will cause a bit in
  the Interrupt Request register corresponding to the proper PA to be set"
  *(4.1.1)*, with no mention of the mask, which decides only whether the IOP
  goes on to *post* an interrupt. A driver is free to mask everything off and
  poll `RIRQ` instead — and HP-UX's panic printer does exactly that.

> Opcode **000** is still unnamed anywhere in hand, and the IOP refuses it
> rather than guessing. (An earlier reading of this section also listed
> 011, 050 and 053 as unnamed; all three are documented — `RIF 1`, `RIOB`
> and `RIOP` above — and the emulator already implements them under those
> names.)

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

The full register allocation *(530/540 Service, 2-29/2-30)*:

| Register | Direction | Contents |
|---|---|---|
| `R1` | write | bit 1 clear fan-high, 2 enable self-test, 3 clear UPS warning, 4 clear key-input interrupt, 5 initiate stack self-test, **7 ROM DMA enable** (sets the address counters to all ones and the continuous poll response) |
| `R2` | read | bit 0 RESET pressed, 1 power-fail warning, 2 fan high, 3 self-test enabled, 5 **RTC/NVM battery fail** (latched at power-up, valid only within four seconds of it), 12 START pressed, 13 continuous self-test latched, 14 NMI, 15 MEM DUMP pressed |
| `R3` | read | self-identity code **4007 hex** — bit 14 is what the bootstrap scans select codes downward for |
| `R4` / `R6` | read | loader ROM data |
| `R8` / `R12` | write | RTC / NVM / ROM address; **readable too**, so a bit pattern written and read back tests the data bus |
| `R9` | read/write | RTC data, low four bits |
| `R10` | read/write | I/O to the service processor and service panel |
| `R13` | read/write | NVM data byte |

The **RTC** is sixteen four-bit locations behind a 32.768 kHz crystal: `R12`
latches a location, then reads or writes of `R9` move data, and the address
**auto-increments** afterwards so a block moves without re-latching. The
**NVM** is 2048 eight-bit bytes of CMOS RAM holding configuration and service
data; its address goes to `R12` **in complemented form**. Both run off a
NiCad battery for at least ten days with the machine off. Power-fail sets `R2`
bit 1 and answers an I/O poll on SC7; as little as **1 ms** may elapse before
shutdown, and the bit self-clears if power returns.

> **Observed.** HP-UX 5.30 drives exactly this: it polls a ready bit in `R1`,
> then sweeps `R12` from `ffff` downward through `fff800` writing `R13` —
> complemented, that is NVM 0 through 0x7ff, the whole part in order —
> restoring the configuration the manual says the OS should restore after a
> battery failure. Answering the `R1` poll (bit 7) is what lets the sweep
> start; before that the kernel spun on it 200,000 times in three emulated
> seconds. `R11` answers NVM data alongside `R13` in the running system.

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
> This emulator therefore *reconstructs* that state: the image is placed at
> 1 MB and the loader's own stack hangs off the top of installed memory, with
> the shape the powerup module expects (a base below the marker and half a
> megabyte above it). Both are inventions that work, not documented values.
> They had been absolute addresses at 14-15 MB, which only existed because the
> driver claimed 16 MB of RAM; a real machine has at most 10, so the stack now
> scales with what is fitted. The load address is 1 MB when the machine can
> take it and otherwise adapts, loading the image high with a small stack
> window under top of memory -- so the base 1 MB machine (two 512 KB boards,
> now the emulator's default) boots the images whose relocated world fits
> below the image; one that does not fit is the machine genuinely out of
> memory, and the boot says so.
> Those are recoverable by disassembling the HP-UX boot image once the CPU core
> exists — see [SOFTWARE.md](SOFTWARE.md#the-loader-rom-question), and §3.3
> below for what has already been recovered.

#### 2.5.1 Every ROM in a 520, and what is *not* in one

HP's smaller machines keep BASIC in ROM, so it is worth stating plainly what
this family does instead. **No part of the Series 500's language system is in
ROM.** BASIC 2.0 is a 372,932-byte image of **thirteen segments** living
entirely on its two discs — `SYSTEM POWERUP`, `TRAP HANDLERS`, `BASIC IO/MM
RTS`, `BASIC COMPILER`, `BASIC`, `HUMAN INTERFACE`, `BASIC INIT`, `IOS and
DDMs`, `SDF_DISC`, `PWP SELF TEST`, `BASIC PARSER/RTS`, `SUN OS KERNEL` and
`AP` — every one of which checksums and is placed in the code segment table
at boot. The only firmware in the machine is:

| ROM | Where | What it holds |
|---|---|---|
| **09020-80000 / -80001** | bootstrap loader card at SC7 unit 1, *or* the Winchester controller when a fixed disc is fitted | 8 KB: loader code **and the machine ID** |
| 1818-1989 | fixed disc controller | that controller's own program |
| 1818-1897 | 9130K flexible disc controller | the 8039's firmware |
| — | thermal printer | 8 KB: 4 KB firmware + 4 KB characters |
| — | keyboard board | the keyboard microprocessor's program (with 64 bytes of RAM) |
| — | graphics/digital video board | character and attribute ROMs |

*(520 Service, 2-52/2-53 and the CE Handbook's parts lists. The "ignore
references to ROMs" note in the 520 manual applies to the 98770A/98780A
**display** service manuals, not to the machine's own firmware.)*

The bootstrap loader card shares select code 7 with the flexible disc
controller — controller is unit 0, loader card unit 1, switched by FR8 — and
its ROM is byte-addressed through a 16-bit counter that **resets to zero on
interface clear or on a read of FR3**, incrementing after every byte read.

> **The ID ROM is the one real gap.** That 8 KB holds "loader code and machine
> ID information **for protection of software rights**", and the serial number
> is programmed into each replacement part — when ordering one, HP required the
> machine's serial number. No dump of either revision is known to exist, so the
> emulator presents the loader card's identity (8) but serves no ROM bytes
> behind it, and the boot itself is performed at a high level instead (§2.5).
> Any check BASIC makes against the machine ID therefore reads nothing. That is
> the only part of a 520's firmware the emulation cannot reproduce.

### 2.6 I/O cards (HP-CIO)

The plug-in I/O cards are standard **HP-CIO** cards shared with HP's other
computer lines. Their technical references — HP's "27132A MP-CIO Technical
Reference Package" (27132A is the *documentation product's* number, not a
card; an earlier revision of this table mistook it for one) — are now in
[bitsavers/](bitsavers/) with text extractions in [notes/](notes/), and
everything below cites them.

| Card | CIO ID | Function |
|---|---|---|
| 27112A | 1 | GPIO, 16-bit parallel *(27132-90003)* |
| 27110A | 2 | HP-IB, IEEE-488-1978, up to 14 devices, controller or slave, hardware self-test *(27132-90002)* |
| 27128A | 3 | single-channel asynchronous serial (ASI), RS-232C / V.24 / V.28 *(27132-90001)* |
| 27130A | 5 | 8-channel asynchronous multiplexer, two 512-byte buffers per channel *(27132-90006)* |
| 27122A | — | PSI programmable serial (bisync / HDLC / SDLC, RJE); identity set by downloaded firmware *(27132-90005)* |
| 98760 | — | 520 built-in colour graphics display — see §2.7 |
| 97060A | — | external graphics processor, with built-in HP-IB — see §2.7.5 |

The **CIO ID** column is the card identity byte "per the CHANNEL I/O standard
assignment" *(27112A Tech Ref, 3-25)*, returned as the first byte of the
response to the IDY order. The powerup device scans observed in BASIC and
HP-UX read the same value with `RIF3` — matching the 520's internal
interfaces, which answer 4 (flexible disc), 5 (fixed disc) and 8 (bootstrap
loader) there. FR3 as the identity register is consistent across every card
observed so far.

#### 2.6.0 How the CIO protocol reaches the FR registers *(observed)*

The card references describe a channel vocabulary — write_control,
write_command, write_order, read/write_data, read_status — and the I/O ERS
describes sixteen FR registers per interface (§2.3). **No document in hand
joins the two.** What follows was read off HP-UX 5.30 driving a card in slot
0 of a 540, and it matches the internal disc controller's protocol closely
enough that the two are evidently one house design:

| | |
|---|---|
| `FR1` read | card status: **bit 7 configured / self test passed, bit 6 self test complete, bit 0 an event code is waiting**. A driver polls here and will not proceed until bits 7 and 6 are both set — answering `0x80` alone leaves it polling forever (200,000 reads in three emulated seconds); `0xc0` sets it going. |
| `FR1` write | commands: 1 is the attention-request disable the IOP's own poll issues (I/O ERS 4.4, and the CIO write_control byte's bit 0), 2 acknowledges the event standing in FR9. |
| `FR3` read | not an identity at all: a readiness register. The scan (`CONFIGURATION`, runtime `0x152fc`) reads it, and **if it reads 9 writes 0 to FR1 and FR8** — a reset — then reads it again and registers the slot only when it reads **0**, skipping it otherwise. The CIO card-ID byte (§2.6) is a different thing, carried in the identify *response*. |
| `FR8` write | the transaction command byte. **0 = identify.** |
| `FR9` write | a configuration word, `(2 << 4) | pa` — `0x20` in slot 0. LCS80 sends the disc controller the identical word; the high nibble's meaning is unestablished on either card. |
| `FR9` read | the event code being announced. Dispatched by high nibble: 1, 2 and 4 are completion classes, 3 and anything above `0x4f` an error. |

A transaction is an **ICEI command list** (§2.2, opcode 070), exactly as for
the disc: the driver writes the configuration word, then builds a list of
`WIF8 <command>`, `SIL <level>`, `DDMA`, `WDS`, `WDA`, `WDC`, `SDEC` and
points the device's DRT entry at it. For identify the list's `WDC` is **2** —
the answer is one halfword, and that halfword types the slot. When the leg
completes the card posts an event, asserts ASYNC, and the driver reads the
code from FR9.

**How the halfword types the slot** (`CORE SYSTEM` +0x2f16, read off the
running kernel). The DMA lands the two bytes at the device record's **+2**.
The driver then splits them: the **high** byte is stored at +0 and the low
byte back at +2, and it is the high byte that is the card ID — 3 for the
ASI, 2 for the HP-IB, the numbers §2.6 gives. It is range-checked with `CRB`
against **1..7** and computed-branched through a seven-arm table; out of
range stores error **30** at +6 and abandons the slot. So a card that
answers its ID in the low byte is typed 0 and rejected, which is what every
card modelled here did until the halfwords were corrected to `0x0300` and
`0x0200`.

Observed in full on a 540 boot, in order: powerup presence test (FR1, FR3 ×3,
`WIF0 1<<pa`, FR1 echo) → driver polls FR1 for `0xc0` → `WIF9 (2<<4)|pa` →
ICEI with `WIF8 0` and a 2-byte input leg → completion event → `RIF9`. The
kernel walks **every** registered slot this way; put cards in select codes 0
through 4 and each gets the identical dialogue with its own configuration
word.

Two rules the handler enforces, both found by getting them wrong:

- **A completion must not arrive before the driver is waiting for it.** The
  handler's completion path (CORE SYSTEM `0x26050`) tests a byte at the
  device record's **+201** — "a transfer is outstanding" — and returns
  silently when it is clear. An event posted synchronously inside the DMA is
  therefore discarded and the driver waits forever. Real hardware needs
  microseconds to move the data and is always beaten by the driver's setup;
  the emulated cards take a millisecond or two of host tick instead.
- **Reading FR9 consumes the event.** This kernel writes no acknowledgement
  back, so a code left standing is read forever. (The 520's BASIC driver
  *does* acknowledge, with a write of 2 to FR1.)

The event handler itself, read from the code at CORE SYSTEM `0x26000`: poll
FR1 → index the device table at `DB+240` by select code → lock the record's
`+204` → re-read FR1 → `RIF9` → mask the code with `0xf0` → computed branch.
Class `0x00` and `0x30` do nothing, `0x10`, `0x20` and `0x40` are completion
classes reached through the `+201` gate, and anything above `0x40` is the
error path.

**The identify answer types the slot, and the ID is in its HIGH byte.**
(An earlier reading here said the answer was discarded; that was wrong, and
it was wrong because the card was answering zero in the byte that matters,
so nothing downstream ever changed.) `CORE SYSTEM +0x2f16` splits the
halfword the two-byte DMA delivered — high byte to the device record's `+0`,
low byte to `+2` — then range-checks the high byte against 1..7 with `CRB`
and computed-branches through a seven-arm table; out of range it stores
error 30 and abandons the slot. So `identity` is `id << 8 | firmware`:
`0x0300` for the ASI, `0x0200` for the HP-IB.

#### 2.6.1 Common channel protocol

All four documented cards implement the same CIO card-level protocol, each
supporting a subset (a card **accepts and discards** any control, command or
order it does not support — *27110A Tech Ref, 4-1/4-2*):

- **write_control** — a bit-per-function byte: bit 0 `ARD` attention-request
  disable, 1 `ARE` attention-request enable, 2 `RQA` request attention,
  3 NMI acknowledge, 4 `DEN` device enable, 5 `DCL` device clear. (The
  I/O ERS's attention mechanism — §4.4 there — has the IOP writing 1 to a
  card's FR1 on an attention poll response; that write is this register,
  bit 0 = ARD. Kernel drivers observed writing FR1 during resets are using
  the same register.)
- **write_command** (opcode in bits 7:4): 0 `DSC[subch]` disconnect
  subchannel + abort, 1 `AEK[n]` asynchronous event acknowledge, 2
  `CSC[subch]` connect subchannel, 4 `RSC[subch]` resume subchannel.
- **write_order** (opcode in bits 7:4): 0 `IDY` identify, 1 `DLD` download,
  2 `PAU` pause, 3 `DIS` disconnect, 4 `RS` read status, 5 `WC` write
  control, 6 `RD` read data, 7 `WD` write data, 8 `RTS` / 9 `WTC`
  transparent status/control.
- **Attention Request (ARQ)**: the card posts **one Level-2 status byte at a
  time**; every ARQ must be acknowledged by a host read_status before the
  next ARQ-raising event can be reported. `RQA` from the host is answered
  with an `RFC` (ready-for-command) status byte; spontaneous events post
  `AES` (asynchronous event sensed) *(27110A Tech Ref, 4-3/4-4)*.
- **IDY** returns an identity block: byte 0 = card ID, then
  firmware ID, a 16-bit firmware revision, a multiplexing-capability byte,
  ports−1, max-requests-per-port−1, and the maximum guaranteed data
  blocking size *(27128A Tech Ref, 4-67)*.
- **Reset lattice** *(27110A Tech Ref, 4-6)*: `PPON` resets everything;
  `RST` resets the channel interface and the card (self-test runs, LED
  lights then clears); `DCL` = RST but preserves the device address; `DSCn`
  aborts only the transaction in flight, losing up to the card's buffered
  bytes (14 on the HP-IB card).

The ASI and MUX layer a **transaction protocol** above this: each read or
write is opened by a Connect Logical Channel (CLC) request block —

```
byte 0   S | BLK | -- | -- | request code (4 bits)      + high logical channel no.
byte 1-2 logical channel number (host-assigned, kept for the transaction)
byte 3   subfunction code (request-specific)
byte 4   port ID (ASI: always 0; MUX: logical port, remappable via WCC SF 34)
byte 5-6 data length, 16-bit
```

with request codes 1 read device data, 2 write device data, 3 control device
(ASI only), 4 read card information, 5 write card configuration, 6 control
card *(27128A Tech Ref, 4-3/4-4; 27130A Tech Ref, 4-3)*.

#### 2.6.2 27110A HP-IB — the mass-storage path

The card that matters most for emulation: external CS/80 discs and tapes hang
off it, and its firmware **speaks CS/80 natively**. Host requests are
"transactions" of up to three Level-2 parts — Control block, Data block,
Status block *(Tech Ref, 4-8)*. The control block's first byte routes on bits
7:6: `00` = general card control (opcode in 5:0), `10` = **CS/80** (bit 5
clear) or mag tape (bit 5 set), `11` = raw HP-IB data transfer; bits 3:0 are
data-present, read/write and CRC flags *(Tech Ref, 4-9)*.

General opcodes (hex) *(Tech Ref, table 4-1)*: 1/2 set/clear REN, 3 send
IFC, 4/5 configure/read PPOLL response, 6 configure SRQ, 7 read reason for
interrupt, 8/9 write/read interrupt mask, A wait for interrupt, **B go
online** (the card powers up offline and inert), C go offline, D/E
assert/deassert ATN, F PPOLL sense, 10 conduct PPOLL, 11/12 configure/read
HP-IB address, 13 AMIGO identify configuration, 14 last secondary address,
15/16 freeze on parity fail on/off.

A **CS/80 transaction** *(Tech Ref, 4-30/4-31)* carries: byte 1 don't-wait +
SPD/UNT/UNL flags, bytes 2–5 unused, byte 6+ the device's HP-IB address
(0–7) and a secondary (F2 = control, E5 = transparent), then the CS/80
command message itself; the card runs the whole bus dialogue (talk address,
UNL, listen address, secondary, data, PPOLL wait) and returns a status block
of TSTAT, then QSTAT, then — if QSTAT ≠ 0 — the 20-byte CS/80 Request
Status report it fetched automatically. TSTAT 0 = no exceptional condition;
1–8 encode read-termination causes (EOI / count / IF / MSA, with odd-count
variants) *(Tech Ref, table 4-2)*. AMIGO identify is not a distinct
transaction — it is composed from raw data transfers *(Tech Ref, 4-35)*.

Reset behaviour and the ARQ freeze rules are §2.6.1's. IFC is **not**
asserted at power-up; nothing touches the bus until "go online".

#### 2.6.2a The 530/540 console is a serial card

Worth stating plainly, because it changes what "bring up the console" means
on those machines: **the 530 and 540 have no console hardware of their own.**
The CE handbook's installation procedure says to put the "System Terminal
(console) Interface Card in slot 0" — select code 0 — and the System Disc
HP-IB in slot 5, then sets the terminal to 9600 baud, 8 bits, no parity,
ENQ/ACK, XON/XOFF pacing *(9030/40 CE Handbook, 2-3)*. Both the ASI and the
MUX are listed as the card for that slot.

HP-UX's own module directory confirms it. The 5.30 boot image (the 7914 unit
save) carries these segments among others:

| Segment | Module | |
|---|---|---|
| 11 | `TTY SUBSYSTEM` | standard |
| 32 | **`HP27128/30`** | one driver for *both* the ASI and the MUX |
| 43 | `INTERNAL_HP-IB` | the built-in HP-IB the internal 9.9 MB disc hangs off |
| 51 | **`HP27110`** | the plug-in HP-IB card |
| 10 | `SDF_DISC` | the filesystem |

So on a 540 the whole console path is documented hardware (§2.6.3), and the
whole mass-storage path is CS/80 over HP-IB (§2.6.2, §2.8) — including the
internal disc, which the service manual reaches through an "Internal HP-IB
interface" *(530/540 Service, 3-13)* rather than the 520's LCS80 controller.
The loader ROM's error codes name the same protocol: `CS80 DEVICE: SELECT
CODE NN`, `HPIB CARD: SELECT CODE NN` *(same sheet)*.

#### 2.6.3 27128A ASI

Z-80A-based, one RS-232 port to 19,200 baud full-duplex. Its identity block:
ID 3, firmware ID 0, MPX = 1 (in-channel multiplexing), 1 port, 3 requests
per port, guaranteed blocking size 200 *(Tech Ref, 4-67)*. The firmware owns
the byte stream: received characters run a fixed pipeline — error
conditions, signal-character detect, software handshake (host/device
ENQ-ACK and X-ON/X-OFF, all four programmable), edit mode
(backspace/line-delete with quoting), character stripping, single/double
text terminators, prompt sequence, end-on-count — and are delivered as
**terminated records** whose termination code reports which condition ended
them (codes > 16 = the same code + an error flag) *(Tech Ref, 4-5/4-66)*.
Configuration is all through write-card-configuration subfunctions (baud,
format, handshake characters, timeouts).

##### The registers, as HP-UX 5.30 actually drives them

*(The FR-level mapping is still a model — no document joins the CIO protocol
to the FOCUS IOP's sixteen FRs. What follows is what the 540's console
bring-up forces, and it now agrees with the documented card protocol rather
than sitting beside it.)*

**FR8 is the order register, opcode in bits 7:4.** Three of three values
HP-UX writes there decode under the documented order table *(27110A 4-2)* and
under nothing else:

| FR8 | Order | What follows |
|---|---|---|
| `0x00` | `IDY` identify | a 9-byte input leg — the identity information block |
| `0x88` | `RTS` read transparent status | a 17-byte input leg, the longest reply |
| `0x20` | `PAU` pause | nothing; the list then waits for the card |
| `0x58` | `WC` write control | an **output** leg carrying the request block (this is the HP-IB card's) |

**FR9 is the configuration register**, `mode << 4 | pa` — `0x20` from the
powerup scan, then `0x00`, `0x20`, `0x40` from the console driver's reset
path. **A write to it is a command the card answers.** `HP-UX KERNEL +0xc63c`
is explicit:

```
WIF1 0x20          device clear
(short delay)
WIF1 0x10          device enable
RIF1 until bit 6   self test complete
RIF1 until bit 7   configured
WIF9 (0 << 4)|pa   ---- mode 0
RIF1 until bit 0   an event is waiting
RIF9               its HIGH NIBBLE must be 3, else go back to polling
RIF1 until bit 7
WIF9 (2 << 4)|pa   ---- mode 2, not waited on
...
WIF9 (4 << 4)|pa   ---- mode 4
IFPL, bit pa, up to 15000 times
```

The emulated card answers a mode-0 write with an asynchronous event of class
3 (`0x3n`; the kernel's generic event handler does nothing for that class,
consistent with it being private to this driver). That much is forced — the
loop has no other exit. The mode-4 interface poll is **not** answered, and
that path is the panic printer's, not the console driver's: see below.

##### The identity block is nine bytes, and reading two is not enough

The powerup scan asks for `WDC 2` and types the slot from what comes back;
**the console open asks for all nine** with the same `IDY` order. A card that
answers only the halfword leaves the transfer short, and the consequences ran
a long way: no completion, the driver's timed wait expiring with exception
56, `HP27128/30` re-raising it, the TTY subsystem retrying and then binding
the **NULL** driver, and the first request to that stub raising 116 —
`System halted due to system error.`, printed by the kernel's raw-FR panic
writer (which is what drives the mode-4 handshake above; it is a
console-of-last-resort, not the console driver).

The layout is *27128A Tech Ref 4-67*, and the 27130A MUX repeats it with card
ID 5 *(4-46)*:

```
        7                                 0
   byte 0            card ID = 3
        1           firmware ID = 0
        2        16-bit binary number
        3      firmware revision code
        4   MPX                    bp mode
        5   No. of active ports - 1 = 0
        6   max requests/port - 1 = 2
        7        maximum guaranteed
        8     data blocking size = 200
```

Byte 4's field widths are not given; MPX is drawn at the bit-7 end and
`bp mode` at the bit-0 end. **The 27110A is not the same card here**: its
Level-2 identify message is two bytes, an id code then capability and
revision nibbles *(27110A 4-6)*, so `0x0202` rather than a nine-byte block.

##### The transaction protocol, as the console driver drives it

A transaction is opened by a **CLC order and the request block that
follows it**, and the card asks for it: *"Each read or write is preceded by
a Connect Logical Channel (CLC) order followed by a request block from the
host computer. The CLC request is in response to an SRQ signal, requesting
the next order, from the ASI"* *(4-2)*.

`0xb8` on FR8 is that order — **order 11**, outside the 27110A's table
(which lists 10–15 unsupported, and an HP-IB card has no logical channels)
but exactly what an ASI needs. HP-UX writes it and then DMAs seven bytes
**out**; nothing else fits.

**The request block's request code is in BYTE 2**, not byte 0. The fields
come in the order the prose lists them:

```
byte 0-1  logical channel number, host-assigned
byte 2    S | BLK | -- | -- | request code (4 bits)
byte 3    subfunction code
byte 4    port ID (the ASI has one port, so always 0)
byte 5-6  data length, 16-bit unsigned
```

The console driver's first block settles it: `00 00 04 fd 00 00 01` reads
as request 4 *read card information*, subfunction 253 *get the card switch
setting*, one byte — coherent, and the only coherent reading. Taking the
code from byte 0 gives request 0, which the same page marks "not used".
The rest of the bring-up reads the same way: request 5 *write card
configuration* SF 1 and SF 9, request 6 *control card* SF 3 and SF 5, then
request 2 *write device data*, 80 bytes at a time.

`RTS` is the card's side of the conversation — how it tells the host what
it wants, dispatched by the host's interrupt service through a six-arm
table on the opcode (`CORE SYSTEM +0x384c`). **`SWI` is what answers a
CLC**: "switch to the logical channel given in the LCN field and continue
the transaction there" *(4-60)*. Without it the host re-issues the same CLC
for ever.

##### FR6 is the byte-at-a-time read port

The kernel's panic printer cannot set up DMA on a halted machine, so it
drives the card through registers alone: write an order to FR8, wait for
the interface poll (unless the order was `0x28`), and read the answer one
byte at a time from **FR6** (`HP-UX KERNEL +0xc67c`, `IOR io=0x31` = RIF6).
Same queue as an inbound DMA leg, one byte per read.

Its transfers are equally deliberate: `CDMK`, `WIMK 0`, then `SDC` — Start
DMA, Clear IRQ, **not** the `SDEC` that would enable the interrupt — and
then a spin on `RIRQ & 1<<pa`. That is why the Interrupt Request register
has to be set by a DMA termination whatever the mask says (§2.2).

##### The Read Status block

Returned by the `RS` order "during the status part of a transaction.
Following the status block, the logical channel is automatically
disconnected and the LCN is released" *(4-64)*. Its **length varies by
request type**, and the host reads it with a count sized for the longest,
so the short ones end on Device End:

```
byte 0    card dependent request status (0 = no error)
          -- ends here for all CONTROL requests            (1 byte)
byte 1-2  length of data transfer
          -- ends here for all WRITE requests              (3 bytes)
byte 3-4  length of data remaining in the current record
          -- ends here for READ CARD INFORMATION           (5 bytes)
byte 5    message type for a read request
byte 6    text terminator for message type 1
          -- ends here for READ DEVICE DATA                (7 bytes)
```

Byte 0's codes are enumerated on the same page: 1 illegal subfunction,
2/3 illegal configuration value/length, 4 illegal or unimplemented
request, 5 illegal port ID, 6 host write-data overrun, 7 block mode not
allowed, 8 transfer length too large, 9 no buffer space, 10 link already
connected, 252–255 the modem cases.

##### `RD` is the data phase of whatever transaction is open

A CLC has already said what that is. A *read device data* request wants a
record off the terminal, but *read card information* has *already* put its
answer in the card's outbound queue — fetching a terminal record on `RD`
throws it away, which is exactly what happened to the console open's first
transaction (the switch byte it asks for with subfunction 253).

##### Where the 540 console stops

**It prints.** The panic report reaches the terminal in full — the message,
the register file, the stack, the I/O state and the segment table. That is
the whole write path working end to end: CLC, request block, configuration
subfunctions, and 80-column `write device data`.

What still fails is the **console driver's own open**. Its command list
cycles `PAU` → suspend on ICEI → resume when the card asks for an order →
`RTS` → read 17 bytes → dispatch, and it keeps choosing to poll rather than
opening a transaction, because the emulated card answers `IDL`, "nothing to
do". Its CLC transaction is built and waiting in the list at all times
(`WIF8 0xb8`, `WDS 0x834` outbound, seven bytes) and the gate in front of
it is simply never pointed at it. Forcing any other RTS opcode as a
constant is worse, not better — so what a real ASI reports at this point,
and when, is the open question. The event list *(4-30/4-31)* is the place
to look: eleven asynchronous events in priority order, of which "transmit
buffer is empty" and "a data record was received" are the two a console
would turn on.

#### 2.6.4 27130A 8-channel MUX

The same firmware family as the ASI (Z-80A, identical CLC model and receive
pipeline) with eight ports behind one select code: identity ID 5, port ID
meaningful (logical→physical mapping configurable via WCC SF 34), request
codes 1/2/4/5/6 *(Tech Ref, 4-3)*. This is the terminal fan-out card for
multi-user HP-UX on the 530/540.

#### 2.6.5 27112A GPIO

The only fully register-level card — no microprocessor between the channel
and the wires. Two write registers and three read registers, selected by the
last order *(Tech Ref, 3-21..3-25)*:

| Order | Register | Content |
|---|---|---|
| `WD` | Data Output (16 bit) | drives DOUT[15:0] |
| `WC` | Control Output | CTL[1:0] drive lines + status-interrupt enables |
| `RD` | Data Input (16 bit) | DIN[15:0] as of last input clock |
| `RS` | Status Input | bit 7:5 INT enables echo, 3:2 CTL echo, 1:0 STS[1:0] |
| `IDY` | Identity | constant **1** |

Identity, status and control interactions force a data-part length of one
(DEND on the first byte); data interactions handshake with the peripheral
(PEND → DEND) and may repeat without re-issuing the order. Subchannel
connection comes first via write_adapter_command (STT), after which the card
raises SRQ whenever ready *(Tech Ref, 3-25/3-26)*. Word or byte transfers,
switch-selected handshake modes. This is the card the 97060A graphics
processor connects through (§2.7.5).

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

> **Read off HP's own software, and now implemented.** Nothing in the
> collection gives the meaning of these registers, so all of the following
> was measured on a running machine rather than transcribed. The card is
> `src/iop/focusgfx.h`, fitted at select code 1 on `hp9020`; `FOCUS_NOGFX`
> removes it and `FOCUS_SC1LOG` traces every access with the segment and PC
> that made it.
>
> **1. `FR1` is one word read by four different gates**, and a display has
> to answer all four at once. `0x04c0` does:
>
> | reader | test | meaning |
> |---|---|---|
> | `SYSTEM POWERUP` +0x525a | `FR1 & 0x0F30 == 0` | **must FAIL** — see below |
> | `SUN OS KERNEL` +0x1df6 | bits 8–11 | device **class**: 4 or 8 → type 16, 10 → type 17 |
> | `SUN OS KERNEL` +0x28d6 | bit 6 | ready. Clear spins ~598,700 times and times out |
> | `SUN OS KERNEL` +0x240e | bit 7 | clear ⇒ the kernel resets (`FR1 = 6`, `FR1 = 1`) and escapes |
>
> **The first one is the counter-intuitive one and it is the whole trick.**
> The powerup scan's gate is a filter, not a presence test: a card with
> class bits set is *skipped*, its bit left CLEAR in the present bitmap at
> `DB+152`, and it is precisely that clear bit which sends the kernel's
> typing pass down the class-based path at +0x1df6. A card that answers the
> scan the way a generic interface does — echoing the `FR0` select pattern
> — gets registered, its bit set, and is then skipped by the typing pass
> forever. That is why a logging card parked on select code 1 never bound a
> driver whatever identity it answered on `FR3`: `FR3`'s low byte is only
> the fallback path, taken when the class nibble is zero.
>
> **2. The driver.** `HP98770` — segment 43 of the `500ENV` diagnostics
> environment, the internal display's driver — sets its interface word to
> **9 (channel 1, PA 1 = select code 1)** and binds only if the device type
> is **16 or 17**. It then runs HP's own power-up self test for the display
> (`PWP SELF TEST` local entry 32 for type 16, entry 30 for type 17) and
> allocates its control block only if that passes. It plants 559 and 454 as
> the maximum X and Y, and divides them for the aspect ratio — the
> 560 × 455 raster of §2.7.1.
>
> **3. What the self test demands**, in the order it asks (its reset is
> `PWP SELF TEST` local entry 21, and each failure has its own escape code):
>
> | check | requirement | escape |
> |---|---|---|
> | `RIST` (read interface status) | odd | 1 |
> | write `FR1 = 0`, then `RIFG` | **even** — the reset drops the flag | 2 |
> | `IFPL` (interface poll) | this PA's bit **clear** | 3 |
> | write `FR1 = 0x100`, then wait | the flag comes back | 5 |
> | graphics memory, patterns `0xAAAA` / `0xCCCC` | read back what was written | 8 |
>
> So `RIFG` is the command handshake — `FR1 = 0` is a reset that drops it,
> a command raises it again — and the interface poll is separate from the
> service request, which is where the emulator's default (poll chained to
> service) had the card asking for attention forever.
>
> **4. Completion is an interrupt.** The kernel's wait (entry 21) arms the
> device record and blocks on a semaphore that only the interrupt service
> posts, so a card that never raises leaves every command timing out. The
> card raises on each command write.
>
> **5. The register model.** Traffic to the display is `FR0` and `FR1` with
> the **IC field of the interface address** selecting between them — the
> kernel's register primitives (entries 17 and 18) build their operand as
> `select code | register << 6`. So a command goes to `FR1` and its data to
> `FR0`, and the IOP already routes this correctly (`iop_core::register_of`).
>
> **What this bought.** `ERROR 702 IN Clear_screen` is gone: HP's 9020 Test
> Supervisor loads, runs, prints its banner and takes operator input. The
> command set itself is not modelled — commands are logged and acknowledged
> — so the graphics *memory* test still fails with escape 8, and nothing
> draws yet. The commands seen so far are `0x100` and `0x180` (init),
> `0x1a8`/`0x1a9`/`0x1a0`/`0x1a1` with `FR0` data (the memory fill), and
> from the driver `0x100` and `0x12B`.

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

### 2.8 CS/80 — the mass-storage command set

Every Series 500-era HP disc and tape speaks **CS/80** (Command Set '80),
whether reached through the 27110A card (§2.6.2) or through the 520's
internal disc interface — the BASIC kernel's internal-disc driver module is
literally named `LCS80`. The full specification is now in hand:
`5955-3442_CS80_Instruction_Set_Programming_Manual.pdf` (118 pages, no text
layer in the original — OCR'd into [notes/](notes/)). Citations below are
its section-page numbers.

**Transaction model** *(1-7..1-10)*: every operation is a *transaction* of
up to three phases — a **command message** (host→device: one or more
complementary opcodes plus at most one real-time / general-purpose /
diagnostic opcode, each with parameters), an optional **execution message**
(the data), and a mandatory one-byte **reporting message**:

```
QSTAT = 0  normal completion
        1  hard error — issue Request Status to learn what
        2  power-on — device reset / media changed / operator intervention
```

*(3-13)* — exactly the semantics the running BASIC driver was observed to
enforce (a cold device must answer 2 until a Request Status collects it).

**Key opcodes** (full table: manual appendix B, table B-1):

| op | command | class |
|---|---|---|
| `0x00` | Locate and Read | real time |
| `0x02` | Locate and Write | real time |
| `0x04` | Locate and Verify | real time |
| `0x06` | Spare Block (param bit S: skip/jump spare) | real time |
| `0x08` | Copy Data | real time |
| `0x0A` | Cold Load Read | real time — bootstrap: read without prior addressing |
| `0x0D` | Request Status (20-byte report) | diagnostic |
| `0x0E` / `0x0F` | Release / Release Denied | general purpose |
| `0x10`/`0x11` | Set Address, single-vector / 3-vector (6-byte parameter) | complementary |
| `0x12` | Set Block Displacement (6-byte signed) | complementary |
| `0x18` | Set Length (4-byte; all-ones = "whole volume") | complementary |
| `0x20`+u | Set Unit (u = 0–14; 15 = device controller) | complementary |
| `0x30`–`0x33` | Initiate Utility (up to 8-byte parameter) | diagnostic |
| `0x34` | No Op | complementary |
| `0x35` | Describe | general purpose |
| `0x38` | Set Options | complementary |
| `0x39` / `0x3A` | Set RPS / Set Retry Time | complementary |
| `0x3B` | Set Release (param: T = suppress 2 s timeout, Z = auto-release) | complementary |
| `0x3C`/`0x3D` | Set Burst (off/on, 1-byte parameter) | complementary |
| `0x3E` | Set Status Mask (8-byte parameter) | complementary |
| `0x40`+v | Set Volume (v = 0–7) | complementary |
| `0x48`+t | Set Return Addressing Mode (single / 3-vector) | complementary |

**Describe** *(2-25/2-26)* returns up to 256 bytes: a 5-byte controller
field, then per unit a 19-byte unit field and 13-byte volume field(s) —
addressed to unit 15 it enumerates everything, addressed to a unit it
returns `[controller][unit][current volume]`. Field layout: controller =
installed-unit mask (2), max burst rate kB/s (2), controller type (1);
unit = generic type (0 fixed disc / 1 removable / 2 tape), 6-digit BCD
product number, bytes-per-block, blocks-bufferable, burst size, block time
µs, continuous transfer rate, optimal retry time, access time, max
interleave, fixed/removable volume masks; volume = max cylinder (3), max
head (1), max sector (2), **max single-vector address (6)**, current
interleave (1). A block-addressed (single-vector) volume reports max
cylinder/head/sector as zero — precisely the shape the 520's driver
requires before it will take the single-vector path. Real Describe
responses captured from a 7908, 7914, 7935 and 9133H are in
[bitsavers/cs80describes/](bitsavers/cs80describes/) as reference data.

**Request Status** *(2-35, table 2-5)* returns 20 bytes: 2 identification
(volume/unit, lowest unit with status pending), 8 of error flags in four
2-byte fields — reject (bit 2 channel parity, 5 illegal opcode, 6 module
addressing, 7 address bounds, 8 parameter bounds, 9 illegal parameter, 10
message sequence, 12 message length), fault (17 cross-unit, 19 controller
fault, 22 unit fault, 24 diagnostic result, 26–28 release required, 30
power fail, 31 retransmit; all unmaskable), access (32 illegal parallel
operation, 33 uninitialized media, 34 no spares available, 35 not ready,
36 write protect, 41 unrecoverable data, 43 end of file, 44 end of
volume), information (48–50 request release, 51 media wear, 52 latency
induced, 55 auto sparing, 57 recoverable data, 58 marginal data, 61
maintenance track overflow) — then 10 parameter bytes bound to the
highest-priority reported error. Reading the
report clears it. The **Clear** transparent command aborts the current
operation, resets all complementary parameters to power-on values and
resets status except a pending diagnostic result *(3-3)*.

**Internal drives.** The 520's flexible drive is the **9130K**; its service
manual (`09130-90030`, also now in [bitsavers/](bitsavers/)) covers the raw
mechanism and its ribbon interface, not the 520's controller card: 3.5"
media, 70 tracks over two heads, 16 sectors × 256 data bytes (362 with
overhead) per track, 286,720 formatted bytes, logical track addresses
written in sector ID fields for defect stepping *(9130K Service, 1-5/3-9)*.
This confirms the geometry recovered from the TD0 boot images. The
CS/80-level behaviour of the internal interface (identity 4/5 at FR3, the
FR1 command/reset register, FR0/FR6 data latch, QSTAT semantics) was
recovered from the running system and is recorded in the emulator source
(`src/iop/focusdisc.h`), not any manual in hand.

---

### 2.9 Internal thermal printer *(520 Service 2-41…2-44; CE Handbook 5-6)*

The one peripheral in this machine whose programming interface the manuals
give outright, rather than leaving to be read off HP's binaries.

An optional module on the 9020A/B and standard on the 9020C, with its own
logic board: an 8039-class microprocessor, 64 bytes of internal RAM, 8 KB of
external ROM (4 KB firmware, 4 KB fonts — 512 characters of 8 bytes, Roman
Extended and Katakana) and a 256-byte print RAM. **Its font ROM is not
dumped**, the same position §2.5 records for the display's.

**It is not a text device.** The CE Handbook calls it an Internal Thermal
*Graphics* Printer: true overprinting; inverse, underline, overline and 150%
tall in any combination; pixel-by-pixel graphics dump from the CRT at 560
dots/line, 77 dpi; seven user-definable characters; US ASCII **and Line
Drawing**, Roman Extension **or** Katakana; programmable vertical pitch,
lines per page and margins. 80 columns, 5 × 7 characters in a 7 × 12 field.

#### 2.9.1 The register interface

It **shares select code 6** with the keyboard, RTC/NVM, beeper and the
optional input device — one interface, one or more registers per device.

| | |
|---|---|
| enabled for | R0, R1, R2, R4, R5, R6, R7 |
| R1 | **shared**; the printer drives only its upper 4 bits |
| writes to R0, R2, R4, R6 | clock data into the printer's data register |
| writes to R5 | **interface command**; its parameters go to R7 |
| reads of R0, R2, R4, R5, R6, R7 | present the **status byte** |

Data is a stream of ASCII bytes: characters fill an 80-byte line buffer,
printed on the 81st or on a control code; a subset of control codes acts
immediately; escape sequences start with code 27 and normally end with a
capital letter, and the ones that do not apply are ignored. The escape set
itself is **not** in either manual — these are hardware books — so it has to
come off a run, which is what `FOCUS_PRINTLOG` is for.

The status byte, MSB first, is `PSFL DSTF MSTF OOPP HDFL VHLO PTTH HDTH`:
power supply failure, dangerous self-test failure, don't care, out of paper,
printhead control line failure, head voltage low, power transistor hot,
printhead hot. **A healthy printer with paper reads 0x00.**

#### 2.9.2 What the powerup self test requires

`PWP SELF TEST` +0x47c is the printer's test (DA 6, SA 0), and the CE
Handbook's failure list turns each of its escapes into a requirement. Read
off a running machine:

- **R1 bit 15 is PRESENT.** The test's first act is `RIF 1`, `LSR 15`; clear
  and it skips the printer entirely — which is exactly a machine without the
  option. Bit 12 must be clear (failure 09, "enabled for poll response after
  reset").
- **01** — after a five-second wait it waits on the interface flag.
- **02** — "flag did not go false after reading FR0": taking the status
  drops the flag, and it has to come back or 01 fires instead. A flag that
  only fell, or only stood, fails one or the other.
- **04** — the **loopback**: write R5 = 1, then write and read R0 for each of
  1, 2, 4 … 128. In loopback the data register reads back instead of the
  status byte.
- **05 / 07** — the interface poll must be **set** after the loopback and
  **clear** after `FR1 = 0`, the interface reset. Select code 6 has one poll
  line and the keyboard shares it, so a printer that polls at any other time
  costs the machine its keystrokes.

#### 2.9.3 What is modelled

`src/iop/focusprint.h`, reached through `console_card` — select code 6 is one
interface with several devices behind it, so the keyboard board owns the slot
and routes the printer's registers. `FOCUS_PRINTER` fits it (off by default:
it is an option, and fitting it registers another device),
`FOCUS_PRINTLOG` traces it, `FOCUS_PRINTSTATUS` injects fault bits,
`FOCUS_PRINTPRESENT` withholds the R1 presence bit.

The presence bit, the flag handshake, the loopback and the poll are all in
and graded by HP's own test, which gets as far as **failure 08, "printer
status false"**.

**It prints.** `PRINTER IS 6` then `PRINT` puts the line on paper:

```
$ ... -prin paper.bin        FOCUS_PRINTTEXT=paper.txt
$ od -c paper.bin
H E L L O   W O R L D \r \n   4 2   \r \n T H I R D \r \n
```

The path is **DMA, not byte writes**, and four things had to be right:

- **R1 bit 12 arms the transfer.** The driver sets it at HUMAN INTERFACE
  +0x40a4 immediately before starting the block.
- **The card must not accept data until then.** Taking it earlier let the
  IOP run the whole block at set-up time, before the count was programmed,
  and the residual count the driver reads back was never zero.
- **R1 bit 13 must be SET.** The driver builds `status | (R1 & 0xF000)` at
  +0x40d8 and takes it apart at +0x4144: status bit 4 (out of paper) escapes
  133, and bit 13 clear escapes 126 — the `ERROR 81: Device/Interface
  hardware failure` a printer without it reports after printing the line
  perfectly well.
- **Bit 12 must still be standing at completion.** The interrupt service
  picks the printer out of select code 6's several devices by ANDing R1 with
  0x1000 (+0x3d68) and skips the whole completion path when it reads zero.
  Clearing it at completion left the driver blocked on a semaphore nobody
  posted. "Armed for data" and "poll enabled" are therefore separate states;
  conflating them made the IOP start a second transfer over the first.

The last of those was the one that mattered beyond the printer: the driver
reads a residual DMA count back and requires it zero, and it read the full
transfer length. That was **not** a printer defect but the IOP's `RDCS`
count, which the I/O ERS biases by two (§2.2). Fixing it silenced `ERROR 81`
and corrected the same wrong count in the disc path.

**What is still wrong.** The powerup self test stops at failure 08, "printer
status false", so the boot carries one `Self test error 1: IO address 6, 0`
line. Printing itself is unaffected. The escape sequences are still only
logged, not interpreted, and nothing renders the 560-dot raster.

**R0 is contested and the manual wins.** This model used it for the
keyboard's pending keycode, which the 520 manual does not assign to the
keyboard at all; with a printer fitted it presents the printer's status. It
costs nothing either way at powerup — the only read of R0 in a whole boot is
the self test's, before any key is pressed, and "no key pending" and
"printer healthy" are both 0x00.


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

**Offset `0x1c` is the volume's block size**, not a bootable flag: `0x400`
= 1024 bytes on boot media, `0x100` = 256 on data media. That resolves what
looked like two unrelated conventions into one — **block 1 always says what
is on the volume**, carrying the boot descriptor on boot media and the SDF
file directory on data media, and block 0 is always the 48-byte header.
Reading it this way was what let the emulated bootstrap loader take each
volume's slice length from the medium (below); the load is byte-identical to
`bootimage.py`'s assembly for both acquired boot sets.

Offset `0x28` is the **first free block**, so a boot volume's image slice
runs from block 2 to there. Offset `0x2c` is `0x42285` on the boot discs and
zero elsewhere; it is identical across BASIC boot volumes 1 and 2, so it
describes the product rather than the individual disc — as does `0x14`.

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
from sector 3. Each slice's length comes from the volume header — `(first
free block − 2) × block size`, capped by what is left of the image — which
gives 267,264 and 105,668 bytes for the BASIC set and 267,264 / 267,264 /
146,140 for the three-volume diagnostics set, matching the assembled images
exactly. [`tools/bootimage.py`](../tools/bootimage.py) reassembles it,
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

1. **Loader / ID ROM** — no dump located, of either revision. The fetch
   protocol is documented (§2.5) and both acquired systems supply the
   `SYSTEM POWERUP` entry point, so booting is a soft blocker and is done at
   a high level instead. What cannot be substituted is the other half of that
   part: it carries the **machine ID used "for protection of software
   rights"**, serial-numbered per machine (§2.5.1). Any check the language
   system makes against it reads nothing in emulation. Whether BASIC 2.0
   makes such a check, and what it does when it fails, is untested — it is
   the one firmware-shaped hole left in a 520.
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
3. **Disc interface** — largely closed: the CS/80 instruction set is fully
   documented (§2.8) and the internal interface's register behaviour has been
   recovered from the running system (`src/iop/focusdisc.h`). Still open: the
   register-level controller documentation for the 520's internal disc
   interface card itself (the 9130K service manual covers only the raw
   flexible-drive mechanism), and the fixed 9.9 MB drive's mechanism. One
   protocol residue: the driver's request wrapper sends CS/80 opcode `0x09`,
   which does not exist in the CS/80 standard (table B-1) — its meaning is
   still unestablished.
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
| `27132-90001_27128A_Asynchronous_Serial_Interface_Technical_Reference_Jun83.pdf` | ASI card — §2.6.3 |
| `27132-90002_27110A_HP-IB_Interface_Tech_Jun83.pdf` | HP-IB card — §2.6.2 |
| `27132-90003_27112A_General_Purpose_IO_Technical_Reference_Dec84.pdf` | GPIO card — §2.6.5 |
| `27132-90005_PSI_Programmable_Serial_Interface_Hardware_Ref_Dec84.pdf` | PSI card hardware |
| `27132-90006_27130A_8_Channel_Multiplexer_Technical_Reference_Jun83.pdf` | 8-channel MUX — §2.6.4 |
| `5955-3442_CS80_Instruction_Set_Programming_Manual.pdf` | **the CS/80 specification** — §2.8. Original has no text layer; OCR'd to notes/ (118 pp) |
| `09130-90030_9130K_Service_Jul81.pdf` | 9130K flexible drive mechanism — §2.8 |
| `cs80describes/` | Describe responses captured from real drives (7908, 7914 ×2, 7935, 9133H) — reference data for §2.8 |

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
