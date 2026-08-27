# Validating the implementation against the original documentation

The emulator's specification is `docs/FOCUS.md`, a derived reading of the
*Focus Machine Instruction Set ERS*, A-1FE1-3020-B, 12/06/82. The source
carries 156 citations of that ERS. Until now nothing checked either against
the document itself: `tools/check_opcodes.py` validates the *opcode table*
against the ERS prose, and nothing validated the *semantics*.

This is the record of that check.

**Status.** The mechanical passes — is the document still readable, and does
the source cite it correctly — are complete, and **their findings have been
fixed**; section 5 records what changed. The semantic pass covers a defined
slice, the instructions real code executes most. Within that slice the
condition-code pass is complete: it found sixteen instructions that did not
set the indicator their ERS entry names, and **those are fixed**, with the
behavioural effect measured rather than assumed (section 4). Section 4 also
says what remains unchecked.

---

## 1. Method and instrument

`tools/ers.py` reads the ERS. Its four modes are what an audit needs:

```sh
tools/ers.py --coverage     the opcode chart against the ERS
tools/ers.py --insn ADD     one instruction's pseudocode and Indicators line
tools/ers.py --sheet 91     one sheet, drawing frame stripped
tools/ers.py --cites        every "ERS sheet N" in src/, checked
```

Three facts about the document make this work, and all three were
established rather than assumed:

- **Sheet number = PDF page number.** "ERS sheet 90", as the source cites
  it, is page 90 of the PDF. Verified against several instructions whose
  sheet is independently known.
- **The pseudocode survives the text layer.** So does the `Indicators =`
  line, which is what gives an instruction's condition-code effect. These
  are the two things an audit compares against.
- **Section 6.2 (sheets 54–130)** describes each instruction under a
  numbered heading, and a description may run onto the next sheet — ADD's
  opcode is on 74 and its `Indicators` line on 75, so the reader follows
  the break.

The OCR is damaged in ways that had to be handled rather than worked
around: item numbers render as `21 .` as often as `21.`; mnemonics are
misread along a small stable set of glyph pairs (`S8IT` for SBIT, `TOR` for
IOR, `IXTT` for IXIT, `DR` for BR); and the scanned drawing frame
interleaves with the prose. `ers.py` handles each explicitly and falls back
to the tesseract layer in `docs/notes/`, which reads some headings the
embedded layer loses outright.

---

## 2. Reachability: can the ERS still be read?

Before any semantics, the question of how much of the document is legible
at all. Of the 217 real mnemonics in `docs/opcodes.csv` (excluding the five
`ESCAPE_*` chart entries, which name encoding escapes rather than
instructions):

| | count | |
|---|---:|---|
| `exact` | 194 | described under their own heading |
| `ocr` | 1 | heading found under a misreading |
| `variant` | 8 | described inside a base instruction's text — the ERS writes `LDS` under `LD`, `PCLS` under `PCL` |
| `adjudicated` | 11 | heading destroyed; located by reading the page (section 5) |
| `unplaced` | 3 | not located at all |

**Finding R1 — three instructions cannot be located in the ERS at all, and
they are ones the dispatcher depends on.**

```
UP   DOWN   GMPY
```

Neither their headings nor their `Format N Opcode M=v` clauses survive
anywhere in section 6.2's text layer, in either the embedded text or the
tesseract pass. No amount of pattern work reaches them; they need the page
images read at 400 dpi, the method `docs/FOCUS.md` §3.1 used for the STATUS
field diagram.

All three were named from HP's own toolchain instead — `/bin/as`'s flat
table and `pascomp`, via `tools/astable.py` and `tools/hptable.py` — which
is why the project has them at all. **They have never been checked against
HP's prose, and cannot be until someone reads the pages.** `UP` and `DOWN`
are the semaphore pair the whole dispatcher blocks and wakes on, so this is
the most valuable gap in the document, not the least.

Originally this finding named seventeen instructions. Fourteen were
recovered — eleven by reading the page and three by teaching the reader
what the scan actually produces (section 5).

---

## 3. Citations: does the source point where it says?

`tools/ers.py --cites` reads every `ERS sheet N` in `src/` and, where the
citing line also names an instruction, checks that the cited sheet is the
one describing it.

| | count |
|---|---:|
| citations of sheets outside §6.2 (architecture chapters; not checkable this way) | 60 |
| citations of a §6.2 sheet | 95 |
| … naming an instruction that sheet describes | 23 |
| … naming one it does not | **0** |
| … naming one that cannot be placed (heading lost, per R1) | 4 |

**Finding C1 — nine citations named the wrong sheet. All are fixed.** Each
was confirmed by reading both the cited sheet and the actual one before
being changed:

| site | instruction | was | now |
|---|---|---:|---:|
| `focusdasm.cpp` | EXF / DPF | 116 | **73** |
| `focusdasm.cpp` | the CCF table | 90 | **91** |
| `focusexec_alu.ipp` | BCCS | 90 | **91** |
| `focusexec_alu.ipp` | the CCF table | 90 | **91** |
| `focusexec_mem.ipp` | SOL | 121 | **100** |
| `focusexec_mem.ipp` | SXIT | 93 | **97** |
| `focusexec_mem.ipp` | LRA / LRAS | 58 | **61** |
| `exectest.cpp` | PCL / PCLS | 92 | **95** |
| `exectest.cpp` | INCM/DECM/INCF/DECF | "sheets 81.1-81.3" | **sheet 80, items 81.1-81.3** |

Two are worth calling out beyond the table:

- **EXF** was cited as sheet 116, which holds LINS (List Insert) — nothing
  to do with field extraction. EXF is item 53 on **73**. The largest gap of
  the nine and the one most likely to send a reader somewhere useless.
- **BCCS** was off by one in three places at once: sheet 90 ends with ANDI,
  XORI, ORI and item 108 (BR); BCC/BCCS is item 109 on **91**, and so is
  the CCF table both disassembler and core cite. One slip, propagated.

Two more citations were reworded rather than renumbered, because the number
was right and the wording was not:

- `focusexec_alu.ipp` cited "ERS sheet 108.1" for BRSI. 108.1 is an **item**
  number, not a sheet; the text now says so.
- `focusexec_mem.ipp`'s SXIT comment never named SXIT, only the EXIT it
  contrasts itself with. It does now — which also lets `--cites` check it.

Three flagged citations were **correct and left alone**: `SCON` really is on
sheets 111–112, `SBIT` on 66, and `SXIT` on 97 once renumbered. Each was
flagged only because the comment mentions a second instruction in passing.
`--cites` now takes the first mnemonic on the line as the subject, which is
how these comments are written, and skips instructions it cannot place.

None of this affected behaviour — a citation is a signpost, not a
specification. But the citations exist precisely so the next reader can get
back to the document quickly, and a signpost pointing 43 sheets away costs
more than no signpost at all.

---

## 4. Semantics: does the code do what the sheet says?

Prioritised by what real code actually executes: `tools/isa_coverage.py`
weighting over both boot images (478,183 decoded instructions). The top
twenty opcodes are 70% of all instructions in the corpus, so this is where a
defect costs most.

### Checked

| instruction | share | sheet | verdict |
|---|---:|---:|---|
| `BCCS` | 6.3% | 91 | **MATCH** |
| `LDXI` | 5.1% | 69 | **MATCH** |
| `LRA` / `LRAS` | 5.2% | 61 | **MATCH** |
| `STAX` | 1.4% | 66 | **MATCH** |
| `TEST` | 0.9% | 69 | **MATCH** |
| `ADD` | 0.7% | 74–75 | **MATCH** |
| `MPY` | — | 75 | **MATCH** |

Detail worth recording, because each is a place two readings were possible:

- **`BCCS` condition mask.** The ERS gives both a pseudocode form
  (`CCF(0:1)` selects CCG) and a numeric table (`1` branches on CCL, `2` on
  CCE, `4` on CCG). ERS bit 0 is the MSB, so the two agree, and `cc_taken`
  implements the numeric table exactly. This was an open encoding question
  once (FOCUS.md §8.3); it is now checked against the sheet.
- **`LDXI`.** The ERS zeroes `X(0:24)` and loads `X(24:8)` from the operand —
  the immediate is 8 bits, not the full field. `exec_fmt5` takes `imm` as
  `u8` and assigns it, which zero-extends. Correct, and worth noting given
  the project already found `LDI`/`LNI`'s width read wrongly (24 bits, not
  19) — the same class of mistake, caught here by construction.
- **`ADD` carry and indicators.** The `Indicators` line is on sheet 75, not
  74 with the opcode: `CIR(19:1)=0` gives CCA and Overflow, `=1` gives CCA
  **only if the result is non-zero**, plus Overflow. The second clause is a
  real subtlety and it is the kind of thing a reader who stopped at the page
  break would miss.
- **`STAX`.** `Indicators = CCA on the new X` — the condition code reflects
  X after the store, not the popped value. The implementation orders it that
  way.

### Finding S1 (fixed) — the doubleword and stack-rearrangement instructions did not set the condition code

The ERS states an `Indicators =` effect for every instruction. Comparing
that line against whether the implementation touches the condition code
(`tools/ers.py --indicators`) turns up one coherent group. Each row below
was confirmed by reading both the ERS entry and the case body:

| instruction | ERS `Indicators =` | the code |
|---|---|---|
| `ZERO` fmt3 op6 | CCA | `push(0)` |
| `DUP` fmt3 op45 | CCA | `push(stk(0))` |
| `DDUP` fmt3 op46 | CCA on doubleword | pushes two |
| `XCH` fmt3 op32 | CCA on new TOS | swaps two |
| `DXCH` fmt3 op16 | CCA on new TOS doubleword pair | swaps four |
| `XAX` fmt3 op35 | CCA on new TOS | swaps A and X |
| `CAB` fmt3 op56 | CCA on the new TOS | rotates three |
| `LDD` fmt6 op20 | CCA | pushes two |
| `LDDS` fmt1 op4 | (described under LDD) | pushes two |
| `DIVS` fmt3 op33 | CCA on quotient, Overflow | sets neither |

None of them writes the condition code.

What makes this a pattern rather than ten coincidences is the contrast with
their immediate neighbours, which all set it correctly: `LDS` — the single-
word load, and 18.3% of every instruction real code executes — ends with
`cca(stk(0))`, and so do `LDXA`, `LDIS`, `TEST`, `STAX`, and the whole
`LSI`/`LBSI`/`LDSI`/`LHSI` family through `stack_indirect`. The single-word
load sets the code; the doubleword load beside it does not. That reads as an
omission, not a decision.

`ZERO` and `DUP` alone are 3.4% of executed instructions.

### Finding S2 (fixed) — BCY and BNC did not clear Carry

ERS sheet 92, items 110 and 111: both `BCY` (Branch on Carry) and `BNC`
(Branch on No Carry) end `Indicators = Carry cleared`. The implementation
branches and returns:

```cpp
case 060: case 064:                     // BCY
    if (r.test(sf::C)) rel();
    return exec_trap::none;
case 061: case 065:                     // BNC
    if (!r.test(sf::C)) rel();
    return exec_trap::none;
```

The Overflow pair immediately below it *does* clear its flag — `BOV` and
`BNO` both `r.put(sf::O, 0)`. The two pairs are written the same way in the
ERS and differently in the code, which is the signature of an oversight.

This one has a plausible failure mode rather than a theoretical one. `ADD`'s
pseudocode is `(S-1) + (S) + If CIR(19:1)=1 then Carry else 0`, so `ADD,B`
adds the carry bit. A multi-word addition loop that tests carry with `BCY`
and adds it with `ADD,B` would, on this implementation, keep seeing a carry
that was never cleared.

### Both are fixed, and the blast radius was measured

Sixteen instructions were brought into compliance. Unlike everything in
section 5 these are behavioural changes, so each was made against the
golden harness and the result read off rather than assumed.

**S1, the missing codes.** `ZERO`, `DUP`, `XCH`, `XAX` and `CAB` now set
`cca` on the new top of stack. `DDUP`, `DXCH`, `LDD` and `LDDS` set it on
the doubleword through a new `dcca(high, low)` helper — a doubleword's sign
lives in the word *beneath* the top, so `cca(stk(0))` would call
`0xffffffff:00000001` positive. `TSTD` already did this inline and now
shares the helper. `FLTD` and `FLTS` use `set_fp_cc`, because `FADD`'s entry
also reads "Indicators = CCA" and the floating-point group already reads
that as the FP comparison. `PSHN` sets the code only when it pushed
something, as its entry requires. `DIVS` reports the quotient rather than
the remainder left on top — and, since the ERS names Overflow too, now
catches `INT_MIN / -1`, which was additionally undefined behaviour in C++.

**S2, the carry branches.** `BCY` and `BNC` clear Carry, in exactly the
shape `BOV` and `BNO` already used for Overflow.

**One in the other direction.** `CPAA` was setting `CCA` where sheet 118
says Indicators Unaffected, clobbering a code its caller may have been
relying on. That call is removed.

`PSEB` now sets CCL, which its entry requires on the path that does not
dispatch — the only path this implementation has. Entering the dispatcher
from `PSEB` is still not modelled, so CCL is always the honest answer here.

#### What it changed

The golden harness captures about 4.7 million lines of behaviour: two
multi-million-instruction traces, three real boots, and every log channel
the three systems reach. The total effect of all sixteen fixes:

| probe | lines | changed |
|---|---:|---:|
| `bootrun-basic` | 1,553,790 | 0 |
| `bootrun-diag` | 2,000,032 | 0 |
| `mame-basic`, `mame-diag` | 831 | 0 |
| `knobs-eatrace` | 487,812 | 10 |
| `knobs-hpux` | 44,133 | 16 |
| `mame-hpux` | 418 | 1 |

Every changed line is the same word: a saved `STATUS` going from
`0x00060002` to `0x00068002`. That single bit is the condition-code field —
`CC` occupies STATUS bits 16:2 in ERS numbering — moving from **CCG to
CCE**. A stack marker that used to record "greater" now records "equal",
which is what the instruction that saved it should have set all along.

Nothing else moved. No boot diverges, no trap fires differently, HP-UX
reaches the same panic at the same instruction with the same register file.
The two 2-million-step traces are byte-identical, which says the BASIC and
diagnostics paths never read a condition code from any of these sixteen.

That is the useful result in both directions: the defects were real and are
now visibly corrected, and they were not what stands between this emulator
and a working HP-UX.

#### Locked in

`src/test/exectest.cpp` gained `test_condition_codes` — 30 assertions over
the fixed instructions, including the two cases a naive fix gets wrong: a
negative doubleword whose top word is positive, and a `PSHN` with X = 0
that must leave the code alone. The suite goes 618 checks to 648 with the
same 15 recorded failures.

### Findings G1-G3 (fixed) — the segmentation group's traps

The trap causes are specified on sheets 47-48, and three of them were
being raised for the wrong condition. All three sit in `enter_segment`,
which is the code every external procedure call goes through.

Sheet 48, cause 21, STT Violation, gives **two** conditions:

> 1) An STT entry number is greater than the STT length in the referenced
>    segment.
> 2) The STT number in an external program pointer points to an external
>    program pointer in the referenced segment.

And sheet 47, cause 17, is about something else entirely:

> External Program Pointer Violation. An external program pointer contains
> a code segment number of zero, 1024, or 3072. Or an attempt to EXIT to
> code segment 1024 or 3072.

| | finding |
|---|---|
| **G1** | `enter_segment` raised cause **17** when the called segment's STT slot held an external pointer. That is cause 21 condition 2. |
| **G2** | Cause 17's own condition -- a pointer naming segment 0, 1024 or 3072 -- was implemented for `EXIT` but not on entry, so an external pointer into a sub-table header was never caught. |
| **G3** | `enter_segment` read the called segment's STT slot with no length test at all. `do_call` bounds the *first* lookup against its own `PL`; this is the second lookup, into the STT of the segment being entered, and cause 21 condition 1 says "in the referenced segment". |

Condition 1 also settles a question the scaffold work depends on: the test
is "greater than", so the entry at exactly the STT length is the last
valid one. `do_call`'s `offset > limit` is right, and the frontier
analysis that rests on it is not resting on an off-by-one.

**What the fixes changed: nothing.** All seventeen golden probes stay
identical, and the `MASTERMIND` reproducer still stops at
`TRAP:00000021 00001578 00064001 00000099`, the same cause with the same
parameters. That is worth stating plainly, because the hope was otherwise:
these three are on the scaffold's path, and if BASIC had been failing to
link because it was handed cause 17 where it expected 21, this would have
moved. It did not. The scaffold trap comes from `do_call`'s own limit
check against the *calling* segment, exactly where the project's notes
put it, and none of the three corrected paths is involved.

### Finding S3 (fixed) — CRB popped its bounds on the Check Trap path

Compare Range & Branch (sheet 93) is the CASE-dispatch instruction: a value
under a lower and an upper bound, branching when the value falls outside,
and taking a Check Trap instead when the branch field is zero — which is how
a CASE with no default reports an unmatched selector.

The sheet is explicit about the stack:

> The bounds parameters are popped from the stack if the integer is within
> the range or if the branch is taken. **They are not popped for the trap
> case.**

The implementation popped unconditionally, before the range test, so the
Check Trap path removed two words the handler is entitled to read. Fixed by
moving the pop into the two branches that the sheet says perform it.
`exectest` gained the case, which needs traps disabled — `take_trap` then
returns without touching the stack, so what CRB itself did is what is left.
Against the old code it fails four assertions.

**The endpoint question is also now settled, and the code was already
right.** The comment carried an open question: the scanned pseudocode reads
`If (S-1) < (S-2) < (S)`, strict at both ends, but the code treated the
bounds as inclusive and said "revisit if real code disagrees". Real code
disagrees, decisively. Disassembling the BASIC boot image finds 309 CRB
sites, and the constants pushed as bounds settle it:

| bounds | reading |
|---|---|
| 48, 57 — at three sites | `'0'`..`'9'`, a digit test |
| 34, 90 | `'"'`..`'Z'`, a character class |
| −24, 24 | a symmetric range |
| 1, 10 | a ten-way CASE |

Under the strict reading, 48..57 classifies only `'1'`..`'8'` as digits and
BASIC could not scan the numerals 0 or 9. Inclusive it is; the scanned `<`
is either OCR damage to `≤` or the document being loose. The evidence now
sits in the comment in place of the open question.

### The scaffold path, audited end to end (all correct)

Debugging the trap-021 frontier put five components of the segmentation and
addressing core under the ERS one after another. Every one matches, and
that is worth recording so the next person down this path does not repeat
it. The list is also the reason the remaining divergence has to be in
BASIC's own placement rather than in the machine.

| component | sheet | checked |
|---|---|---|
| STT length field and limit test | 28, 48 | `(13:19)`, "four times the number of entries", and cause 21's test is *greater than*, so the entry at exactly the length is valid. `do_call`'s `offset > limit` is right. |
| external program pointer decode | 28 | `(1:19)` four-times-entry-number, `(20:12)` segment. Reproduces the recorded values: `0x80340006` is entry 208 of segment 6. |
| data pointer evaluation | 56 | The pseudocode contains `If PTR (2:11) = 0 then EDS Pointer Violation Trap` -- literally what the core does. |
| `CPTR` tag preservation | 121 | `(S)(2:30) := Offset` keeps the tag; the core writes `ers32::set(ptr, 2, 30, offset)`. |
| `EXIT` stack discipline | 96 | `S := Q`, unwind through the marker, `S := S-4-CIR(24:8)`. The core pops 16 bytes plus `4n`. |

One deviation found and left alone: `CPTR`'s **external-pointer branch**
with bit 3 set. Sheet 121 calls for a full evaluation of the pointer, a
check that `OFFSET(0:13)` is zero, and storing the *evaluated* `PTR`; the
core instead folds the delta into the `(13:19)` offset field and range-
checks it. Equivalent for an unchained pointer, not obviously so for a
chained one. It **is** on the scaffold path: the compiler builds the
compose destination with exactly this instruction and an external base
pointer, so the deviating branch runs. It does not change the outcome there
because the delta stays inside the segment and no chain walk is needed, but
an earlier note saying the branch was not reached was wrong. Reported rather
than changed, since correcting it needs the evaluation procedure wired into
`CPTR` and nothing currently exercises the chained case.

### Not yet checked

The audit stops here, with roughly 70% of executed instructions covered by
frequency, seven instructions checked in full detail, and every
instruction's condition-code effect checked, hand-verified and brought into
compliance. Not done:

- **the pseudocode body** of the remaining top-twenty — LDS, STS, LDIS,
  PCLS, ZERO, PCL, EXIT, CMPI, LDI, ADDI, ASL, LD, CMP, PLDA, TRY. Only
  their `Indicators` lines have been compared, not their semantics;
- **`WTC`**, the one instruction `--indicators` still reports. Its entry
  says "Indicators same as for RFC", and RFC has no locatable heading, so
  the requirement cannot be read off the document at all;
- **the trap, dispatcher and segmentation groups** (`focusexec_trap.ipp`,
  `focusexec_seg.ipp`, `focusexec_disp.ipp`) against sheets 44–52 and the
  flowcharts on 140–142. Still the highest-value remaining slice: it is what
  the typed-program scaffold (trap 021) and the HP-UX console bring-up both
  run through;
- **UP, DOWN and GMPY** (Finding R1), which need the page images;
- **the I/O subsystem** — `src/iop/` against `FOCUS_IO_ERS_Dec82` and the
  card technical references. Expect a different result shape: the
  FR-register join is documented in no single manual and was reconstructed
  from behaviour, so those entries will read `UNDOCUMENTED`.

One tool limitation worth knowing: `--indicators` reports one row per
mnemonic and resolves variants to their base, so a variant whose base is
already reported can be missed. `LDDS` was found by hand, not by the tool.

### Known deliberate departures

Recorded here so a later pass does not rediscover them as defects. Each is
already argued in `README.md` or `docs/FOCUS.md`:

| departure | why |
|---|---|
| bounds test uses `OFFSET ≤ LENGTH-1` | sheet 55's pseudocode says `<`, which would make the last word of every segment unreachable; sheet 18 gives the range as inclusive (FOCUS.md §4.1) |
| `LDI`/`LNI` immediates are 24 bits | the 19-bit offset-field reading is contradicted by real code; both disassemblers had agreed on the wrong one |
| neither CST nor DST flag bit is maintained | SUNOS keeps one table serving as both, and the two layouts disagree about word 0's bits; the powerup module clears them and nothing rewrites them |
| `LLSH` compares bits 1–31 only | heap free-block sizes carry a flag in bit 0 and first-fit must still order numerically |
| trap return address follows sheet 52 notes 1/2 | only debug and arithmetic traps point at the *next* instruction; the rest point at the faulting one |

#### The CS/80 command set is implemented twice, on purpose

`cs80_device` (`src/iop/focushpib.h`) serves the HP-IB card and `disc_card`
(`src/iop/focusdisc.h`) serves the internal disc. They parse the same command
set, and they are deliberately **not** shared: each was built against the
traffic of a different driver -- LCS80 for the internal disc, HP-UX's HP-IB
driver for the other -- and each answers what its own driver actually sends.
The differences below are the record of that, so a later pass does not read
them as one copy having drifted from the other:

| difference | focusdisc.h (internal) | focushpib.h (CS/80 over HP-IB) |
|---|---|---|
| opcode `0x0a`, cold load read | not decoded; LCS80 never sends it | `locate_read()`, same as `0x00` |
| opcode `0x33`, initiate diagnostic | three parameter bytes, per Instruction Set manual 2-31 | lumped with `0x30`-`0x32` as eight |
| `request_status` identification bytes | unit then volume, one byte each | one packed byte `(volume << 4) \| unit`, then zero |

The `0x33` length is the one that had a real bug behind it: reading it as a
two-byte command left the third byte to execute as a Locate and Read, which
failed, put an error in QSTAT, and made the driver abandon an INITIALIZE
half-way. The comment at the site tells that story. Both implementations
otherwise agree, including `set_flag`/`flags_up`, which are the same two
lines in each.

---

## 5. What was fixed, and how the reader got better

The findings above are closed. Three kinds of change:

**Nine wrong sheet numbers**, listed in section 3, plus two rewordings.
Comments only; no behaviour changed, and `golden.sh` confirms it.

**Fourteen of the seventeen unreadable headings recovered.** Four were the
reader's fault rather than the document's, and fixing each recovered a
group:

| the scan produces | the reader assumed | recovered |
|---|---|---|
| `4.6 LBIT [Load Bit]` | the name is in parentheses | LBIT and others |
| `21 .    DELB` | the item's dot has no space before it | DELB, and the sheet-67 group |
| `llO. XGDS`, `l~1. ASL`, `1A9. MUC` | the item number is digits | XGDS, ASL, MUC |
| `19.  DEL  (Delete  A)  Format 3` | widely-spaced lines are the drawing's title block | DEL, DUP |

That last one was a regression I introduced while stripping the drawing
frame: headings are as widely columnated as any title-block row. They are
told apart by the bracketed name, which the title block never has.

The remaining eleven have headings the scan destroys past any pattern's
reach — `TSTS` for TSTB, `SRE` for BRE, `BRa` for BRO, `DrS?` for DISP,
`Ct"l P B` for CMPB, `lOW` for IOW, `CNT0` for CNTO. Two, CSL and CTCB,
have no legible heading at all and were placed by **item-number gap**:
section 6.2 numbers its descriptions in sequence, so a gap between two
known items contains exactly one instruction, and the opcode confirms
which. CSL is the gap between LSR (44) and CSR (46) on sheet 73; CTCB is
the gap between ESCP (167) and GTSP (169) on 127.

All eleven are recorded in `ADJUDICATED` in `tools/ers.py` with the sheet,
the item number, and how the line actually reads, so each claim can be
rechecked rather than taken on trust. `--coverage` reports them as
`adjudicated`, never as clean headings: they are weaker evidence and the
report says so.

**One dead comparison in `src/test/bootprobe.cpp`.** Its spin detector —
"a tight self-loop means it is spinning, not progressing" — tested a
saturating `u8` visit counter against 20000, which it could never reach.
A spinning probe ran its entire budget instead of stopping. The counter is
now `u16`. This is the one behavioural fix here; `bootprobe` is an
experiment harness, not a test, and is not covered by `golden.sh`.

### A harness fault found along the way

While re-verifying, MAME probes began failing intermittently with
*truncated* output — never different output, and a different probe each
time. Every probe matched the baseline exactly when run alone. The cause is
environmental (this machine has no swap and a busy desktop), not a
regression: a run cut short is a lost measurement, not a change.

`golden.sh` now distinguishes the two. It reports a probe killed by a
signal as `KILLED` rather than as a difference, retries a differing probe
once and says so, and writes its working files compressed — the
uncompressed traces came to a quarter of a gigabyte in `/tmp`, which is
tmpfs, so the harness had been taking memory from the emulator it was
measuring. Two short runs in a row still fail, so a real regression still
reproduces.

---

## 6. Reproducing this

```sh
tools/ers.py --coverage        # section 2
tools/ers.py --cites           # section 3
tools/ers.py --indicators      # section 4, findings S1 and S2
tools/isa_coverage.py /tmp/focus-golden/media/{basic,diag}.bin   # the weighting
tools/ers.py --insn BCC        # any one instruction
```

`--coverage` and `--cites` build an index of section 6.2 by running
`pdftotext` over 77 sheets, which takes about a minute.

`--cites` exits non-zero when a citation names a sheet that does not
describe it, so it can be run as a check. It reports zero today.
