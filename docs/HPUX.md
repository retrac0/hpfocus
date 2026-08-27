# HP-UX 5.30 on the 540 — kernel internals map

A working map of the kernel this project boots from the 7914 unit save,
for debugging: where things are, what the numbers mean, and which tools
answer which questions. Everything here was read off the running system
or the disassembly; nothing in this file comes from documentation, so
each entry says how it was established. Companion references:
`docs/HARDWARE.md` §2.6 (the CIO card protocols), `docs/FOCUS.md` (the
ISA), and the boot-history notes in the project memory.

## 1. Image and runtime geometry

- The boot image is bytes `0x400 .. 0x400+611740` of
  `software/cct/7914_unitSave_hpux4.0.bin` (the loader reads it off the
  CS/80 disc). `tools/segdump.py` lists its 27 modules.
- **Runtime = file + 0x14c98** for the current 2 MB configuration —
  every code segment relocates by the same delta because the loader
  places them contiguously. This delta changes with memory
  configuration; recompute from any FOCUS_STATEAT CST dump
  (`base(TRAP HANDLERS) - hdr(TRAP HANDLERS) - 8`).
- **PB of a module = its file segment header + 8** = `code_start - 36`
  in segdump's terms. So a PB-relative offset (a `ret` in a stack
  marker, a `P-PB` in a report) turns into a file offset for
  `tools/focusdis.py` as `code_start - 36 + ret`. `tools/panicdump.py`
  does this automatically.
- Runtime CST (`FOCUS_STATEAT` prints it with names): seg 1 TRAP
  HANDLERS, 2 CORE SYSTEM, 4 HP-UX ENTRYPTS, 6 HP-UX KERNEL, 8 IOS and
  DDMs, 10 SDF_DISC, 11 TTY SUBSYSTEM, 26 MATH LIBRARY, 29 HP-UX IO
  DRIVERS, 32 HP27128/30, 33 HP27125A, 35 HPIB_FLEX, 38 SERIAL, 40
  HP7970, 42 HP268X, 43 INTERNAL_HP-IB, 46 MEMORY_VOL, 47 CIPERLP, 49
  CONFIGURATION, 50 MEMORY INTRINSIC, 51 HP27110, 53 HP27140, 61
  HP98700, 70 WIN/H9000, 100 9030/40 FR PANEL. Segments 3/5/7/9 etc.
  are data. The numbering is stable for this image + configuration, not
  a property of the image alone.

## 2. Absolute low memory

| Address | What | Established by |
|---|---|---|
| 0 | pointer to the CST | ERS; every report |
| 128·ch + 16·pa | DRT entry (word 3 = ICEI command-list pointer, and it advances) | I/O ERS 4.1.4 + watching |
| 920 | doorbell semaphore (the request server parks here; `SMSG` bit 10 + `DISP` rings it) | SEMLOG + disasm 0x190e8 |
| 928 | software clock lock | write-watch |
| 932/936 | the software clock, a double: **microseconds since 1900** (2208988800e6 at an unset 1970 RTC); +~1e6/s by the timer handler at TRAP HANDLERS +0xe72 (run 0x22b0a) | write-watch + decode |
| 1024 + 8·level | per-level interrupt lists, (tail, head) | I/O ERS 4.1.4 |

## 3. Error numbers ("escapes")

`ESCP` is the kernel's software exception; `FOCUS_ESCLOG` logs every
raise, `tools/escmap.py` maps a number to its raising sites statically.
Known meanings:

| # | Meaning | Evidence |
|---|---|---|
| 55 | success — the timed-wait completion value (no constant ESCP site exists for it) | 0x24d92 requires 55 |
| 56 | **timed wait expired** — raised by each driver's own timeout site (HP27128/30 +0x1782, INTERNAL_HP-IB, HP27140) | escmap + traces |
| 66 | console/device open failed — CORE SYSTEM converts a 56 into it (raise at CORE SYSTEM +0x3fd2, run 0x27906) | observed cascade |
| 116 | request sent to the NULL driver (the NO_CARD stub at CORE SYSTEM +0x7f82, run 0x2b8b6) → "System halted due to system error." | last session |
| 30 | select-code scan: identify byte out of range 1..7 | card-typing work |
| 91 | **out of range / not supported** — used as a *returned result*, not only a raise. CORE SYSTEM +0xf0f4 raises it on a `CRB` against 0..43; HP27128/30 +0x5d8 *writes* it into a request's result cell when the RS block's transfer length does not match what was asked | §6 |
| 353 | HP27128/30 +0x5b0 writes it into the same result cell on its other failure arm | disasm |
| −1 | generic re-raise/abort argument, 22 sites; also the "unset" value of a request result cell | escmap |

A result cell is read back through `Q-20,I` and compared with **1**, not 55
— a different vocabulary from the completion statuses above.

The TRY-handler idiom: `SB+12` holds the escape being handled (the
reports print it twice — head word 0 and 3). Handlers compare it against
constants (`LDS SB+12; CMPI #n`) and either recover or re-raise.

## 4. The panic report

Printed by the raw-FR console writer at HP-UX KERNEL +0xc63c.. (run
0x43f30..0x44390) — a console-of-last-resort that drives the ASI without
DMA or interrupts: orders on FR8, byte-at-a-time reads on FR6, `SDC` +
`RIRQ`-polling for its transfers. `tools/panicdump.py` decodes the
report; its docstring carries the layout. One decoded detail worth
repeating: the SEGS table is one halfword per CST entry, normally
0x4C00; observed 0x4D00 for the kernel, 0x4F00 for SDF_DISC, 0x4E00 for
CIPERLP at the console-open panic — a per-module state whose values are
not yet named.

## 5. CORE SYSTEM's data (DB = 0x11bb8 at 2 MB)

The one DB is shared by all privileged code. Known cells (byte offsets):

| DB+ | What | Evidence |
|---|---|---|
| 0 | semaphore, taken by the ASI event/RTS handler (SEML DB+0) | 0x274a8 |
| 36 | RTS-handler in-progress flag (0 = idle) | 0x2751e |
| 40/44 | pending-work descriptor the handler consumes (STD DB+40 clears) | 0x274b2.. |
| 52 | current RTS opcode being dispatched | 0x274c8/0x274d4 |
| 72 | copy of the descriptor for the arms | 0x274b8 |
| 84, 92/96, 116, 136, 144/148 | per-query result cells; the IDL arm stores −1 into all of them ("no answer") and 0 into DB+36 | 0x27500..0x2751e |
| 156 | address the handler stores a completion into | 0x274bc |
| 196, 204 | table addresses read by the IDL arm | 0x27504/0x27514 |
| 488 | heap list head (BASIC's kernel and this one share the heap design) | BASIC work |
| 2080 | last recorded boot error (941 = clock not set) | BASIC work |
| 4416 | a kernel semaphore (SEML DB+4416 in the callout daemon) | 0x33f10 |

## 6. HP-UX BOOTS — the console open, and the five defects behind it

**As of 2026-08-25 HP-UX 5.30 boots and prints its banner to the console.**

```
(c) Copyright 1983, 1984, 1985, 1986, 1987 Hewlett-Packard Company
(c) Copyright 1979 The Regents of the University of Colorado, a body corporate
(c) Copyright 1979, 1980, 1983 The Regents of the University of California
(c) Copyright 1980, 1984 AT&T Technologies. All Rights Reserved.
                    RESTRICTED RIGHTS LEGEND
          Use,  duplication,  or disclosure by the Government  is
          subject to restrictions as set forth in subdivision (b)
          (3)  (ii)  of the Rights in Technical Data and Computer
          Software clause at 52.227-7013.
                    HEWLETT-PACKARD COMPANY
                        3000 Hanover St.
                      Palo Alto, CA  94303
CLOCK AND DATE NOT SET
```

No "System halted", no escape 66, 56 or 91 — the whole escape log is the
five benign `941`s, and the last of them is the message printed above. The
machine then idles for sixty emulated seconds at 123% speed. Run it with
`tools/runhpux.sh -str 60 -video none`.

**Next frontier: what comes after the boot report** — root filesystem and
`init`. Nothing further reaches the console yet.

### The last defect: the length of data transfer

The driver's status-block check is at HP27128/30 **+0x5d8** (image
`0x59968`). It reads the block through the pointer at `Q+24`, and the
pointer measures **`0x1e9de0`, word-aligned, while the card's bytes DMA to
`0x1e9de1`** — so the driver's struct has a leading pad byte and the
manual's byte 0 lands at struct byte 1. With that, the check is exactly the
27128A's RS block (4-64):

| byte | | |
|---|---|---|
| 0 | card dependent request status | must be 0 |
| 1-2 | length of data transfer | must equal the length requested |
| 3-4 | length of data remaining in the record | |
| 5-6 | message type, text terminator | read device data only |

Traced with `FOCUS_EATRACE=6e600:26`: the status byte **passed**, the
expected length at `Q-44` was **1**, and the length compare **failed**. On
a mismatch +0x5d8 writes **91** into the request's result cell, and
+0x1822 reads that cell (`LDS Q-20,I; CMPN #1`), wants 1, and raises what
it found.

`m_transferred` counted **every** leg, because `dma_read`/`dma_write`
incremented unconditionally. The three bytes of a transparent-status read
went into it, so read-card-information reported a four-byte transfer where
one byte was asked for. The manual defines the field as "the data transfer
initiated by the card for the host read or write request", so only `RD` and
`WD` count now.

### The four before it

The `7x66` cascade this file spent most of its length circling was **not in
the kernel**. It was three defects in the emulated ASI card and one in how
the IOP is clocked, and every single-axis sweep recorded in §6b traded one
failure for another because all four had to go together.

#### What the kernel is doing (settled from the disassembly)

Runtime = image + `0x14c98`; CORE SYSTEM PB = `0x23934`.

`BR P+8,I,X` takes its table at *instruction address + 8*, and each entry is
**self-relative** — the target is the entry's own address plus its value.
Two independent checks pin both conventions, and with them the handler
decodes cleanly:

| image | runtime | |
|---|---|---|
| `0x12804` | `0x2749c` | `STS DB+0,X` — the completion closes the channel, state := 0 |
| `0x12810` | `0x274a8` | loop head, `SEML DB+0` |
| `0x1281a` | `0x274b2` | `LDS DB+40; BRZ` — work queue empty, fall through |
| `0x12838` | `0x274d0` | `LDS DB+52` → `STAX; LSXL 2; CMPI #5; BR P+8,I,X` |

The six arms are the card's six report opcodes, in order:

| `DB+52` | runtime | |
|---|---|---|
| 0 IDL | `0x27500` | −1 into the result cells, clear `DB+36`/`DB+0`, `ICEI`, `IXIT` — **the clean exit** |
| 1 SWI | `0x27530` | LCN bounds-check, then dispatch on the channel state |
| 2 EDD | `0x275bc` | same shape, `CRB` requires state ∈ [4,16] |
| 3/4/5 | | LCD / AES / ERT |

The SWI arm's inner state table (base `0x128b8`, index = the state word, so
states run 0,4,8,…): state 4 → `0x128d4`, which sets state := 24 and arms
the transfer; **state 0 → `0x276c8`, the error entry** that calls `STT[353]`
and tears the channel down, stamping 66 over the 55 the transaction had
already earned.

**`DB+52` is not a variable.** No `STS`/`STD` anywhere in the image writes
it: it is the low byte of the 17-byte report block the command list DMAs to
`0x1e980b`, so it changes only when a Read Transparent Status actually runs.
(`DB` for this handler is `0x1e97d4`, not the `0x11bb8` of §5.)

And **every arm ends `... IOC io=0x38; IXIT`** — release the lock, re-issue
ICEI to fetch the next report, return. The design gives the CPU exactly one
interrupt per freshly-DMA'd report. Each defect below is that invariant
being broken.

1. **Read Transparent Status destroyed the transaction's data.** `ORD_RTS`
   cleared the card's one outbound queue, throwing away the switch byte the
   CLC's `read_card_information()` had prepared for subfunction 253. The
   guard in `ORD_RD` written to protect that byte could not: it was already
   gone. Measured — the `RD` leg logged no `DMA-done`, no `DMA-dend` and no
   raise at all. RTS now answers out of its own buffer (`m_report`), chosen
   by `out_now()`.
2. **The status block left a report owed.** `dma_complete()` arms `EDD`
   when the data phase ends; `read_status_block()` cleared `m_request` but
   not `m_rts_report`, so the next RTS said EDD about a finished
   transaction — and the kernel's EDD arm does not acknowledge anything, it
   *arms a transfer*. It now clears the owed report, so the next RTS reads
   IDL.
3. **Pause did not pause.** `ORD_PAU` set `m_want_order` immediately. That
   was invisible while the IOP polled once a millisecond, because the poll
   set the cadence; the moment a list could resume promptly it turned the
   driver's PAU/RTS loop into a tight ring of interrupt-ICEI-status and the
   driver's own task never ran. PAU is now a real wait (`FOCUS_ASIPAUSE`,
   1 ms), and the idle loop measures **exactly 1000 status reads a second**
   — the cadence the real driver shows (999 in the second before the open).
4. **The IOP was a visitor, not a processor.** This is the structural one.
   `poll_and_service()` ran on the same 1 ms beat as the cards' own
   timekeeping, so the CPU got tens of thousands of instructions between
   one command-list entry and the next, and every hand-off between the two
   processors was a race the CPU always won. Three of them, all measured:
   the driver writes its transaction's branch target into the list at
   `0x1e9864` and the interrupt handler overwrote it again before the IOP
   ever read it; the list parked one entry short of the RTS that refreshes
   the report buffer; and a leg's completion interrupt arrived before that
   leg's own list had finished. The poll now runs on its own timer
   (`FOCUS_IOPPOLL`), and `CMD_ICEI` does not suspend for a poll the
   interface is already answering.

### The sequence, before and after

```
before                              after
2.322  CLC                          2.318  CLC
2.323  RTS -> SWI, data destroyed    2.319  RTS -> SWI
2.323  RD  -> nothing, no completion 2.319  RD  -> DMA-done, the switch byte
2.324  RS  -> raise ACCEPTED         2.319  RS  -> status block, length 1
       list parked, RTS never runs   2.319  RTS -> IDL, then DCEI
       stale SWI vs state 0          2.320  handler takes arm 0, clean
       -> teardown -> 7x66           2.320  the open completes; the boot
                                            report reaches the terminal
```

### The poll rate is a band with a measured edge at each end

`FOCUS_IOPPOLL` defaults to **400 us**, in the middle of it:

| us | HP-UX console open | 520 boot |
|---|---|---|
| 900, 1000 | `2x56 + 66` return | fine |
| 700 … 200 | **clean** | **fine** |
| 150, 100 | clean | **never reaches its banner** |

What the 520 cannot take below ~200 us is **not yet known** and is the one
loose end of this work. It is not the uncounted-DMA pacing — that was
suspected, fixed anyway (`run_dma`'s `uncounted` argument, so an uncounted
transfer advances on the millisecond beat and its throughput does not follow
the poll rate), and the 520 still failed at 100 us afterwards.

### A/B controls for all of it

`FOCUS_IOPPOLL=1000` and `FOCUS_IOPPARKALWAYS=1` together restore the old
IOP behaviour; `FOCUS_ASIPAUSE` sweeps the Pause. Both IOP knobs on gives
back the original `7x66`, which is how each change above was shown to be
load-bearing.

---

## 6b. The hunt, as it was recorded before the answer (kept for the dead ends)

All addresses runtime for this configuration.

- **Channel record 0x1e97d4** (allocated ~t=1.27s by CORE SYSTEM
  +0x51e2, run 0x26e7a region): +0 lock, +8/+12/+16 identity info
  (8, buf, 2), +0x20 state, +0x30 event/status pair, +0x40 = 0x1e9db4,
  +0x48 pointers 0x1e9a58/0x1e9a8c.
- **ICEI command lists** hang off the DRT for select code 0:
  - `0x1fe948` — the boot-scan identify list (WIF8 0, 2-byte read).
  - `0x1e9820` — the driver's steady-state list: two Increment-and-
    Branch gates, `PAU` (WIF8 0x20), a third gate, then ICEI-suspend.
    **Gate at 0x182c/count 0x1834 targets 0x1e9948 — armed by whom is
    the open question**; it never fires.
  - `0x1e9854` — the post-suspend list: gate at +0xc (target cell
    0x1e9864, count 0x1e9868) → the transaction at `0x1e9874` when
    armed, else fall through to the RTS poll (WIF8 0x88, 17-byte read
    into 0x1e980b).
  - `0x1e9874` — a full transaction: WIF8 (0x88 observed), WDS 0x824,
    WDA 0x1e980b, WDC 0x11, SDEC, then 0x74 (DCEI).
  - The **driver's queuing routine at run 0x27518** writes the gate
    target (0x1e9864 ← 0x1e9874) and count (0x1e9868 ← −1), then
    issues ICEI at 0x2752c. The count is cleared by our own 065
    implementation as it takes the branch, so the PCs seen zeroing it
    are wherever the CPU happened to be during `iop_tick` — not kernel
    code.
  - **The driver queues its real transaction at t≈2.27s** — `CORE SYSTEM
    +0x2b5c` (run **0x277f4**) writes `0x1e9864 ← 0x1e9948` — and with
    per-PA `DCE` (below) it now runs: the console's own CLC, request 4
    subfunction 253, get the card switch setting.
  - *(Resolved 2026-08-22: the apparent disagreement between `FOCUS_DUMP`
    reading `0x1e9948` at that word and the list log reading `0x1e9874`
    was not a contradiction — all 1,964 list runs happened BEFORE the
    arming write, because `DCE` was stopping the IOP outright. Watch out
    for `FOCUS_WWATCH`'s 400-line cap when reasoning about "the last
    write": it silently truncates.)*
- **The RTS dispatch** (the handler the card's answers drive): reads the
  17-byte block at 0x1e980b, `CMPI #5`-guards the opcode and
  computed-branches through a six-arm table at run 0x274e0. Arm 0 (IDL)
  is the "nothing to do" path (stores −1 into the DB result cells).
  The other five arms — SWI/EDD/LCD/AES/ERT per 27128A 4-60 — are where
  the open's progress has to come from.
- **The open's sequence, as it now runs**: `RTS` → `CLC` (request 4 sf
  253) → `PAU` → `RTS` → `RD` (the data phase, which must deliver the
  switch byte the request prepared, not a terminal record) → `RS` (the
  read-status block, 5 bytes for a read-card-information request). It
  then stops: the transaction completes with **66** in the record's +32
  rather than 55, so the wait at CORE SYSTEM +0x4330 escapes 66 directly
  instead of timing out with 56. Next question: what makes the kernel
  call this transaction failed — the state machine at run 0x27918
  compares +32 against 66 explicitly, so 66 is a known state there, not
  a generic error.
- **The abort, exactly.** The interrupt handler's RTS dispatch arm for
  **SWI** (run **0x27530**) does two checks before it acts:
  `LDS DB+56; LSR 16` takes the LCN out of the report, compares it with
  the limit at `DB+12`, and branches to the abort on greater; then
  `LSXL 3; LDS DB+236,X; STAX; LDX DB+0,X; BR P+8,I,X` dispatches on
  that channel's **state**, and **state 0 lands on the abort as well**
  (table at 0x2754c, base 0x27550, entry 0 = +0x178 = 0x276c8).
  The abort at run **0x27a60** issues `DCE`, `DDMA`, `DINT`, `CIRQ`,
  writes −1 to the record's +36 and +52, and walks the outstanding list
  storing **66** into each entry's +32, +12 and +4. That is where the
  66 the console open dies on comes from — and note the record's +32
  held **55, success**, three milliseconds earlier: the transaction
  worked and was then torn down.
- **Ruled out** (both swept, no effect): the LCN echoed in the SWI
  report (`FOCUS_ASILCN` 0..3) and the completion-event delay
  (`FOCUS_ASIEVDELAY` 2..60 ms). The kernel sees channel state 0
  whatever we report and whenever we report it.
- **CORRECTION (2026-08-22, second pass): the arm does NOT loop.**
  Tracing the SWI arm from its first hit shows `0x27530 … 0x27584` →
  `0x276a8 … 0x276be` → **IXIT**, straight back to the dispatcher's
  `STOP` at run 0x182f0. The stale re-dispatch that aborts happens on a
  **later interrupt**, not by falling through. (Related correction:
  orders logged with `p=0x182f2` are the IOP executing a command list
  while the CPU idles at that STOP — they are list-driven, not issued by
  the handler.)
- **The list after a CLC, fully mapped**, and it is exactly right:
  `0x1e9950` runs the CLC (`WIF8 0xb8`, `WDS 0x834` out, 7 bytes) then
  suspends; the poll resumes into the RTS entry, which reports SWI; the
  handler arms the transfer and IXITs; `0x1e998c` runs **`RD`** (1 byte
  to 0x1e9dd4) and suspends; `0x1e99c8` runs **`RS`** (7 bytes to
  0x1e9de1) and suspends **with `resume=0x1e9874` — the RTS entry**.
  So after the status block the list is parked exactly where it would
  refresh the report buffer.
- **THE RACE.** The `RS` leg's completion interrupt reaches the CPU
  *before* that suspended RTS runs, so the handler dispatches the stale
  SWI still sitting in `DB+52` against a channel the completion has just
  closed — and that is the abort. The fix has to make the parked RTS run
  before the completion interrupt is taken. `FOCUS_IOPDEFER` gets that
  ordering right in principle (the DMA completes and the list resumes
  inside the same `poll_and_service`, before the CPU runs again) but
  breaks the `RD`/`RS` issuance for a separate reason, so neither
  setting is yet a whole answer.
- **THE FIX, FOUND AND NOT YET SHIPPABLE.** `take_request_block()` sets
  `m_want_order` (the SRQ) only for write-family requests. Setting it
  for **every** request — the card drives the whole transaction, and the
  same SRQ asks for the `RD`, `WD` and `RS` legs that follow the CLC,
  not just the CLC — **removes the entire failure chain**: measured over
  a fourteen-second boot the escape log drops from `7×66` to nothing but
  the five benign `941`s. No timeout, no abort, no 116. That is the
  console open no longer failing.
  **Resolved, and the result is a negative one.** The storm it exposed
  was two real IOP defects — a device being linked onto its level list
  while already the tail, so the list self-referenced and never emptied,
  and an interrupt line re-asserted rather than edged, re-setting the
  message bit the service had just cleared. Both are fixed. With them
  fixed the card change is **neutral**: 28 transactions and `7×66` with
  or without it, exactly the committed behaviour. So the clean escape
  log it produced was **the storm masking the failure**, not the failure
  being fixed — the machine was looping in the handler and never got far
  enough to fail. The change is not kept.
  The original text follows for the record: the machine then fell into
  an **interrupt storm**.
  The handler dispatches, finds IDL, takes the clean arm at run 0x276d6
  (release the lock, `IXIT`), the CPU reaches the dispatcher's `STOP` —
  and an interrupt fires again immediately, 10,000+ times a second
  against ~100 RTS transactions a second. So the interrupt line is
  staying asserted rather than being re-raised: look at what clears
  `m_int_posted` when the handler consumes a report but issues no
  `CIRQ`, and at whether `074`/`DCEI` should be clearing the request as
  `ICEI` does. Fixing that is the whole remaining distance on the
  console.
- **The older (partly superseded) reading of the loop.** Traced end to
  end:
  a CLC opens the transaction (state −1 → **4**); the card reports SWI
  and the SWI arm sets state **24**; `RD` and `RS` run the data and
  status phases; the kernel completes, and at run **0x2749c** (guarded
  by `LDX DB+48`, which the SWI arm set) it **closes the channel**,
  state → 0. The handler then loops back to run 0x274a8, takes `DB+0`'s
  lock, finds its pending-work queue at `DB+40` **empty**, falls through
  to run 0x274d0 and **re-dispatches the stale report buffer `DB+52`**
  — still SWI — against the now-closed channel. State 0 → abort.
  `DB+52` is the RTS block itself (`DB+56` is where the LCN is read
  from, and the block DMAs to 0x1e980b), so "refreshing" it needs
  another RTS transaction, which cannot happen inside the same
  interrupt.
- **Measured**: what the card reports after a CLC decides everything.
  Sweeping it (`FOCUS_ASICLCRTS`) over a 10-second boot, counting the
  66 cascade: SWI(1) 7, EDD(2) **0**, LCD(3) 7, AES(4) 6, ERT(5) **0**.
  **EDD and ERT eliminate the abort completely.** EDD is not yet a win
  overall — it trades the abort for a busy loop in the same handler
  region (34M instructions in 0x27000–0x27fff) and the panic no longer
  reaches the terminal — so SWI remains the default, since the printed
  panic is a real capability. But it localises the answer: the card's
  post-CLC report is the lever, and the right one is probably
  state-dependent rather than constant.
- **Three axes, each measured alone, each trading one failure for
  another.** The sequencing of report / transfer / completion is what is
  wrong, and no single axis fixes it:

  | change | result |
  |---|---|
  | default (SWI, synchronous DMA) | abort 66; **panic prints** |
  | constant EDD or ERT after a CLC (`FOCUS_ASICLCRTS=2` or `5`) | no abort, but a busy loop in 0x27000–0x27fff and the panic stops printing |
  | deferred DMA (`FOCUS_IOPDEFER`) | no abort — SWI lands on a LATER RTS read, so the completion no longer runs inside the handler — but the driver then never issues `RD`/`RS`, and a later timeout (56) follows at ~4.4s |

  Deferring is measured **safe**: BASIC on floppy, BASIC on the installed
  disc, the 9020 diagnostics environment and the printed panic are all
  unaffected. It is off by default because it buys no win on its own and
  the immediate-DMA path has a documented reason (§`run_dma`).
- **Tried, and reverted: tying the report to its interrupt.** Holding
  the owed report until `tick()` actually raises the attention — so the
  driver's polling list cannot consume a report meant for the interrupt
  handler — is a fair model, and combined with `FOCUS_IOPDEFER` it made
  the driver issue **`RD` and `RS` for the first time**, the correct
  order sequence. It still aborted, and it **broke the panic printer**:
  that printer polls FR1/FR6 and never takes an interrupt, so a report
  held for one never reaches it. **Any model here has to serve both
  readers** — the interrupt-driven console driver and the polling
  printer — which is a real constraint on the design, and the reason the
  simple version is wrong.
- **The combination worth trying next** follows from those three rows:
  deferred DMA *plus* a **state-dependent** report — SWI only while a
  transfer is actually pending, EDD once the data phase has run — so
  that the handler never re-dispatches a stale opcode and the driver
  still gets told to run the transfer. Both axes now have knobs.
- **Tried and negative**: padding the read-status block out to the seven
  bytes the host asks for, so the leg ends on COUNT rather than Device
  End (in case the driver keys off the termination). No change, with or
  without deferred DMA. The short block is not the problem.
- **The work queue, watched.** `DB+40` is the handler's pending-work
  queue: run 0x2787a pushes the transaction (0x1e9948) onto it, run
  0x274c4 pops it, and the handler dispatches what it popped. The loop
  at 0x274a8 re-reads it, and only when it is EMPTY falls through to the
  stale re-dispatch. So the design is "drain the queue, then confirm the
  card has nothing left" — and the confirmation reads a buffer that only
  an RTS transaction refreshes. Whatever refreshes it between the
  completion and the loop is the missing piece; the handler issues RTS
  itself elsewhere (orders appear at TRAP HANDLERS +0x162, run
  0x182f2), so finding *that* call and why it is skipped here is the
  most direct remaining thread.
- **What the handler's loop really demands.** Its terminating condition
  is the report buffer reading **IDL**: `0x274d0` re-dispatches `DB+52`,
  and arm 0 is the clean exit while arm 1 against a closed channel is
  the abort. `DB+52` is the RTS block in memory, refreshed only by an
  RTS transaction, and the handler issues none between completing the
  transfer and looping. So either the completion must not close the
  channel when it does (the close at `0x2749c` is gated on `DB+48`,
  which the SWI arm itself sets), or the transfer must still be
  outstanding at the loop. That points at the data phase ending too
  early — `RD` moved one byte and `RS` five — rather than at the report
  vocabulary, which is where the next look should go.
- **The channel state IS populated** — the earlier "never populated"
  reading was wrong. The table is at `DB+232`, eight bytes per LCN:
  the **absolute pointer** at `+232+8k` and the **DB-relative offset**
  of the same struct at `+236+8k`. Both forms are used: the state
  dispatch takes the offset (`LDS DB+236,X` then `LDX DB+0,X`), the
  teardown takes the pointer (`ADXI #232`). For this boot `DB=0x1e97d4`
  and the three structs are `0x1e9910`, `0x1e9a94`, `0x1e9c18`, 388
  bytes each. Watching the first one's state word
  (`FOCUS_WWATCH=1e9910:1e9913`) shows the documented lifecycle running
  **correctly**:

  | t | P | state | |
  |---|---|---|---|
  | 1.269 | +0x3546 | 0 | init |
  | 2.268 | +0x3db0 | −1 | claimed |
  | 2.268 | +0x3e62 | 4 | CLC issued |
  | 2.270 | +0x3c3e | 24 | the SWI arm — it *does* run |
  | 2.271 | +0x3b68 | 0 | the completion closes the channel |

  The transaction blocks are right too: CLC (0x834), `RD` (0x824 to
  0x1e9dd4) and `RS` (0x824 to 0x1e9de1) are all built, with the struct
  pointing back at `DB` at +68. The card dialogue is not what is broken.
- **Attribution needs `FOCUS_IOALL`, not `FOCUS_ASIDBG`.** The order
  log says what the card saw and nothing about who sent it, and it
  carries no timestamps; `FOCUS_IOALL` prints `p=` for every I/O. Doing
  this properly gives the driver's whole transaction, order for order:

  | order | issued from |
  |---|---|
  | `0xb8` CLC | `p=0x276bc` — CORE SYSTEM **+0x3d88**, the handler |
  | `0x20` PAU / `0x88` RTS | the poll |
  | `0x68` RD | `p=0x276bc` |
  | `0x49` RS | `p=0x182f2` — TRAP HANDLERS +0x162, the IOP list |

  and the card answers SWI, then EDD. **The driver's transaction runs
  and succeeds.** The 27 later CLCs come from HP-UX KERNEL +0xc898 and
  +0xca9c — the panic printer. The 999 IDL polls that precede all this
  are the handler's idle poll from `p=0x2752c` over the second before
  the open: normal, not a symptom. Open and panic are three
  milliseconds apart, which is why they look adjacent in an
  untimestamped card log.
- **The abort path, traced instruction by instruction.**
  `FOCUS_BREAK=2749c:1 FOCUS_TRACEN=4000` records the whole way from the
  close to the panic, and it confirms the re-dispatch reading above
  rather than replacing it:

      +0x3b68  the completion closes the channel (state -> 0)
      +0x3b74  the handler loop head; the work queue is empty
      +0x3b9c  LDS DB+52 -> STAX -> LSXL 2 -> BR P+8,I,X   the re-dispatch
      +0x3bfc  arm 0: LCN = DB+56 >> 16, bounds-checked against DB+12,
               LDS DB+236,X (X = LCN*8), LDX DB+0,X, BR P+8,I,X
      +0x3d94  the state dispatch's arm for state 0 — the error path
      +0x4118  the teardown: four abort orders, block+36/+52 := -1,
               then state -> 0 and status -> 66 through the DB+232 walk
      +0x4330  the completion check reads the status, sees 66, raises it

  So there are **two** dispatches, not one. `DB+52` selects the outer arm
  and measures **0 at all eight visits** (`FOCUS_BREAK=274d8:8`, X is the
  arm); the inner one dispatches on the channel's own state word, which
  the close zeroed two instructions earlier — and state 0 is the error
  arm. That is exactly the documented "re-dispatch against a closed
  channel", now measured rather than inferred.
- **The 66 is a completion status, not a raise.** Block+32 is the
  completion status: 55 is success. `FOCUS_WWATCH=1e9930:1e9933` catches
  exactly two writes — **55 at +0x41e0** when the transaction is set up,
  then **66 at +0x4160** in the teardown, which overwrites it. The reader
  at +0x4330 (`LDIS #32; PLDA,B; CMPI #55; BCCS.GL; EXIT N=6; ESCP`)
  returns normally on 55 and otherwise **raises the status itself** — so
  the first escape 66 of the boot is this field, and none of escmap's
  constant-66 sites is involved.
- **Two dead ends, recorded so they are not re-run.** (1) There is no
  missing semaphore post. The one task that blocks in the failure window
  — on `0xbd798`, at CORE SYSTEM +0x59ba — is a **seg 70 WIN/H9000**
  task on stack `SB=0xcde00`, while the whole failing chain runs on
  `SB=0xad0a0`. Unrelated. The console driver takes no semaphores at
  all: `FOCUS_SEMALL` censuses the entire boot at **1554** UP/DOWN
  operations and none belongs to the open. (2) `WAIT` is not a blocking
  wait — ERS sheet 95 makes it a calibrated delay — so `LDIS #5; WAIT`
  at +0x410e times nothing out, and the loop at +0x40d0 is an open-coded
  wake-all that the failing path never enters.
- **NEXT.** The close at +0x3b68 and the re-dispatch at +0x3b9c are in
  the same handler pass, so one of them is wrong. `DB+52` never changes
  (0 at every visit), which makes the outer arm a constant — worth
  checking whether anything is supposed to refresh it, since the docs'
  older note has run 0x2787a pushing work onto `DB+40` and run 0x274c4
  popping it. Either the close must not zero the state while the loop
  will still dispatch on it, or the loop must not reach the state
  dispatch with an empty queue.
- **The report/DMA matrix, re-measured (2026-08-22).** Twelve 14-second
  runs, `FOCUS_ASICLCRTS` × `FOCUS_IOPDEFER`, counting escapes. The five
  `941`s are benign and present in every row.

  | post-CLC report | plain | + `FOCUS_IOPDEFER` |
  |---|---|---|
  | default / SWI(1) | 7×66 | 2×56, 6×66 |
  | **EDD(2)** | **3×91, no 66 at all** | 2×56, 6×66 |
  | LCD(3) | 7×66 | 2×56, 6×66 |
  | AES(4) | 2×56, 6×66 | 2×56, 6×66 |
  | ERT(5) | 2×56, 6×66 | 2×56, 6×66 |

  Deferred DMA now adds `56`s in **every** configuration and removes no
  `66`s; it is not worth pursuing on its own. ERT has moved since the
  older sweep (it used to give no cascade, it now behaves like AES).
- **EDD is a real advance, and the busy loop it used to cost is gone.**
  This file recorded EDD as trading the abort for a busy loop in
  `0x27000–0x27fff`. `FOCUS_PCPROF` over 30 emulated seconds says that
  is no longer true — the two IOP interrupt fixes removed it:

      P=00043000..00043fff  35,917,865      <- the new stall
      P=00100000..00100fff   1,986,358
      P=0009b000..0009bfff   1,429,762
      P=00027000..00027fff      57,154      <- was ~34,000,000

  So under EDD the console driver's whole abort chain is eliminated:
  no 66, no handler loop. The driver raises **escape 91** three times
  at t=2.28 (seg 32 +0x1822, then +0x2fae, then HP-UX KERNEL +0xb4 —
  the same three propagation sites the 66 used) and the machine then
  goes completely static: identical P/Q/S at t=5, t=15 and t=29.
- **What it stalls on is the panic printer, not the driver.** The spin
  is HP-UX KERNEL **+0xc65c**, and it is two instructions:
  `LDS Q+24; IOR io=0x09; BRO; BCCS.ALW` — `CMD_RIF1`, read interface
  FR1. `FOCUS_IOALL` confirms 4000 consecutive `op=011 pa=0
  p=00043fe0` and nothing else. That is the printer polling the console
  card's FR1 forever, which is exactly why the panic stops reaching the
  terminal under EDD, and it is the **"the report has two readers"**
  constraint in concrete form: the report the printer is waiting for
  has already been consumed.
  **NEXT:** identify escape 91 (`escmap --value 91` gives SERIAL
  +0x1e08 and three CORE SYSTEM sites; none appear in the log, so the
  first 91 is a re-raise at seg 32 +0x1822), and decide whether the
  printer's FR1 poll can be served without taking the report away from
  the interrupt-driven reader. EDD looks adoptable once it can.
- **The panic printer bypasses the driver completely.** Asked directly:
  it does not go through the console driver, or the IOP command list, or
  interrupts. It drives the card register by register.

  | | driver path | panic printer |
  |---|---|---|
  | I/O from seg 32 HP27128/30 | **zero, all boot** | — |
  | how orders reach FR8 | `IOC io=0x38` = **ICEI**, then `IXIT`; the **IOP** runs a command list | `LDIS #184; IOW io=0x42` — writes the order byte itself |
  | issuing site | CORE SYSTEM +0x3bf8 (999×) and +0x3d88 (2×) | HP-UX KERNEL +0xc898, +0xca9c |
  | orders produced | PAU 999, RTS ~1000, CLC 1, RD 1, RS 1, IDY 2 | CLC 27, WD 25, RS 27, RTS 54 |
  | DMA setup | in the list | its own (`op 003/004/005`, 52× each) |
  | completion | interrupt | polls FR1 itself: `IOR io=0x09` at +0xc65c, 892 reads |

  The console DDM issues **no I/O at all** — nor do TTY SUBSYSTEM or
  SERIAL. Everything on the driver side is CORE SYSTEM starting a list
  and the IOP executing it. The printer, by contrast, has the literal
  `0xb8` in an immediate and writes it to the order register inline.
- **So "the report has two readers" is structural, not incidental.** One
  reader is the IOP command list, running on the driver's behalf and
  interrupt-driven; the other is the printer, polling FR1 from the CPU
  with no list and no interrupt. They contend for the same one-shot RTS
  report, and whichever arrives first consumes it. That is exactly why
  EDD fixes the driver and hangs the printer — and why any fix here has
  to serve both, as this file has said all along.
- **Attribution warning, second instance.** `FOCUS_IOALL`'s `p=` is the
  CPU's PC, so **list-driven** I/O is attributed to whatever was
  running. The 878 RTS orders that appear to come from WIN/H9000 are
  not its: WIN/H9000 +0x19f1c is `DECM Q-35; CMPN #0; BCC.EL`, a delay
  loop containing no I/O instruction. Likewise the 999 PAU orders "from"
  CORE SYSTEM +0x3bf8 — that site issues ICEI (`op=070`), not the order
  write (`op=0102`). Only `IOW`/`IOR`/`IOC` sites confirmed in the
  disassembly are real attributions; the printer's are, the driver
  path's are not.
- **FIXED: the card's completion event was a slot, not a queue.** The
  contention above is not abstract, and instrumenting `m_pending_event`
  showed it directly: the printer writes FR9 and arms `configured`
  (0x30), the driver's list arms `done` (0x10) microseconds later and
  **overwrites** it, the tick queues 0x10, and the printer — which
  requires a high nibble of 3 — rejects it and polls FR1 for ever. That
  was the livelock at HP-UX KERNEL +0xc65c.
  A plain queue is wrong the other way, and measuring caught that too:
  `answer()` arms `done` on every transparent-status read, ~1000 times a
  boot, so the backlog reached **137** entries at one announcement per
  tick and the printer's code sat behind all of them. `arm_event()` now
  appends a new code and ignores a duplicate — collapsing repeats is
  what the single slot did correctly — and the queue stays two deep.

  | EDD (`FOCUS_ASICLCRTS=2`) | before | after |
  |---|---|---|
  | emulated speed | 93% | **3381%** |
  | instructions in `0x43000–0x43fff` | 35,917,865 | never reaches the 40M mark in 90 s |
  | printer reads of event 0x30 | 0 | 4 |
  | state | livelock on the FR1 poll | idle |

  The default configuration is unchanged (7×66, panic prints) and BASIC
  on the installed disc still boots clean.
  **It still does not print the panic text under EDD** — no `WD` orders
  are issued, so something later in the printer's sequence is not yet
  satisfied. That is the next thread.
- **Card and I/O logs are timestamped now.** `FOCUS_ASIDBG`,
  `FOCUS_IOALL` and the event log all carry emulated seconds
  (`iop.trace_time`). This closes the gap that produced two wrong
  conclusions above: an untimestamped order stream cannot be
  partitioned by eye, and `p=` is useless for list-driven I/O.
- **`FOCUS_ASICLCRTS=2` is not a candidate fix — it lies to the
  printer.** The knob forces the post-CLC report for **every** CLC, and
  the printer issues 27 of its own. Told `EDD` ("end of data for this
  transaction") the moment it connects, the printer concludes its data
  phase is already over and never sends any. Measured, printer orders:

  | | default | `ASICLCRTS=2` |
  |---|---|---|
  | CLC | 27 | 27 |
  | **WD** | **25** | **0** |
  | **PAU** | 0 | **25** |
  | RS | 27 | 2 |

  Twenty-five `WD` become twenty-five `PAU`, one for one. So the EDD
  result above is not "the driver fixed and the printer unlucky" — it
  is one global override breaking every CLC-issuer, and it happens to
  suppress the driver's abort as a side effect. The card already does
  the state-dependent thing correctly: `take_request_block()` arms
  `SWI` after a CLC and `dma_complete()` arms `EDD` after an `RD`/`WD`.
- **The transaction, timestamped end to end** (current build):

      2.321  status := 55, the transaction is set up
      2.322  CLC reaches the card
      2.323  the card reports SWI
      2.324  status := 66, the abort

  Three milliseconds, and the SWI does arrive one millisecond before
  the abort. The frontier is exactly where the traced path left it.
  **Do not compare timestamps across builds** — the event-queue fix
  moved this whole sequence about 50 ms later, because the printer's
  configuration handshake now completes instead of livelocking.
- **The open itself** runs in the doorbell-server task: TTY SUBSYSTEM
  +0x2eb8 (run 0x60ca4 region, frame chain seg 11 ret 0x776) → seg 32
  HP27128/30 frames (rets 0x560/0x1108/0x2674) → CORE SYSTEM timed
  wait. Timeout = escape 56 at the driver's site +0x1782, converted to
  66 at CORE SYSTEM +0x3fd2, dying finally as 116 from the NULL stub.

## 7. Tool crib

| Question | Tool |
|---|---|
| what error is this number, who raises it | `tools/escmap.py image --value N` |
| decode a panic to modules+files | `tools/runhpux.sh ... \| tools/panicdump.py --image ...` |
| what module is P in | FOCUS_STATEAT CST dump, or §1 delta |
| first anomalous escape | `FOCUS_ESCLOG` (+ `FOCUS_ESCVAL=<hex>` to break on one) |
| who writes/reads a cell | `FOCUS_WWATCH`/`FOCUS_RWATCH` (`*FROM` to arm late) |
| card dialogues | `FOCUS_ASILOG` + `FOCUS_ASILOGMAX`, `FOCUS_HPIBLOG`, `FOCUS_IOALL`, `FOCUS_ICEILOG` |
| **who issued an order** | `FOCUS_IOALL` (`p=`). `FOCUS_ASIDBG` shows what the card saw, never who sent it, and has no timestamps — do not partition it by eye |
| semaphore census | `FOCUS_SEMALL` + `FOCUS_SEMALLMAX`/`FOCUS_SEMALLSKIP` (default cap 400 hides everything past ~1.4 s) |
| where time goes | `FOCUS_PCPROF` |
| task blocking | `FOCUS_SEMLOG` (timestamped) |


## 8. The console drain, and how it stopped the boot (SOLVED 2026-08-26)

For months the machine printed its boot report and stopped dead. It was one
missing thing, and it was in the card, not the kernel: **the emulated ASI
never sensed the "transmit buffer is empty" asynchronous event**, so nothing
ever told the console driver that its output had drained. The tty subsystem
drains the console as the last act of printing the boot report, and a drain
that never completes is a boot that never starts `init`.

With the event modelled, `PCL STT[648]` returns, `PCL STT[647]` runs, and the
machine goes on to the whole device configuration pass. `FOCUS_ASINOASYNC=1`
is the A/B control: with it the boot stops after the banner again, exactly as
it did.

### The chain, from the stop back to the card

Every link measured, none inferred:

| | |
|---|---|
| `HP-UX KERNEL +0x22d6c` | `PCL STT[648]` = TTY SUBSYSTEM entry 2. `FOCUS_WATCH=37a04:37a20`: reached at 2.32 s, never returns; `+0x22d70` (CONFIGURATION entry 3) and `+0x22d74` (start init) never run |
| TTY SUBSYSTEM entry 2 (`PB+0x708`) | prints the report line by line — request code **17** through the driver — then issues code **24**, waits 400 ms (`PCL STT[76]` = CORE SYSTEM entry 228) and issues code **10**. `FOCUS_WATCH=5e654:5e67a`: nothing after the code-24 call ever executes |
| HP27128/30 `+0x2160`..`+0x2174` | the code-24 arm: if `chan+140` is non-zero it **sleeps on `chan+160`** at priority 73 through HP-UX KERNEL entry 51 |
| HP-UX KERNEL entry 51 → CORE SYSTEM entry 89 | the kernel's `sleep()`: `PSLA` test-and-set on two objects, decrement, and `DISP` at `+0x54aa`. The task parks there and the dispatcher never brings it back |
| `chan+140` (`0x1e8a28`) | set to 1 by **every** write at HP27128/30 `+0x2e46`, and cleared by exactly two sites. `FOCUS_WWATCH=1e8a28:1e8a2b`: seventeen writes of 1, no write of 0 |
| the write path's own clear (`+0x2e70`) | skipped: on a write whose status block came back with byte 1 = 0 it branches past the wake (`BRZ` at `+0x2eb6`, traced taken) |
| the other clear (`+0x23da`, inside entry 7) | the console's **event daemon** — the task the open creates with `LPPS STT[7]` through CORE SYSTEM entry 206. It clears `chan+140` and wakes `chan+160`, and it is blocked |
| the daemon's block | `DOWN` on `[chan+108]` = `0x1e8a00` at HP27128/30 `+0x22cc`, t=2.338. `FOCUS_SEMALL` over the whole boot finds **one** operation on that word: this `DOWN`, count −1 |
| what would post it | the request block at `0x1e9db4`, whose `+8` holds `80040020` — the label of seg 32 entry 16, eleven instructions that take the channel out of the request and `UP` its `chan+108`. `FOCUS_RWATCH` on `+8` sees no read all boot |
| who calls that callback | **HP27128/30 entry 3**, the driver's asynchronous-event handler. Its label `8000c020` lives at the interrupt handler's `DB+28` (`0x1e97f0`, confirmed by `FOCUS_FIND`), and its request block at `DB+32` (`0x1e97f4` = `0x1e9db4`). It copies the event block into the request and calls `[req+8]` |
| who calls entry 3 | CORE SYSTEM's Read-Transparent-Status dispatcher, **arm 4 — AES** — at run `0x2761c`: `LDB DB+57; CMPM DB+16` (the port ID, bounded), then `LDS DB+4; LDS DB+32; LRAS DB+56; CPAA; LDS DB+28; PCLS STT[0]`. It is the **only** caller |

So the entire chain hangs off one report opcode. The six-arm table at run
`0x274e8` decodes as IDL `0x27500`, SWI `0x27530`, EDD `0x275bc`, LCD
`0x275f4`, AES `0x2761c`, ERT `0x27646` — and our card reported IDL, SWI and
EDD and **never AES**.

### The event, and what the manual says about it

27128A 4-31 lists the eleven asynchronous events in priority order; number 5
is "transmit buffer is empty", and 4-62 gives its code in the event block:
**3**. That is exactly the constant the daemon's dispatch compares against
(`LDS Q+72; CMPI #3` at `+0x23aa`, Q+72 being the byte the handler copied
into `req+0`), and 255 — "completion for open link request" — is the other
constant the driver's entry 3 tests. The vocabulary matches end to end.

The mask matters too: "IF ALL INTERRUPT CONDITIONS ARE DISABLED, NO
ASYNCHRONOUS INTERRUPT WILL OCCUR" (4-32). The host interrupt mask is byte 22
of the configuration block and byte 0 of subfunction 21, bit 2 being
transmit-buffer-empty. **HP-UX writes `0x37`** — break, data available,
transmit buffer empty, and both signal characters — measured, and it is why
the model can honour the mask rather than firing unconditionally.

### The fix, and the second bug it uncovered

`src/iop/focusasi.h`:

- `dma_complete()` senses **event 3** at the end of a write-device-data data
  phase: every character has reached the terminal, which on this card is as
  far as the SIO ever gets.
- Read Transparent Status reports **AES** when an event is waiting and no
  transaction is open (`!m_rts_report && op == IDL && !m_request`) — never
  over the top of a logical channel, because AES replaces the LCN field.
- `record_ready()` in `tick()` now senses **event 1** as well as posting
  `STAT_AES`; the status byte was only half of what 4-6 describes, and a
  keystroke could never have reached a driver that was not already reading.
- **`take_data()` was giving the whole configuration block to the terminal.**
  Only write-device-data's bytes are characters; write card configuration,
  control device and control card send their data on the same order. That is
  where the run of garbage ahead of the boot banner came from, and it is also
  why the interrupt mask could not be read. Fixed, and the banner now starts
  the output.

`FOCUS_ASINOASYNC` silences the sensed events, which is the card exactly as
it behaved before, and puts the stop back.


### The file system, and why it could not be mounted even when it is reached

`FOCUS_HPIBLOG` still shows the HP-IB card getting only its bring-up (`0b`,
`03`, `01`) plus one `12` after the boot report, and SDF_DISC still executes
only at module-init time: the configuration pass in §9 fails before anything
mounts anything.

Underneath that sits a second, independent blocker. `tools/sdf7914.py`
establishes that the 7914 unit save is **tape 1 of two**, together holding
one 130,815-block SDF volume:

    tape 1   disc blocks 1..65,406        offset = (block - 1) * 1024
    tape 2   disc blocks 65,409..130,814  offset = (block - 3) * 1024
    never imaged: block 0, and blocks 65,407..65,408

and `-hard1` mounts **tape 1 alone**: 66,975,744 bytes = 261,624 CS/80
blocks of 256 = SDF blocks 1..65,406, exactly what `cio5` prints. **The FA
file — the volume's inode table — starts at disc block 69,087, which is on
tape 2.** So the half of the volume the machine can address contains no
file-system metadata at all.

Read across both tapes the volume is intact and complete, and everything a
boot needs is on it:

    /etc/init      33,652   /etc/inittab      826
    /bin/sh        79,640   /dev/console      character special

So bringing up the file system needs the drive to present the **whole**
volume — the two tapes joined at their documented origins, with block 0 and
65,407–65,408 as holes — and block 0 is the SDF volume header, which was
never imaged and would have to be synthesised from what the FA file already
pins (`s_blksz` 1024, `s_fa`, `s_maxblk`). Note that today the loader reads
its boot descriptor from file offset 0, which *is* disc block 1 because the
image starts there; a whole-volume image shifts that by one block.


## 9. Current frontier: the device configuration pass

`PCL STT[647]` — CONFIGURATION entry 3 — now runs, at 3.28 s, and walks every
driver module in the image: `2 -> 70`, `61`, `51`, `47`, `46`, `43`, `42`,
`40`, `38`, `35`, `33`, `32`, `29`, `11`, `8`, each module's entry 2 called
from CORE SYSTEM `+0x7e58`. Then it fails, and the machine prints **"System
halted due to system error."** at about 14 s.

Two escapes, in order, both traced to a break:

- **115 at CORE SYSTEM `+0x7e48`.** Entry 315 walks the registered-driver
  list from `DB+220`, asking each driver its identity (request code 11) and
  comparing the answer with the key it was given. The key is **0**, no driver
  answers 0, the walk reaches the `NULL` stub and runs out. The key comes
  from a 20-byte configuration record CONFIGURATION built for itself at
  1.231 s (`FOCUS_WWATCH=afb68:afb7b`, written from seg 49 `+0x13de`): its
  fields are `{0x00e00000, 0, 1, 5, 0}` — key 0, then 1 and 5.
- **110 at IOS and DDMs `+0xaf36`**, which is fatal. IOS calls the driver
  with request 11 and then request 7, takes the top five bits of the word at
  `[req]+24` as a device class, and escapes 110 when it does not match the
  class asked for — again 0. The frames above it are **HP27110** (`0x6ab50`,
  `0x6cebe`), the HP-IB interface driver: the device being configured is the
  system disc's interface at select code 5.

So the next question is what fills that configuration record's key, and why
the class the HP-IB path reports is not the one the record asks for.

Underneath that sits the second, independent blocker recorded below: the
mounted image is tape 1 of a two-tape volume, and the inode table is on
tape 2, so even a configured disc has no file-system metadata to read.


### The doorbell, and why the obvious knob is not the answer

Separately, `MSG=0x00200000` — bit 10, the doorbell — stands set through the
console work, and HP-UX's `MASK=0x08000000` leaves it unmasked.
`src/cpu/focus/focusexec.h` deliberately excludes bit 10 from message-trap
delivery, and **`FOCUS_MSG10TRAP=1` is not the fix**: re-tested 2026-08-26,
with it on the boot produces no banner, no escapes and no output at all —
worse, not better, exactly as the 2026-08-20 note recorded. The poll-only
reading is right, and the dispatcher's own handshake is visible at
TRAP HANDLERS `+0x148`:

    PSEB; INCM DB+908
    LDI #0x200000; SMPB 2        ; consume MSG bit 10
    PMPB 1; LDI #0x200000; NOT; AND
    STOP,B                       ; stop, with bit 10 left enabled to wake it

Our `PMPB`/`SMPB` (`focusexec_mem.ipp` cases 031/032) match the ERS, and the
dispatcher is measurably cycling through this loop rather than wedged.

**Semaphore 920 (absolute 0x398), the doorbell server's park, has exactly
one operation all boot** — the `DOWN` at t=1.220 — and nothing posts it.
But it parked *before* the console work, and the whole console open, the
card configuration and the banner all completed with it parked, so it is
probably not on this critical path.

### Instrument note

`FOCUS_TRIP` fires correctly on the 540's serial console (it is wired to the
ASI's `on_char`), but **its stack walk is useless there**: the byte reaches
the card by IOP DMA while the CPU sits at the dispatcher `STOP`, so the walk
reports the dispatcher every time. It answers "what was on screen", not
"who printed it". Same attribution trap as `FOCUS_IOALL`'s `p=`.
