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
| −1 | generic re-raise/abort argument, 22 sites | escmap |

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

## 6. The console-open machinery (current frontier)

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
