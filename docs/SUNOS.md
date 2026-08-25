# SUNOS: the kernel's interface to BASIC, read off the boot image

The Series 500's BASIC system is not one program. It is a dozen code
segments — an operating system called SUN OS, a compiler, a parser, an
interpreter, a human interface, device drivers — that call each other
across segment boundaries. This is the map of who calls whom, and it can be
read out of the boot image without running anything.

## 1. The mechanism: the STT *is* the interface

A FOCUS code segment ends with its **Segment Transfer Table**, and *every*
transfer between segments goes through one. `PCL`/`PCLS` and `LPP`/`LPPS`
name an entry by number; the entry is either a **local program pointer**
into this segment or an **external** one naming another segment and an entry
in *its* table *(ISET ERS §3.1.2, sheet 28)*.

```
word at PL   :0:U:                     LENGTH              Length = 4 × entries
             0  2                   13                31
local  entry :0:U:      30-bit PB-relative byte offset
external     :  4 × STT entry number  :  code segment number  :
             0                       20                     31
```

Two consequences make the whole interface legible:

- a segment's **own table is the list of entry points it publishes**;
- an **external entry in anyone else's table is a call into that segment**.

So the kernel's published system calls are its local STT entries, and the
system calls each subsystem actually uses are the external entries in its
own table naming segment 2. No disassembly is needed to enumerate either.

Read them with [`tools/stt.py`](../tools/stt.py):

```sh
tools/stt.py image.bin                 # one line per segment
tools/stt.py image.bin --segment 2     # the kernel's table in full
tools/stt.py image.bin --callers 2     # every call into the kernel
tools/stt.py image.bin --graph         # the call graph
```

### Two readings the tool depends on

**The image declares its own CST numbering.** The first halfword of each
28-byte segment header is the segment's CST number, and it is *not* the
segment's position in the chain: the kernel is eleventh in the BASIC 2.0
image and declares 2. Checked against a running machine, every one matches
the CST that `SYSTEM POWERUP` builds — kernel 2, IO/MM RTS 3, compiler 4,
parser 5, BASIC 6, human interface 7, IOS 8.

**`0xffffffff` is an unused slot,** not a pointer. Read as one it would name
segment 4095, the largest a 12-bit field can hold and a segment no image
defines. Tables are allocated with room to spare and the surplus filled.

## 2. The segments

From the BASIC 2.0 boot image, in CST order:

| CST | name | STT entries | published (local) | calls out | unused |
|---:|---|---:|---:|---:|---:|
| 0 | SYSTEM POWERUP | — | — | — | — |
| 1 | TRAP HANDLERS | 226 | 91 | 19 | 116 |
| 2 | **SUN OS KERNEL** | 514 | **383** | 49 | 82 |
| 3 | BASIC IO/MM RTS | 336 | 246 | 73 | 17 |
| 4 | BASIC COMPILER | 449 | 261 | 182 | 6 |
| 5 | BASIC PARSER/RTS | 323 | 216 | 62 | 45 |
| 6 | BASIC | 444 | 253 | 186 | 5 |
| 7 | HUMAN INTERFACE | 309 | 230 | 75 | 4 |
| 8 | IOS and DDMs | 202 | 119 | 55 | 28 |
| 10 | SDF_DISC | 136 | 82 | 52 | 2 |
| 13 | PWP SELF TEST | 64 | 38 | 18 | 8 |
| 15 | BASIC INIT | 12 | 2 | 10 | 0 |
| 20 | AP | 613 | 299 | 283 | 31 |

`SYSTEM POWERUP` publishes no table: it runs before the CST exists and is
entered by the loader, not called.

## 3. The call graph

```
  1  TRAP HANDLERS      seg2(18)  seg7(1)
  2  SUN OS KERNEL      seg1(19)  seg8(27)  seg17(3)
  3  BASIC IO/MM RTS    seg2(35)  seg4(2)   seg5(10)  seg6(21)  seg7(4)  seg8(1)
  4  BASIC COMPILER     seg2(8)   seg3(63)  seg5(55)  seg6(52)  seg7(4)
  5  BASIC PARSER/RTS   seg1(5)   seg2(6)   seg3(6)   seg4(19)  seg6(25) seg7(1)
  6  BASIC              seg1(8)   seg2(33)  seg3(36)  seg4(34)  seg5(48) seg7(20) seg8(2) seg15(2) seg20(3)
  7  HUMAN INTERFACE    seg1(4)   seg2(46)  seg3(9)   seg6(11)  seg8(1)  seg13(4)
  8  IOS and DDMs       seg1(1)   seg2(54)
 10  SDF_DISC           seg2(12)  seg8(40)
 13  PWP SELF TEST      seg1(1)   seg2(17)
 15  BASIC INIT         seg5(1)   seg6(9)
 20  AP                 seg2(42)  seg3(38)  seg4(81)  seg5(50)  seg6(27) seg7(45)
```

Things worth reading off it:

- **Every segment calls the kernel.** Ten do, 271 references in all. Nothing
  else is universal.
- **The kernel calls back into only three places** — the trap handlers (19),
  IOS and DDMs (27), and segment 17, which is not in the boot image and must
  be an option binary loaded later. A kernel that calls its own device layer
  and its own trap vectors and nothing else is a tidy shape.
- **The language system is a ring, not a stack.** BASIC (6) calls the
  compiler (4) thirty-four times and the compiler calls BASIC back
  fifty-two; BASIC calls the parser (5) forty-eight times and the parser
  calls BASIC twenty-five. Compilation and execution are interleaved, which
  is what one would expect of a system that compiles a statement at a time.
- **SDF_DISC talks to IOS, not to the hardware** — 40 of its 52 outward
  calls go to segment 8.

## 4. The kernel's surface

The kernel publishes **383 entry points**; **126 of them are called from
other segments**. The remaining 257 are internal, reachable only from within
segment 2 — the STT is how the kernel calls *itself* across its own 62 KB as
well as how others call in.

The most widely used entries, by how many different segments name them:

| entry | callers | from |
|---:|---:|---|
| 81 | 7 | trap handlers, IO/MM, BASIC, HI, IOS, SDF, AP |
| 83 | 6 | trap handlers, IO/MM, BASIC, HI, IOS, AP |
| 129, 131 | 6 | IO/MM, compiler, parser, BASIC, IOS, AP |
| 133 | 6 | trap handlers, IO/MM, parser, BASIC, IOS, AP |
| 144 | 6 | IO/MM, compiler, BASIC, HI, IOS, AP |
| 405 | 6 | trap handlers, IO/MM, BASIC, HI, IOS, AP |
| 4 | 5 | compiler, parser, BASIC, HI, AP |
| 141 | 5 | IO/MM, compiler, BASIC, IOS, AP |
| 157, 158 | 5 | IO/MM, HI, IOS, SDF, PWP |

The numbers cluster, and the clusters line up with callers:

| block | who uses it | likely |
|---|---|---|
| 3–23 | HI, IOS, PWP self test | early/bring-up and console |
| 42–63 | HI, IOS, PWP, AP, BASIC | more of the same |
| 73–101 | everyone | the core group — 81 and 83 are the most called things in the system |
| 129–160 | compiler, parser, BASIC, IO/MM, IOS | memory and segment services, which is where the RUN investigation lives |
| 176–188, 206–243 | BASIC, HI, IO/MM, AP | language runtime |
| 279–317 | IOS, SDF_DISC | file system and device |
| 376–382, 430–463 | IOS, IO/MM, compiler, AP | I/O |

Naming them needs disassembly; the grouping is what the tables give for free.

## 5. Using this for differential debugging

The point of a static map is that a running machine can be checked against
it. Two things it makes possible:

- **Any inter-segment call observed at run time should appear in this
  table.** A `PCL` resolving to a (segment, entry) pair that no STT
  publishes means either the table was built wrong or the pointer was.
- **The converse is the useful one for the RUN frontier.** A container built
  for a compiled program gets a table of the same shape as these — program
  entries first, then links out. The failure documented in the README is
  precisely a container whose link group into segment 6 is absent, and this
  map says what a complete one looks like.

`FOCUS_SEGCALL` logs the runtime side: every transfer, as caller segment,
target segment and STT entry. `enter_segment` is the one place every
inter-segment call passes through, so the log is complete by construction.

### What the first diff found

Running `MASTERMIND` off the demo disc gives 5,185 transfers, 378 distinct
`(from, to, entry)` triples:

| | |
|---:|---|
| 297 | match an external entry the image publishes |
| 48 | between image segments, but published by no table |
| 33 | involve a segment the image does not contain |

The 33 are runtime segments — options loaded after boot, and the containers
BASIC builds for compiled programs — so they cannot be in a boot image and
their absence is not a finding. The 48 are more interesting and also benign:
most are `seg 2 -> seg 2`, the kernel reaching its own entries through a
pointer that is not in its STT. `PCL` with a zero offset takes its pointer
from the stack rather than the table *(ISET ERS sheet 95)*, so a
dynamically-built pointer leaves no static trace. That is a mechanism worth
knowing about before treating any absence as a defect.

### The pair it turned up

Filtering the log to the user containers puts the RUN failure beside a
working example of the same call:

```
segcall 3076 -> 6 entry 208     x2      a container that has its segment-6 links
segcall 3076 -> 6 entry 216     x1      ... and uses the very hook 3089 lacks
segcall 3076 -> 3 entry 59      x1
segcall 3088 -> 3089 entry 1    x1      something starts MASTERMIND
segcall 3089 -> 6 entry 139     x1      it runs, using its own entry 4
                                        then calls STT[9] and traps
```

Segment 3076 is a runtime-built container that received the segment-6 group
and calls entry 216 through it without trouble. Segment 3089 is
`MASTERMIND`'s, built two milliseconds before it ran, and it is the one
whose table stops at six. Two containers of the same kind in the same run,
one linked and one not — which is a far better bench than comparing a user
container against BASIC's own.

See [SOFTWARE.md](SOFTWARE.md) for that investigation and
[DIAGNOSTICS.md](DIAGNOSTICS.md) for the transfer-table rule that HP's own
programs obey.

---

## 6. Every operator goes through a container, and that is one bug, not two

The trap-021 investigation above treats runtime-built containers as a
stored-program problem. They are not. **Any expression with an operator in
it, typed at the prompt, is executed out of a container** — which puts the
immediate-mode arithmetic failure and the stored-program failure on the same
machinery.

`src/test/basictest.sh` is the instrument: a table of (keystrokes, expected
screen line) cases, each booted headless, driven with `FOCUS_TYPE` and read
back through `FOCUS_DIMDUMP` and `tools/screen.py`.

### What separates the working cases from the broken ones

| typed | shows | |
|---|---|---|
| `DISP 7`, `DISP "HI"`, `DISP (7)` | correct | |
| `DISP 4,5` | ` 4          5` | correct — two literals, two columns |
| `DISP 1+1`, `1+0`, `100+200`, `9-8`, `3*4`, `-7` | ` 9` | no trap |
| `DISP 1=1` | ` 0` | should be 1 |
| `DISP "A"&"B"` | trap 001 | |
| `DISP 8/2`, `DISP SQR(4)` | trap 020 | `BTD` is unimplemented |

`FOCUS_TRIP=' 9'` walks the stack back from the moment the character reaches
the screen, and the two paths are not the same path at all:

```
DISP 7      10 frames, every one in segment 7 (HUMAN INTERFACE)
DISP 1+1    13 frames: kernel -> segment 3076 -> segment 3 (IO/MM RTS)
                    -> kernel -> segment 22 -> segment 7
```

A literal never compiles anything. An operator builds a container first.

### The container does not encode the operator

Container 3076 sits at `0x7655c` (user CST entry 4, at `0xfd500 + 16*4`,
whose base comes from word 0 of the CPU dedicated block). Dumping and
disassembling it gives a fixed scaffold — a TRY/ESCP handler table, then:

```
PB+0154  LDIS #1        the operands
PB+0156  LDIS #1
PB+0158  LDIS #9
PB+015a  PCLS STT[13]   -> segment 3 entry 188
PB+015c  LDIS #1
PB+015e  PCLS STT[14]   -> segment 3 entry 156     the print
```

Three measurements pin down what it is:

- **`DISP 2+3` changes exactly two halfwords**, `2c01 2c01` to `2c02 2c03`.
  The operand values are compiled in.
- **`DISP 1*1` produces a byte-identical container to `DISP 1+1`.** The
  operator is *not* compiled in. The container is a scaffold, and the
  operator must live in a structure it walks.
- **At the print call the stack is `(1, 1, 1)`** — the two operands and the
  immediate. No sum was computed, and no double-precision 2.0 exists
  anywhere on the stack.

STT[13] resolves to a three-instruction stub, `LDIS #1; PCLS STT[148];
EXIT N=1`, which takes one parameter and returns nothing — matching the one
word observed popped across the call. Both it and STT[14] are `EXIT N=1`.

### Division is the useful control

`DISP 8/2` traps 020 on the unimplemented `BTD`, and the state dump has
`40100000 00000000` — IEEE-754 **4.0** — sitting on the stack. The division
computed the right answer; only the *formatter* failed, and it failed on a
missing instruction rather than a wrong one. So the arithmetic underneath is
not what is broken, and `BTD`/`DTB` (sheets 84–85) are squarely on the path
that displays a computed number.

### Two candidates ruled out by measurement

- **The condition-code work (`bb9ae1a`) is not the cause.** Reversing all
  three files of that commit, rebuilding and re-running gives ` 9`, ` 0` and
  ` 7` for `DISP 1+1`, `DISP 1=1` and `DISP 7` — identical in every case.
- **The keystrokes are not being mangled.** `FOCUS_DIMDUMP=30` shows the
  echo growing `D`, `DI`, `DIS`, `DISP`, `DISP 1`, `DISP 1+1`, and then the
  answer. BASIC received the line in full.


---

## 7. The second-statement crash: a TRY frame created once, deleted per statement

`LIST` works. `LIST` twice halts with `TRAP:00000001`. This is the cleanest
reproducer the container machinery has offered, and it is **a different
defect from the trap-021 short table**: this container's transfer table is
correctly sized — length word `0x28` at `PL = 0x82328` gives ten entries and
the body calls exactly `STT[10]`. It also involves no segment growth, so
prologue and body are in one image, which is what makes it readable.

`src/test/basictest.sh` carries both cases (`list`, `list-twice`).

### The accounting

The TRY chain belongs to a **stack**, not a code segment — its head is
`(SB+2)`. Tallying `FOCUS_TRYLOG` by code address is therefore misleading;
tally by `sb`. On the container's stack (`sb=0x82428`):

```
 705  TRY,B  p=0x821fe  head=0        container prologue creates -> head 0xbf
 722  ESCP,B p=0x24028  head=0xbf     session init: frame intact
....  many BASIC/kernel creates and deletes, all balanced
1557  TRY    p=0x822da  head=0xbf     statement 1's body deletes it -> head 0
1568  ESCP,B p=0x24028  head=0        the same escape, now with an empty chain
1660  TRY    p=0x822da  head=0        statement 2: nothing to delete -> TRAP
```

**One create against one delete per statement.** The frame the body eats is
the one the prologue made at session start, and nothing replaces it.

### The emulator is right to trap

ERS sheet 126 gives `TRY`: the create (`CIR(19:1)=1`) saves the old `(SB+2)`
into the frame at `(S-3)` and stores the new head; the delete restores
`(SB+2) := (TEMP-3)`, pops four words, and bounds-tests `Q+4 < TEMP < S`.
Create and delete are balanced in our implementation (`LRAS` pushes the
recovery word, create adds three, delete removes four). With head 0,
`TEMP = SB` and the test fails. The trap is correct; **a create is
genuinely missing.**

### Where the missing create is not

The container has two `TRY,B` sites:

```
+0x60   LRAS P+10        pushes PB+0x6a as the recovery address
+0x62   TRY,B            create                       [runs once, at init]
+0x64   LDIS #0 / LRAS Q+1
+0x68   PCLS STT[7]      -> segment 6 entry 208
+0x6a   LRAS P+0
+0x6c   TRY,B            create                       [NEVER runs]
...
+0x130  the body, entered per statement
+0x13e  TRY              delete                       [runs per statement]
+0x140  BR P-220         -> +0x64, past the create at +0x62
```

+0x6a is the recovery address the first create registers, and `LRAS P+0;
TRY,B` there is the classic re-arm shape — land on recovery, immediately
create the next frame. It would balance the books exactly. **It is never
reached, in any flow, including session init**, because `PCLS STT[7]` never
returns. So the design cannot be relying on it, and the re-arm is dead code
in this BASIC.

Control enters the body by a *planted return address*, not a call:
`FOCUS_SEGCALL` records no transfer into 3076 at all, and the body is
reached by `EXIT N=2` at BASIC `0x233ac`. At session init the analogous
`EXIT` (BASIC `0x204e2`) returns to **+0x60**, ahead of the create; per
statement it returns to **+0x130**, past it. The container header holds
`0x130` at base+0x34, which is presumably where the resume offset comes
from.

**Still open:** what is supposed to put a frame on the chain before the
second statement's body runs. The evidence does not name it, so no fix is
offered here.

### A control, and its limit

`FOCUS_TRYNOP=1` (absorb the delete when the chain is empty instead of
faulting) makes two `LIST`s work — BASIC prints `AVAILABLE MEMORY = 487760`
twice over. So the statement loop *is* sustainable without that frame, which
is worth knowing. It is a mask, not a fix, and it does not generalise:
`DISP 1`/`DISP 2`/`DISP 3` still halts, now at the grown container's
+0x162, and `NEW` and `LET` are unchanged.

### Two cautions for the next reader

- **Static disassembly of segment 6 is shifted.** `segdump.py` reports
  BASIC's code at image `0x184f8` with 30,372 bytes, but the CST length is
  30,407, and `image + 0x57c8` decodes bytes that live at runtime
  `PB+0x57ec` — a `0x24` shift. A listing taken that way reads as plausible
  code and is wrong. Entry 208 was misread this way as a `CRB` CASE
  dispatcher; live, it is `LDXI #45; LDS SB+12; LDI #4020; LRAS; LRAS;
  PCLS STT[138]`. Dump from memory (`FOCUS_DUMP`) and disassemble that.
  Segment 3's mapping was checked against a runtime trace and is sound.
- **`TRY` and `ESCP` already deviate from the sheet deliberately.** Both use
  `SL` as the upper bound where the ERS says `S`. The comment in
  [focusexec_alu.ipp](../src/cpu/focus/focusexec_alu.ipp) explains why, and
  it matters here: at statement 1's delete `TEMP == S` exactly, which the
  ERS's strict `TEMP < S` would refuse. Neither bound changes the two cases
  above, but anyone tightening them should re-run `list-twice` first.


---

## 8. The compiler emits arithmetic for `/` and nothing for `+`, `-`, `*`

`DISP 5` prints ` 5`; `DISP 4+1` prints ` 9`. The container is a red
herring for this one — the fault is in what the **BASIC COMPILER**
(segment 4) writes into it.

Watching the emitted bytes (`FOCUS_WWATCH` over the container's code area,
`FOCUS_WWFROM=19`) shows the compiler writing the statement one byte at a
time from **COMPILER +0xf54**. Comparing two statements that differ only in
the operator:

| addr | `DISP 8/2` | `DISP 8+2` |
|---|---|---|
| 0x766b0 | `2c08` LDIS #8 | `2c08` LDIS #8 |
| 0x766b2 | `0022` **FLTD** | `2c02` LDIS #2 |
| 0x766b4 | `2c02` LDIS #2 | `2c09` **LDIS #9** |
| 0x766b6 | `0022` **FLTD** | `320d` PCLS STT[13] |
| 0x766b8 | `002c` **FDIV** | `2c01` LDIS #1 |
| 0x766ba | `320d` PCLS STT[13] | `320e` PCLS STT[14] |

Division compiles correctly — both operands floated, then the operation.
**`+`, `-` and `*` all emit a single `LDIS #9` where the arithmetic
belongs**, and all three emit byte-identical code (`DISP 1+1` and
`DISP 1*1` produce byte-identical containers). The opcodes are `docs/opcodes.csv`
octal: FLTD 042, FADD 051, FSUB 052, FMPY 053, FDIV 054.

**The printed 9 is that literal.** Patching the emitted constant from
`2c09` to `2c07` with
`FOCUS_POKEPC=766b0:766b4:2c07320d` makes `DISP 4+1` print ` 7`. The
formatter is simply printing the last thing pushed.

Two readings are ruled out:

- **Not the operands.** `DISP 40+17` emits `2c28` and `2c11` — 40 and 17.
  The operand values are compiled in correctly; only the operation is
  missing.
- **Not constant folding.** No folded double appears anywhere in the
  container (`DISP 8+2` would need `40240000 00000000`), and the emitted
  constant is 9 for every operand pair tried: 1+1, 4+1, 2+3, 8+2, 40+17.

The formatter itself is innocent: segment 3 entry 188 (the container's
STT[13]) is a real number-formatting routine — `LDD DB+7996`, a character
buffer at `DB+7576`, `PCLS STT[219]`, `PCLS STT[148]` — and the double it
loads is 0.0, because nothing computed a result to put there.

**So the emulator is mis-executing the compiler.** The compiler is HP's
code; it emits correct arithmetic for one operator and not for three
others, which it cannot have done on real hardware. The next step is to
diff the compiler's own execution for `+` against `/` and find where the
operator dispatch diverges.

## 9. `NEW` is a third, separate defect: a frame too shallow

`NEW` traps 001 at container +0x152 (`PB=0x7657c`), and the frame chain is
intact at that point (head still `0xbf`), so it is **not** the
second-statement TRY defect of section 7.

The faulting instruction is `0x8424` = **`LDDS Q+144`**. With Q = 0x8245b
that addresses 0x824eb, while S = 0x824df — it reads sixteen bytes **above
the stack top**.

Our bound is right. ERS sheet 18, section 2.2.5 "Bounds Checking", lists
the ranges the microcode enforces, and for stack segments it is **`SB to
S`** — which is exactly `in_stack()` in
[focusaddr.h](../src/cpu/focus/focusaddr.h). So the composed code is
reading a local the frame never reserved: the statement wants stack the
container did not allocate.

Same theme as section 8, then, and probably the same root: the composed
container is incomplete — missing the arithmetic for `+`/`-`/`*`, and
missing the stack the statement's locals need.


---

## 10. The compiler's state is not being trashed

The natural hypothesis for section 8 -- that something corrupts the
compiler's state, so it stops producing valid code -- is disconfirmed by
four measurements.

- **Nothing uninitialised is read.** `FOCUS_RAMFILL=a5` poisons every RAM
  board; `DISP 8+2` emits the identical bytes (`2c08 2c02 2c09 320d`) and
  prints the identical ` 9`.
- **No corrupted markers, no runaway.** `FOCUS_QCHECK` (misaligned Q or an
  impossible CSN) and `FOCUS_RUNAWAY` (execution leaving real memory)
  report nothing across the whole session.
- **The compiler's code is never written.** `FOCUS_WWATCH` over the whole
  of segment 4 (`0x130dc..0x1d0cb`) from t=1 records **zero** stores. Its
  code and tables are intact.
- **Its registers are well formed.** At the compose site
  (COMPILER +0x209a): `SB 82428 <= Q 8271b <= S 8272b <= SL 84007`,
  `DB 7bc9c <= DL 7e7ab`, `PB 130dc <= P <= PL`, Q and S both word
  aligned, DST non-zero. (Sampling with `FOCUS_STATEAT` is not the way to
  check this -- those samples catch the CPU idle on the ICS, showing
  PB=0/PL=0xfffffffc, which is the dispatcher's context and not BASIC's.)

The loader hand-off was separately validated in 2026-08-23 (commit
08327c3, `docs/IMPLEMENTATION.md`): load address arbitrary, parameter
block consumed exactly as written, nothing reading uninitialised RAM.

### The operator dispatch is healthy too

Tracing the compiler from the compose site for `DISP 8+2` and `DISP 8/2`
and diffing the two instruction streams: they are **identical for 2,526
instructions**, then split at COMPILER +0x3678. The split is a deliberate
CASE dispatch at +0x3664:

```
ZERO / LDIS #61     bounds 0..61 -- a 62-way dispatch
CRB
LSL 1 / STAX,B
LDH P+24,X          halfword jump table
ADAX,B
BR P+16,X           computed branch
```

`+` lands at +0x45ec and `/` at +0x4644 -- two distinct, valid handler
entries. So the compiler selects the right handler for each operator, and
the fault is inside the handler that `+`, `-` and `*` share, not in the
state it runs on or the dispatch that reaches it.

**That is where to look next:** the `+` handler at COMPILER +0x45ec, whose
first dozen halfwords are nearly identical to the `/` handler's at +0x4644
(they diverge at `4405 4404` versus `4405 2303`). One instruction in that
region is behaving differently from the hardware.


---

## 11. Two arithmetic paths: the integer one is broken, the real one needs BTD

Section 8 showed the compiler emitting `FLTD/FLTD/FDIV` for `/` and a bare
`LDIS #9` for `+`, `-`, `*`. The fork is now identified, and it is
**integer versus real**, not one operator versus another.

Breaking at the shared code-generation call (`PCLS STT[177]`) in each
handler and reading the argument:

| | type code passed |
|---|---|
| `DISP 8+2` (handler COMPILER +0x45ec) | **2** |
| `DISP 8/2` (handler COMPILER +0x4644) | **4** |

`/` forces its operands to real; `+`, `-` and `*` keep the operands' own
type, which for two integer literals is 2. **The integer path is the
broken one.**

The prediction that follows is confirmed. Give `+` or `*` a real operand
and it takes the other path:

| expression | result |
|---|---|
| `DISP 8+2`, `8-2`, `8*2` | prints ` 9` — the spurious literal |
| `DISP 4.5+1`, `4+1.5`, `2.5*2`, `8/2` | **TRAP 020** — unimplemented `BTD` |

**CORRECTION (same day).** The line above originally read "so real
arithmetic already works", inferred from `4.5+1` trapping 020 exactly as
`8/2` did. That inference was wrong, and implementing BTD exposed it:
breaking at `BTD,B` shows the double it is handed for `DISP 4.5+1` is the
bit pattern `0000000000000009` -- the integer 9 from the compiler's
`LDIS #9`, read as a double, which is a denormal. `4.5+1` was never on the
working path; it fails the same way `8+2` does and only *looks* different
because a real-typed value sends the 9 through the float formatter.

What is true is narrower: **`/` and the functions compile real arithmetic
correctly**, and for those `DISP 8/2` really does leave IEEE `40100000
00000000` = 4.0 on the stack with only the conversion missing. `+`, `-`
and `*` are broken whatever the operand type.

### This makes BTD/DTB the highest-value item, not a marginal gap

Two things changed:

- **Its blast radius is much larger than `SQR`.** Every real-typed
  expression in BASIC ends at this trap.
- **It can now be validated end to end**, which is what made it risky
  before. The ERS digit packing is OCR-damaged in the bit positions that
  matter, but there are now several cases with known answers --
  `DISP 8/2` must print 4, `DISP 4.5+1` must print 5.5, `DISP 2.5*2` must
  print 5, `DISP SQR(4)` must print 2. A wrong packing shows up
  immediately as a wrong digit rather than hiding.

`src/test/basictest.sh` carries `real-add` and `real-mul` for this.

The integer path remains a separate defect: the emitted `LDIS #9` where an
integer operation belongs, with the 9 shared by `+`, `-` and `*` and so
not an operator code.


---

## 12. BTD, DTB and POT implemented -- BASIC does floating point

With the three conversions in place BASIC prints real numbers:

```
DISP 8/2      ->  4              DISP SQR(4)  ->  2
DISP 9/2      ->  4.5            DISP SQR(2)  ->  1.41421356237
DISP 1/3      ->  .333333333333
DISP 100/8    ->  12.5
```

`src/test/basictest.sh` goes from 5 passing to 11, the six new ones all
real arithmetic. `exectest` gains 101 assertions (758 checks) with the
recorded 15 failures unchanged.

**POT was required alongside them.** It sits two halfwords past the BTD
call and was equally unimplemented, so BTD alone would only have moved the
trap. It also shares its whole body with `DTB,B`: both hand back A and B
with `A/B = 10^k`, split at 22 -- the largest power of ten a double holds
exactly -- so both halves stay exact for `|k| <= 44` and in range for the
whole legal `-308 < k < 308`.

### The packed layout, settled by BASIC rather than by the sheet

The scanned field list `(S-2)(15:17)` is damaged, and two readings of it
were wrong. Each was caught within one run, because a wrong layout prints
a wrong digit:

| attempt | what BASIC did |
|---|---|
| mantissa as a **binary integer** | `TRAP 032` illegal decimal |
| **17 BCD digits**, `(S-2)(28:4)` | printed `0.004` for `8/2`, `0.002` for `SQR(4)` |
| **20 BCD digits**, `(S-2)(16:16)` | correct |

The first is decisive on its own: BASIC runs `DLAD` and `DLSB` straight
down this mantissa, so it must be BCD -- a nibble above nine faults. The
second is decisive about the width: right digits with the point three
places out is exactly the three digits a short field leaves off.

So the format is a ten-bit exponent in `(S-2)(0:10)` and **twenty BCD
digits** -- four in `(S-2)(16:16)`, eight in each of `(S-1)` and `(S)`,
most significant first -- with one digit before the point and nineteen
after. `DTB,B`'s exponent offset is 19 to match, not the sheet's 16; the
two have to agree or the scaling misses by the difference.

### One deliberate departure

Sheet 86 specifies `BTD,B` as a *fast approximation* to `floor(log10 N)`
that "will be in error by +1" for about 4% of values, and says the plain
form corrects it. An exact `floor(log10)` is computed instead. The
caller's loop adjusts in both directions -- BTD's CCL and CCG arms move
the exponent by one and retry -- so being right first time only makes it
converge on the first pass.


---

## 13. Assignment and `NEW`: two more incomplete emissions

Both fail in the container, neither is an ISA gap, and neither is the
formatter. They join section 8's missing arithmetic as the same kind of
fault: the compiler emits a sequence with a piece left out.

### `LET A=6` -- an `INCM` with no pointer

Trap 030 (unexpected pointer type) at container +0x160. The faulting
instruction is `f4b80003` = **`INCM S-3,I`**, and the stack under it is

```
S-0 = 00000000   S-1 = 40180000      <- 6.0
S-2 = 00000000   S-3 = 40180000      <- 6.0 again
```

`40180000 00000000` is the double 6.0. `INCM ,I` dereferences `(S-3)` as a
pointer and gets the *high half* of that double, which decodes as an
external pointer to user segment 3, offset 0 -- hence the trap.

The sequence itself is self-consistent, and stepping the stack through it
accounts for every word:

```
PB+0150  PCLS STT[12]      net zero, measured
PB+0152  LDIS #6
PB+0154  FLTD              6 becomes the double 6.0
PB+0156  DDUP
PB+0158  STD
PB+015c  DDUP
PB+0160  INCM S-3,I        <- traps
```

So nothing put a pointer there. **The container's *other* `INCM S-3,I`,
at +0x140, works** -- because `e400002c` = `LPP STT[11]` runs immediately
before it and pushes a program pointer. The one at +0x160 has a `DDUP`
before it instead. Same instruction, same addressing, one with its pointer
and one without.

Note what this rules out: `LET` is *not* the `LDIS #9` defect. It emits
`FLTD` and builds a proper double, so the literal path is fine here.

### `NEW` -- a frame that was never reserved

Trap 001 at container +0x152 on `0x8424` = **`LDDS Q+144`**. With
Q = 0x8245b that addresses 0x824eb while S = 0x824df -- sixteen bytes above
the stack top. Our bound is right: ERS sheet 18 section 2.2.5 gives stack
segments as `SB to S`.

The frame is short from the start, not consumed by anything: S is
`0x824df` **both before and after** the `PCLS STT[12]` two halfwords
earlier, so that call is net zero and was never going to supply it. The
body simply reads a local that the frame does not contain -- 132 bytes of
frame against a reference at 144.

A tempting explanation, checked and rejected: the container header carries
a size-shaped word at base+0x38 that differs per statement -- `0x18` (24)
for LIST, `0x3c` (60) for DISP, `0x40` (64) for NEW -- and 64 words would
be ample where 33 is not. **It is never read at runtime.** `FOCUS_RWATCH`
over it across a whole `NEW` records no access, so it is written by the
compiler and not consulted by anything. It is not the mechanism.

What is left is section 7's re-entry: statements re-enter the body at
+0x130 inside the *session's* activation, with Q fixed and no fresh frame.
Most statements need little and survive; `NEW` needs 144 bytes and does
not. That makes `NEW` a second symptom of the missing per-statement
activation rather than a defect of its own.

Also ruled out for both: `ADDS` and `SUBS`, the instructions that reserve
a frame, are implemented correctly including the zero-operand form where
the count comes off the stack.
