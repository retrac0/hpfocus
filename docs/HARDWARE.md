# HP 9000 Model 540 — Hardware Specification

Working specification for a simulator of the HP 9000 Series 500 Model 540
(product name **HP 9040**), built on the HP **FOCUS** microprocessor.

Everything below is sourced from the primary documents in [bitsavers/](bitsavers/).
Citations are given as *(doc, sheet N)*. Where a fact comes from a secondary
source it is marked **[secondary]** and should be confirmed before it is relied
on for emulation behaviour.

---

## 1. Machine overview

The 540 is the free-standing cabinet member of the Series 500 family. All four
models (520/530/540/550) share the same processor stack; they differ in
packaging, display, and I/O expansion.

| | |
|---|---|
| Introduced | 1982 |
| CPU | 1–3 × FOCUS, 32-bit, 18 MHz |
| Process | NMOS-III, 1.5 µm **[secondary]** |
| Control store | ~9,000 words × 38 bits, microcoded **[secondary]** |
| Cache | 16 KB, off-chip **[secondary]** |
| RAM | up to 10 MB |
| Internal disc | 9.896 MB formatted, 85 ms avg access, 205 ms max *(530/540 Service, 4849)* |
| OS | HP-UX 1.0–5.3. **The 540 does not run BASIC** — that is 520-only. |

The FOCUS instruction set is "patterned after the AMIGO machine instruction
set" and implements the HP 3000 stack architecture widened to 32 bits
*(ISET ERS, 4)*.

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
> per-CPU dedicated memory blocks (§6), and the `PMPB`/`SMPB` instructions;
> none of that is needed to boot.

---

## 2. Programmer's model

FOCUS is a **stack machine with no programmer-visible general-purpose
registers**. The registers below are the ones reachable from the instruction
set. All are 32 bits wide **except FLAGS, which is 8 bits** *(ISET ERS, 10)*.

Addressing is always relative to one of these registers, so code and data
segments stay relocatable. **The microcode bounds-checks every memory
reference** *(ISET ERS, 10)*.

### 2.1 Code segment registers *(ISET ERS, 10)*

| Reg | Name | Contents | Alignment |
|---|---|---|---|
| `P` | Program Counter | absolute address of the executing instruction | LSB = 0 |
| `PB` | Program Base | lowest absolute address of the code segment | 2 LSBs = 0 |
| `PL` | Program Limit | absolute address of the last word of the segment | 2 LSBs = 0 |

### 2.2 Stack registers *(ISET ERS, 10–11)*

| Reg | Name | Contents | Alignment |
|---|---|---|---|
| `SB` | Stack Base | lowest absolute address of the stack segment | 2 LSBs = 0 |
| `SL` | Stack Limit | highest absolute address of the stack segment | 2 LSBs = 1 |
| `S` | Stack Pointer | absolute address of top-of-stack | 2 LSBs = 1 |
| `Q` | Stack Marker Pointer | absolute address of the nearest stack marker | 2 LSBs = 1 |

The stack is **word-oriented**: in the ERS pseudocode, `S := S + 1` advances
one 32-bit word, not one byte *(ISET ERS, 57)*.

The top four stack words are named **A, B, C, D**, with A topmost
*(ISET ERS, 57)*.

### 2.3 Data segment registers *(ISET ERS, 11)*

| Reg | Name | Contents | Alignment |
|---|---|---|---|
| `DB` | Data Base | lowest absolute address of the global data segment | 2 LSBs = 0 |
| `DL` | Data Limit | highest absolute address of the data segment | 2 LSBs = 1 |
| `DST` | Data Segment Table Pointer | absolute address of entry 0 of the current user's DST | 2 LSBs = 0 |

### 2.4 Other registers *(ISET ERS, 11–14)*

| Reg | Name | Purpose |
|---|---|---|
| `X` | Index | **byte** offset used by indexed addressing modes |
| `STATUS` | Status | see §2.5 |
| `FLAGS` | Flags | 8-bit; debug-aid state, see §2.6 |
| `MSG` | Message | one bit per interrupt condition; a 1 causes an interrupt |
| `MASK` | Message Mask | 1 disables the corresponding `MSG` bit. Bits 3–5 hold the CPU's MPB channel number |
| `BRKPT` | Breakpoint | absolute address; a Breakpoint Trap fires just before executing it |
| `SAR` | Slave Address | receives the address half of an address/data pair written to this CPU over the MPB |
| `SDR` | Slave Data | receives the data half of the same |

`CIR`, the Current Instruction Register, holds the instruction being executed
but **is not accessible from the instruction set** — although instruction
descriptions refer to its bits (e.g. `CIR(19:1)`). 16-bit instructions are
right-justified in `CIR` with zero in the left halfword *(ISET ERS, 57)*.

> **Bit numbering.** Throughout the ERS, **bit 0 is the most significant
> (left-most) bit**, and a field is written `(N:M)` meaning M bits starting at
> bit N *(ISET ERS, 57)*. This is the opposite of the modern convention — get
> it wrong and nothing will work.

### 2.5 STATUS register layout *(ISET ERS, 12)*

The OCR of the field diagram is unusable, so this was read from the page image
at 400 dpi. Bit 0 is the MSB.

| Bits | Field | | Bits | Field |
|---|---|---|---|---|
| 0 | `TV` | | 11–12 | `RM` |
| 1 | `SL` | | 13 | `I` |
| 2 | `EP` | | 14 | `M` |
| 3 | `SS` | | 15 | *reserved* |
| 4 | `ES` | | 16–17 | `CC` |
| 5 | `IR` | | 18 | *reserved* |
| 6 | `C` | | 19 | `P` |
| 7 | `O` | | 20–31 | `CSN` |
| 8 | `T` | | | |
| 9 | `EM` | | | |
| 10 | `MI` | | | |

What makes this reading trustworthy is that it is *forced*: all seventeen
fields named in the accompanying text land in distinct, non-overlapping slots,
and together with bits 15 and 18 they tile the register exactly. There is no
slack for an alternative assignment. Bits 15 and 18 carry no label in the
diagram and no description in the text.

The layout is asserted in code — `test_status_layout()` in
[`src/test/statetest.cpp`](../src/test/statetest.cpp) checks the
non-overlap-and-tiling property, so a future edit that breaks it fails the
build rather than silently corrupting the model.

Field meanings:

| Field | Meaning |
|---|---|
| `P` | Instruction Pending — set by an instruction that gets interrupted; cleared when it finally completes |
| `TV` | Trace Variables — enables the trace-variables trap |
| `SL` | Start of Line — enables the start-of-line trap |
| `EP` | End of Procedure — enables the end-of-procedure trap |
| `SS` | Start of Subroutine — enables that trap |
| `ES` | End of Subroutine — enables that trap |
| `M` | Mode — 1 = privileged mode |
| `I` | Interrupt — 1 enables Message Register interrupts |
| `T` | Trap — 1 enables user traps |
| `O` | Overflow — set by microcode on arithmetic overflow. **Cleared only by `BOV`, `BNO`, and `SETR`** |
| `C` | Carry — set to the carry out of an arithmetic operation |
| `MI` | Machine Instruction Trap — 1 requests a trap before each instruction |
| `EM` | Enable Machine Instruction Trap — managed by microcode; the trap fires only when `EM` **and** `MI` are both 1 |
| `CC` | Condition Code — `CCG`=00, `CCE`=10, `CCL`=01 or 11. Hardware only ever writes 01 for `CCL` |
| `RM` | Rounding Mode — 00 nearest-even (default), 01 toward +∞, 10 toward zero, 11 toward −∞ |
| `IR` | Inexact Result — set by binary FP when the result is inexact. **Cleared only by `BIR` and `SETR`** |
| `CSN` | Code Segment Number — CST entry of the executing code segment |

Three condition-code interpretations:

- **CCA** — `CCL` if operand < 0, `CCE` if = 0, `CCG` if > 0
- **CCB** — `CCL` if special ASCII, `CCE` if alphabetic `[a-zA-Z]`, `CCG` if numeric `[0-9]`
- **CCC** — `CCL` if op1 < op2, `CCE` if equal, `CCG` if op1 > op2

### 2.6 FLAGS register *(ISET ERS, 13)*

Eight bits used with the debug-aid instructions (`SOL`, `SOP`, `EOP`, …):

| Bit | Set to 1 when |
|---|---|
| 0 | breakpoints/line traces requested anywhere in the current user's program |
| 1 | breakpoints/line traces requested anywhere in the OS (incl. sharable code) |
| 2 | the user is stepping through any of their procedures |
| 3 | any OS procedures are being stepped through |
| 4 | line-count data is being generated for any user procedure |
| 5 | line-count data is being generated for any OS procedure |
| 6 | any variables in the user's program are being traced |
| 7 | any variables in the OS are being traced |

These combine with the **two most significant bits of `CSN`** to derive an
overall debug bit:

| CSN MSBs | Overall debug bit = 1 iff |
|---|---|
| 00 | flag bits 1, 3, 5, or 7 |
| 01 | flag bits 1, 3, 5, 6, or 7 |
| 10 | flag bits 1, 3, 5, 6, or 7 |
| 11 | flag bits 0, 2, 4, or 6 |

### 2.7 MSG (Message) register *(ISET ERS, 13)*

| Bit | Condition |
|---|---|
| 0 | memory double-bit error |
| 1 | MPB slave address error |
| 2 | MPB slave data error |
| 3–5 | always zero |
| 6 | MPB slave channel write data valid |
| 7 | illegal I/O opcode |
| 8 | MPB error detected by I/O |
| 9 | double-bit error detected by I/O |
| 10–11 | unused |
| 12 | used by the `WTC` instruction |
| 13 | timer interrupt |
| 14 | I/O attention request acknowledge |
| 15 | Memory Controller interrupt |
| 16–31 | I/O device interrupts |

---

## 3. Addressing

### 3.1 Base register selection

Every memory-referencing instruction carries a 3-bit `BASE` field. The offset
is **always a positive quantity** *(ISET ERS, 53)*; the base encoding chooses
the direction:

| BASE | Effective location |
|---|---|
| 0 | `P + offset` |
| 1 | `P − offset` |
| 2 | `DB + offset` |
| 3 | `DL − offset` |
| 4 | `Q + offset` |
| 5 | `Q − offset` |
| 6 | `SB + offset` |
| 7 | `S − offset` |

Modes: top-of-stack, direct, indirect, indexed, and indirect+indexed. Unless an
instruction says otherwise, **post-indexing** is used *(ISET ERS, 57)*.
All indirect pointers are full words and must be word-aligned *(ISET ERS, 57)*.
`X` holds a **byte** index.

The three most significant bits of every absolute address must be zero
*(ISET ERS, 54)* — so the architectural address space is 29 bits.

### 3.1.1 Indirect pointer formats *(ISET ERS, 16–17)*

Four pointer formats. The latter three are collectively *data segment
pointers*; the first is used only with code segments.

| Format | Tag (bits 0–1) | Payload | Target |
|---|---|---|---|
| Self-relative | — | 32-bit **signed** byte offset | address of the pointer + offset |
| Stack segment | `11` | 30-bit offset, bits 2–31 | `SB` + offset |
| Global data segment | `10` | 30-bit offset, bits 2–31 | `DB` + offset |
| External data segment | bit 0 = `0` | bit 1 selects system/user DST, segment number (2:11), offset (13:19) | via the DST |

Self-relative pointers "can be distinguished from machine instructions only by
their usage" *(ISET ERS, 16)* — nothing in the encoding marks them.

### 3.1.2 Bounds *(ISET ERS, 18)*

The microcode detects every access outside these ranges:

| Segment | Range |
|---|---|
| Code | `PB` to `PL` |
| Stack | `SB` to **`S`** |
| Global data | `DB` to `DL` |
| Unpaged external data | starting address to starting address + length − 1 |

Note the stack is bounded by `S`, the stack *pointer*, not by `SL` — addresses
above the top of stack are out of bounds even though the segment extends
further. For chained external data segments the tests are applied individually
to each segment in the chain.

### 3.1.3 Worked example — ERS Figure 2.1 *(sheet 19)*

The ERS supplies a complete worked address computation: memory contents for a
program, stack, global data and external data segment, an index register of
20, and the resulting top-of-stack for ten `LOAD` instructions. It is the best
available conformance test for an addressing implementation, and it is
reproduced exactly by
[`src/test/addrtest.cpp`](../src/test/addrtest.cpp).

| Instruction | Result | Path exercised |
|---|---|---|
| `LOAD P+20` | −60 | direct |
| `LOAD P+20,I` | 10 | self-relative pointer |
| `LOAD P+20,X` | 100 | indexed |
| `LOAD P+20,I,X` | 5 | **post**-indexing after indirection |
| `LOAD Q+12,I` | 300 | stack segment pointer |
| `LOAD Q+32,I` | 27 | global data segment pointer |
| `LOAD Q−20,I` | −63 | external data segment pointer |
| `LOAD Q−20,I,X` | 31 | index applied to the EDS *offset* |
| `LOAD DB+60,I` | 18 | global pointer from global data |
| `LOAD DB+72,I` | 52 | EDS pointer from global data |

Two semantics are pinned down by this figure that the prose leaves implicit:
indirection resolves **before** indexing, and for an external data segment the
index is added to the offset within the segment rather than to the resolved
absolute address.

### 3.2 Data segment pointer evaluation

The ERS gives explicit pseudocode for resolving a data segment pointer `PTR`
to an absolute address *(ISET ERS, 55–56)*; a simulator needs this verbatim.
Dispatch is on the top two bits:

- `PTR(0:2) = 2` → `ADDR := DB + PTR(2:30)`; bounds-check against `DB`/`DL`.
- `PTR(0:2) = 3` → `ADDR := SB + PTR(2:30)`; bounds-check against `SB`/`S`.
- `PTR(0:1) = 0` → external data segment. `PTR(1:1)` selects the system vs.
  user DST; `PTR(2:11)` is the segment number; `PTR(13:19)` is the offset.
  This path walks the DST entry, handles **segment chaining** (following
  `LINK` when the offset runs past `LENGTH`) and, for paged segments, indexes
  a page table at `LOCATION + 8*PAGENUM`.

Traps raised along the way: EDS Pointer Violation, DST Violation, EDS Bounds
Violation, Data Segment Write Violation, Privileged Mode Data Violation,
Absent Segment / Absent Page Table / Absent Page, Page Table Violation,
Unexpected EDS Type.

Side effects to model: the `R` (referenced) bit is set on every access, and the
`D` (dirty) bit is set by any *modify-memory* instruction. Table entries are
read and written back under a semaphore *(ISET ERS, 55)*.

### 3.3 Data formats *(ISET ERS, §2.3)*

Integers, binary floating point (IEEE 754 **Draft 5.11** — single and double,
no extended format), decimal (BCD) numbers, byte strings, and the stack marker.
See §7 for the floating-point conformance detail.

---

## 4. Segmentation

Two segment kinds — **code** and **data** — described by two tables.

- **Code Segment Table (CST)** with an associated **Segment Transfer Table
  (STT)** *(ISET ERS, 25–28)*. `PCL`/`PCLS`/`LPP`/`LPPS` take STT entry numbers.
- **Data Segment Table (DST)**, with **data segment chaining** and optional
  paging *(ISET ERS, 30–36)*.

There are three CST pointers — system, sharable, and per-user — plus a Device
Reference Table (DRT) with one entry per I/O device. See §6 for where they live.

---

## 5. Instruction encoding

Both 16- and 32-bit formats. **254 opcodes** are available; 32-bit
instructions are always full-word aligned *(ISET ERS, 53)*. All opcodes in the
ERS are quoted in **octal**.

The 16-bit base format is:

```
 bit  0  1  2 | 3 | 4 | 5  6  7 | 8 ............... 15
      OP1     | I | X |  BASE   |      OFFSET
```

Six formats in total:

| Fmt | Selected by | Layout |
|---|---|---|
| 1 | `OP1` = 2..6 | `OP1(0:3) I X BASE OFFSET(8:8)` — the direct 16-bit memory ops |
| 2 | `OP1` = 0 | `000 b3 OP2(4:6) OPERAND(10:6)` — 6-bit operand forms |
| 3 | `OP1` = 0, `OP2` = 0 | `000 b3 000000 OP3(10:6)` — stack operations |
| 4 | `OP1` = 0, `OP2` = 1 | `000 b3 000001 OP3(10:6)` — specials |
| 5 | `OP1` = 1 | `001 OP2(3:5) OPERAND(8:8)` — 8-bit operand forms; **no index or indirect bit** |
| 6 | `OP1` = 7 | `111 OP2(3:5) I X BASE(10:4) OFFSET(13:19)` — 32-bit instructions |

In formats 2, 3 and 4, **bit 3 serves as the indirect bit, the index bit, or an
instruction-specific indicator**, depending on the instruction *(ISET ERS, 54)*.

In format 6, `BASE` is **3 bits at 10–12**, with `OFFSET` running 13–31. (The
ERS text reads ambiguously as "bits 10-13", but the tick marks under the field
diagram are `0 3 8 9 10 13 31`, Appendix B shows a combined `I X Bas` field
spanning bits 8–12, and 13–31 is exactly the 19 bits the text specifies.
Confirmed empirically — see §5.2.)

### 5.1.1 Conditional branches *(ISET ERS, 90)*

`BCCS` (format 1, opcode 6) and `BCC` (format 6, opcode 31) do not use the
normal `I X BASE` layout. Their fields are:

| | `BCCS` (16-bit) | `BCC` (32-bit) |
|---|---|---|
| Condition mask `CCF` | bits 4–6, i.e. `CIR(20:3)` | bits 9–11, i.e. `CIR(9:3)` |
| Sign | bit 7, `CIR(23:1)` | bit 12, `CIR(12:1)` |
| Offset | bits 8–15, **halfwords** | bits 13–31, **bytes** |

Sign is 0 = `+`, 1 = `−`. The mask bits select which condition codes branch:
`CCF(0:1)` for `CCG`, `CCF(1:1)` for `CCE`, `CCF(2:1)` for `CCL`. All three set
is an unconditional branch. The target is relative to `P`, which holds the
address of the *executing* instruction — confirmed empirically against real
code (§5.2).

### 5.1.2 Bit-field instructions *(ISET ERS, 116)*

`EXF` (Extract Field, format 6 opcode 0) and `DPF` (Deposit Field, format 6
opcode 1) also depart from the `I X BASE OFFSET` layout:

| Field | Bits | Meaning |
|---|---|---|
| sign | 8 | `EXF` only: 0 zero-extends the result, 1 sign-extends it |
| index | 9 | if set, `CIR(22:10) += X` before use |
| — | 10–21 | must be zero |
| `K` | 22–26 | 32 − the number of bits in the field |
| `J` | 27–31 | the first (leftmost) bit of the field |

The field width is `L = 32 − K`. `EXF` extracts bits `J … J+L-1` from the
top-of-stack, right-justifies them and pushes the result; `DPF` deposits the
`L` least significant bits of the top-of-stack into bits `J … J+L-1` of the
second stack word. Bits beyond 31 read as 0 and are lost on deposit. Both set
`CCA` on the new top-of-stack.

The requirement that bits 10–21 be zero is a useful decoder sanity check, and
it holds in real code: `e00003b1` in the HP-UX kernel decodes to
`EXF start=17, width=3` with those bits clear.

### 5.1 Opcode map

The complete opcode map is transcribed in **[opcodes.csv](opcodes.csv)**, taken
from **Appendix B** (sheet 137) — a single fold-out chart, reproduced as
[notes/appendixB-137.png](notes/appendixB-137.png).

**Appendix B is authoritative.** The OCR of the section 6.2 prose disagrees with
it in several places; where they conflict, the chart is correct. Two confirmed
OCR errors in the prose: `LRA` is format 6 opcode **36** (not 35), and `STB` is
format 6 opcode **34**, with **35** being `STD` and **25** being `STH`.

Instruction groups *(ISET ERS, §6.2)*: load/store (58), stack & register
manipulation (67), arithmetic & logical (74), program control & branch (90),
move/scan/string (101), I/O & interrupt (114), special (116).

### 5.2 Empirical validation

The opcode map and the format decoding have been checked against real FOCUS
code — the HP-UX 5.01 kernel and the BASIC 2.0 boot disc — using
[`tools/focusdis.py`](../tools/focusdis.py).

The decisive evidence is structural. At offset `0x100` in `os.req`:

```
LDH    Q-3,X
BCCS.GE  +26   -> 0x11e
DUP / ANDI #255 / DUP
CMPI #4  / BCCS.E +20 -> 0x122      case 4
DUP
CMPI #2  / BCCS.E +60 -> 0x150      case 2
CMPI #1  / BCCS.E +80 -> 0x168      case 1
DEL
BCCS.ALW +92 -> 0x178               default
```

A switch dispatch whose three case targets hold structurally parallel bodies
(`0x122` and `0x150` both open `DEL / INCM Q-n`), and whose two fall-through
paths converge on the same default. That cannot arise from a mis-decoded
opcode table.

Bulk statistics, over the HP-UX kernel and BASIC, against a control region of
English manual text from `SHPUX_MAN`:

| Region | Branch density | Targets on an instruction boundary |
|---|---|---|
| HP-UX kernel `0x100`–`0x8000` | 7.0% | 99.9% (chance 89.9%) |
| BASIC `500BT1` `0x900`–`0x2400` | 9.7% | 99.7% (chance 91.2%) |
| **Control: manual text** | **0.1%** | — (chance 100%, no signal) |

Note that a raw "fraction of bytes that decode to a known opcode" figure is
worthless here — 254 of 256 encodings are assigned, so English text scores
99.3%. Branch density is the discriminator, and it differs by a factor of
70–97×. Zero branch targets landed on an odd address in any region, as
required since `P`'s LSB must be 0.

---

## 6. Dedicated memory locations *(ISET ERS, Appendix D, sheet 143)*

Addresses are **octal**.

| Location | Use |
|---|---|
| `0` | System Code Segment Table pointer |
| `4` | Sharable Code Segment Table pointer |
| `10` | reserved — external data segment pointer to the first paged data segment in the system DST |
| `10`–`14` | reserved |
| `20`–`174` | CPU dedicated locations |
| `200`–`1774` | Device Reference Table |
| `2000`–`2174` | head and tail pointers |
| `2200`– | System Data Segment Table |

**CPU dedicated locations** are four-word blocks. A CPU on MPB channel *N*
(1 < N < 7) uses the block at decimal address `16*N`:

| Word | Contents |
|---|---|
| 1 | User Code Segment Table pointer |
| 2 | Current Task Control Block pointer |
| 3 | `QI` — interrupt value for `Q` |
| 4 | `SLI` — interrupt value for `SL` |

All three CST pointers and the TCB pointer are absolute addresses ending in
`00`; `QI` and `SLI` must end in `11`.

**Head/tail pointers** are used by the CPU and I/O hardware and must not be
touched by the OS. For priority level *N* (0 < N < 15) the head pointer is at
decimal `1028 + 8*N` and the tail at decimal `1024 + 8*N`. Each holds the
absolute address of the DRT entry for the first (last) device queued at that
priority; a zero tail pointer means the list is empty.

**Device Reference Table** — one four-word entry per possible device. A device
on MPB channel *N* with peripheral address *P* has device number
`128*N + 16*P` (decimal), **which is also the absolute address of its DRT
entry**. Entry format is in ERS §5.1.3.

---

## 7. Interrupts and traps

- **External interrupts** *(ISET ERS, 38–43)* — structure, response time, and
  the interrupt handler.
- **Internal interrupts and traps** *(ISET ERS, 44–52)* — the interrupt/trap
  sequence and the full list of causes.

Interrupt response time is **~25 µs**, rising to **~32 µs** if the `IXIT`
workaround in §9 is applied *(ISET ERS, Appendix G)*.

### 7.1 IEEE floating point *(ISET ERS, Appendix F, sheet 145)*

Conformance is against **Draft 5.11** of the proposed IEEE standard (as
published in the October 1979 ACM SIGNUM Newsletter), not the ratified 754-1985.

- Single and double basic formats. **No extended format.**
- All four rounding modes, selected by `RM` in `STATUS` via the `SRM` instruction.
- Provided: add, subtract, multiply, divide, negate, compare, and conversions
  between single/double/32-bit integer.
- **Not provided in hardware**: square root, remainder, integer-part.
- Fast binary↔BCD conversion instructions exist, but use algorithms that differ
  from the standard's; full conformance is left to system software.
- Comparison sets `CC` to `00` for `>`, `10` for `=`, `01` for `<`.

Appendix E (sheet 144) gives the exact instruction sequences for decimal↔binary
floating-point conversion — useful as simulator test vectors.

---

## 8. I/O subsystem

Source: **FOCUS I/O ERS** (I/O System ERS, F. Gross, 09/20/82, 80 sheets).

The I/O channel is the **IOP**, "a valid implementation of a subset of a CSG
Channel Adapter" *(IO ERS, 4)*.

- **8 I/O channels per IOP**, each with DMA *(Hardware Technical Data)*.
- Bandwidth: 5.1 MB/s burst, 973 KB/s multiplexed **[secondary]**.
- Conforms to the **HP-CIO** (HP Channel I/O) standard.
- Up to three IOPs; each additional IOP reaches the rear panel by 50-conductor
  ribbon cable and can drive a **97098A I/O Expander** (eight more slots).

### 8.1 Select codes *(530/540 Service, sheet 2-27)*

| Select code | Assignment |
|---|---|
| 0–6 | I/O backplane |
| 7 | System Control Module |
| 8–15 | first additional IOP |
| 16–23 | second additional IOP |

Backplane slots are physically labelled with their select code numbers.

### 8.2 IOP functional areas *(IO ERS, table of contents)*

System communication; interface polling; direct I/O; DMA (single and multiple
transfers per poll response, plus DMA Verify and DMA Match options); interrupt
requests (Interrupt Request Register, Interrupt Mask Register, mapping
peripheral address → interrupt level); MPB I/O commands; the interval timer;
initialisation and self-test.

CPU-side I/O is done with three instructions — **`IOW`, `IOR`, `IOC`** (format
5, opcodes 17, 20, 21 octal), each taking an I/O opcode as its 8-bit operand
*(IO ERS, §6)*.

### 8.3 System Control Module *(530/540 Service, sheet 2-27)*

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

### 8.5 Bootstrap protocol *(530/540 Service, sheet 2-30/2-31)*

The 8 KB loader ROM lives on the SCM, addressed through the same locations as
the RTC and NVM. **The mechanism is fully documented**, even though no dump of
the ROM contents is known to exist (§10).

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
> exists — see [SOFTWARE.md](SOFTWARE.md#the-loader-rom-question).

### 8.4 Peripherals

| Card | Function |
|---|---|
| 27110A | HP-IB, IEEE-488-1978, up to 14 devices, DMA with two performance modes, controller or slave, hardware self-test |
| 27128A | single-channel asynchronous serial, RS-232C / V.24 / V.28 |
| 27130A | 8-channel asynchronous multiplexer, two 512-byte buffers per channel |
| 27132A | MP-CIO, 8/16-bit parallel with DMA; clocked and transparent modes |
| 98760 | graphics |
| 97060A | graphics processor, with built-in HP-IB |

---

## 9. Known hardware bugs *(ISET ERS, Appendix G, sheets 148–149)*

A simulator should reproduce these only if it aims for bug-compatibility, but
it **must** understand them because contemporary software was written to avoid
them.

The CPU release date is readable from **control store location 6 via the `RCS`
instruction**; the low 32 bits are 8 hex digits, `YYYYMMDD`.

### Present in CPU 4.0 (1982-03-19) and earlier

- **`IXIT`** — stack overflow if interrupted at the wrong moment. Avoided by
  disabling interrupts before every `IXIT`, which raises interrupt response
  time from ~25 µs to ~32 µs.
- **Instruction alignment** — it is not possible to branch to a 16-bit
  instruction sitting in the right half of a 32-bit word whose left halfword
  holds arbitrary data. Affects every non-sequential fetch: `PCL`, `EXIT`,
  `IXIT`, `SXIT`, `ESCP`, `DISP`, `PSEB`, all branches, all traps, all
  interrupts. If the right halfword is a branch target, the left halfword must
  look like a 16-bit instruction.
- **`SAS`** — no trace-variable trap when the source string's current length is
  zero.
- **Reset trap** — the CPU enters the handler in *secondary* address mode; an
  `IOR` in the handler switches it to primary.

### Fixed in CPU 4.0; present in 3.3 (1981-11-02) and earlier

- **`BRSI`** — must not be used with external program pointers; causes
  erroneous instruction sequencing.
- **`ESCP`** — with `CIR(19:1) = 0`, can lose the delta-Q entry of a pseudo
  stack marker created by `TRY`. Workaround: precede with `DUP`.
- **`LDD(S)`, `LDSI`, `STD`, `SDSI`** — can touch non-existent memory (slave
  address + slave data error) when the first word accessed is the last word in
  a segment or page with nothing mapped after it.
- **Unimplemented instruction trap** — format 4 opcode 75, format 5 opcode 30,
  and format 6 opcode 37 should trap but do not.

---

## 10. Open questions

1. **SCM loader ROM** — no dump located. The protocol is documented (§8.5) and
   both acquired systems supply the `SYSTEM POWERUP` entry point, so this is a
   soft blocker.
2. **Memory Controller ERS** — referenced repeatedly by the Instruction Set ERS
   (for `SAR`/`SDR` and MPB semantics) but **not present on bitsavers**.
3. **Stack marker layout** — ERS §2.3.5, sheet 23; not yet transcribed.
4. **Disc interface** — the internal 9.9 MB drive's controller and protocol are
   not yet pinned down.
5. **SYSTEM POWERUP entry point** — the module layout is understood (32-byte
   binary header, 16-byte name, variable-length SCCS string, body) but the
   header words `1ffc / 0000ce2c / 00001064 / e307fffc` are unidentified and
   the exact entry offset is not yet known. Dense code begins around `0x100`.

*Resolved:* format 6 `BASE` width (3 bits, §5) and the `BCCS`/`BCC` branch
encoding (§5.1.1), both confirmed against real code; the `STATUS` bit layout
(§2.5), read from the page image and forced by a tiling argument; and the
`EXF`/`DPF` sub-fields (§5.1.2).

---

## Sources

Primary, in [bitsavers/](bitsavers/):

| File | Notes |
|---|---|
| `FOCUS_Instruction_Set_ERS_Dec82.pdf` | **The** CPU reference. 150 sheets, A-1FE1-3020-B, J. Fiasconaro, 09/23/82, issued 12/06/82 |
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

Secondary: [OpenPA — HP FOCUS Processor](https://www.openpa.net/focus_processor_hp.html),
[OpenPA — HP 9000 500 FOCUS Computers](https://www.openpa.net/systems/hp-9000_520.html).
