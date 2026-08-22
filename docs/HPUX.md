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
- **NEXT.** The per-LCN channel state the arm reads — the table based at
  `DB+236` with the state word at each entry's `+0` — **is never
  populated**. Find its writer with `FOCUS_WWATCH` over the table (read
  `DB+236` and `DB+12` live first to size it), then find that writer's
  guard. Either the kernel opens the channel through a path we have not
  triggered, or it expects the card to have said something else before
  SWI.
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
| where time goes | `FOCUS_PCPROF` |
| task blocking | `FOCUS_SEMLOG` (timestamped) |
