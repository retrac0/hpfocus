# Known issues

What the emulator does wrong, or does not do, as far as anyone knows.
Each entry says what you would see, when, and what to do about it.
Problems that were found and fixed are not listed here (the commit log
has them), and neither are things that only older builds did, except
where they leave something behind on your disc. Add to this file as you
find more.

Many of these entries were measured on alpha-0.1, the standalone FOCUS
core with its out-of-tree MAME driver, before the emulator became the
`hp9k5xx` MAME driver; every alpha-0.1 fix that applies is in the MAME
driver too. An entry that was not measured again on the MAME driver says
so.

[INSTALL.md](INSTALL.md) covers the normal way to run everything.
`tools/smoketest.sh` tells you in a couple of minutes whether your build
boots HP-UX at all, and `tests/run_tests.py` is the whole suite. How it is
tested is in [docs/VALIDATION.md](docs/VALIDATION.md).

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

*(Measured on alpha-0.1.)*

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

- MAME sets the clock from your computer at every power-up, and HP-UX
  keeps GMT in the chip. A date you set inside HP-UX with `date` lasts
  only until the next boot.
- The disc's time zone is `PST8PDT` with 1980s daylight-saving rules, so
  today's dates show Pacific *standard* time all year (in summer, an hour
  behind real Pacific time). Set `TZ` in HP-UX for your own zone.
- HP-UX 5.30 accepts a boot-time clock only from 1970-01-01 to
  2038-01-19 03:14:07 GMT. Outside that range it boots at time 0 and
  `date` says 31 Dec 1969.
- On a brand-new battery the first boot says `CLOCK AND DATE NOT SET`,
  because HP-UX initialises the battery memory on that boot; the second
  boot has the date. `tools/runhpux.sh` avoids this by copying a battery
  HP-UX has already initialised (`var/pristine/nvram`). Run by hand
  (`mame/focus hp9k540 ...`), MAME keeps the battery in
  `mame/nvram/hp9k540/scm_rtc`, so only the very first run says so.

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
- **No Break.** A Break on a multiplexer port is not detected.
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
- **The 520's flexible discs are never saved.** What BASIC writes to a
  flexible disc (`-flop1` ...) stays in memory: the image file is not
  changed, and a soft reset puts the original disc back in the drive.
  (This is read from the driver's code; nobody has stored a file on one
  and looked.) Keep work on the fixed disc (`tools/runbasic-hd.sh`).
- **No save states.** The driver does not claim save-state support and
  the state of several devices (the cards' queues among them) is not
  registered, so a loaded state (F6/F7, `-state`, `-autosave`, `-rewind`)
  does not restore the machine. What persists is the disc and the battery.
- **The debugger and ^C.** MAME's debugger works, with the FOCUS
  disassembler. While it holds the machine at a breakpoint the stop file
  `tools/runhpux.sh`'s ^C makes is not seen until you let the machine
  run; a second ^C ends MAME at once, without writing the battery.

## BASIC 2.0 and the 520

- **MAME's red warning, once per windowed run.** The 520 (`hp9k520`) is
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
  window: a key typed by a script between 0.05 and 0.7 s after power-on
  does the same to a run without one, and a key typed at 1 s does not
  (measured on alpha-0.1). Whether a real 520 did this is not known.
- **BASIC wants capitals, and plain typing is lower case.** Typed without
  Shift, letters reach the 520 in lower case, and BASIC does not accept
  its keywords that way: `disp 7` answers `IMPROPER EXPRESSION`. Press
  Caps Lock once, or hold Shift. MAME saves the Caps Lock setting with its
  configuration, so it stays on for later runs. On the 520's keyboard the
  shifted letter is the lower-case one, and with Caps Lock on the window
  behaves the same way.
- **Return executes; the keypad's Enter stores.** In the 520's window,
  Return is EXECUTE, which runs a command (`RUN`, `LIST`, `DISP 7`); a
  numbered program line ended with it answers `IMPROPER COMMAND`, and the
  line stays on the input line: Esc (CLR LINE) clears it. The keypad's
  Enter is STORE, which stores the line. A typed program needs its `END`
  line, or RUN stops with `ERROR 5 IN 10: Improper context terminator`.
  In the test harness's typing, `\n` is EXECUTE and the `STORE` key
  stores.
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
- **The thermal printer is an option.** Fit it with the machine
  configuration's "Internal printer" switch (MAME's menu, or the cfg
  file); it prints to a page image. With it fitted alpha-0.1's BASIC banner
  reported `Self test error 1: IO address 6, 0`, and printed correctly all
  the same; not checked again on the MAME driver.
- **The clock.** On a new battery BASIC reports `CLOCK AND DATE NOT SET`
  once: MAME sets the 520's clock from the host at power-up, and BASIC
  initialises the battery memory on the first boot. MAME keeps the 520's
  battery in `mame/nvram/hp9k520`, so later runs have the date (local
  time).
- **Soft reset.** MAME's soft reset (F3) brings BASIC back as a fresh
  power-on does, from the fixed disc and from the floppies, and brings a
  multiuser 540 back to a working login. The 9020 diagnostics were not
  tried after one.

## HP-UX 5.01 on the 520

- **It reaches its copyright banner and no further.** The release tape
  lost the whole userland (`SCOMMANDS`), and the kernel loops on escapes
  without starting `init` ([docs/HPUX.md](docs/HPUX.md) section 11). The
  banner also says `CLOCK AND DATE NOT SET` on the first run of its
  machine (`var/hpux501`), whose battery HP-UX initialises then.

## CPU accuracy

These affect programs that depend on them. Neither operating system's own
code is known to.

- **Expect more instruction-level defects.** HP-UX, BASIC and HP's
  diagnostics run, and some 1,700 instruction-set edge cases from the ERS
  pass under HP-UX (`hpux-cc-isa`), but instruction coverage is not
  complete.
- **Interrupted string moves resume the stand-in way.** An `MVB` or
  `TRAN` that faults part way (an absent page) leaves its stack operands
  rewritten -- the count reduced and the pointers advanced -- and X the
  bytes moved after the fault, where the ERS keeps the stack as the
  program wrote it and resumes through `STATUS.P` (docs/FOCUS.md). HP-UX
  and BASIC never tell the two apart. `SLD`, `SAS` and `SCON`, which end
  in such a move, start again from their descriptors after a fault in it;
  neither system pages the data they move.
- **Condition codes:** the character-class condition code (CCB) is never
  produced.
- **String moves are not interrupted.** `MVB`, `FILB`, `CMPB`, `SCU`,
  `SCW`, `TRAN` and `PMVA` run to completion inside one emulated
  instruction: no interrupt is taken during a long move, and it costs one
  instruction's worth of emulated time however long it is. (A move that
  meets an absent page does stop there and resume.)
- **Rarely used instructions:** `STOP,B` does not restore MASK and STATUS
  when it wakes. `CPTR,B` forms the pointer a full evaluation would end on
  without setting Referenced bits or reporting absence. Code-segment use
  counts wrap at 0 and 4095 instead of raising System Error. `CTCB` and
  `GMPY`, two instructions known only from HP's assembler, are not
  implemented.

## Building and platforms

- **Only Arch Linux has been tried.** The package lists for Debian/Ubuntu
  and Fedora in INSTALL.md were written from those distributions' package
  names and have not been run. macOS has not been tried at all: the
  scripts are written for it, but nothing has been built or run on a Mac.
  Windows is not supported.
- **Wayland and macOS have not been tried.** The window has been used
  under X11 on Arch Linux.
- **The first build is long.** About 1,190 files: expect 20 to 40 minutes
  on an idle 8-core machine, and much longer on a busy or smaller one.
- **MAME is a fork, pinned.** The emulator is the `hp9k5xx` branch of the
  project's MAME fork, and the `mame` submodule pins a commit on it. That
  fork is not published yet, so a fresh clone has nowhere to fetch the
  pinned commit from: `tools/setup.sh --mame-source` needs a clone that
  has it.
- **Python 3.8 or later.** That floor was checked by reading the code, not
  by running it on 3.8.
