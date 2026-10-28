# The HP FOCUS Processor — Architecture and Instruction Set

A modern specification of the HP FOCUS 32-bit microprocessor (1982), the CPU
of the HP 9000 Series 500. It replaces day-to-day reliance on the OCR of the
*Focus Machine Instruction Set ERS* (A-1FE1-3020-B, J. Fiasconaro, 12/06/82),
whose text layer is badly damaged, with a single document reconstructed from
three cross-checked sources:

1. **The Appendix B opcode chart** (sheet 137), legible at 400 dpi and hand
   transcribed into [opcodes.csv](opcodes.csv). Authoritative for encodings,
   and since confirmed opcode-for-opcode by the instruction tables inside
   HP's own recovered toolchain (`/usr/lib/pascomp` and `/bin/as` —
   Appendix B).
2. **The emulator in [src/cpu/focus/](../src/cpu/focus/)** — an execution
   core carrying 522 unit assertions, all ten worked examples of ERS
   Figure 2.1, and a disassembler differentially tested over 550,030 real
   instructions. Authoritative for the semantics it implements.
3. **The ERS prose** (sections 2–6), used where legible, and always
   cross-read against the other two. Where the prose OCR conflicts with the
   chart, the chart wins; every such conflict is listed in
   [Appendix B](#appendix-b--adjudicated-readings) of this document.

Statements below that rest on a contested or damaged reading carry an
explicit note. Everything else is stated plainly and is safe to build on.

Machine-level context (the MPB bus, IOP, memory controller, boot path) is in
[HARDWARE.md](HARDWARE.md); this document is the CPU and its instruction set.

---

## Conventions

These conventions are the ERS's own, kept because every primary source and
all project tooling uses them. **Get the first one wrong and nothing works.**

- **Bit 0 is the most significant (leftmost) bit.** A field written `(N:M)`
  means *M bits starting at bit N*. So in a 32-bit word, `(0:1)` is the sign
  bit and `(29:3)` is the low three bits.
- **Opcodes are octal**, as in the ERS and Appendix B.
- **FOCUS is big-endian**: byte 0 of a word occupies bits `(0:8)`.
- A **word** is 32 bits; a halfword 16; a doubleword 64. The stack is
  word-oriented: `S := S + 1` in ERS pseudocode advances one word.
- **`E`** denotes the effective address computed by an instruction's
  addressing fields (§5).
- **`(S)`** is the word at the top of stack, `(S-1)` the word beneath it, and
  so on. The top four stack words are also named **A, B, C, D**, A topmost.
- Two pointer conventions coexist in ERS pseudocode: a top-of-stack style
  pointer (`S`, `Q`, `SL`, `DL`) ends in binary `11` and addresses the last
  byte of its word; a base pointer (`SB`, `DB`, `PB`, `DST`) ends in `00`.
- **`b`** denotes the per-instruction modifier bit, `CIR(19:1)` — bit 3 of a
  16-bit instruction (§8.2).

---

## 1. The processor

FOCUS is the first commercial 32-bit microprocessor: a microcoded, segmented,
stack-oriented CPU with **no programmer-visible general-purpose registers**.
The ERS describes the instruction set as "patterned after the AMIGO machine
instruction set" — it is the HP 3000 stack architecture widened from 16 to 32
bits, with the 3000's code/data segmentation, stack markers, and privileged
mode carried over largely intact.

| | |
|---|---|
| Introduced | 1982, in the HP 9000 Series 500 (models 520/530/540/550) |
| Word size | 32 bits, big-endian |
| Address space | 29-bit byte-addressed (the 3 MSBs of every address are 0) |
| Clock | 18 MHz |
| Process | NMOS-III, 1.5 µm *[secondary]* |
| Microcode | ~9,000 words × 38 bits of control store *[secondary]* |
| Registers | 18 architectural registers, all special-purpose |
| Instructions | 16- and 32-bit encodings; 207 mnemonics over 212 of 254 opcode points (four known only from HP's assembler, §10.19) |
| Floating point | IEEE 754 **Draft 5.11**, binary32/binary64, in microcode |
| Multiprocessing | up to 3 CPUs on the Memory Processor Bus under OS control |

Two consequences of the stack design shape everything below: every operand
either lives on the stack or is named relative to a base register, so code
and data are position-independent by construction; and the microcode
bounds-checks **every** memory reference against the current segment, so
there is no unchecked address arithmetic anywhere in the instruction set.

The CPU's own revision is software-readable: control store location 6, read
with `RCS`, holds the release date as eight hex digits `YYYYMMDD`. Software
uses it to select workarounds for the silicon bugs in Appendix C.

---

## 2. Machine model

### 2.1 Memory

Byte-addressed, big-endian, 2^29 bytes. Halfwords must be 2-byte aligned,
words and doublewords 4-byte aligned. 32-bit instructions must be word
aligned; 16-bit instructions halfword aligned.

### 2.2 Data types

| Type | Size | Notes |
|---|---|---|
| Byte | 8 | unsigned in loads (`LDB` zero-extends) |
| Halfword integer | 16 | two's complement; `LDH` sign-extends on load |
| Word integer | 32 | two's complement; the native type |
| Doubleword integer | 64 | high word at the lower address / deeper stack slot |
| Binary floating, single | 32 | IEEE Draft 5.11 basic single = binary32 |
| Binary floating, double | 64 | = binary64; high word deeper on the stack |
| Decimal floating | 96 | three words, see below |
| Decimal integer | 32 | 8 BCD digits, unsigned (`DLAD`/`DLSB`) |
| Byte string | var | current-length word + byte array, see §2.3 |
| Pointers | 32 | four data formats + two program formats, §5.2–§5.3 |
| Stack marker | 4 words | §4.2 |

**Decimal floating point** *(ERS §2.3.3, sheet 22)* — three words:

```
word 0   | EXPONENT (0:10) | 0 0 0 0 0 | S |  D1  D2  D3  D4 |
           0                 10          15  16            31
word 1   |  D5  D6  D7  D8  D9  D10  D11  D12  |
word 2   |  D13 D14 D15 D16 D17 | 0 ... 0 (12 bits) |
```

`S` is the sign; the exponent is a 10-bit two's complement power of ten.
Numbers are normalized with the decimal point between D1 and D2 and D1 ≠ 0;
zero is all three words zero. Each digit is BCD `0000`–`1001`. The 8-digit
unsigned decimal integer format is exactly word 1 of this layout.

### 2.3 Byte strings *(ERS §2.3.4)*

A string variable is a word holding the **current length** C, followed by a
byte array of allocated (**maximum**) length M; bytes are numbered 1..M and
only the first C are valid. The string instructions (§10.13) operate through
a four-word **string descriptor** on the stack:

```
(S-3)  MAXLEN   maximum length M of the target string variable
(S-2)  PTR      data segment pointer (or local program pointer) to the
                current-length word; forced word-aligned
(S-1)  FIRST    first byte of the substring, 1-based
(S)    LEN      substring length; negative means "through end of string"
```

`SLD` and `SCON` build **string temps** on the stack: an `NWORDS` size word,
the string variable itself, and a descriptor with `FIRST=1`, `LEN=-1`.

---

## 3. Registers

Eighteen registers are reachable from the instruction set. All are 32 bits
except FLAGS (8 bits). The low-bit patterns are architectural invariants:
a register that must end in `11` points at the last byte of its word.

| Reg | Name | Contents | Low bits |
|---|---|---|---|
| `P` | Program Counter | absolute address of the **executing** instruction | `0` |
| `PB` | Program Base | first byte of the current code segment | `00` |
| `PL` | Program Limit | last word of the current code segment | `00` |
| `SB` | Stack Base | first byte of the stack segment | `00` |
| `SL` | Stack Limit | last byte of the stack segment | `11` |
| `S` | Stack Pointer | last byte of the top-of-stack word | `11` |
| `Q` | Stack Marker Pointer | last byte of the newest stack marker | `11` |
| `DB` | Data Base | first byte of the global data segment | `00` |
| `DL` | Data Limit | last byte of the global data segment | `11` |
| `DST` | Data Segment Table Pointer | entry 0 of the current user's DST | `00` |
| `X` | Index | **byte** offset used by indexed addressing; scratch for several instructions | — |
| `STATUS` | Status | §3.1 | — |
| `FLAGS` | Flags (8-bit) | debug-aid summary bits, §3.2 | — |
| `MSG` | Message | one bit per interrupt condition, §3.3 | — |
| `MASK` | Message Mask | 1 disables the matching MSG bit; `(3:3)` holds the CPU's MPB channel number | — |
| `BRKPT` | Breakpoint | absolute address; Breakpoint Trap fires just before executing it | — |
| `SAR` | Slave Address | address half of an MPB pair written to this CPU | — |
| `SDR` | Slave Data | data half of the same | — |

`CIR`, the Current Instruction Register, holds the executing instruction but
is not itself addressable. A 16-bit instruction sits **right-justified** in
CIR with zero in the left halfword — which is why instruction descriptions
name fields like `CIR(19:1)` even for 16-bit forms.

The four topmost stack words A–D are held in an on-chip register cache; the
cache is transparent to software.

### 3.1 STATUS

Read by `PSHR`, written by `SETR` (with the restrictions under `SETR`,
§10.4), saved in every stack marker, and partially cleared on trap entry.
The field diagram's OCR is unusable; this layout was read from the page
image and is forced — the seventeen described fields tile the register
exactly, with no alternative assignment possible. It is asserted in
`src/test/statetest.cpp`.

```
bit   0    1    2    3    4    5    6    7    8    9    10   11-12
     TV   SL   EP   SS   ES   IR   C    O    T    EM   MI    RM

bit  13   14   15    16-17   18   19   20-31
      I    M   rsv     CC    rsv   P    CSN
```

| Field | Meaning |
|---|---|
| `TV` | Trace Variables trap enable |
| `SL` | Start of Line trap enable |
| `EP` | End of Procedure trap enable |
| `SS` | Start of Subroutine trap enable |
| `ES` | End of Subroutine trap enable |
| `IR` | Inexact Result — set by binary FP; cleared only by `BIR` and `SETR` |
| `C` | Carry out of an arithmetic operation |
| `O` | Overflow — cleared only by `BOV`, `BNO`, and `SETR` |
| `T` | User trap enable (integer overflow, divide by zero) |
| `EM` | Enable Machine-instruction trap — microcode-managed; the trap fires only when `EM` and `MI` are both 1. Cleared by the trap itself before its marker is pushed (note 4), set again when an instruction completes |
| `MI` | Machine-Instruction trap request — trap before each instruction, **in either mode**. HP-UX 5.30 delivers signals with it: the syscall stub sets `MI` (`PSHR 4; OR #0x00200000; SETR 4`) and restarts the interrupted call without leaving the kernel, and the trap taken at the very next instruction is what builds the handler call. `MI` and `EM` are carried across `EXIT` and `IXIT` rather than restored from the marker — the handler's cleared status must survive its own return, or the trap re-fires on the instruction it returned to. Both facts were read off the kernel's code, not the ERS, and are the emulator's rule since 2026-08-28 |
| `RM` | FP rounding mode: `00` nearest-even, `01` toward +∞, `10` toward 0, `11` toward −∞ |
| `I` | Interrupt enable for Message register interrupts |
| `M` | Mode: 1 = privileged |
| `CC` | Condition code: `CCG`=00, `CCL`=01, `CCE`=10. Hardware never writes `11`, but software may (`BUN` branches on it); `11` is accepted as `CCL` |
| `P` | Instruction Pending — set when a long instruction is interrupted mid-flight; cleared when it completes |
| `CSN` | Code Segment Number of the executing segment (CST entry) |

**Condition code interpretations.** Instructions set CC under one of three
rules, named in each description:

- **CCA** (arithmetic): `CCL` if value < 0, `CCE` if = 0, `CCG` if > 0.
- **CCB** (byte): `CCL` for special ASCII, `CCE` for alphabetic
  `[a-zA-Z]`, `CCG` for numeric `[0-9]`.
- **CCC** (comparison): `CCL` if op1 < op2, `CCE` if equal, `CCG` if
  op1 > op2.

### 3.2 FLAGS and the overall debug bit

Eight bits summarizing, for the whole system, which debug facilities are
active anywhere. Bit n = 1 when:

| Bit | Condition |
|---|---|
| 0 | breakpoints/line traces requested somewhere in the current user's program |
| 1 | breakpoints/line traces requested somewhere in the OS |
| 2 | the user is stepping any of their procedures |
| 3 | any OS procedure is being stepped |
| 4 | line-count data is being generated for a user procedure |
| 5 | line-count data is being generated for an OS procedure |
| 6 | variables in the user's program are being traced |
| 7 | variables in the OS are being traced |

The **overall debug bit**, tested by `SOP`, derives from FLAGS and the two
MSBs of `CSN` (which classify the executing segment):

| CSN(0:2) | Overall debug bit = 1 iff FLAGS bit … |
|---|---|
| `00` | 1, 3, 5, or 7 |
| `01` | 1, 3, 5, 6, or 7 |
| `10` | 1, 3, 5, 6, or 7 |
| `11` | 0, 2, 4, or 6 |

### 3.3 MSG and MASK

A 1 in MSG requests an interrupt; a 1 in the same MASK bit suppresses it.
Interrupts as a whole are gated by `STATUS.I`.

| MSG bit | Condition |
|---|---|
| 0 | memory double-bit error |
| 1 | MPB slave address error |
| 2 | MPB slave data error |
| 3–5 | always 0 (the field holds the CPU channel number in **MASK**) |
| 6 | MPB slave channel write data valid (SAR/SDR loaded) |
| 7 | illegal I/O opcode |
| 8 | MPB error detected by I/O |
| 9 | double-bit error detected by I/O |
| 10–11 | unused |
| 12 | write-to-channel complete (used by `WTC`) |
| 13 | interval timer |
| 14 | I/O attention request acknowledge |
| 15 | Memory Controller interrupt |
| 16–31 | I/O device interrupts |

---

## 4. The stack

### 4.1 Organization

The stack grows **upward** (toward higher addresses): a push is
`S := S + 4 bytes`. Valid addresses in the stack segment run from `SB`
through `S` — addresses between `S` and `SL` belong to the segment but are
**out of bounds** until pushed. `SL` bounds growth: pushing past it raises
Stack Overflow.

`SL` is not the end of the segment. Above it every stack carries a **stack
overflow area of at least 64 words**, and the trap is *not* raised inside
the instruction that crosses `SL`: it is condition 6 of the
end-of-instruction list (§5.2), tested again just before the first
instruction of any trap or interrupt handler. The reserve is what makes
that safe — an instruction can run to completion after taking `S` past
`SL`, and the pending traps and interrupts still have somewhere to push
their markers. Cause 024's handler then runs **on the ICS**, so it arrives
through the interrupt frame rather than an in-stack marker, grows the
stack segment, and returns with `IXIT`; taken while already on the ICS,
the microcode simply raises `SL` by 64 words itself. Software leans on all
of this: BASIC's stacks routinely outgrow their segments, and its cause-024
handler is where they are extended.

### 4.2 The stack marker

`PCL`/`PCLS`, traps, and interrupts push a four-word marker and point `Q` at
it; `EXIT` (and `IXIT`) unwind it. Relative to the resulting `Q`:

```
(Q-3)   X                saved index register
(Q-2)   return address   PB-relative byte address of the next instruction
(Q-1)   STATUS           caller's status register
(Q)     ΔQ               S − Q_old : distance back to the previous marker
```

Walking `Q := Q − (Q)` therefore threads every dynamic frame back to the
stack base. Local variables sit above the marker (positive `Q+` offsets);
parameters below it (`Q−` offsets). One word below each frame's base a
compiler conventionally stores the enclosing lexical frame's pointer, which
is what `LIA` walks (§10.11).

`TRY` builds a *pseudo marker* of the same shape for language-level
exception handling, chained not through ΔQ but through a head pointer kept
at `(SB+2)`; see `TRY`/`ESCP` (§10.11).

---

## 5. Addressing

### 5.1 Effective address computation

Memory-referencing instructions carry indirect (`I`) and index (`X`) bits
and a 3-bit `BASE` field. The offset in the instruction is **always
positive**; the base encoding supplies the direction:

| BASE | E (direct) | Typical use |
|---|---|---|
| 0 | `P + offset` | constants, jump tables in code |
| 1 | `P − offset` | same, backward |
| 2 | `DB + offset` | global data |
| 3 | `DL − offset` | global data, end-relative |
| 4 | `Q + offset` | local variables |
| 5 | `Q − offset` | parameters |
| 6 | `SB + offset` | stack segment absolute |
| 7 | `S − offset` | temporaries near the top of stack |

Modes: direct, indirect, indexed, indirect+indexed. Unless an instruction
says otherwise, indexing is **post-indexing**: indirection resolves first,
then `X` (a **byte** count, sign extended) is added. `BR` is the notable
exception — it pre-indexes. Indirect pointers are full words and must be
word aligned.

With `I` set, the word at the direct address is fetched and interpreted as
a pointer: **self-relative** when the base is P-relative, a **data segment
pointer** otherwise. For an external data segment pointer, a post-index is
added to the pointer's *offset field* (checked against that segment's
bounds), not to the resolved absolute address.

The ten worked examples of ERS Figure 2.1 *(sheet 19)* exercise every path:
HP supplied the memory contents of a program, stack, global data and external
data segment, an index register of 20, and the resulting top-of-stack for ten
`LOAD` instructions. They are reproduced bit-exactly by
`src/test/addrtest.cpp`.

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

This figure is what pins down the two rules the prose leaves implicit:
indirection resolves **before** indexing, and for an external data segment
the index is added to the offset within the segment rather than to the
resolved absolute address.

### 5.2 Data segment pointers

Dispatch is on the top bits:

| Format | Tag | Payload | Resolves to |
|---|---|---|---|
| Self-relative | — | 32-bit signed byte offset | pointer's own address + offset (code segments only; nothing in the encoding marks these — context alone distinguishes them from instructions) |
| Stack segment | `(0:2)`=`11` | byte offset in `(2:30)` | `SB + offset` |
| Global data | `(0:2)`=`10` | byte offset in `(2:30)` | `DB + offset` |
| External (EDS) | `(0:1)`=`0` | DST select `(1:1)` (0=system, 1=user), segment number `(2:11)`, byte offset `(13:19)` | via the DST (§6.2) |

Bounds: stack pointers check `SB..S`, global pointers `DB..DL`, external
pointers the DST entry's length (per segment in a chain). Traps that the
evaluation can raise: EDS Pointer Violation, DST Violation, EDS Bounds
Violation, Data Segment Write Violation, Privileged Mode Data Violation,
Absent Segment / Page Table / Page, Page Table Violation, Unexpected EDS
Type. Side effects: every evaluation sets the entry's `R` (referenced) bit;
modify-class references set `D` (dirty); table updates happen under a
memory semaphore.

### 5.3 Program pointers

Stored in Segment Transfer Tables and used by `PCL`, `LPP`, `BR ,I` and
`BRSI`:

| Format | Tag | Payload |
|---|---|---|
| Local | `(0:1)`=`0` | PB-relative byte offset in `(2:30)`; a `U` (uncallable) bit |
| External | `(0:1)`=`1` | STT byte offset in `(1:19)` (4 × entry number), code segment number in `(20:12)` |

Evaluating an external program pointer walks the CST for the named segment,
requires the target STT entry there to hold a *local* pointer, and enters at
it. In unprivileged mode a transfer to an uncallable target raises
Uncallable Procedure; a privileged caller entering an unprivileged segment
**remains privileged**.

---

## 6. Segmentation

Two segment kinds, described by two table structures. Segments make the
29-bit space effectively larger than physical memory and give the OS its
protection model: nothing an unprivileged program can express escapes its
own code segment, stack, global data, and the external segments its DST
grants.

### 6.1 Code segments — CST and STT

Up to 4096 code segments per **Code Segment Table**; three CSTs exist
(system, sharable, per-user — pointers at the dedicated locations, §7.4).
A CST entry is four words; word 1 holds `LENGTH−1` (low two bits `11`) and
word 2 the segment's absolute `LOCATION`, exactly as in a DST entry. Entry
*N* is at `CST + 16N`. Segment attributes (mode, uncallable) live in the
entry; their word-0 bit positions have not yet been re-derived from the
scans.

Each code segment ends with its **Segment Transfer Table**: the word at
`PL` carries the STT length in `(13:19)`, and entry *n* is the word at
`PL − 4n`, holding a local or external program pointer. All inter-segment
control transfer goes through STT entries; `PCL`/`PCLS` and `LPP`/`LPPS`
name them by entry number.

**Code segment 1 is reserved**: its STT entries are the internal trap
vectors (§7.2).

### 6.2 Data segments — the DST

Up to **4096 four-word entries**, split 2048/2048 between operating system
and user. Entry 0 of each half is special: its first word holds 16 × the
number of entries in that half (= the half's byte size, doubling as the
bound for DST Violation). The system half is assumed at absolute `2200₈`;
the user half's base is the `DST` register. Entry *N* is at `base + 16N`.

```
word 0  |0| USE COUNT |  OS   | rsv |PAGESIZE|P|W|D|R|M|A|
         0  1......12  13..17 18.21 22....25 26 ....... 31
word 1  LENGTH − 1        (bits 30-31 = 11: lengths are multiples of 4)
word 2  LOCATION          (bits 30-31 = 00 for unpaged segments)
word 3  |C|  LINK  |            OS use             |
         0  1...12   13........................31
```

| Field | Meaning |
|---|---|
| `A` | Absent — segment/page table not in memory; rest of entry is then OS scratch |
| `M` | Mode — privileged-only access *(ignored for paged segments)* |
| `R` | Referenced — set on every access |
| `D` | Dirty — set on modification *(ignored for paged)* |
| `W` | Writable *(ignored for paged)* |
| `P` | Paged |
| `PAGESIZE` | page = 2^PAGESIZE bytes; that many low offset bits index within the page. Values 0 and 1 forbidden. *(ignored for unpaged)* |
| `USE COUNT` | tasks using the segment; traps on decrement past 0 or increment past 4095 |
| `C`, `LINK` | segment chaining, below *(ignored for paged)* |

**Paged segments**: word 2 locates a page table of two-word entries —
word 0 mirrors the flag layout (with bit 26 = 1), word 1 is the page frame
`LOCATION` (low PAGESIZE bits zero). The final address is
`page LOCATION OR within-page offset` — an OR, since frames are
page-aligned. Entry for page *n* is at `LOCATION + 8n`.

**Chaining**: when an offset runs past `LENGTH`, the walk continues.
`C`=0: scattered chain — `LINK` names the next DST entry and the spent
length is subtracted from the offset. `C`=1: the segments occupy
consecutive DST entries of equal length, so the walk indexes directly, and
`LINK` is the highest entry number of the run.

Bounds are inclusive of the last addressable byte (`start .. start +
LENGTH − 1`); the evaluation pseudocode's `< LENGTH−1` test on sheet 55 is
an off-by-one against sheet 18 and Appendix B usage, and is read inclusive.

---

## 7. Traps and interrupts

### 7.1 The internal trap sequence *(ERS §5.2.2, sheets 44–45)*

Code segment 1 is reserved for handlers and assumed resident. On any
internal trap or interrupt the microcode:

1. clears the condition and forms an external program pointer to segment 1,
   STT entry = the cause number;
2. pushes a stack marker (§4.2), return address = the next instruction (or
   the *same* instruction for restartable faults, with `STATUS.P` set);
   **which of the two is decided by the cause**, not by the instruction —
   see the note-1/note-2 split below;
3. adjusts code segment use counts — the exited segment's down, segment
   1's up (ERS 5.2.1 step 3); the handler's `EXIT` does the reverse, so
   an uncounted entry leaves the interrupted segment one higher for ever.
   HP-UX's relocator refuses any segment with a count, and one stray count
   on a user library segment was enough to stall its compactor;
4. pushes one parameter word (the program pointer itself when the cause has
   no parameter);
5. sets `PB`/`PL` from CST entry 1;
6. clears STATUS **except `M` and `CSN`**;
7. sets `P` from the STT entry named by the cause.

### 7.2 Trap causes

**Where the marker points.** Notes 1 and 2 at the end of the ERS's cause
list (sheet 52) divide every cause in two, and the difference decides
whether a trap can be recovered from at all:

> **Note 1:** In these cases the trap condition is detected at the end of
> an instruction and the offset in the stack marker points to the **next**
> instruction.
> **Note 2:** In these cases the trap condition is detected during an
> instruction and the offset in the stack marker points to **this**
> instruction.

Note 1 is the short list — the debug and arithmetic traps, the ones a
completed instruction reports about itself: `002` check, `003` breakpoint,
`004` machine instruction, `020` unimplemented instruction, `031` user
trap, `034`/`035` floating point, `044` start of line, `045` variable
trace, `046`/`047` start/end of procedure, `050`/`051` start/end of
subroutine, `054` message trap, `056` start-of-line check. **Everything
else is note 2**, so a handler that remedies the cause and returns lands
back on the instruction that complained and it now succeeds. Note 3 says so
explicitly for several of them: "the instruction sequence which caused this
trap is guaranteed to be resumable after the cause of the trap is
remedied". Note 4 adds that `004`, `013`, `037`, `040`, `042` and `043`
clear `STATUS.EM` before pushing the marker.

The cause number *is* the STT entry in segment 1. In octal:

| # | Cause | | # | Cause |
|---|---|---|---|---|
| 001 | Bounds Violation | | 027 | Privileged Mode Data |
| 002 | Check Trap (`CHEK`, `CRB`) | | 030 | Unexpected Pointer Type |
| 003 | Breakpoint | | 031 | User Trap (STATUS.T gated; param bits 24-27 0 = overflow, 2 = divide by zero; 28-31 result type) |
| 004 | Machine Instruction Trap | | 032 | Illegal Decimal Number |
| 005 | String Trap | | 033 | Exponent Size |
| 010 | Reset | | 034 | FP Operand |
| 011 | Page Table Violation | | 035 | FP Result |
| 012 | Inconsistent Registers | | 036 | Unexpected EDS Type |
| 013 | EDS Bounds Violation | | 037 | Absent Code Segment |
| 014 | System Error (param 0 = too many `PSEB`) | | 040 | Absent Page |
| 015 | EDS Pointer Violation | | 041 | Uncallable Procedure |
| 016 | Pointer Conversion | | 042 | Absent Data Segment |
| 017 | External Program Pointer | | 043 | Absent Page Table |
| 020 | Unimplemented Instruction | | 044 | Start of Line |
| 021 | STT Violation (param 1 = length, 2 = external pointer in STT) | | 045 | Variable Trace |
| 022 | CST Violation | | 046 | Start of Procedure |
| 023 | DST Violation | | 047 | End of Procedure |
| 024 | Stack Overflow | | 050 | Start of Subroutine |
| 025 | Stack Underflow | | 051 | End of Subroutine |
| 026 | Privileged Mode (param 1 = instruction, 2 = `EXIT` to privileged, 3 = `EXIT` changing I or setting P) | | 052 | Code Segment Violation |
| | | | 053 | Branch Violation |
| | | | 054 | Message Trap |
| | | | 055 | Instruction Sequencing |
| | | | 056 | Start of Line Check |
| | | | 057 | Data Segment Write |

A cause with no parameter of its own passes the external program pointer
the trap sequence formed: segment 1, the cause's STT entry (sheet 44,
step 4).

### 7.3 External interrupts

An unmasked MSG bit, with `STATUS.I` set, interrupts between instructions
(long instructions checkpoint through `X` and `STATUS.P` and resume). The
microcode switches to the **Interrupt Control Stack**: `Q` and `SL` are
loaded from `QI` and `SLI` in this CPU's dedicated memory block, a marker is
pushed, and the handler runs with, above the marker: a pointer to the global
data segment at `(Q+1)`, the previous interrupt mask at `(Q+2)`, and the
interrupting device number at `(Q+3)`. `IXIT` unwinds all of this (§10.16).
Interrupt response is ~25 µs (~32 µs with the Appendix G `IXIT` workaround).

### 7.4 Dedicated memory *(ERS Appendix D)*

Octal byte addresses:

| Location | Use |
|---|---|
| `0` | system CST pointer |
| `4` | sharable CST pointer |
| `10`–`14` | reserved (incl. EDS pointer to first paged system segment) |
| `20`–`174` | CPU dedicated blocks: for MPB channel N, four words at decimal `16N` — user CST pointer, current TCB pointer, `QI`, `SLI` |
| `200`–`1774` | Device Reference Table (device number = its DRT address = `128·channel + 16·PA`) |
| `2000`–`2174` | interrupt head/tail pointers (priority N: head at decimal `1028+8N`, tail at `1024+8N`) |
| `2200`– | system DST |

The CPU dedicated blocks serve MPB channels 1–6. All three CST pointers and
the TCB pointer are absolute addresses ending in `00`; `QI` and `SLI` must
end in `11`.

The head/tail pointers are maintained by the CPU and I/O hardware and must
not be touched by the OS. Each holds the absolute address of the DRT entry
for the first (last) device queued at that priority; a zero tail pointer
means the list is empty.

A Device Reference Table entry is four words; a device on MPB channel *N* at
peripheral address *P* has device number `128N + 16P` (decimal), which is
also the absolute address of its DRT entry. The entry format is in ERS
§5.1.3.

---

## 8. Instruction encoding

### 8.1 The six formats

Instructions are 16 or 32 bits. The 3-bit field `OP1 = (0:3)` selects the
family; two escape levels reach the short operand-less forms:

```
OP1 = 2..6   Format 1    16-bit memory reference / short branch
OP1 = 0      Formats 2/3/4 (6-bit operand family)
               OP2 = 2..77  Format 2   shifts, short branches, strings…
               OP2 = 0      Format 3   stack operations (no operand)
               OP2 = 1      Format 4   specials (no operand)
OP1 = 1      Format 5    16-bit, 8-bit immediate operand
OP1 = 7      Format 6    32-bit
```

```
Fmt 1   OP1(0:3)  I(3:1)  X(4:1)  BASE(5:3)  OFFSET(8:8)
Fmt 2   000       b(3:1)  OP2(4:6)           OPERAND(10:6)
Fmt 3   000       b(3:1)  000000             OP3(10:6)
Fmt 4   000       b(3:1)  000001             OP3(10:6)
Fmt 5   001       OP2(3:5)                   OPERAND(8:8)     no I or X bit
Fmt 6   111       OP2(3:5)  I(8:1)  X(9:1)  BASE(10:3)  OFFSET(13:19)
```

Format 1 offsets are scaled: **word** offsets for `LDS`/`LDDS`/`STS`, byte
for `LRAS`, halfwords for `BCCS`. Format 6 offsets are bytes.

The format 6 `BASE` field is 3 bits at 10–12 with the offset filling 13–31 —
the ERS text's "bits 10-13" is a misprint, settled by the diagram tick
marks, Appendix B, and real code.

**Alignment.** A 32-bit instruction must be word aligned. In CPU 4.0 and
earlier this bites harder than the rule suggests: any *branched-to* 16-bit
instruction in the right half of a word must have something that decodes as
an instruction in the left half (Appendix C).

### 8.2 The modifier bit `b`

In formats 2, 3, and 4, bit 3 — `CIR(19:1)` once the instruction is
right-justified in CIR — is a per-instruction modifier: an index flag for
the shift and stack-indirect groups, a direction flag for the byte-string
group, "with carry" for `ADD`/`SUB`, and so on. Its meaning is given with
each instruction; the disassemblers render it `,B` (or `,X` where it means
index).

### 8.3 Irregular encodings

**Conditional branches** (`BCCS` fmt 1 op 6; `BCC` fmt 6 op 31) replace
`I X BASE` with a condition mask and sign:

| | CCF mask | sign (0=+, 1=−) | offset |
|---|---|---|---|
| `BCCS` | `CIR(20:3)` | `CIR(23:1)` | `(24:8)`, **halfwords** |
| `BCC` | `CIR(9:3)` | `CIR(12:1)` | `(13:19)`, **bytes** |

`CCF(0:1)` selects CCG, `(1:1)` CCE, `(2:1)` CCL; mask 7 is unconditional,
mask 0 never branches. The target is relative to `P`, the address of the
branch itself.

**Format 2 short branches** (`BIR`, `BUN`, `BCY`, `BNC`, `BOV`, `BNO`,
`BRZ`, `BRE`, `BRO`, `CRB`): the 6-bit operand is an unsigned **halfword**
offset from `P`; the sign is `CIR(23:1)` — which is why each appears twice
in the opcode map (+4 octal = backward). `b` is documented as an indirect
bit for this group.

**`EXF` / `DPF`** (fmt 6 ops 0/1): no memory reference. `J = CIR(27:5)` is
the field's first (leftmost) bit in the source word, `K = CIR(22:5)` is
32 − field width. Bit 8: sign-extend the extracted field (`EXF` only).
Bit 9: add `X` to the packed `(22:10)` J/K pair before use. Bits 10–21 must
be zero — a useful decoder sanity check that holds in real code.

**`CHEK`** (fmt 6 op 2): two immediates — lower bound `CIR(8:5)`, upper
bound `CIR(13:19)`, both unsigned. Verified against the HP-UX 5.30 kernel:
its 34 word-aligned `CHEK`s are all either `[0, n]` (a CASE-selector guard)
or the empty range `[1, 0]` — the compiler's idiom for an unconditional
Check Trap. The empty-range idiom also settles the comparison as
**inclusive** (`lo ≤ v ≤ hi`); the prose's strict `<` would make `[0, n]`
reject its own endpoints.

**`LIA`** (fmt 6 op 5): lexical level in `CIR(8:4)` (displacing the I/X
bits), sign in `(12:1)`, byte offset in `(13:19)`. **`LIAS`** (fmt 5 op
25): level `CIR(24:2)`, positive byte offset `CIR(26:6)`.

**Shift group** (fmt 2 ops 2–10): the count is the low 6 bits of
`OPERAND + (X if b else 0)` — `b` is an index bit here.

---

## 9. Opcode maps

Transcribed from Appendix B (sheet 137); `—` marks unassigned encodings.
Two adjudicated mnemonic readings differ from a raw glyph reading of the
chart — see Appendix B of this document for the evidence (`BRSI`, `RFC`).
Four opcodes the chart leaves blank — marked ‡ — are named by the
instruction table inside the shipped assembler, `/bin/as`; see Appendix B
and §10.19.

### Format 1 (OP1, octal)

| Op | Mnemonic | Operand |
|---|---|---|
| 0 | — escape | to formats 2/3/4 |
| 1 | — escape | to format 5 |
| 2 | `LDS` | word offset, I X base |
| 3 | `LRAS` | byte offset, I X base |
| 4 | `LDDS` | word offset, I X base |
| 5 | `STS` | word offset, I X base |
| 6 | `BCCS` | CCF, sign, halfword offset |
| 7 | — escape | to format 6 |

### Format 2 (OP2, octal)

| Op | Mn | | Op | Mn | | Op | Mn | | Op | Mn |
|---|---|---|---|---|---|---|---|---|---|---|
| 0 | *fmt 3* | | 25 | `SLD` | | 50 | `PMVA` | | 64 | `BCY` − |
| 1 | *fmt 4* | | 26 | `SVAL` | | 51 | `MVB` | | 65 | `BNC` − |
| 2 | `LSL` | | 27 | `SCON` | | 52 | `SCW` | | 66 | `BOV` − |
| 3 | `LSR` | | 30 | `SAS` | | 53 | `SCU` | | 67 | `BNO` − |
| 4 | `CSL` | | 31 | `PMPB` | | 54 | `CMPB` | | 70 | `BRZ` + |
| 5 | `CSR` | | 32 | `SMPB` | | 56 | `TRAN` | | 71 | `BRE` + |
| 6 | `ASL` | | 33 | `SRM` | | 57 | `FILB` | | 72 | `BRO` + |
| 7 | `ASR` | | 34 | `PSHR` | | 60 | `BCY` + | | 73 | `CRB` + |
| 10 | `LSXL` | | 35 | `SETR` | | 61 | `BNC` + | | 74 | `BRZ` − |
| | | | 36 | `LLSH` | | 62 | `BOV` + | | 75 | `BRE` − |
| | | | 37 | `TCC` | | 63 | `BNO` + | | 76 | `BRO` − |
| | | | 40 | `BIR` + | | | | | 77 | `CRB` − |
| | | | 41 | `BUN` + | | | | | | |
| | | | 44 | `BIR` − | | | | | | |
| | | | 45 | `BUN` − | | | | | | |

(+/− mark the forward/backward branch encodings; ops 11–24, 42–43, 46–47
and 55 are unassigned.)

### Format 3 — stack operations (OP3, octal)

| Op | Mn | | Op | Mn | | Op | Mn | | Op | Mn |
|---|---|---|---|---|---|---|---|---|---|---|
| 0 | `NOP` | | 20 | `ADD` | | 40 | `DEL` | | 60 | `LADD` |
| 1 | `DELB` | | 21 | `SUB` | | 41 | `FIXD` | | 61 | `LSUB` |
| 2 | `DDEL` | | 22 | `MPY` | | 42 | `FLTD` | | 62 | `LMPY` |
| 3 | `FTST` | | 23 | `DIV` | | 43 | `STAX` | | 64 | `NOT` |
| 4 | `FTSS` | | 24 | `NEG` | | 44 | `LDXA` | | 65 | `OR` |
| 5 | `PSHN` | | 25 | `TEST` | | 45 | `DUP` | | 66 | `XOR` |
| 6 | `ZERO` | | 26 | `TSTD` | | 46 | `DDUP` | | 67 | `AND` |
| 7 | `DZRO` | | 27 | `TSTB` | | 50 | `FCMP` | | 70 | `FLTS` |
| 11 | `DLAD` | | 30 | `CDSF` | | 51 | `FADD` | | 71 | `FIXS` |
| 12 | `DLSB` | | 31 | `CSDF` | | 52 | `FSUB` | | 72 | `FADS` |
| 16 | `DXCH` | | 32 | `XCH` | | 53 | `FMPY` | | 73 | `FSBS` |
| 17 | `CMP` | | 33 | `DIVS` | | 54 | `FDIV` | | 74 | `FMPS` |
| | | | 34 | `ISC` | | 55 | `FNEG` | | 75 | `FDVS` |
| | | | 35 | `XAX` | | 56 | `CAB` | | 76 | `FNGS` |
| | | | 36 | `ADAX` | | 57 | `LCMP` | | 77 | `FCPS` |
| | | | 37 | `ADXA` | | | | | | |

### Format 4 — specials (OP3, octal)

| Op | Mn | | Op | Mn | | Op | Mn | | Op | Mn |
|---|---|---|---|---|---|---|---|---|---|---|
| 0 | `DTB` | | 20 | `UP` ‡ | | 47 | `PLBA` | | 64 | `PLDA` |
| 1 | `BTD` | | 21 | `DOWN` ‡ | | 50 | `PSBA` | | 65 | `PSTA` |
| 2 | `LBIT` | | 24 | `CTCB` ‡ | | 51 | `PLHA` | | 66 | `CPAA` |
| 3 | `SBIT` | | 25 | `SSI` | | 52 | `PSHA` | | 67 | `CPEP` |
| 4 | `EOS` | | 26 | `SBSI` | | 53 | `POT` | | 70 | `CPTR` |
| 5 | `LSI` | | 27 | `SDSI` | | 54 | `CNTO` | | 71 | `VPTR` |
| 6 | `LBSI` | | 30 | `SHSI` | | 55 | `FLMO` | | 72 | `LINS` |
| 7 | `LDSI` | | 31 | `RCS` | | 56 | `RFC` | | 73 | `LDEL` |
| 10 | `LHSI` | | 37 | `BRSI` | | 57 | `WTC` | | 74 | `IXIT` |
| 11 | `INT` | | 40 | `TRY` | | 60 | `SMSG` | | 76 | `WAIT` |
| 12 | `GTSP` | | 41 | `ESCP` | | 62 | `RSC` | | 77 | `STOP` |
| 13 | `MUC` | | 42 | `ALM` | | 63 | `PSLA` | | | |
| 14 | `XGDS` | | 43 | `CCI` | | | | | | |
| 15 | `PSDB` | | 44 | `EOP` | | | | | | |
| 16 | `DISP` | | | | | | | | | |
| 17 | `PSEB` | | | | | | | | | |

(‡ marks opcodes blank in the chart, named by `/bin/as` — §10.19. Ops
22–23, 45–46 and 61 are unassigned; 32–36 are struck through in the chart —
explicitly unused. Op 75 is unassigned but fails to raise the Unimplemented
Instruction trap on CPU 4.0 and earlier.)

### Format 5 (OP2, octal; 8-bit operand)

| Op | Mn | Operand | | Op | Mn | Operand |
|---|---|---|---|---|---|---|
| 0 | `SOL` | parameter | | 20 | `IOR` | I/O opcode |
| 1 | `SOP` | parameter | | 21 | `IOC` | I/O opcode |
| 2 | `CMPN` | immediate | | 22 | `PCLS` | STT entry number |
| 3 | `CMPI` | immediate | | 23 | `EXIT` | N field |
| 4 | `ADDI` | immediate | | 24 | `SXIT` | N field |
| 5 | `SUBI` | immediate | | 25 | `LIAS` | level + byte offset |
| 6 | `MPYI` | immediate | | 26 | `ADXI` | immediate |
| 7 | `DIVI` | immediate | | 27 | `SBXI` | immediate |
| 12 | `LDXI` | immediate | | 31 | `SOLC` | parameter |
| 13 | `LDXN` | immediate | | 32 | `ADDS` | immediate |
| 14 | `LDIS` | immediate | | 33 | `SUBS` | immediate |
| 15 | `LNIS` | immediate | | 34 | `LPPS` | STT entry number |
| 16 | `SOS` | parameter | | 35 | `ORI` | immediate |
| 17 | `IOW` | I/O opcode | | 36 | `XORI` | immediate |
| | | | | 37 | `ANDI` | immediate |

(Ops 10–11 and 30 unassigned; op 30 fails to trap on CPU ≤ 4.0.)

### Format 6 (OP2, octal; 32-bit)

| Op | Mn | Operand | | Op | Mn | Operand |
|---|---|---|---|---|---|---|
| 0 | `EXF` | J, K fields | | 23 | `DECM` | byte offset, I X base |
| 1 | `DPF` | J, K fields | | 24 | `INCM` | byte offset, I X base |
| 2 | `CHEK` | two immediates | | 25 | `STH` | byte offset, I X base |
| 3 | `PCL` | 4 × STT entry | | 26 | `LDX` | byte offset, I X base |
| 4 | `LPP` | 4 × STT entry | | 27 | `INCF` | byte offset, I X base |
| 5 | `LIA` | level, byte offset | | 30 | `BR` | byte offset, I X base |
| 6 | `LDI` | immediate | | 31 | `BCC` | CCF, sign, byte offset |
| 7 | `LNI` | immediate | | 32 | `DECF` | byte offset, I X base |
| 10 | `LD` | byte offset, I X base | | 33 | `SEML` | byte offset, I X base |
| 13 | `ST` | byte offset, I X base | | 34 | `STB` | byte offset, I X base |
| 14 | `LDB` | byte offset, I X base | | 35 | `STD` | byte offset, I X base |
| 15 | `CMPM` | byte offset, I X base | | 36 | `LRA` | byte offset, I X base |
| 20 | `LDD` | byte offset, I X base | | 37 | — | fails to trap on ≤ 4.0 |
| 21 | `GMPY` ‡ | see §10.19 | | | | |
| 22 | `LDH` | byte offset, I X base | | | | |

(Ops 11, 12, 16 and 17 are unassigned. ‡ `GMPY` is blank in the chart; the
name is from `/bin/as` — §10.19.)

---

## 10. Instructions

Each entry gives: mnemonic(s) — full name — encoding (`F<fmt>:<octal op>`),
operation, the meaning of `b` where the instruction uses it, and the
indicators it sets. "Privileged" means Mode Violation in user mode. Stack
depths are words.

### 10.1 Loads

**`LD` / `LDS`** — Load — `F6:10` byte offset / `F1:2` word offset, I X base
Push the word at E. *CCA.*

**`LDB`** — Load Byte — `F6:14`, I X base
Push the byte at E, zero-extended. *CCB.*

**`LDH`** — Load Halfword — `F6:22`, I X base
Push the halfword at E, **sign-extended**. *CCA.*

**`LDD` / `LDDS`** — Load Double — `F6:20` byte offset / `F1:4` word offset, I X base
Push the words at E and E+4, in that order (high word deeper). *CCA.* See
Appendix C (segment-edge prefetch bug on CPU ≤ 3.3).

**`LSI`, `LBSI`, `LHSI`, `LDSI`** — Load (word/byte/halfword/double) Stack
Indirect — `F4:5 / 6 / 10 / 7`
Pop a data segment pointer, evaluate it (`b`: post-index by X), and push
what it points to — byte zero-extended, halfword sign-extended, double as
two words. *CCA (`LBSI`: CCB).*

**`LDI` / `LDIS`** — Load Immediate — `F6:6` (19-bit) / `F5:14` (8-bit)
Push the zero-extended immediate. *CCA.*

**`LNI` / `LNIS`** — Load Negative Immediate — `F6:7` / `F5:15`
Push the negated immediate. *CCA.*

**`LRA` / `LRAS`** — Load Relative Address — `F6:36` / `F1:3`, byte offset, I X base
Push a *pointer to* E rather than its contents — the addressing modes'
inverse. Direct, P-relative: push `E − PB` (a plain code offset). Direct,
DB/DL-relative: push `E − DB` tagged `10`; Q/SB/S-relative: push `E − SB`
tagged `11` — i.e. a well-formed data segment pointer. Indexing applies to
the resulting pointer. Indirect: push the pointer word at the direct
address unchanged (indexed if X). No memory reference is bounds-checked in
direct mode because none is made. *CC unaffected.*

**`LBIT`** — Load Bit — `F4:2`
`(S)` is a signed **bit** offset: bits `(0:29)` (arithmetically shifted
into X as a byte offset) select the byte, `(29:3)` the bit within it,
counted from the MSB. `(S-1)` is a data segment pointer to the base. Pops
both, pushes the addressed bit as 0/1. X is left holding the byte offset.
*CCA.*

**`ZERO` / `DZRO`** — Push Zero / Double Push Zero — `F3:6` / `F3:7`
Push one / two zero words. *CC unaffected.*

**`PSHN`** — Push N Words — `F3:5`
Push X copies of zero (`b`=0) or of the popped TOS (`b`=1); X counts down
to 0. Interruptible. *CC unaffected.*

### 10.2 Stores

**`ST` / `STS`** — Store — `F6:13` byte offset / `F1:5` word offset, I X base
Pop the TOS into the word at E. *CC unaffected.*

**`STB`** — Store Byte — `F6:34` · **`STH`** — Store Halfword — `F6:25`
Pop and store the low byte / low halfword at E. *CC unaffected.*

**`STD`** — Store Double — `F6:35`
Pop two words; store them at E (deeper word) and E+4 (top word). *CC
unaffected.* Appendix C bug on CPU ≤ 3.3.

**`SSI`, `SBSI`, `SHSI`, `SDSI`** — Store (word/byte/halfword/double) Stack
Indirect — `F4:25 / 26 / 30 / 27`
The value sits on top and the data segment pointer beneath it (`b`:
post-indexed) — `(S-1)` for the single-word forms, `(S-2)` under SDSI's
two value words. Pop everything, store through the pointer. *CC
unaffected.*

**`SBIT`** — Store Bit — `F4:3`
As `LBIT`, with the bit value taken from `(S-2)(31:1)`; pops all three
operands and writes the single bit. X is left holding the byte offset.
*CC unaffected.*

### 10.3 Stack rearrangement

**`DEL`** — Delete A — `F3:40` · **`DELB`** — Delete B — `F3:1` ·
**`DDEL`** — Double Delete — `F3:2`
Drop the top word / the second word / the top two words. *CC unaffected.*

**`DUP`** — Duplicate A — `F3:45` · **`DDUP`** — Double Duplicate — `F3:46`
Push a copy of the top word / of the top doubleword. *CC unaffected.*

**`XCH`** — Exchange A and B — `F3:32` · **`DXCH`** — Double Exchange —
`F3:16` (swaps doublewords AB and CD) · **`CAB`** — Rotate ABC — `F3:56`
(C becomes the top: A B C → C A B). *CC unaffected.*

**`ADDS` / `SUBS`** — Add to / Subtract from S — `F5:32` / `F5:33`
`S ± 4·imm`: allocate or cut back stack space. *CC unaffected.*

### 10.4 Registers in bulk

**`PSHR`** — Push Registers — `F2:34`
The operand is a select mask, pushed in this order: bit `(31)` S (as
`S−SB−1`, its value before any push), `(30)` Q (as `Q−SB`), `(29)` STATUS,
`(28)` DST, `(27)` BRKPT, `(26)` FLAGS (right-justified). `b` additionally
pushes P, PB, PL, DB, DL, SB, SL — all seven, absolute. *CC unaffected.*

**`SETR`** — Set Registers — `F2:35`
The inverse, popping in reverse order (FLAGS first … S last, restored as
`(S)+SB+1`). Setting BRKPT, DST, or S is privileged. Unprivileged mode can
alter only STATUS`(0:9)`, `(10:3)` and `(16:2)` — not `EM`, `I`, `M`, `P`,
or `CSN`; privileged mode still cannot alter `EM`, `P`, or `CSN`. If the
new registers do not satisfy `SB < Q < S < SL`, Q and S are restored and
Inconsistent Registers traps. DST is not saved by traps or interrupts —
`IXIT` restores it from the TCB. *Indicators only as loaded via STATUS.*

**`XGDS`** — Exchange Global Data Segment — `F4:14`
`(S)` holds an external data segment pointer (offset ignored) naming a new
global segment; its DST entry must have `W` set. DB/DL are set from the
entry, use counts are moved, and the pointer's segment number is exchanged
with the global segment number in the current Task Control Block, so the
TOS afterwards names the *old* global segment. Privileged. *CC unaffected.*

**`GTSP`** — Get Task Segment Pointers — `F4:12`
Pop an absolute TCB address into the Current-TCB dedicated location; set
`DST` from the TCB's second word; push external data segment pointers
(offset 0) for the task's stack and global segments from the TCB's third
word, then the CPU's MPB channel number. Privileged. *CC unaffected.*

### 10.5 Integer arithmetic and comparison

**`ADD`** — Add — `F3:20` · **`SUB`** — Subtract — `F3:21`
Pop two words, push `B + A` / `B − A` as two's complement. `b`: include
Carry (`SUB` subtracts `1−C`), for multi-precision chains — and then CC is
updated **only if the result is nonzero**, so a zero partial doesn't erase
the class of the lower words. Carry after `SUB` means no borrow, as after
`LSUB`; a doubleword subtract is `LSUB` on the low words, then `SUB,B`.
Checked under HP-UX by `tests/hpux/carry.s`. *CCA, Carry, Overflow.*

**`ADDI` / `SUBI`** — Add / Subtract Immediate — `F5:4` / `F5:5`
TOS ± the 8-bit immediate, in place. *CCA.*

**`MPY`** — Multiply — `F3:22`
Pop two words, form the 64-bit signed product. `b`=1: push it as a
doubleword. `b`=0: push the low word only; Overflow set unless the top 33
bits of the product were all equal. *CCA, Overflow (single form).*

**`MPYI`** — Multiply Immediate — `F5:6` — TOS × imm, in place. *CCA.*

**`DIV`** — Divide — `F3:23`
`(S-2,S-1)` is a **doubleword** dividend, `(S)` the word divisor. Result:
doubleword quotient in `(S-2,S-1)`, remainder replacing `(S)` — depth
unchanged. `b`=0: remainder takes the dividend's sign; `b`=1: remainder
forced non-negative. Always `dividend = quotient·divisor + remainder`.
Divide by zero raises User Trap (param 2). *CCA on quotient, Overflow.*

**`DIVS`** — Divide, Single-Word Dividend — `F3:33`
`(S-1) ÷ (S)`: quotient replaces `(S-1)`, remainder `(S)`. Same `b` and
trap rules as `DIV`. *CCA on quotient, Overflow.*

**`DIVI`** — Divide Immediate — `F5:7` — TOS ÷ imm, quotient only, in
place. *CCA.*

**`NEG`** — Negate — `F3:24` — Two's complement the TOS in place. *CCA,
Overflow.*

**`CMP`** — Compare — `F3:17`
Set CCC from `(S-1)` (op1) versus `(S)` (op2). `b`=1 pops both; `b`=0 pops
only the TOS, leaving op1 — the idiom that keeps a loop counter live
through the test. *CCC.*

**`CMPI` / `CMPN`** — Compare Immediate / Negative Immediate — `F5:3` / `F5:2`
Pop the TOS; CCC against `+imm` / `−imm`. *CCC.*

**`CMPM`** — Compare Memory — `F6:15`, I X base
Pop the TOS; CCC against the word at E. *CCC.*

**`TEST`** — Test TOS — `F3:25` · **`TSTD`** — Test Double — `F3:26` ·
**`TSTB`** — Test Byte — `F3:27`
Set CC from the top word / doubleword (CCA), or byte (`CCG` if the low
byte is nonzero, else `CCE`). `b` pops the operand(s). *CCA / CCA / see
text.*

**`ISC`** — Integer Size Check — `F3:34`
Set Overflow if the TOS does not fit a halfword (`(0:17)` not all equal).
Operand stays. *CCA, Overflow.*

**`LADD` / `LSUB`** — Logical Add / Subtract — `F3:60` / `F3:61`
Unsigned add/subtract of the top two words, Overflow untouched — the
multi-precision building block. `b` chains the carry in: `LADD,B` adds
Carry, `LSUB,B` subtracts 1−Carry, so after `LSUB` Carry set means *no*
borrow (as `DLAD`/`DLSB`). BASIC's software double multiply depends on
`LADD,B`. *CCG if the result is nonzero; CCE if zero and `b` clear,
else CC unchanged; Carry.*

**`LMPY`** — Logical Multiply — `F3:62`
Unsigned 32×32 → full 64-bit product pushed as a doubleword. *CC
unaffected.*

**`LCMP`** — Logical Compare — `F3:57`
Pop two; CCC on the **unsigned** comparison of `(S-1)` vs `(S)`. *CCC.*

**`INCM` / `DECM`** — Increment / Decrement Memory — `F6:24` / `F6:23`, I X base
Add +1 / −1 to the word at E, in memory. *CCA on the new value.*

**`INCF` / `DECF`** — Increment / Decrement Memory by Four — `F6:27` / `F6:32`, I X base
Add +4 / −4 to the word at E — pointer stepping without touching the
stack. *CCA on the new value.*

### 10.6 The X register

**`LDX`** — Load Index — `F6:26`, I X base — `X :=` word at E. *CC
unaffected.*
**`LDXI` / `LDXN`** — Load X (Negative) Immediate — `F5:12` / `F5:13` —
`X := ±imm`.
**`LDXA`** — Load X onto Stack — `F3:44` — push X. *CCA.*
**`STAX`** — Store A in X — `F3:43` — pop TOS into X. `b` variants exist
per the chart. *CC unaffected.*
**`XAX`** — Exchange X and A — `F3:35` — swap TOS and X. *CC unaffected.*
**`ADAX`** — Add A to X — `F3:36` — `X += (S)` (operand handling per `b`).
*CC unaffected.*
**`ADXA`** — Add X to A — `F3:37` — TOS `+= X` in place. *CCA.*
**`ADXI` / `SBXI`** — Add / Subtract Immediate to X — `F5:26` / `F5:27` —
`X ±= imm`. *CC unaffected.*

### 10.7 Logical operations, shifts, bit fields

**`NOT`** — Logical Complement — `F3:64` · **`OR`** — `F3:65` · **`XOR`** —
Exclusive OR — `F3:66` · **`AND`** — `F3:67`
Bitwise ops; the dyadic forms pop both operands and push the result. *CCA.*

**`ORI`, `XORI`, `ANDI`** — Immediate forms — `F5:35 / 36 / 37`
TOS op imm (zero-extended), in place. *CCA.*

**`LSL` / `LSR`** — Logical Shift Left / Right — `F2:2` / `F2:3`
**`ASL` / `ASR`** — Arithmetic Shift Left / Right — `F2:6` / `F2:7`
**`CSL` / `CSR`** — Circular Shift Left / Right — `F2:4` / `F2:5`
TOS shifted in place. Count = low 6 bits of `operand (+ X if b)`. `ASR`
copies the sign in; `ASL` shifts like `LSL` but sets Overflow if the sign
bit changes. Circular shifts rotate. *CCA (+ Overflow for `ASL`).*

**`LSXL`** — Logical Shift X Left — `F2:10` — `X <<= count`. *CC
unaffected.*

**`EXF`** — Extract Field — `F6:0`
Pop a word; push the field of width `32−K` starting at bit `J`,
right-justified, zero-extended (sign-extended when bit 8 is set). Bits past
bit 31 read as zero. Encoding in §8.3. *CCA.*

**`DPF`** — Deposit Field — `F6:1`
Pop the source; deposit its low `32−K` bits into bits `J..J+L−1` of the new
TOS, in place. *CCA.*

**`CNTO`** — Count Ones — `F4:54`
Population count of the TOS; result replaces it (`b`=0) or is pushed
(`b`=1). *CC unaffected.*

**`FLMO`** — Find Leftmost One — `F4:55`
Bit number (MSB = 0) of the highest set bit, 32 if TOS = 0; replace or push
per `b`. *CC unaffected.*

### 10.8 Binary floating point

Formats are IEEE Draft 5.11 single (binary32) and double (binary64); no
extended format, no hardware sqrt/rem/integer-part. Rounding follows
`STATUS.RM`; inexact operations set `STATUS.IR`. Operand/result traps: FP
Operand (033/034 group) and FP Result. Doubles occupy two stack words, high
word deeper. FP condition codes: `CCL` <, `CCE` =, `CCG` > (or sign class
for the monadic ops).

Conformance *(ERS Appendix F, sheet 145)* is against Draft 5.11 of the
proposed IEEE standard as published in the October 1979 ACM SIGNUM
Newsletter — not the ratified 754-1985. The fast binary↔BCD conversion
instructions (§10.9) use algorithms that differ from the standard's; full
conformance is left to system software. ERS Appendix E (sheet 144) gives the
exact instruction sequences for decimal↔binary floating-point conversion —
useful as simulator test vectors.

**`FADD`, `FSUB`, `FMPY`, `FDIV`** — Floating Add/Subtract/Multiply/Divide
— `F3:51 / 52 / 53 / 54`
Pop two doubles, push the double result. *CC per result sign, IR.*

**`FADS`, `FSBS`, `FMPS`, `FDVS`** — single-word forms — `F3:72 / 73 / 74 / 75`
Same on singles. *CC, IR.*

**`FNEG` / `FNGS`** — Floating Negate double/single — `F3:55` / `F3:76`
Negate in place. *CC.*

**`FCMP`** — Floating Compare — `F3:50` · **`FCPS`** — single — `F3:77`
CCC on the deeper operand versus the top one. The top operand is popped;
`b` pops the second as well. *CCC.*

**`FTST` / `FTSS`** — Floating Test double/single — `F3:3` / `F3:4`
CC from the operand's sign class; `b` deletes it. *CC.*

**`FLTS` / `FLTD`** — Float Single/Double — `F3:70` / `F3:42`
Convert the integer TOS to single (rounded; *IR*) / double (exact). *CCA.*

**`FIXS` / `FIXD`** — Fix Single/Double — `F3:71` / `F3:41`
Convert to integer: `b`=0 truncates toward zero, `b`=1 rounds (±0.5 to the
magnitude). *CCA, Overflow.*
Both indicators are load-bearing, and Overflow especially: HP-UX's `MATH
LIBRARY` implements truncate-to-integral-**double** as *clear the user-trap
enable, `FIXD`, `BOV` to a slow path that masks the fraction off by
exponent, otherwise `FLTD` back* — so a value beyond 32 bits is routed
entirely by this flag, and a core that saturates instead of setting it
returns ±2³¹ for every large argument. That is a whole-system failure, not
a rounding one: the kernel's 10 Hz callout daemon rounds the microsecond
clock up to the next 100 ms through that routine, and a deadline in the
past means it never sleeps.

**`CSDF`** — Convert Single to Double — `F3:31` — widen; a zero word is
pushed first so a mid-instruction FP Operand trap leaves a sane stack.
*CCA.*
**`CDSF`** — Convert Double to Single — `F3:30` — narrow with rounding.
*CCA, IR.*

**`SRM`** — Set Rounding Mode — `F2:33`
`RM :=` low 2 bits of the operand, or of the popped TOS when `b` is set.
*CC unaffected.*

### 10.9 Decimal arithmetic

Formats in §2.2. Malformed BCD raises Illegal Decimal Number; out-of-range
exponents raise Exponent Size.

**`DLAD` / `DLSB`** — Decimal Logical Add / Subtract — `F3:11` / `F3:12`
Pop two 8-digit unsigned decimal words; push sum/difference. `b`: include
Carry (subtract applies `1−Carry`), with the same "CC only if nonzero"
chaining rule as `ADD`/`SUB`. *CCG/CCE + Carry.*

**`DTB`** — Decimal to Binary — `F4:0`
`b`=0: pop a 3-word decimal float; treating its 17 digits as an integer
(exponent forced to 16), push the binary-double equivalent. *CCA, IR.*
`b`=1: pop the exponent word; push binary doubles A and B with
`A/B = 10^(exponent−16)`, for the scale step. *CCG.* Traps Exponent Size
outside −308 < e < 308. Appendix E of the ERS gives the full conversion
routine built from these.

**`BTD`** — Binary to Decimal — `F4:1`
`b`=1: pop a binary double N; push the integer `I ≈ ⌊log₁₀ N⌋` (a fast
approximation, low by 1 for ~4 % of inputs — corrected by the `b`=0 phase).
*CCA.* `b`=0: with I in `(S-2)` and the scaled double on top, produce the
3-word decimal float with exponent I — or, if the mantissa's integer part
fell outside 1..9, pop N, nudge I, and report *CCL/CCG* so the caller
rescales and retries. *CCE on success.*

**`POT`** — Power of Ten — `F4:53`
Pop integer I (−308 < I < 308, else Exponent Size); push binary doubles A,
B with `A/B = 10^I`. The two-double representation carries more precision
than one double can. *CCG.*

### 10.10 Branches

All format 2 branches: 6-bit halfword offset, sign in the opcode's
`CIR(23:1)` (§8.3), `b` = indirect. Targets are P-relative.

**`BR`** — Branch — `F6:30`, I X base
Unconditional. **Pre-indexing** in all cases. P-relative indirect uses a
self-relative pointer; the other bases fetch a **program pointer** — local
(`P := PB + (2:30)`) or external (full CST/STT evaluation, use counts
moved). A direct branch with a non-P base is a Branch Violation, as is any
attempt to reach a privileged callable segment from user mode. The U-bit
of a local pointer is ignored. *CC unaffected.*

**`BRSI`** — Branch Stack Indirect — `F4:37`
A faster `BR S-0,I` that also pops the pointer. Appendix C: on CPU ≤ 3.3
it must not be used with external program pointers. *CC unaffected.*
(Mnemonic reading adjudicated — see Appendix B.)

**`BCC` / `BCCS`** — Branch on Condition Code — `F6:31` / `F1:6`
Branch if CC is in the mask (§8.3): mask bit 0 → CCG, 1 → CCE, 2 → CCL;
`7` always, `0` never. Assemblers write the mask as `.L/.E/.G/.LE/.GE/
.GL/.ALW/.NVR`. *CC unaffected.*

**`BCY` / `BNC`** — Branch on (No) Carry — `F2:60,64` / `F2:61,65`
Branch if Carry set / clear. *Carry cleared either way.*

**`BOV` / `BNO`** — Branch on (No) Overflow — `F2:62,66` / `F2:63,67`
Branch if Overflow set / clear. *Overflow cleared either way* — with
`SETR`, the only ways to clear it.

**`BIR`** — Branch on Inexact Result — `F2:40,44`
If `STATUS.IR`: clear it, then branch — or raise **FP Result Trap when the
offset is 0**, turning the flag into an exception. *IR cleared.*

**`BUN`** — Branch on Unused Condition Code — `F2:41,45`
Branch if CC = `11`, the pattern hardware never writes — an OS-private
signal channel through saved STATUS words. *CC unaffected.*

**`BRZ`** — Branch on TOS Zero — `F2:70,74` · **`BRE`** — on TOS Even —
`F2:71,75` · **`BRO`** — on TOS Odd — `F2:72,76`
Pop the TOS; branch on its value / parity. *CC unaffected.*

**`CRB`** — Compare Range and Branch — `F2:73,77`
`(S-2)` value, `(S-1)` lower bound, `(S)` upper bound, inclusive. In
range: pop the bounds, *CCE*, fall through. Out of range: *CCG*, then
branch — or **Check Trap when the offset is 0** (the CASE-with-no-default
idiom; the operands stay on the stack for the handler in the trap case).

**`CHEK`** — Check — `F6:2`
Trap (Check Trap, STT 2) unless `lo ≤ (S) ≤ hi` for the two immediates of
§8.3. TOS stays. The compiler emits the empty range `CHEK 1,0` as an
unconditional trap. *CC unaffected.*

**`TCC`** — Test Condition Code — `F2:37`
Push 1 if CC matches the 3-bit mask in the operand (same mask as `BCC`),
else 0. *CC unaffected.*

**`CCI`** — Convert Condition Code to Integer — `F4:43`
Push −1 / 0 / +1 for CCL / CCE / CCG. *CC unaffected.*

### 10.11 Procedures

**`PCL` / `PCLS`** — Procedure Call — `F6:3` / `F5:22`
Offset = `4 ×` STT entry number (`PCLS` scales its 8-bit operand by 4).
Offset 0 takes the program pointer from the popped TOS instead of
`(PL − offset)`. Push a stack marker (§4.2), `Q := S`, and transfer through
the pointer — local or external, with the privilege rules of §5.3. *CC
unaffected.*

**`EXIT`** — Procedure Exit — `F5:23`
Cut back to the marker (`S := Q`), restore Q (via ΔQ), STATUS, X; reload
PB/PL from the CSN in the restored STATUS (calls return across segments);
`P := PB + return`; then drop the marker and `N` words of parameters
(`S -= 4 + N` words). Unprivileged code may not exit to privileged mode,
change `I`, or set `P`(ending). *Indicators = restored STATUS.*

**`SXIT`** — Subroutine Exit — `F5:24`
Return from an `LRA`+`BR`-called subroutine: no marker exists, only a
PB-relative return address on top. `P := PB + (S)`; pop it and `N`
parameter words. *CC unaffected.*

**`LPP` / `LPPS`** — Load Program Pointer — `F6:4` / `F5:34`
Offset = 4 × STT entry (≤ the STT length at `PL`, else STT Violation).
Push the entry; a *local* entry is first rewritten as the equivalent
external pointer (this CSN, this entry) — the exportable form. *CC
unaffected.*

**`LIA` / `LIAS`** — Load Intermediate Address — `F6:5` / `F5:25`
Build a data segment pointer to a lexically enclosing frame: start from Q
as a stack pointer, follow `level` static links (each fetched from one
word below the frame a link points at), then add the signed byte offset.
Encodings in §8.3. *CC unaffected.*

**`TRY`** — Try — `F4:40`
`b`=1: push a 3-word extension turning the top of stack into a pseudo
marker — `(S)` ΔQ-shaped link, `(S-1)` STATUS, `(S-2)` the recovery
address (taken from what was on top), `(S-3)` link to the previous TRY
frame as an SB-relative offset — and record it as the newest frame at
`(SB+2)`. `b`=0: unwind the newest frame without executing it (end of the
protected region), restoring `(SB+2)` and cutting S back; `(SB+4) := 0`.
Bounds-checked against `Q < frame < S`. *CC unaffected.*

**`ESCP`** — Escape — `F4:41`
Raise the language-level exception: `b`=0 stores the popped TOS at
`(SB+3)` as the escape code and always escapes; `b`=1 escapes only if
`(SB+4) ≠ 0`. Escaping marks `(SB+4) := −1`, pops frames to the innermost
TRY (bounds-checked), points `Q` at its pseudo marker and performs an
`EXIT 0` through it — landing at the recovery address with the saved
STATUS. *Indicators from the restored STATUS.* Appendix C bug on ≤ 3.3.

### 10.12 Debug aids

Compilers pepper code with these; each is a fast no-op until its enable
bit is set. Parameters (the 8-bit operand) ride into the trap for the
debugger's use. All *CC unaffected*.

**`SOL`** — Start of Line — `F5:0` — trap (044) if `STATUS.SL`.
**`SOLC`** — Start of Line Check — `F5:31` — store `P−PB` at `(Q+1)`
(Bounds Violation if `Q = S`); trap (056) if the word at `(SB+1)` ≠ 0. The
OS defines that word's meaning; no bounds test is applied to it.
**`SOP`** — Start of Procedure — `F5:1` — clear the five debug enables
`STATUS(0:5)`, then trap (046) if the overall debug bit (§3.2) is set.
**`EOP`** — End of Procedure — `F4:44` — trap (047) if `STATUS.EP`. Place
before `EXIT`/`IXIT`.
**`SOS`** — Start of Subroutine — `F5:16` — trap (050) if `STATUS.SS`.
**`EOS`** — End of Subroutine — `F4:4` — trap (051) if `STATUS.ES`. Place
before `SXIT`.

### 10.13 Byte moves and scans

Shared conventions for `MVB`, `SCW`, `SCU`, `CMPB`, `TRAN` (and `FILB`,
`PMVA` where noted): the stack carries, top down, a signed byte **count**,
a **target** pointer, and a **source** pointer. Within the 6-bit operand,
bit `(29:1)` (`DS`) selects whether the source is a **data segment
pointer** (1) or a **local program pointer** (0) — a PB-relative byte
offset bounds-checked against the code segment, the form a compiled-in
constant table takes. `(30:2)` (`SDEC`) is how many of the three operand
words to pop at completion. `b` is the direction: 0 forward, 1 backward
(count treated as negative). All are interruptible: X holds the running
element count, and the count word on the stack is continuously updated to
the remainder, so the instruction resumes cleanly. On completion X holds
the elements processed and `(S)` the count not processed.

Resuming cleanly means more than the count. The loop in every one of these
sheets begins `X := 0`, and HP's compiled code never clears X before an
`MVB` or `SCU`, so the hardware cannot tell a re-executed instruction from
a fresh one: the **pointer words on the stack must be advanced too**, past
the bytes already moved, when the instruction is interrupted by a trap.
The emulator learned this from HP-UX's exec loader (HPUX.md 10.3): a 1 KB
block copy into a paged data segment trapped 040 at the page boundary, the
count came back reduced but the pointers did not, and the re-executed copy
put the remainder at the original target, leaving the first 0x138 bytes of
every page the pager mapped on a fault as zeros -- init's C library lost
its character-class table and `getpwnam("root")` failed. `MVB` and `TRAN`
(which translates in place) now write the advanced pointers back; the scan
and compare instructions restart from the beginning harmlessly.

A data segment pointer is only physically contiguous within one page of a
paged segment, or within one segment of a chain: the walk is a logical
one, and the pointer is evaluated afresh at each page or segment boundary
(the emulator's `byte_cursor`). Run on through physical memory instead and
a copy into a paged segment spills into whatever frames follow — HP-UX's
pager fills a process's data segment from swap with a single 3 KB `MVB`,
and that is exactly how its exec images came back with their pages
permuted.

**`MVB`** — Move Bytes — `F2:51`
Copy count bytes source → target in direction `b`. Overlapping moves are
well-defined byte-at-a-time in the chosen direction. *CC unaffected.*

**`PMVA`** — Privileged Move Bytes, Absolute Addresses — `F2:50`
As `MVB` but both pointers are raw absolute addresses — no evaluation, no
bounds. Privileged. *CC unaffected.*

**`FILB`** — Fill Bytes — `F2:57`
`(S)` count, `(S-1)` target data segment pointer, `(S-2)` fill byte in
`(24:8)`. Store count copies forward. *CC unaffected.*

**`SCW`** — Scan While — `F2:52` · **`SCU`** — Scan Until — `F2:53`
`(S-1)` is a control word: test character in `(24:8)`, terminal character
in `(16:8)`. Walk the source: `SCW` continues **while** each byte equals
the test character; `SCU` continues **until** a byte equals either
character. Each step writes the byte just examined into the control word's
`(0:8)` and its XOR with the terminal into `(8:8)`, so the caller can see
what stopped the scan. *CCA on the last character scanned (if the original
count was positive).*

**`CMPB`** — Compare Bytes — `F2:54`
Walk source and target until inequality or the count runs out. *CCE* if
the ranges matched (or count ≤ 0); else *CCG/CCL* per source byte > / <
target byte, with X and the count telling where.

**`TRAN`** — Translate — `F2:56`
The "source" is a 256-byte table; each byte of the target string indexes
it and is replaced by the entry, in place. *CC unaffected.*

### 10.14 String descriptors

These operate on the descriptor of §2.3; every one first validates it
(`MAXLEN ≥ 0`, `1 ≤ FIRST ≤ CURLEN+1`, `CURLEN ≤ MAXLEN`, substring within
bounds…), raising **String Trap** on failure. `LEN < 0` or over-long means
"through end of string". Operand bit `(29:1)` selects data-segment vs
local-program source pointer as in §10.13; bit `(27:1)` (`N`) requests the
computed actual length be written back to the `LEN` word.

**`SVAL`** — String Validate — `F2:26`
Validation only (plus the `N` write-back). The checks are not necessarily
made in the listed order. *CC unaffected.*

**`SLD`** — String Load — `F2:25`
Replace the descriptor with a **string temp** holding a copy of the
substring: allocate `((LEN+3) div 4 + 1)` words above S (Stack Overflow
checked against SL), copy via the `MVB` machinery, and leave `NWORDS` plus
a descriptor (`FIRST=1`, `LEN=−1`) for it. X is destroyed. *CC
unaffected.*

**`SAS`** — String Assign — `F2:30`
Assign the source substring into a target string variable, updating the
target's current length; target parameters come from a second descriptor
(or, per operand bit `(31:1)`, from a string temp built by `SLD`/`SCON`
directly below the source's address on the stack). *CC unaffected.*

**`SCON`** — String Concatenate — `F2:27`
Extend the string temp below the source descriptor by the source
substring, growing the temp in place (Stack Overflow checked); the temp's
descriptor and MAXLEN are updated. With `SLD` this evaluates arbitrary
string expressions on the stack. *CC unaffected.*

### 10.15 Pointers, semaphores, lists

**`CPAA`** — Convert Pointer to Absolute Address — `F4:66`
Replace the data segment pointer on the TOS with the absolute address it
evaluates to (`b`: post-indexed) — the bridge to the privileged
absolute-address group. *CCA.*

**`CPEP`** — Convert Pointer to External Data Segment Pointer — `F4:67`
Rewrite a stack or global pointer as the equivalent EDS pointer, taking
segment numbers from the current TCB (so: not on the ICS); stack pointers
get `SB`'s segment-relative value folded in. An EDS pointer with `b` set
is fully evaluated first. Pointer Conversion Trap if the 19-bit offset
field can't hold the result. *CC unaffected.*

**`CPTR`** — Convert Pointer — `F4:70`
`(S)` signed byte delta, `(S-1)` data segment pointer: pop both, push the
pointer advanced by delta (same format), and leave the delta in X. For an
EDS pointer, `b`=1 performs a full evaluation (for paged segments);
otherwise the delta folds into the offset field. Pointer Conversion Trap
on overflow of the pointer format. *CC unaffected.*

**`VPTR`** — Validate Pointer — `F4:71`
Non-trapping probe: `(S)` pointer (popped), `(S-1)` required modes (WRITE
in `(30:1)`, PRIV in `(31:1)`), `(S-2)` an SB-relative byte bound for
stack pointers. *CCG* valid; *CCE* valid but segment absent; *CCL*
invalid — where "valid" checks bounds (`b`: post-indexed), the write bit,
and mode against the DST without faulting.

**`SEML`** — Semaphore Load — `F6:33`, I X base
Push the word at E and store −1 there, indivisibly on the MPB. TOS = old
value: ≥ 0 means the semaphore was free and is now yours. *CCA.*

**`PSLA`** — Privileged Semaphore Load, Absolute — `F4:63`
The same exchange on an absolute address from the TOS (`b`: indexed),
result replacing it. Privileged. *CCA.*

**`MUC`** — Modify Use Count — `F4:13`
Evaluate the data segment pointer on the TOS; adjust the use count of the
segment (or page) it lands in — `b`=1 increments, else decrements. No
trap on failure: *CCG* done, *CCE* the segment/page/table was absent,
*CCL* any other evaluation error. How the memory manager pins pages.

**`LLSH`** — Linked List Search — `F2:36`
`(S)` absolute address of a link word, `(S-1)` test value, `(S-2)` byte
offset from link to target word, X a count limit. Follow links while
X > 0, the link ≠ 0 (nil), and the target-vs-test comparison matches the
3-bit CC mask in the operand — `(S)` tracks the current link.
Privileged. *CCL* stopped on the count, *CCE* on the comparison, *CCG* on
nil.

**`LINS`** — List Insert — `F4:72` · **`LDEL`** — List Delete — `F4:73`
Doubly-linked-list primitives over absolute addresses: `(S)` the entry's
forward-link address, `(S-1)` the new entry / the forward-to-backward
byte offset, `(S-2)` that offset (LINS). Zero is nil; an insert into an
empty list makes the entry circular. Privileged. *LINS: CC unaffected;
LDEL: CCE if the list emptied, else CCG.*

### 10.16 Privileged absolute-address group

Bypass segmentation entirely: the operand address is absolute, optionally
post-indexed by X when `b` is set. All privileged.

**`PLBA` / `PLHA` / `PLDA`** — Load Byte / Halfword / Word from Absolute
Address — `F4:47 / 51 / 64`
Replace the TOS address with the byte (zero-extended) / halfword / word it
addresses. *CCA.*

**`PSBA` / `PSHA` / `PSTA`** — Store Byte / Halfword / Word into Absolute
Address — `F4:50 / 52 / 65`
`(S)` value, `(S-1)` address; pop both and store. *CC unaffected.*

(`PMVA`, the block form, is described with the byte group, §10.13.)

### 10.17 I/O and the MPB

CPU-side I/O is three instructions handing an 8-bit I/O opcode (§8 of the
I/O ERS; the hardware ignores its MSB) to an interface named by a word:
`(22:4)` interface-control field, `(26:3)` MPB channel, `(29:3)`
peripheral address; `(0:22)` must be zero. All privileged. A refused or
illegal operation sets MSG bit 7 and raises the Message Trap.

**`IOW`** — I/O Write — `F5:17` — `(S)` data, `(S-1)` interface word; pop
both and write. *CC unaffected.*
**`IOR`** — I/O Read — `F5:20` — pop the interface word, push the data
word read. (Internally a write-then-wait exchange; X carries the first
write's data word.) *CC unaffected.*
**`IOC`** — I/O Control — `F5:21` — pop the interface word, issue the
control. *CC unaffected.*

**`PMPB`** — Push MPB Registers — `F2:31`
Operand bit `(31)` pushes MASK, `(30)` MSG, `(29)` the CPU channel number
(right-justified), in that order. *CC unaffected.*

**`SMPB`** — Set MPB Registers — `F2:32`
The inverse, popping per bit: `(29)` set the CPU channel number in
MASK`(3:3)` (must not be 0); `(30)` **clear** the MSG bits that are 1 in
the popped word; `(31)` load MASK from the popped word, bits 3–5
preserved. Privileged. *CC unaffected.*

**`RFC`** — Read From Channel — `F4:56`
Read one word from the MPB address in the low 29 bits of the TOS (which
stays); push the data. `b` selects secondary address mode. For Memory
Controllers, or peers with an agreed protocol. Privileged. *CCE clean;
CCG one of MSG(1)/MSG(2) set; CCL both.* (Mnemonic reading adjudicated —
Appendix B.)

**`WTC`** — Write To Channel — `F4:57`
Write `(S)` to the MPB address in `(S-1)`'s low 29 bits; neither operand
is popped. `b`: secondary address mode. If the address MSB is 1 the CPU
waits for MSG(12) and clears it — a synchronous handshake. Privileged.
*Indicators as `RFC`.*

**`RSC`** — Read Slave Channel — `F4:62`
Push SAR then SDR — the address/data pair another MPB master wrote at this
CPU. Only meaningful after MSG(6) has been seen and cleared (the Message
Trap microcode clears it). Privileged. *CC unaffected.*

**`SMSG`** — Send Message — `F4:60`
Send the 25-bit message in the TOS's low 29 bits to its channel; `b` makes
it a broadcast to all channels. TOS stays. Privileged. *CC unaffected.*

**`ALM`** — Address Lockout Mode — `F4:42`
`b`=1 enables (the default), `b`=0 disables the MPB fairness guarantee for
lower-priority processors. Privileged. *CC unaffected.*

### 10.18 Interrupt structure and machine control

**`INT`** — Interrupts — `F4:11`
`STATUS.I := b` — enable or disable Message register interrupts.
Privileged. *CC unaffected.*

**`PSDB` / `PSEB`** — Pseudo Interrupt Disable / Enable — `F4:15` / `F4:17`
Increment / decrement the dispatcher-disable count at `(QI−4)`. `PSEB` on
a zero count raises Inconsistent Registers. While the count is nonzero,
`DISP` defers (below) instead of dispatching. Privileged. *CC unaffected.*

**`DISP`** — Dispatch — `F4:16`
The only way into the dispatcher. If already on the ICS or
pseudo-disabled, just set the deferred flag `(QI)(0:1)` and *CCG*/*CCL*;
otherwise save the current process (marker; `S−SB` at `(SB−1)`), mount the
ICS (`Q := QI`, `SL := SLI`, `SB := QI − 64 bytes`), set DB/DL and the
stack-base word from the global-segment pointer at `(Q+1)`, reload PB/PL
from the CSN in `(Q−1)`, and enter the dispatcher with *CCE*. Privileged.

**`IXIT`** — Interrupt Exit — `F4:74`
Exit from trap handlers, interrupt service, and the dispatcher (all run on
the ICS). Depending on `(Q)`, `(QI)` and the deferred-dispatch flag it:
returns to the interrupted process (restoring DST and state from the TCB),
returns to an interrupted handler or interrupted dispatcher, or (re)starts
the dispatcher. Assumes the marker at Q with the global-segment pointer,
previous mask, and device number at `(Q+1..Q+3)` (§7.3). Privileged. *CC
unaffected.* Appendix C bug: on CPU ≤ 4.0, interrupts must be disabled
around `IXIT`.

**`RCS`** — Read Control Store — `F4:31`
Replace the TOS (a control-store location) with that location's low word.
Location 6 is the CPU release date, `YYYYMMDD` in hex digits. *CC
unaffected.*

**`WAIT`** — Wait — `F4:76`
Decrement the TOS to 0 at 1 µs per step; interruptible. `b` is copied to
debug pad 5 (drives the self-test LED). *CC unaffected.*

**`STOP`** — Stop — `F4:77`
`b`=0: halt; manual intervention restarts. `b`=1: sleep — MASK is saved
and replaced from the popped TOS, `STATUS.I` set, and the CPU waits for
any unmasked MSG bit, then restores MASK and STATUS and continues. Either
way `S−SB` is stored at `(SB−1)` when SB ≠ 0. Privileged. *CC unaffected.*

**`NOP`** — No Operation — `F3:0`. *CC unaffected.*

### 10.19 Recovered instructions (not in the ERS)

Four opcodes appear nowhere in the ERS — chart or prose — and not in the
Pascal compiler's table, but are named by the instruction table inside the
shipped HP-UX assembler, `/bin/as` (Appendix B). The HP-UX kernels execute
`UP` and `DOWN` heavily (746 and 419 sites in the 5.01 kernel — more often
than `SSI`); user binaries, BASIC and the diagnostics never do. They are
kernel-only systems instructions, emitted by HP's internal MODCAL
compiler. The encodings are certain. `UP` and `DOWN` have since been
deciphered and are emulated; `CTCB` and `GMPY` have not — the
disassembler prints their names and the core raises Unimplemented
Instruction on execution.

**`UP` / `DOWN`** — Semaphore Up / Down — `F4:20` / `F4:21`
The semaphore signal/wait pair — the blocking counterpart to the
`SEML`/`PSLA` atomic exchange (§10.15). Privileged. The assembler gives
them the operand class of `PSHN` and `SRM`, so `b` is meaningful: set, the
pointer is popped.

The semantics below are not read from any HP document. They are recovered
from the BASIC kernel's *software* implementation of the same structure
(the routine at 0x45720 of the diagnostics environment, together with the
dispatcher that consumes its lists), which fixes every convention the
microcode has to share with the OS around it.

The operand is a pointer to a two-word semaphore: word 0 the wait-list
head, doubling as the multiprocessor lock (−1 while held), word 1 the
count. As with the other privileged instructions the pointer may be a
tagged data segment pointer or a raw absolute address — an absolute reads
as an external pointer with segment number zero, which is invalid as an
EDS reference and so is free to carry that meaning. Both HP-UX kernels
feed both forms; both their call sites use the same `{0, 1}` layout, an
unlocked one-unit mutex.

The wait list is the one both kernels' software semaphore keeps (HP-UX
CORE SYSTEM +0x5068..+0x5498, SUN OS KERNEL 0x4a76..0x4b52): a **circular**
`LINS`/`LDEL` list of the waiters' link nodes, in priority order, with the
head word pointing at the first. HP-UX runs the software path and these
instructions on the same semaphores, so the microcode has to keep the list
exactly that way.

`DOWN` decrements the count. If it stays non-negative the instruction is
done. Otherwise the task blocks. Its wait-link node at `CTCBP−8`
(`{forward, backward}`, the backward word 4 bytes on) goes into the list
after every waiter whose priority value, at node−4 (`CTCBP−12`), is not
greater than its own: the software's `LLSH 3` over the waiters already
queued, then `LINS`. The head moves only if the node went in first. The
semaphore is noted at node+112, the state word at `CTCBP−16` takes 2 in the
low halfword, and the task leaves through the dispatcher exactly as `DISP`
enters it. The TCB stays spliced into the dispatcher's priority chain — the
`LLSH` walk skips anything whose state is not 1.

`UP` increments the count. If it is still non-positive there is a waiter,
the head. The last one simply empties the list; otherwise it is `LDEL`ed
and its forward link becomes the head. Its state word is set back to 1,
and the MSG doorbell bit the dispatcher's idle mask always leaves enabled
is raised, so a processor sitting in `STOP` returns to find it.

A single waiter's node is circular: it points at itself. An earlier model
pushed waiters on the head of a nil-terminated list and woke them by
following the forward link, which left a lone waiter on its own list after
it was woken. With the clock unset that never mattered. Once the clock was
accepted, the timing moved and HP-UX's IOS lock ended up "waking" a running
task while two real waiters slept for good: the multiuser boot stopped
after `/etc/rc`'s `expreserve` whenever the 27140A was fitted.

A semaphore already locked (−1) by another processor is handled by
re-executing the instruction. *CC unaffected.*

**`CTCB`** — `F4:24`
No operand. The name matches the "Current TCB Pointer" dedicated word
that `GTSP` maintains (§10.4, §7.4) — presumably reads or updates it.
Semantics undetermined.

**`GMPY`** — `F6:21`
The assembler's operand syntax differs from the ordinary
memory-reference group's. Semantics undetermined.

---

## Appendix A — Alphabetical index

Format:octal, both given in the chart's terms. 16/32-bit pairs are listed
separately.

| Mn | F:op | Name | | Mn | F:op | Name |
|---|---|---|---|---|---|---|
| `ADAX` | 3:36 | Add A to X | | `LDEL` | 4:73 | List Delete |
| `ADD` | 3:20 | Add | | `LDH` | 6:22 | Load Halfword |
| `ADDI` | 5:4 | Add Immediate | | `LDI` | 6:6 | Load Immediate |
| `ADDS` | 5:32 | Add to S | | `LDIS` | 5:14 | Load Immediate (16-bit) |
| `ADXA` | 3:37 | Add X to A | | `LDS` | 1:2 | Load (16-bit) |
| `ADXI` | 5:26 | Add Immediate to X | | `LDSI` | 4:7 | Load Double Stack Indirect |
| `ALM` | 4:42 | Address Lockout Mode | | `LDX` | 6:26 | Load Index |
| `AND` | 3:67 | Logical AND | | `LDXA` | 3:44 | Load X onto Stack |
| `ANDI` | 5:37 | AND Immediate | | `LDXI` | 5:12 | Load X Immediate |
| `ASL` | 2:6 | Arithmetic Shift Left | | `LDXN` | 5:13 | Load X Negative Immediate |
| `ASR` | 2:7 | Arithmetic Shift Right | | `LHSI` | 4:10 | Load Halfword Stack Indirect |
| `BCC` | 6:31 | Branch on Condition Code | | `LIA` | 6:5 | Load Intermediate Address |
| `BCCS` | 1:6 | Branch on Condition Code (16-bit) | | `LIAS` | 5:25 | Load Intermediate Address (16-bit) |
| `BCY` | 2:60,64 | Branch on Carry | | `LINS` | 4:72 | List Insert |
| `BIR` | 2:40,44 | Branch on Inexact Result | | `LLSH` | 2:36 | Linked List Search |
| `BNC` | 2:61,65 | Branch on No Carry | | `LMPY` | 3:62 | Logical Multiply |
| `BNO` | 2:63,67 | Branch on No Overflow | | `LNI` | 6:7 | Load Negative Immediate |
| `BOV` | 2:62,66 | Branch on Overflow | | `LNIS` | 5:15 | Load Negative Immediate (16-bit) |
| `BR` | 6:30 | Branch | | `LPP` | 6:4 | Load Program Pointer |
| `BRE` | 2:71,75 | Branch on TOS Even | | `LPPS` | 5:34 | Load Program Pointer (16-bit) |
| `BRO` | 2:72,76 | Branch on TOS Odd | | `LRA` | 6:36 | Load Relative Address |
| `BRSI` | 4:37 | Branch Stack Indirect | | `LRAS` | 1:3 | Load Relative Address (16-bit) |
| `BRZ` | 2:70,74 | Branch on TOS Zero | | `LSI` | 4:5 | Load Stack Indirect |
| `BTD` | 4:1 | Binary to Decimal | | `LSL` | 2:2 | Logical Shift Left |
| `BUN` | 2:41,45 | Branch on Unused Condition Code | | `LSR` | 2:3 | Logical Shift Right |
| `CAB` | 3:56 | Rotate ABC | | `LSUB` | 3:61 | Logical Subtract |
| `CCI` | 4:43 | Convert Condition Code to Integer | | `LSXL` | 2:10 | Logical Shift X Left |
| `CDSF` | 3:30 | Convert Double to Single Floating | | `MPY` | 3:22 | Multiply |
| `CHEK` | 6:2 | Check | | `MPYI` | 5:6 | Multiply Immediate |
| `CMP` | 3:17 | Compare | | `MUC` | 4:13 | Modify Use Count |
| `CMPB` | 2:54 | Compare Bytes | | `MVB` | 2:51 | Move Bytes |
| `CMPI` | 5:3 | Compare Immediate | | `NEG` | 3:24 | Negate |
| `CMPM` | 6:15 | Compare Memory | | `NOP` | 3:0 | No Operation |
| `CMPN` | 5:2 | Compare Negative Immediate | | `NOT` | 3:64 | Logical Complement |
| `CNTO` | 4:54 | Count Ones | | `OR` | 3:65 | Logical OR |
| `CPAA` | 4:66 | Convert Pointer to Absolute Address | | `ORI` | 5:35 | OR Immediate |
| `CPEP` | 4:67 | Convert Pointer to EDS Pointer | | `PCL` | 6:3 | Procedure Call |
| `CPTR` | 4:70 | Convert Pointer | | `PCLS` | 5:22 | Procedure Call (16-bit) |
| `CRB` | 2:73,77 | Compare Range and Branch | | `PLBA` | 4:47 | Priv Load Byte, Absolute |
| `CSDF` | 3:31 | Convert Single to Double Floating | | `PLDA` | 4:64 | Priv Load, Absolute |
| `CSL` | 2:4 | Circular Shift Left | | `PLHA` | 4:51 | Priv Load Halfword, Absolute |
| `CSR` | 2:5 | Circular Shift Right | | `PMPB` | 2:31 | Push MPB Registers |
| `CTCB` | 4:24 | Current TCB (not in the ERS, §10.19) | | `PMVA` | 2:50 | Priv Move Bytes, Absolute |
| `DDEL` | 3:2 | Double Delete | | `POT` | 4:53 | Power of Ten |
| `DDUP` | 3:46 | Double Duplicate | | `PSBA` | 4:50 | Priv Store Byte, Absolute |
| `DECF` | 6:32 | Decrement Memory by Four | | `PSDB` | 4:15 | Pseudo Interrupt Disable |
| `DECM` | 6:23 | Decrement Memory | | `PSEB` | 4:17 | Pseudo Interrupt Enable |
| `DEL` | 3:40 | Delete A | | `PSHA` | 4:52 | Priv Store Halfword, Absolute |
| `DELB` | 3:1 | Delete B | | `PSHN` | 3:5 | Push N Words |
| `DISP` | 4:16 | Dispatch | | `PSHR` | 2:34 | Push Registers |
| `DIV` | 3:23 | Divide (doubleword dividend) | | `PSLA` | 4:63 | Priv Semaphore Load, Absolute |
| `DIVI` | 5:7 | Divide Immediate | | `PSTA` | 4:65 | Priv Store, Absolute |
| `DIVS` | 3:33 | Divide, Single-Word Dividend | | `RCS` | 4:31 | Read Control Store |
| `DLAD` | 3:11 | Decimal Logical Add | | `RFC` | 4:56 | Read From Channel |
| `DLSB` | 3:12 | Decimal Logical Subtract | | `RSC` | 4:62 | Read Slave Channel |
| `DOWN` | 4:21 | Semaphore Down (not in the ERS, §10.19) | | `SAS` | 2:30 | String Assign |
| `DPF` | 6:1 | Deposit Field | | `SBIT` | 4:3 | Store Bit |
| `DTB` | 4:0 | Decimal to Binary | | `SBSI` | 4:26 | Store Byte Stack Indirect |
| `DUP` | 3:45 | Duplicate A | | `SBXI` | 5:27 | Subtract Immediate from X |
| `DXCH` | 3:16 | Double Exchange | | `SCON` | 2:27 | String Concatenate |
| `DZRO` | 3:7 | Double Push Zero | | `SCU` | 2:53 | Scan Until |
| `EOP` | 4:44 | End of Procedure | | `SCW` | 2:52 | Scan While |
| `EOS` | 4:4 | End of Subroutine | | `SDSI` | 4:27 | Store Double Stack Indirect |
| `ESCP` | 4:41 | Escape | | `SEML` | 6:33 | Semaphore Load |
| `EXF` | 6:0 | Extract Field | | `SETR` | 2:35 | Set Registers |
| `EXIT` | 5:23 | Procedure Exit | | `SHSI` | 4:30 | Store Halfword Stack Indirect |
| `FADD` | 3:51 | Floating Add | | `SLD` | 2:25 | String Load |
| `FADS` | 3:72 | Floating Add Single | | `SMPB` | 2:32 | Set MPB Registers |
| `FCMP` | 3:50 | Floating Compare | | `SMSG` | 4:60 | Send Message |
| `FCPS` | 3:77 | Floating Compare Single | | `SOL` | 5:0 | Start of Line |
| `FDIV` | 3:54 | Floating Divide | | `SOLC` | 5:31 | Start of Line Check |
| `FDVS` | 3:75 | Floating Divide Single | | `SOP` | 5:1 | Start of Procedure |
| `FILB` | 2:57 | Fill Bytes | | `SOS` | 5:16 | Start of Subroutine |
| `FIXD` | 3:41 | Fix Double | | `SRM` | 2:33 | Set Rounding Mode |
| `FIXS` | 3:71 | Fix Single | | `SSI` | 4:25 | Store Stack Indirect |
| `FLMO` | 4:55 | Find Leftmost One | | `ST` | 6:13 | Store |
| `FLTD` | 3:42 | Float Double | | `STAX` | 3:43 | Store A in X |
| `FLTS` | 3:70 | Float Single | | `STB` | 6:34 | Store Byte |
| `FMPS` | 3:74 | Floating Multiply Single | | `STD` | 6:35 | Store Double |
| `FMPY` | 3:53 | Floating Multiply | | `STH` | 6:25 | Store Halfword |
| `FNEG` | 3:55 | Floating Negate | | `STOP` | 4:77 | Stop |
| `FNGS` | 3:76 | Floating Negate Single | | `STS` | 1:5 | Store (16-bit) |
| `FSBS` | 3:73 | Floating Subtract Single | | `SUB` | 3:21 | Subtract |
| `FSUB` | 3:52 | Floating Subtract | | `SUBI` | 5:5 | Subtract Immediate |
| `FTSS` | 3:4 | Floating Test Single | | `SUBS` | 5:33 | Subtract from S |
| `FTST` | 3:3 | Floating Test | | `SVAL` | 2:26 | String Validate |
| `GMPY` | 6:21 | Not in the ERS (§10.19) | | `SXIT` | 5:24 | Subroutine Exit |
| `GTSP` | 4:12 | Get Task Segment Pointers | | `TCC` | 2:37 | Test Condition Code |
| `INCF` | 6:27 | Increment Memory by Four | | `TEST` | 3:25 | Test TOS |
| `INCM` | 6:24 | Increment Memory | | `TRAN` | 2:56 | Translate |
| `INT` | 4:11 | Interrupts | | `TRY` | 4:40 | Try |
| `IOC` | 5:21 | I/O Control | | `TSTB` | 3:27 | Test Byte |
| `IOR` | 5:20 | I/O Read | | `TSTD` | 3:26 | Test Double |
| `IOW` | 5:17 | I/O Write | | `UP` | 4:20 | Semaphore Up (not in the ERS, §10.19) |
| `ISC` | 3:34 | Integer Size Check | | `VPTR` | 4:71 | Validate Pointer |
| `IXIT` | 4:74 | Interrupt Exit | | `WAIT` | 4:76 | Wait |
| `LADD` | 3:60 | Logical Add | | `WTC` | 4:57 | Write To Channel |
| `LBIT` | 4:2 | Load Bit | | `XAX` | 3:35 | Exchange X and A |
| `LBSI` | 4:6 | Load Byte Stack Indirect | | `XCH` | 3:32 | Exchange A and B |
| `LCMP` | 3:57 | Logical Compare | | `XGDS` | 4:14 | Exchange Global Data Segment |
| `LD` | 6:10 | Load | | `XOR` | 3:66 | Exclusive OR |
| `LDB` | 6:14 | Load Byte | | `XORI` | 5:36 | Exclusive OR Immediate |
| `LDD` | 6:20 | Load Double | | `ZERO` | 3:6 | Push Zero |
| `LDDS` | 1:4 | Load Double (16-bit) | | | | |


## Appendix B — Adjudicated readings

**How conflicts were resolved.** The Appendix B chart is authoritative for
*assignments* (which opcode means what); the prose is used for *names and
semantics*; the emulator's differential and unit tests arbitrate where
either is ambiguous. `tools/check_opcodes.py` maintains the chart-vs-prose
diff mechanically: 25 conflicts, every one OCR damage in the prose (`S8IT`
for `SBIT`, `IXTT` for `IXIT`, `LOB` for `LDB`, `TOR` for `IOR`, `DR` for
`BR`, `WAIT` printed with `DISP`'s opcode, and so on).

**Two mnemonics where the chart's own glyphs are damaged** — adopted here
against the raw chart transcription:

- **`BRSI`** (F4:37). The chart cell prints as `3RSI` — a `B` with a
  degraded left stroke; it was first transcribed `SRSI`. Prose §108.1
  reads "DRSI (Branch Stack Indirect)" and Appendix G reads `BRSI`. Three
  renderings, one instruction; the full name settles the initial as **B**.
- **`RFC`** (F4:56). The chart glyph is ambiguous between `RPC` and `RFC`
  in this typeface (compare the equally degraded `FLMO` cell beside it).
  Prose §158 gives "RFC (Read from Channel)", the natural pair to §159
  `WTC` "Write to Channel". Read as **RFC**.

**Both readings are now confirmed by HP — twice.** `/usr/lib/pascomp`, the
Pascal compiler recovered from the 7914 tapes, carries the instruction set
as an array of 16-byte records — mnemonic, opcode bits in place, operand
class, attribute flags — and it spells them `brsi` and `rfc`.
[`tools/hptable.py`](../tools/hptable.py) reads that table and diffs it
against `opcodes.csv`: **208 assignments agree, none disagree.** A second
HP-authored copy, the 28-byte-record table inside the shipped assembler
`/bin/as`, read by [`tools/astable.py`](../tools/astable.py), also agrees
with every one of the chart's assignments (branches stored in their
forward form) — and it too spells them `brsi` and `rfc`. These are HP's
own machine-readable tables rather than transcriptions of a degraded
chart, so they are the strongest check this project has on the opcode map,
and they independently validate the whole Appendix B reading. The lone
naming disagreement anywhere: the assembler spells F5:031 `sloc`, against
`SOLC` in the compiler and the ERS prose ("Start Of Line Check") — HP's
own typo.

The two tables are equally informative about what is *absent* — and there
they differ. The compiler leaves exactly the same fourteen format 4 slots
unassigned as the Dec-82 chart — `020`–`024`, `032`–`036`, `045`, `046`,
`061`, `075` — even though it lists every documented privileged
instruction (`PLDA`, `PSTA`, `PSLA`, `SEML`, `XGDS`, `GTSP`, `DISP`,
`IXIT`, `RCS`, `WTC`, `SMSG`). But the assembler names four opcodes that
the chart, the compiler and the ERS prose all leave blank: **`UP`
(F4:020), `DOWN` (F4:021), `CTCB` (F4:024) and `GMPY` (F6:021)** — and
the HP-UX kernels execute `020` and `021` in quantity. So the kernel-only
instructions were emitted by HP's internal MODCAL compiler (not on the
tapes) yet sat, named, in the shipped assembler all along. Their
encodings are folded into §9 and §10.19. `UP` and `DOWN` were
subsequently deciphered from the BASIC kernel's software implementation of
the same structure and are emulated; `CTCB` and `GMPY` remain
undetermined.

(`opcodes.csv` and the generated disassembler tables carried `SRSI` and
`RPC` until then.)

**Empirical validation of the opcode map.** The transcription and the format
decoding were checked against real FOCUS code — the HP-UX 5.01 kernel and
the BASIC 2.0 boot disc — using
[`tools/focusdis.py`](../tools/focusdis.py). The decisive evidence is
structural. At offset `0x100` in `os.req`:

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

Bulk statistics, over the HP-UX kernel and BASIC, against a control region
of English manual text from `SHPUX_MAN`:

| Region | Branch density | Targets on an instruction boundary |
|---|---|---|
| HP-UX kernel `0x100`–`0x8000` | 7.0% | 99.9% (chance 89.9%) |
| BASIC `500BT1` `0x900`–`0x2400` | 9.7% | 99.7% (chance 91.2%) |
| **Control: manual text** | **0.1%** | — (chance 100%, no signal) |

A raw "fraction of bytes that decode to a known opcode" figure is worthless
here — 254 of 256 encodings are assigned, so English text scores 99.3%.
Branch density is the discriminator, and it differs by a factor of 70–97×.
Zero branch targets landed on an odd address in any region, as required
since `P`'s LSB must be 0.

The same sweep confirmed two opcode-number errors in the §6.2 prose (both
already right in the chart): `LRA` is format 6 opcode **36** (not 35), and
`STB` is format 6 opcode **34**, with **35** being `STD` and **25** `STH`.

**Encoding questions resolved empirically** (against the HP-UX kernels and
BASIC 2.0):

- Format 6 `BASE` is 3 bits (10–12), offset 13–31.
- `BCCS`/`BCC` field layout (§8.3), targets relative to the executing
  instruction's address.
- `EXF`/`DPF` sub-fields, including the bits-10–21-zero invariant.
- The full `STATUS` layout (§3.1), forced by a tiling argument.
- `CHEK` bounds are `CIR(8:5)` / `CIR(13:19)`, compared **inclusively**;
  the kernel's `CHEK 1,0` always-trap idiom and `CHEK 0,n` CASE guards
  admit no other reading. The same inclusive reading applies to `CRB`
  (the prose's strict inequalities would break every CASE endpoint) and
  to segment bounds (§6.2).
- The `SCW`/`SCU` indicator line is damaged in both instructions; read as
  CCA on the last character scanned when the original count was positive
  — the only CC form that fits a single byte.
- `LBSI`'s indicator prints as "CCE"; read as **CCB** to match `LDB`
  (both load a byte). Unverified against silicon.

**Unassigned encodings.** Formats 2–6 offer 254 opcode points beside the
two escapes; the chart assigns 208 of them and `/bin/as` names four more
(212), format 1 adding its five short forms. Format 4 ops 32–36 are
struck through in the chart; format 4 op 75, format 5 op 30, and
format 6 op 37 are unassigned **but do not raise** the Unimplemented
Instruction trap on CPU 4.0 and earlier (Appendix G) — relevant to linear
sweeps, since a run of `ff ff ff ff` decodes as format 6 op 37.

**Still open.** CST entry word 0 / word 3 bit positions (mode, uncallable,
use count); the exact `STAX`/`ADAX` `b` variants; `LIAS`'s 2+6 level/offset
split is read from the prose and not yet confirmed against real code; and,
the semantics of `CTCB` and `GMPY`, the two `/bin/as`-recovered
opcodes still undeciphered (§10.19).

## Appendix C — Silicon bugs (ERS Appendix G digest)

Software probes the CPU revision via `RCS` location 6 (`YYYYMMDD` hex).

**Present in CPU 4.0 (1982-03-19) and earlier:**

- `IXIT` can overflow the stack if interrupted at the wrong moment;
  contemporary systems disable interrupts around every `IXIT` (raising
  interrupt response from ~25 µs to ~32 µs).
- **Branch-target alignment**: a 16-bit instruction in the right half of a
  word whose left half is arbitrary data cannot be reached by any
  non-sequential transfer (branches, calls, exits, traps, interrupts).
  Linkers keep something instruction-shaped in the left halfword of every
  branch target.
- `SAS` misses the trace-variable trap when the source's current length
  is 0.
- The Reset trap enters its handler in *secondary* MPB address mode; the
  handler's first `IOR` flips it to primary.

**Fixed in 4.0, present in 3.3 (1981-11-02) and earlier:**

- `BRSI` must not be used with external program pointers.
- `ESCP` with `b`=0 can lose the ΔQ word of a `TRY` pseudo marker;
  workaround: precede with `DUP`.
- `LDD(S)`, `LDSI`, `STD`, `SDSI` touch the word after a segment/page
  boundary when the first word is the last mapped word.
- The unassigned encodings noted in Appendix B fail to trap.

## Sources

- *Focus Machine Instruction Set ERS*, A-1FE1-3020-B, J. Fiasconaro,
  09/23/82, issued 12/06/82 — 150 sheets;
  [bitsavers/](bitsavers/FOCUS_Instruction_Set_ERS_Dec82.pdf), text layer
  in [notes/](notes/FOCUS_Instruction_Set_ERS_Dec82.txt), Appendix B chart
  at [notes/appendixB-137.png](notes/appendixB-137.png).
- *FOCUS I/O ERS*, F. Gross, 09/20/82 — the IOP side of §10.17.
- [opcodes.csv](opcodes.csv) — the validated chart transcription;
  `tools/check_opcodes.py` diffs it against a prose scrape.
- `/bin/as` and `/usr/lib/pascomp` (HP-UX 5.30, recovered from the 7914
  unit-save tapes) — HP's own instruction tables, read by
  [tools/astable.py](../tools/astable.py) and
  [tools/hptable.py](../tools/hptable.py).
- [src/cpu/focus/](../src/cpu/focus/) and [src/test/](../src/test/) — the
  executable form of this specification and its evidence.
- *HP Journal*, Aug 1983 / Mar 1984 / May 1984 — the chip set, HP-UX on
  the Series 500, and the machine design story;
  [hpjournal/](hpjournal/).
