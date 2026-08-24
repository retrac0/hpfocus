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
