# Known issues in the alpha

This is what the first alpha does wrong, or does not do, as far as anyone
knows. Most entries come from the alpha's review: eleven reviewers, 111
findings, and the fixes for them in nine lanes. Each entry says what you
would notice and what to do about it. Where a fix exists only in some
builds, the entry says so. Add to this file as you find more.

[INSTALL.md](INSTALL.md) covers the normal way to run everything.
`tools/smoketest.sh` tells you whether your build boots HP-UX at all.

## HP-UX 5.30 on the emulated 9000/540

- **Many logins at once in 2 MB can crash HP-UX.** With seven users
  logging in at the same moment, HP-UX sometimes halts with `System halted
  due to system error. TRAP:00000012`, or shells die with `Memory fault`.
  Whether it happens depends on timing: moving the typing by a second
  changes the outcome, and the pre-alpha build did it too. One cause was a
  disc-write defect, now fixed. A rarer swap-related variant remains under
  investigation. Logging in one terminal at a time has not shown it.
- **Some I/O timings stall the boot.** The emulator's default timing works.
  The non-default `FOCUS_IOPPOLL` values 450, 475 and 600 have been seen to
  stop `/etc/rc` or the console. If you set that knob, unset it.
- **No file-system check, and stopping is a power cut.** HP-UX never runs
  `fsck` at boot on this disc (`/etc/bcheckrc` is empty). Every way of
  stopping the emulator, including a clean one, looks to HP-UX like a
  power failure. Type `sync` before you stop. HP-UX's `/etc/shutdown` is
  on the disc but has not been tried.
- **At least 2 MB of memory.** `tools/runhpux.sh` fits 2 MB, and `--ram 3`
  and `--ram 4` have also been measured to work. With 1 MB the kernel stops
  after `CLOCK AND DATE NOT SET`.
- **The date and the clock.**
  - `tools/runhpux.sh` sets the clock from the host at every power-up. A
    date you set inside HP-UX with `date` lasts only until the next boot,
    unless you run with `FOCUS_RTCTIME=none`.
  - HP-UX 5.30 accepts boot-time clocks only from 1970 to 2038-01-19
    03:14:07 GMT. Outside that range it boots silently at 1 Jan 1970.
  - The disc's time zone is `PST8PDT` with 1980s daylight-saving rules, so
    current dates show Pacific standard time all year. Set `TZ` in HP-UX.
  - Without the alpha's clock fixes (an older build, or a run that bypasses
    the scripts), every boot says `CLOCK AND DATE NOT SET` and dates
    everything February 2036.
  - On a brand-new battery file, the first boot always says `CLOCK AND DATE
    NOT SET`. The scripts avoid this by starting each machine from a primed
    battery.
- **No networking.** The kernel on this disc contains TCP/IP and a LAN
  driver, but no LAN card is emulated and the disc does not start any
  networking.
- **No loader ROM.** No dump of the SCM loader ROM (`09020-80000`/`80001`)
  exists, and the emulator performs the loader's job itself. The 540 starts
  without any MAME warning. The 520 (BASIC, the diagnostics, HP-UX 5.01)
  first shows MAME's red `THIS SYSTEM DOESN'T WORK` screen, because its
  driver is flagged unfinished: press any key to continue.
- **The disc is a real site's 1991 system.** The HP-UX 5.30 disc is a
  unit save of a working installation. It includes that site's user
  accounts, home directories and mailboxes. Please do not publish what
  you find there.

## Terminals

- **Not HP terminals.** The emulated terminals (the MAME window, and
  whatever you connect to a port) do not understand HP 262x escape
  sequences. After `TERM =`, `tset`'s tab-setting sequences show as stray
  characters such as `1 1 1`. They are harmless. Programs that draw on the
  screen, such as `vi`, need a `TERM` your terminal matches.
- **Early typing is lost.** Characters typed within about a second of
  `login:` appearing are discarded: getty flushes its input just after
  printing the prompt, as it did on the real machine.
- **No Break or flow control.** A Break on a multiplexer port is not
  detected. ^S and ^Q are taken out of the input but do not pause output.
- **A raw MAME socket takes one connection.** If you attach a port
  yourself with `-bitb socket.HOST:PORT`, MAME accepts a single
  connection per run. After the client leaves, the port cannot be reached
  again. `tools/runhpux.sh` avoids this by putting its relay,
  `tools/hpuxterm.py`, in between. If you attach ports by hand, use a pty
  instead.
- **Blank screen on connecting.** A terminal that connects after a port's
  `login:` was printed sees the last few kilobytes of output, through the
  relay, or a blank screen. Press Return for a fresh prompt.
- **The console on TCP or a pty needs a newer build.** `--console tcp|pty`
  needs a build whose console card has an rs232 port. The script checks.

## Discs and saved state

- **Discs from earlier builds may be damaged.** Builds before the alpha's
  storage fixes could corrupt any file over 1 KB, and the i-node and
  free-map blocks that `/etc/rc` rewrites on every boot. Start from a
  freshly assembled disc: `tools/runhpux.sh --reset`.
- **Discs assembled by an older `mkhpuxdisc.py`** have the wrong
  `s_maxblk` (130,814). Once such a disc fills, HP-UX can overwrite the
  file system's own extent map. `tools/hpuxconfig.py` repairs this, and
  `tools/runhpux.sh` runs it before every boot. A disc whose map has
  already been overwritten cannot be repaired and must be reassembled.
- **One run per machine.** A second run on the same machine is refused.
  Use `--scratch` or `--state DIR` for another one.
- **`<image>.unsaved`.** When a disc cannot be written back to its own
  file, the session is saved beside it as `<image>.unsaved`, and the run
  says so. This happens with a read-only file, a Teledisk `.td0`, or a file
  another tool changed during the run.
- **The 520's fixed disc is saved only at exit.** The 540's system disc
  is written through as HP-UX writes it. The 520's internal fixed disc
  (BASIC) is still saved only when MAME exits cleanly, so a crash loses
  that session.
- **Old BASIC disc images.** `software/hd/basic.img` and `diag.img` made
  before the alpha's `sdfwrite.py` fix report the wrong AVAILABLE SPACE,
  and CREATE/STORE fail with ERROR 455. Delete them and run
  `tools/setup.sh` (or `tools/mkdiagdisc.sh`) to rebuild them.

## CPU accuracy

These affect programs that depend on them. They do not affect booting.

- **Floating point:** rounding modes and the inexact flag are not modelled,
  and the floating-point traps are never raised. Divide by zero, overflow
  and NaN operands give the host's IEEE result, and the program carries
  on. A C program's `1.0/0.0` does not get SIGFPE.
- **Overflow flag:** `STATUS.O` is assigned rather than sticky.
  `ADDI`, `SUBI`, `MPYI`, `DIVI`, `ADAX`, `ADXA`, `ADXI`, `SBXI`, `INCM`,
  `DECM`, `INCF` and `DECF` never set Overflow.
- **Condition codes:** the character-class condition code (CCB) is never
  produced, and `DZRO` does not set the condition code.
- **Rarely used instructions:** `STOP,B` does not restore MASK/STATUS when
  it wakes. An unprivileged `EXIT` does not take every privileged-mode trap
  the ERS lists. The interrupt mask and request registers do not follow
  the I/O ERS exactly. Neither operating system is known to depend on any
  of these.

## BASIC 2.0 on the 520

- `DISP "A"&"B"` halts with `TRAP:00000001`, and `DISP 4.5+1` shows
  `5.49999999907`.
- In the window, Enter is the 520's STORE key and Ctrl-J its EXECUTE key.
  A command typed with Enter (`RUN`, `DISP 2+3`) answers `IMPROPER LINE
  NUMBER`; Esc clears the line. See INSTALL.md section 8.
- The 520 reports `CLOCK AND DATE NOT SET` unless it has a battery file
  (`FOCUS_KBDNV`) and a seeded clock (`FOCUS_RTCTIME=now`).

## HP-UX 5.01 on the 520

- It reaches its copyright banner and no further. The release tape lost
  the whole userland (`SCOMMANDS`), and the kernel loops on escapes
  without starting `init` (`docs/HPUX.md` section 11).

## Building and platforms

- **macOS has not been tried.** The scripts are written for it, but
  nothing has been built or run on a Mac. Windows is not supported.
- **The first build is long.** About 1,190 files: 20–40 minutes on an idle
  8-core machine.
- **MAME is pinned.** The build is against upstream MAME `a3ee360d`. On
  another MAME revision, `tools/mame/focus-mame.patch` may not apply.
  Making the two changes by hand is described in `tools/link_into_mame.sh`.
- **Python 3.8 or later.** That floor was checked by reading the code, not
  by running it on 3.8.
