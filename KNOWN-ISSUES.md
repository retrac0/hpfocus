# Known issues in the alpha

What the first alpha does wrong, or does not do, as far as anyone knows.
Each entry says what you would see, when, and what to do about it. It
describes this release: problems the alpha's review found and fixed are not
listed here (the commit log has them), and neither are things that only
older builds did, except where they leave something behind on your disc.
Add to this file as you find more.

[INSTALL.md](INSTALL.md) covers the normal way to run everything.
`tools/smoketest.sh` tells you in about a minute whether your build boots
HP-UX at all. What the release was tested with is in
[docs/VALIDATION.md](docs/VALIDATION.md#8-the-hp-ux-release-tests).

## HP-UX 5.30 on the emulated 9000/540

- **Stopping is a power cut, and nothing checks the file system.** HP-UX
  has no shutdown hook in the emulator, and this disc never runs `fsck`
  at boot (`/etc/bcheckrc` is empty). Every disc block HP-UX writes reaches
  the image file as it is written, so a crash or a kill loses none of
  those; what a stop loses is HP-UX's own buffer cache. **Type `sync` at a
  root shell before you stop.** HP-UX's `/etc/shutdown` is on the disc but
  has not been tried. If a disc goes bad, `tools/runhpux.sh --reset`
  returns to the pristine one (and loses your changes).
- **^C while the machine is paused does nothing until it resumes.** The
  first ^C stops a run cleanly at the next emulated millisecond. While the
  machine is paused (MAME's P key, or its menu open) no time passes, so
  nothing happens until you resume it. A second ^C kills MAME at once:
  the disc is safe as above, but run `sync` first if you can.
- **At least 2 MB of memory.** `tools/runhpux.sh` fits 2 MB, and takes
  `--ram 3` and `--ram 4`. Fitted by hand with a single 1 MB board, the
  kernel stops early in its boot.
- **The date and the clock.**
  - `tools/runhpux.sh` sets the clock from your computer at every
    power-up. A date you set inside HP-UX with `date` lasts only until the
    next boot. `FOCUS_RTCTIME=none` leaves the clock as the battery file
    holds it instead (it stands still while the emulator is not running),
    and `FOCUS_RTCTIME=<ISO date>` sets a fixed one.
  - The disc's time zone is `PST8PDT` with 1980s daylight-saving rules, so
    today's dates show Pacific *standard* time all year (in summer, an hour
    behind real Pacific time). Set `TZ` in HP-UX for your own zone.
  - HP-UX 5.30 accepts a boot-time clock only from 1970-01-01 to
    2038-01-19 03:14:07 GMT. Outside that range it boots at time 0 and
    `date` says 31 Dec 1969. The emulator warns when you seed such a date.
  - Run without the scripts (`mame/focus hp9040 ...` by hand), the clock is
    not set: HP-UX says `CLOCK AND DATE NOT SET` and dates files in 2036.
    Set `FOCUS_RTCTIME=now FOCUS_RTCUTC=1` and keep a battery file
    (`FOCUS_SCMNV=<file>`). On a brand-new battery file the first boot still
    says `CLOCK AND DATE NOT SET`, because HP-UX initialises the battery
    memory on that boot; the second boot has the date. The scripts avoid
    this by copying a battery HP-UX has already initialised.
- **Root has no password, and the terminal ports have no access control.**
  The ports listen on 127.0.0.1 only, but anyone with a login on your
  computer can connect to them and log in as root. Set a root password
  (`passwd`) if that matters to you; it is kept on your machine's disc.
- **No networking.** The kernel on this disc contains TCP/IP and a LAN
  driver, but no LAN card is emulated, and the disc does not start any
  networking.
- **The disc is a real site's 1991 system.** The HP-UX 5.30 disc is a unit
  save of a working installation, including that site's user accounts,
  home directories, root's shell history and mailboxes. Please do not
  publish what you find there.
- **The I/O timing knob.** `FOCUS_IOPPOLL` is a debugging knob; the
  default, 400 µs, is what the alpha is tested at. HP-UX boots and runs a
  session at every period tried from 220 to 1000 µs, and the release
  matrix covers 300 to 700. Below about 200 µs the 520 does not reach its
  banner, for a reason not yet known. If you have set the knob and a
  machine stops early in boot, unset it.

## Terminals

- **Not HP terminals.** The emulated terminals (the MAME window, and
  whatever you connect to a port) do not understand HP 262x escape
  sequences. After `TERM =`, `tset`'s tab-setting sequences show as stray
  characters such as `1 1 1` or `&a8C`. They are harmless. Programs that
  draw on the screen, such as `vi`, need a `TERM` your terminal matches.
- **Early typing is lost.** Characters typed within about a second of
  `login:` appearing are discarded: getty flushes its input just after
  printing the prompt, as on the real machine. Wait a moment, or press
  Return for a fresh prompt.
- **A late connection may see no prompt.** Through `tools/hpuxterm.py`
  (the relay `tools/runhpux.sh` starts) you see the last few kilobytes
  the port printed. On a pty or a raw socket you see nothing. Either
  way, press Return for a fresh `login:`.
- **No Break or flow control.** A Break on a multiplexer port is not
  detected. ^S and ^Q are taken out of the input but do not pause output.
  The console card does not model double text terminators or prompt
  sequences, which HP-UX does not configure.
- **A raw MAME socket takes one connection.** If you attach a port
  yourself with `-bitb socket.HOST:PORT`, MAME accepts a single
  connection per run, and once the client leaves, the port cannot be
  reached again until the next run. `tools/runhpux.sh`'s own ports
  (4001-4006, and 4000 with `--console tcp`) go through its relay and take
  any number of connections. If you attach ports by hand, use a pty.

## Discs and saved state

- **Discs written by earlier builds may already be damaged.** Builds before
  the alpha truncated every multi-kilobyte disc write: any file over 1 KB
  that HP-UX wrote, and the i-node and free-map blocks `/etc/rc` rewrites
  on every boot. A disc that was booted on such a build (for instance the
  old `software/cct/7914_hpux_volume.bin`) can hold damaged files whatever
  you run it on now. Start from a fresh disc: `tools/runhpux.sh --reset`.
- **Discs assembled by an older `mkhpuxdisc.py`** have the wrong
  `s_maxblk` (130,814 instead of 129,023). Once such a disc fills, HP-UX
  can overwrite the file system's own extent map. `tools/runhpux.sh`
  repairs this before every boot (`tools/hpuxconfig.py`, or
  `tools/mkhpuxdisc.py --repair DISC` by hand); a disc whose map has
  already been overwritten cannot be repaired and must be reassembled.
- **BASIC fixed discs built by older tools.** A `software/hd/basic.img` or
  `diag.img` built before the alpha's `sdfwrite.py` fix reports the wrong
  AVAILABLE SPACE, and CREATE and STORE fail with `ERROR 455`.
  `tools/setup.sh` keeps an existing `basic.img` (it may hold your files),
  so an upgraded tree keeps the broken one: `tools/setup.sh` and
  `tools/runbasic-hd.sh` now say so when they meet one. Copy off anything
  you stored (`tools/sdfread.py --extract DIR software/hd/basic.img`),
  delete the image and run `tools/setup.sh` (or `tools/mkdiagdisc.sh`).
- **One run per machine.** A second run on the same machine is refused.
  Use `--scratch` or `--state DIR` for another one. A disc booted with
  `--disc` is claimed the same way.
- **`<image>.unsaved`.** When a disc cannot be written back to its own
  file (a read-only file, a Teledisk `.td0`, or a file another tool changed
  during the run), the session is saved beside it as `<image>.unsaved`, and
  the run says so. `tools/runhpux.sh --status` warns when one exists.
- **The 520's fixed disc is saved only at a clean exit.** The 540's system
  disc is written through as HP-UX writes it. The 520's internal fixed
  disc (BASIC) is still written back only when MAME exits cleanly, so a
  crash or a second ^C loses that session's changes.

## BASIC 2.0 and the 520

- **MAME's red warning, once per windowed run.** The 520 (`hp9020`) is
  still flagged as not working in MAME, so a windowed 520 run first shows
  MAME's red warning screen: press any key to start the machine. The 540
  is flagged working and starts at once.
- **Return stores, it does not execute.** In the 520's window, Return is
  the keyboard's ENTER key, which stores a numbered program line; typed
  at a command (`RUN`, `LIST`, `DISP 7`) it answers `IMPROPER LINE
  NUMBER`. The EXECUTE key is **Insert** (MAME calls it Linefeed) or
  Ctrl-J. A typed program needs its `END` line, or RUN stops with
  `ERROR 5 IN 10: Improper context terminator`. In `FOCUS_TYPE` scripts,
  `\r` is ENTER and `\n` is EXECUTE.
- **From the floppies, BASIC has no options.** `tools/runbasic.sh` boots
  the two system floppies as shipped: errors print as bare numbers
  (`ERROR 5 IN 10`), and statements that live in the options, `CREATE`
  among them, answer `IMPROPER COMMAND`. The fixed disc `tools/setup.sh`
  builds has the ERRORS_ENG, IO and MS options installed; use
  `tools/runbasic-hd.sh`.
- **Most of HP's demo programs do not run.** The programs on the BASIC
  demo discs (`software/demos`) load, and several stop early: some want
  partitions, some stop in BASIC's own subprogram linking.
  [docs/DIAGNOSTICS.md](docs/DIAGNOSTICS.md) has the state program by
  program as of August 2026. It was not measured again for the alpha,
  except that `QIX` no longer halts with `TRAP:00000021` after its three
  questions: it clears the screen and has drawn nothing after 150 emulated
  seconds. Programs you type do run; the test suite types and runs one.
- **The clock.** BASIC reports `CLOCK AND DATE NOT SET`: the BASIC scripts
  keep no battery and seed no clock. For a date, run with
  `FOCUS_KBDNV=<file> FOCUS_RTCTIME=now`; from the second run on that
  battery, BASIC has the date (local time).

## HP-UX 5.01 on the 520

- **It reaches its copyright banner and no further.** The release tape
  lost the whole userland (`SCOMMANDS`), and the kernel loops on escapes
  without starting `init` ([docs/HPUX.md](docs/HPUX.md) section 11). The
  banner also says `CLOCK AND DATE NOT SET`, because the script does not
  seed this machine's clock; `FOCUS_RTCTIME=now FOCUS_RTCUTC=1` removes the
  warning from the second run on.

## CPU accuracy

These affect programs that depend on them. Neither operating system's own
code is known to.

- **Floating point:** rounding modes and the inexact flag are not modelled,
  and the floating-point traps are never raised. Divide by zero, overflow
  and NaN operands give the host's IEEE result, and the program carries
  on: a C program's `1.0/0.0` does not get SIGFPE. (Integer divide by
  zero does.)
- **Overflow flag:** `STATUS.O` is assigned rather than sticky. `ADDI`,
  `SUBI`, `DIVI`, `ADAX`, `ADXA`, `ADXI`, `SBXI`, `INCM`, `DECM`, `INCF`
  and `DECF` never set Overflow, so their overflow neither traps nor
  branches `BOV`.
- **Condition codes:** the character-class condition code (CCB) is never
  produced, and `DZRO` does not set the condition code.
- **Rarely used instructions:** `STOP,B` does not restore MASK and STATUS
  when it wakes, and `STOP` without `,B` behaves like `STOP,B`. An
  unprivileged `EXIT` does not take every privileged-mode trap the ERS
  lists. The interrupt mask and request registers (`WIMK`, `RIMK`,
  `RIRQ`) do not follow the I/O ERS exactly. `CTCB` and `GMPY`, two
  instructions known only from HP's assembler, are not implemented.

## Building and platforms

- **macOS has not been tried.** The scripts are written for it, but
  nothing has been built or run on a Mac. Windows is not supported.
- **The first build is long.** About 1,190 files: 20-40 minutes on an idle
  8-core machine, two hours on a busy one at `-j4`.
- **MAME is pinned.** The build is against upstream MAME `a3ee360d`. On
  another MAME revision, `tools/mame/focus-mame.patch` may not apply.
  Making the two changes by hand is described in `tools/link_into_mame.sh`.
- **Python 3.8 or later.** That floor was checked by reading the code, not
  by running it on 3.8.
