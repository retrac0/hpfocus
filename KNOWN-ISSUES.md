# Known issues in the alpha

What the first alpha does wrong, or does not do, as far as anyone knows.
Each entry says what you would see, when, and what to do about it. It
describes this release: problems the alpha's review found and fixed are not
listed here (the commit log has them), and neither are things that only
older builds did, except where they leave something behind on your disc.
Add to this file as you find more.

[INSTALL.md](INSTALL.md) covers the normal way to run everything.
`tools/smoketest.sh` tells you in about a minute whether your build boots
HP-UX at all. What the release was tested with, and what was never tested,
is in [docs/VALIDATION.md](docs/VALIDATION.md#8-the-hp-ux-release-tests).

## HP-UX 5.30 on the emulated 9000/540

### Stopping, and the disc

- **Stop HP-UX before you stop the emulator.** At a root shell type `cd /`
  and then `/etc/stopsys`. HP-UX writes everything out and prints `The
  system is stopped.  You may turn the power off now.`, followed by a
  screen of halt report that begins `System halted due to system error.
  00000010`. That report is the stop, not a crash. (Whether a real 540
  printed it too is not known.) Then close the window, or press ^C.
  `/etc/shutdown` is on the disc and has not been tried; its `-h` option
  ends in `/etc/stopsys`.
- **Stopping the emulator without that is a power cut to HP-UX**, however
  you do it: closing the window, ^C, `kill`.
  - *What you keep.* Every disc block HP-UX writes reaches the image file
    as it is written, so even a crash or `kill -9` keeps those. What a
    power cut loses is what HP-UX still held in its buffer cache: type
    `sync` first if you cannot run `/etc/stopsys`. A `kill -9` in the
    middle of a large write loses the file being written (it is left as a
    nameless empty file) and nothing else.
  - *What it costs.* HP-UX swaps into free blocks of its file system and
    marks them in use. After a power cut they stay marked: 0.3 to 0.9 MB
    each time (488 blocks lost on the pristine disc, 3,282 after six such
    boots), out of 57 MB free. Nothing gives them back, because this disc
    never runs `fsck` at boot: its `/etc/bcheckrc` is empty.
    `/etc/fsclean -v` then says `not ok, run fsck`.
  - *Getting the space back.* Once, as root, `cp /etc/newconfig/bcheckrc
    /etc/bcheckrc`. From then on a boot after a power cut runs `fsck -P`
    (`FILE SYSTEM(S) NOT PROPERLY SHUTDOWN, BEGINNING FILE SYSTEM REPAIR`
    ... `FILE SYSTEM IS FIXED`), which returns the blocks and makes that
    boot about 70 seconds longer; a boot after `/etc/stopsys` says `File
    system is OK, not running fsck` and takes no longer. If `fsck -P`
    meets something it cannot fix, that script leaves you in a shell on
    the console instead of finishing the boot. `tools/runhpux.sh --reset`
    also gets the space back, by starting over from the pristine disc.
- **A full disc cannot be emptied from inside.** Because HP-UX swaps into
  the file system's free space, a disc that fills up takes the swap space
  with it. The console prints `Less than N blocks left on disc ... System
  deadlock possible`, and then every command that is not built into the
  shell, `rm` included, answers `cannot fork: no swap space`. Nothing
  halts. Stop the emulator, start it again, and delete the files then,
  before anything else runs: measured, the reboot came up multiuser with
  2,154 blocks free and `rm` returned the disc to 121,314.
- **If a disc goes bad**, `tools/runhpux.sh --reset` returns to the
  pristine one. It loses your changes.

### Memory and speed

- **At least 2 MB of memory.** `tools/runhpux.sh` fits 2 MB, and takes
  `--ram 3` and `--ram 4`. Fitted by hand with a single 1 MB board, the
  kernel stops early in its boot.
- **Large programs need `--ram 3` or `--ram 4`.** On the default 2 MB, a
  C program that fills a 700 KB array (`char big[700000]; memset(big, 2,
  700000)`) had printed nothing after 25 emulated minutes of swapping.
  The same program finishes on 3 and 4 MB, and a 300 KB array finishes on
  2 MB. ^C stops it, and other terminals go on answering meanwhile.
  Whether a real 2 MB 540 thrashed like this is not known.
- **Heavy load on 2 MB is slow.** Four logins with three background jobs
  each (`find / | sort`, `ls -lR /usr`, `od`, `cat /bin/* | sum`: 44
  processes) need most of 50 emulated minutes on 2 MB, though the logins
  go on answering. Fit 3 or 4 MB for that kind of work. Nearly all of the
  release's testing was done at 2 MB.
- **`malloc(900000)` returns NULL** on 2 MB and on 4 MB alike, although a
  700 KB static array works on 3 and 4 MB. Whether that is HP-UX 5.30's
  own limit or an emulator fault has not been looked into.

### The date and the clock

- `tools/runhpux.sh` sets the clock from your computer at every power-up.
  A date you set inside HP-UX with `date` lasts only until the next boot.
  `FOCUS_RTCTIME=none` leaves the clock as the battery file holds it
  instead (it stands still while the emulator is not running), and
  `FOCUS_RTCTIME=<ISO date>` sets a fixed one.
- The disc's time zone is `PST8PDT` with 1980s daylight-saving rules, so
  today's dates show Pacific *standard* time all year (in summer, an hour
  behind real Pacific time). Set `TZ` in HP-UX for your own zone.
- HP-UX 5.30 accepts a boot-time clock only from 1970-01-01 to
  2038-01-19 03:14:07 GMT. Outside that range it boots at time 0 and
  `date` says 31 Dec 1969. The emulator warns when you seed such a date.
- HP-UX's clock runs slightly fast: 17 seconds gained in six emulated
  hours of an idle multiuser machine (one measurement). Since the clock
  is set again at every boot, it only shows in long sessions.
- Run without the scripts (`mame/focus hp9040 ...` by hand), the clock is
  not set: HP-UX says `CLOCK AND DATE NOT SET` and dates files in 2036.
  Set `FOCUS_RTCTIME=now FOCUS_RTCUTC=1` and keep a battery file
  (`FOCUS_SCMNV=<file>`). On a brand-new battery file the first boot still
  says `CLOCK AND DATE NOT SET`, because HP-UX initialises the battery
  memory on that boot; the second boot has the date. The scripts avoid
  this by copying a battery HP-UX has already initialised.

### The system on the disc

- **Root has no password, and the terminal ports have no access control.**
  The ports (4001 to 4006, and 4000 with `--console tcp`) listen on
  127.0.0.1 only, but anyone with a login on your computer can connect to
  them and log in as root on the guest. Set a root password (`passwd`) if
  that matters to you; it is kept on your machine's disc.
- **The disc is a real site's 1991 system.** The HP-UX 5.30 disc is a unit
  save of a working installation, including that site's user accounts,
  home directories, root's shell history and mailboxes. Please do not
  publish what you find there.
- **Its nightly jobs still run.** `cron` is started at boot. If a session
  runs through 04:00 (HP-UX's time), the site's accounting prints `SYSTEM
  ACCOUNTING STARTED`, `ACCT ERRORS : see /usr/adm/acct/nite/active` and
  `COMPLETED` on the console and mails root, and `/usr/adm/pacct` grows
  with every process run. None of it is an emulator fault.
- **`ps` shows `?` for the terminal ports.** Sessions on tty01 to tty06
  are listed with `?` in the TTY column; the console's say `console`.
  `ps` keeps its own table of device numbers in `/etc/ps_data`, made in
  1991, and the multiuser set-up points the tty nodes at the emulated
  multiplexer. As root, once: `rm /etc/ps_data`. `ps` rebuilds it and
  names the ports.
- **Single-user mode gives `sh` once the machine has been multiuser.** On
  a machine never booted multiuser the single-user shell is root's csh,
  prompt `gator#`. The first multiuser boot changes root's shell to sh
  (`tools/hpuxconfig.py`), and from then on single-user mode gives sh and
  the prompt `unrcad#`, until `--reset`.
- **Signals are System V's.** A handler installed with `signal()` is put
  back to the default when it is called, so a second signal kills the
  program unless the handler installs itself again. That is HP-UX 5.30,
  not the emulator.
- **No networking.** The kernel on this disc contains TCP/IP and a LAN
  driver, but no LAN card is emulated, and the disc does not start any
  networking.

### The I/O timing knob

- `FOCUS_IOPPOLL` is a debugging knob; the default, 400 µs, is the only
  timing everything was measured at. HP-UX booted and ran a console
  session at every period tried from 220 to 1000 µs, and the release
  matrix covers 300 to 700. BASIC on the 520 boots from 50 µs up. (Earlier
  builds stalled HP-UX at 450, 475 and 600 and the 520 below 200; both
  edges are gone.) If you have set the knob and a machine stops early in
  boot, unset it.

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
- **Keys typed fast into the window can be dropped.** The window sees a
  key only if it is still down when a video frame ends, about every 17 ms.
  On an idle computer that loses nothing at ordinary typing speed (keys
  held 30 ms or longer all arrived; at 15 ms about a third were lost). On
  a computer too busy to run the emulator at full speed the frames are
  longer and ordinary typing loses keys: `ls -l / | tail -4` arrived as
  `ls -l  | tl -`. The TCP and pty terminals never lose input; use
  `tools/hpuxterm.py`, or `--console tcp`, when the window does.
- **On a busy computer, logins can seem to hang.** With the emulator
  running well below real time, a port can print its banner and `You have
  mail.` and then nothing for half a minute before the `TERM =` prompt.
  It is slow, not stuck.
- **Long output on a TCP or pty console lags.** With `--console tcp` or
  `--console pty` the console line runs at 9600 baud, but the console card
  takes everything HP-UX writes at once, so a long listing queues up ahead
  of the line: `od -c /bin/csh` queued 188 KB in 30 s, and ^C then took
  45 s to show. Nothing is lost. The window and `--headless` consoles are
  not affected, and the six terminal ports are paced correctly: use one of
  them for long output.
- **A reader who falls far behind is disconnected.** The relay drops a
  connection that is 256 KB behind what its port has printed (a suspended
  terminal under a noisy job). The session carries on; connect again and
  you see the last few kilobytes.
- **A raw MAME socket takes one connection, and can stop the emulator.**
  If you attach a port yourself with `-bitb socket.HOST:PORT`, MAME
  accepts a single connection per run: once the client leaves, the port
  cannot be reached again until the next run. And MAME's socket blocks: a
  client that stops reading (a suspended `nc`) stops the whole emulator
  once the socket's buffer is full, and a single ^C then does nothing.
  `tools/runhpux.sh`'s own ports go through its relay and have neither
  problem. If you attach ports by hand, use a pty.
- **`FOCUS_TYPE` scripts.** A `;` ends one script and starts the next:
  write `\x3b` for a semicolon the machine should see. Two scripts whose
  typing overlaps on one terminal interleave their characters, so leave
  each line time to be typed (INSTALL.md section 7).

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
  `tools/setup.sh` keeps an existing image (it may hold your files), so
  an upgraded tree keeps the broken one: `tools/setup.sh` and
  `tools/runbasic-hd.sh` say so when they meet one. Copy off anything you
  stored (`tools/sdfread.py --extract DIR software/hd/basic.img`), delete
  the image and run `tools/setup.sh` again.
- **One run per machine.** A second run on the same machine is refused.
  Use `--scratch` or `--state DIR` for another one. A disc booted with
  `--disc` is claimed the same way. The lock is a file holding the run's
  process number, `var/hpux530/run.pid`; if a run was killed outright and
  a later, unrelated process happens to have that number, every run is
  refused until you delete the file.
- **`<image>.unsaved`.** When a disc cannot be written back to its own
  file (a read-only file, a Teledisk `.td0`, or a file another tool changed
  during the run), the session is saved beside it as `<image>.unsaved`, and
  the run says so. `tools/runhpux.sh --status` warns when one exists.
- **The 520's fixed disc is saved only at a clean exit.** The 540's system
  disc is written through as HP-UX writes it. The 520's internal fixed
  disc (BASIC) is still written back only when MAME exits cleanly, so a
  crash or a second ^C loses that session's changes.
- **The 520's flexible discs are never saved.** What BASIC writes to a
  flexible disc (`-flop1` ...) stays in memory: the image file is not
  changed, and a soft reset puts the original disc back in the drive.
  (This is read from the driver's code; nobody has stored a file on one
  and looked.) Keep work on the fixed disc (`tools/runbasic-hd.sh`).
- **No save states.** MAME's save states do not work on these machines
  (F6 and F7 with MAME's keys on, `-state`, `-autosave`, `-rewind`): the
  driver registers only the memory, so a loaded state does not restore
  the processor, the I/O processor or the cards. What persists is the
  disc and the battery.
- **MAME's debugger has not been tried.** Nobody ran `-debug` on either
  machine for this release. From the code: while the debugger holds the
  machine at a breakpoint, a ^C is not seen until you let it run, and a
  second ^C ends the emulator at once without saving the battery or the
  520's fixed disc. A machine that is running or paused stops cleanly.

## BASIC 2.0 and the 520

- **MAME's red warning, once per windowed run.** The 520 (`hp9020`) is
  still flagged as not working in MAME, so a windowed 520 run first shows
  MAME's red `THIS SYSTEM DOESN'T WORK` screen: tap any key to start the
  machine. The 540 is flagged working and starts at once.
- **A key held as the 520 powers on stops its boot.** The machine starts
  the moment MAME's warning is dismissed. If the key you dismissed it with
  is a character key and is still down then, it reaches the 520 during its
  power-up, and the boot never gets as far as the display: the window
  stays black. Under a virtual display, keys held 100 ms or less were
  safe and keys held 150 ms or more stopped the boot every time (nine
  runs); Shift is safe. To recover, press Scroll Lock and then F3 (MAME's
  soft reset): BASIC comes up. It is the moment that matters, not the
  window: a key typed by `FOCUS_TYPE` between 0.05 and 0.7 s after
  power-on does the same to a run without one, and a key typed at 1 s
  does not. Whether a real 520 did this is not known.
- **BASIC wants capitals, and plain typing is lower case.** Typed without
  Shift, letters reach the 520 in lower case, and BASIC does not accept
  its keywords that way: `disp 7` answers `IMPROPER EXPRESSION`. Press
  Caps Lock once, or hold Shift. MAME saves the Caps Lock setting with its
  configuration, so it stays on for later runs. On the 520's keyboard the
  shifted letter is the lower-case one, and with Caps Lock on the window
  behaves the same way.
- **Return stores, it does not execute.** In the 520's window, Return is
  the keyboard's ENTER key, which stores a numbered program line; typed
  at a command (`RUN`, `LIST`, `DISP 7`) it answers `IMPROPER LINE
  NUMBER`, and the line stays on the input line: Esc clears it. The
  EXECUTE key is Ctrl-J, or Insert (MAME calls it Linefeed). A typed
  program needs its `END` line, or RUN stops with `ERROR 5 IN 10:
  Improper context terminator`. In `FOCUS_TYPE` scripts, `\r` is ENTER
  and `\n` is EXECUTE.
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
- **With the thermal printer fitted, the boot reports a self-test error.**
  `FOCUS_PRINTER=1` fits the 520's internal printer. BASIC's banner then
  includes `Self test error 1: IO address   6,   0 STATUS: 00000008`, and
  the printer prints correctly all the same (`PRINTER IS 6`, `PRINT`).
  Without the knob there is no printer and no message.
- **The clock.** BASIC reports `CLOCK AND DATE NOT SET`: the BASIC scripts
  keep no battery and seed no clock. For a date, run with
  `FOCUS_KBDNV=<file> FOCUS_RTCTIME=now`; from the second run on that
  battery, BASIC has the date (local time).
- **Soft reset.** MAME's soft reset (F3) brings BASIC back as a fresh
  power-on does, from the fixed disc and from the floppies. The 9020
  diagnostics were not tried after one.

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

- **Expect more instruction-level defects.** Of the 200 instruction
  mnemonics HP-UX executed in a ten-minute multiuser session with the C
  compiler, 81 have no unit test. A one-instruction fuzzer run during the
  review found seven arithmetic faults in minutes that the unit tests had
  not reached; those are fixed, and the fuzzer is not yet part of the
  tests.
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
  produced.
- **Privileged instructions in user mode** are all refused, but not all
  with the trap the ERS names: about 25 of them answer Unimplemented
  Instruction (cause 020) instead of Privileged Mode Violation (cause 026,
  parameter 1), and `DISP` and `IXIT` pass parameter 0.
- **String moves are not interrupted.** `MVB`, `FILB`, `CMPB`, `SCU`,
  `SCW`, `TRAN` and `PMVA` run to completion inside one emulated
  instruction: no interrupt is taken during a long move, and it costs one
  instruction's worth of emulated time however long it is. (A move that
  meets an absent page does stop there and resume.)
- **Rarely used instructions:** `STOP,B` does not restore MASK and STATUS
  when it wakes, and `STOP` without `,B` behaves like `STOP,B`. `CPTR,B`
  forms the pointer a full evaluation would end on without setting
  Referenced bits or reporting absence. Code-segment use counts wrap at 0
  and 4095 instead of raising System Error. The interrupt mask and request
  registers (`WIMK`, `RIMK`, `RIRQ`) do not follow the I/O ERS exactly.
  `CTCB` and `GMPY`, two instructions known only from HP's assembler, are
  not implemented.

## Building and platforms

- **Only Arch Linux has been tried.** The package lists for Debian/Ubuntu
  and Fedora in INSTALL.md were written from those distributions' package
  names and have not been run. macOS has not been tried at all: the
  scripts are written for it, but nothing has been built or run on a Mac.
  Windows is not supported.
- **The window was tested under a virtual display only.** Everything
  INSTALL.md says about the window was driven with synthetic key events
  under Xvfb: logging in and typing, Scroll Lock, Tab, Esc, F3, F5, the
  close button, and on the 520 the red warning, Caps Lock, Return, Insert
  and Ctrl-J. Nobody has yet sat at it on a real desktop, and a Wayland
  session has not been tried.
- **The first build is long.** About 1,190 files. Measured from a fresh
  clone: 1 hour 9 minutes at `-j4` on a busy 8-core machine (another such
  build took two hours), and 2.7 GB on disc when done. Expect 20 to 40
  minutes on an idle 8-core machine.
- **MAME is pinned.** The build is against upstream MAME `a3ee360d`. On
  another MAME revision, `tools/mame/focus-mame.patch` may not apply.
  Making the two changes by hand is described in `tools/link_into_mame.sh`.
- **Python 3.8 or later.** That floor was checked by reading the code, not
  by running it on 3.8.
