# Installing and running hpfocus (alpha)

This takes you from `git clone` to a root shell on HP-UX 5.30, running on an
emulated HP 9000 Model 540. It is an **alpha**: read
[KNOWN-ISSUES.md](KNOWN-ISSUES.md) before you rely on anything.

- [1. What you need](#1-what-you-need)
- [2. Get it and build it](#2-get-it-and-build-it)
- [3. First boot](#3-first-boot)
- [4. More terminals](#4-more-terminals)
- [5. Stopping cleanly](#5-stopping-cleanly)
- [6. The machine's state: what persists, and resetting it](#6-the-machines-state-what-persists-and-resetting-it)
- [7. Without a window: headless and batch runs](#7-without-a-window-headless-and-batch-runs)
- [8. BASIC and HP-UX 5.01 on the 520](#8-basic-and-hp-ux-501-on-the-520)
- [9. Troubleshooting](#9-troubleshooting)
- [10. macOS](#10-macos)

## 1. What you need

- **Linux** on x86-64. Developed and tested on Arch Linux with GCC 16.
  Any distribution with GCC 11+ (or clang 13+) and SDL 2.0.14+ should do.
  macOS is untested: see [section 10](#10-macos).
- **Disc space:** about 3.5 GB free. A finished install measured 2.7 GB:
  a shallow MAME checkout of about 1.2 GB, its build tree, and the HP-UX
  machines in `var/` (about 400 MB).
- **Time:** a first build compiles MAME's core, about 1,190 files. From a
  fresh clone it took 1 hour 9 minutes at `-j4` on a busy 8-core machine
  (and 2 hours on another occasion). Expect 20–40 minutes on an idle one.
  Later builds take seconds.
- **Packages:** a C++20 compiler, make, git, Python 3.8 or later, gzip,
  pkg-config, and the development files of the libraries MAME's SDL build
  links against. On each distribution:

  **Debian / Ubuntu**

      sudo apt-get install git build-essential python3 pkg-config libsdl2-dev \
        libsdl2-ttf-dev libfontconfig-dev libx11-dev libxinerama-dev libxi-dev \
        libxext-dev libgl-dev libasound2-dev libpulse-dev qt6-base-dev

  **Fedora**

      sudo dnf install git gcc gcc-c++ make python3 pkgconf-pkg-config SDL2-devel \
        SDL2_ttf-devel fontconfig-devel libX11-devel libXinerama-devel libXi-devel \
        libXext-devel mesa-libGL-devel alsa-lib-devel pulseaudio-libs-devel qt6-qtbase-devel

  **Arch**

      sudo pacman -S --needed base-devel git python pkgconf sdl2 sdl2_ttf fontconfig \
        libx11 libxinerama libxi libxext libglvnd alsa-lib libpulse qt6-base

  PulseAudio and Qt 6 are optional. Without them, `tools/setup.sh` builds
  with `NO_USE_PULSEAUDIO=1` and `USE_QTDEBUG=0`. The emulator runs without
  sound anyway, and Qt only provides MAME's debugger window.
  `tools/setup.sh --check` checks everything and prints the install
  command for your distribution. Only the Arch list has been run; the
  Debian/Ubuntu and Fedora lists were written from those distributions'
  package names.

## 2. Get it and build it

    git clone <this repository> hpfocus
    cd hpfocus
    tools/setup.sh

`setup.sh` runs these steps:

1. It checks the prerequisites.
2. It fetches the MAME commit that the `mame` submodule pins. This is
   upstream `a3ee360d`, fetched shallow by SHA: about 210 MB to download.
3. It links this tree's sources into MAME and applies the small
   registration patch, `tools/mame/focus-mame.patch`
   (`tools/link_into_mame.sh`).
4. It builds `mame/focus`, logging to `var/setup/build.log`.
5. It prepares the media:
   - the HP-UX 5.30 disc, assembled from the two tape images in
     `software/cct`;
   - a fixed disc for BASIC, `software/hd/basic.img`: BASIC 2.0 with its
     English error texts, I/O and mass-storage options installed, and two
     of HP's utilities on it;
   - a fixed disc with HP's 9020 diagnostics environment,
     `software/hd/diag.img` (`tools/mkdiagdisc.sh`).

It prints a line as each step starts, and during the build a count of
files compiled once a minute. It ends with `Ready. Next:` and the
commands of section 3, and leaves `git status` clean.

Every step is idempotent. If anything stops it (the network, a missing
package, ^C), fix the cause and run `tools/setup.sh` again. It carries on
from where it stopped; run again on a finished install it takes a few
seconds and rebuilds nothing.

Useful options:

| option | what it does |
|---|---|
| `--check` | check the prerequisites only |
| `--no-build` | do everything except the build |
| `-j N` | build with N jobs. The default is one per CPU, reduced if memory is short: a MAME compile job can take 1.5 GB |
| `-- NOWERROR=1` | pass options to MAME's make. NOWERROR=1 helps when a newer compiler warns about MAME's code |
| `--mame-source DIR` | fetch MAME from a mirror or a local clone that has the pinned commit |

`git submodule update --init --depth 1 mame` should also work now that the
pin is an upstream commit, but it downloads MAME's current master first,
and that route has not been run for this release. If you fetch MAME that
way, or put any checkout of `a3ee360d` in `mame/`, run `tools/setup.sh`
afterwards: it uses the tree that is already there.

## 3. First boot

Check that HP-UX comes up on your machine. This takes about a minute and
touches nothing of yours:

    tools/smoketest.sh

It should end with `PASS`. Then boot HP-UX with a window:

    tools/runhpux.sh --multiuser

What happens:

- **The window is the system console.** It opens black, with a cursor,
  and stays that way for the first ten seconds or so while the kernel
  loads. There is no MAME warning screen to dismiss on this machine. Then
  the kernel prints its copyright banner and `/etc/rc` runs. Within a
  minute of power-on (at about 40 emulated seconds) `login:` appears on
  the console and on the six terminal ports.
- **Log in as `root`.** There is no password. Press Return at the two
  `TERM =` prompts: `/etc/profile` asks once and root's `.profile` asks
  again.
- **The prompt is `unrcad#`**, the name of the 1991 machine this disc
  came from. Try `who`, `ls -l /`, `ps -ef` and `df`.

Without `--multiuser`, `tools/runhpux.sh` boots to **single-user mode**.
There, root's shell runs on the console with no login: press Return at its
`TERM =` prompt. On a machine that has never been booted multiuser the
shell is csh and its prompt `gator#`. A multiuser boot changes root's
login shell to sh, and from then on the single-user prompt is `unrcad#`
too.

**Keys:** the window passes every key to the emulated terminal, Tab and
Esc included, and MAME's own keys start switched off. Scroll Lock switches
them on and off again; MAME shows which in a pop-up. While they are on,
Tab opens MAME's menu, F5 pauses and resumes, F3 resets the machine and
Esc quits MAME, cleanly; letters and digits still go to the terminal. On a
computer too busy to run the emulator at full speed the window can drop
keys typed quickly; a terminal port (section 4) never does.

When you have finished, stop HP-UX before you close the window:
[section 5](#5-stopping-cleanly).

## 4. More terminals

A multiuser run prints how to reach each port when it starts:

    tty01    tools/hpuxterm.py 1        (TCP 127.0.0.1:4001)
    ...
    tty06    tools/hpuxterm.py 6        (TCP 127.0.0.1:4006)

In another terminal window, run:

    tools/hpuxterm.py 1

This joins your terminal to `/dev/tty01` (port 0 of the emulated HP 27140A
multiplexer). Log in as above.

- **Ctrl-]** leaves. The session stays logged in, and `tools/hpuxterm.py 1`
  takes you back to it.
- **Each port takes any number of connections over a run.** A small relay
  (the "hub") sits between MAME and your terminal, because MAME on its own
  accepts only one connection per port per run. A new connection takes the
  port over from an old one.
- **Other clients work too.** Run `stty raw -echo; nc 127.0.0.1 4001; stty
  sane`, or `socat -,rawer,escape=0x1d TCP:127.0.0.1:4001`. HP-UX does the
  echoing, so your local terminal must not.
- **`nc` has no way out.** In raw mode ^C goes to HP-UX like any other
  key, so stop `nc` from another terminal (`pkill -x nc`). `hpuxterm.py`
  and the `socat` line above both leave on Ctrl-].
- **The ports already in use?** `--port-base 5000` moves them to
  5001–5006. Then connect with `tools/hpuxterm.py --base 5000 1`, as the
  run prints it, or set `HPFOCUS_PORT_BASE=5000` for both.
- **Pseudo terminals instead:** `--ports pty` fits a pty to each port.
  The run prints `cio2:mux6:port0: pseudo terminal /dev/pts/N`, and you
  attach with `tools/hpuxterm.py /dev/pts/N`, `screen /dev/pts/N 9600`
  or `picocom -b 9600 /dev/pts/N`.
- **A MAME window per port:** add `-cio2:mux6:port0 terminal`, and so on.
  Ports you fit yourself replace the script's: it then puts none on TCP.
- **The console somewhere else:** `--console tcp` (port 4000, through
  the same relay: `tools/hpuxterm.py console`) or `--console pty`. Either
  runs without a window, and works in single-user mode too.

## 5. Stopping cleanly

Stop HP-UX first, then the emulator:

1. At a root shell, type `cd /` and then `/etc/stopsys`. HP-UX writes
   everything out, gives back its swap space and prints `The system is
   stopped.  You may turn the power off now.`, followed by a screen of
   halt report that begins `System halted due to system error.` with the
   code `00000010`. That report is the stop, not a crash.
2. Close the MAME window. For a run without a window, press ^C in the
   terminal that started it.

`/etc/stopsys` works the same in single-user mode. The run prints this
advice when it starts.

**Stopping the emulator without step 1 is a power cut to HP-UX.** Nothing
HP-UX has written is lost: every disc block reaches the disc file as
HP-UX writes it, so even a crash or `kill -9` keeps it. What a power cut
loses is what HP-UX still held in memory, so type `sync` first if you
cannot do step 1. And it costs disc space: HP-UX leaves 0.3 to 0.9 MB of
swap space marked in use each time, and this disc never checks its file
system at boot (`/etc/bcheckrc` on it is empty).
[KNOWN-ISSUES.md](KNOWN-ISSUES.md) says how to get the space back.

^C, `kill` and a closed terminal all end a run cleanly, whether the
machine is running or paused: MAME stops and saves the battery file. It
takes a second or two and ends with a line like `disc: system disc
written through to ...`.

- **A second ^C kills MAME at once.** The disc is safe as above; battery
  changes from the last 30 seconds are lost.

MAME's soft reset (F3, with MAME's keys switched on) reboots the machine.
To HP-UX that is the reset button: run `/etc/stopsys` first there too.
MAME's pause is F5; a paused machine stops as cleanly as a running one.

HP-UX's own `/etc/shutdown` exists on the disc. It has not been tested on
the emulator; its `-h` option ends in `/etc/stopsys`.

## 6. The machine's state: what persists, and resetting it

Everything a run changes lives under `var/`, which is not tracked by git:

| path | what it is |
|---|---|
| `var/pristine/hpux530.bin` | the HP-UX disc exactly as assembled from the tapes. **Read-only**, never booted writable |
| `var/pristine/hpux530-scm.nv` | a battery file in which HP-UX has initialised its configuration memory |
| `var/hpux530/disc.bin` | **your machine's disc**: a copy of the pristine one, which every run boots and changes |
| `var/hpux530/scm.nv` | your machine's battery: the clock and configuration memory |
| `var/hpux530/console.log` | the console transcript of the last windowed run |

So what you do in HP-UX persists from run to run: files, accounts, a
password you set. Commands for managing this state:

    tools/runhpux.sh --status        what the machine is: where it came from, when it was last written
    tools/runhpux.sh --reset         back to the pristine machine (asks nothing; your changes are gone)
    tools/runhpux.sh --fresh ...     reset, then boot
    tools/runhpux.sh --scratch ...   boot a throwaway copy; nothing is kept
    tools/runhpux.sh --state DIR ... keep a second machine in DIR

The run level is set on every boot: `--multiuser` boots run level 2 and the
default boots single-user (`tools/hpuxconfig.py`). Switching between them
keeps everything else on the disc. The first multiuser boot also makes
three lasting changes: root's passwd entry (no expired password, sh for a
shell), gettys on tty01 to tty06, and those six device files pointed at
the emulated multiplexer.

Only one run can use a machine at a time. A second run on the same
`--state` is refused.

**The date.** The scripts set the clock from your computer at every boot,
in GMT (`FOCUS_RTCTIME=now`, `FOCUS_RTCUTC=1`). HP-UX shows it in Pacific
standard time, because that is the `TZ` this disc was set up with (its
daylight-saving rules are from the 1980s). Set `TZ` inside HP-UX for your
own zone. A date set with HP-UX's `date` lasts only until the next boot,
when the script sets the clock again; `FOCUS_RTCTIME=none` leaves the clock
as the battery file holds it, and an ISO date there pins one.

**Upgrading from an earlier tree:** `tools/runhpux.sh` used to boot
`software/cct/7914_hpux_volume.bin` in place. That file is no longer used,
and the script names it when it finds it. Discs written by earlier
builds can hold damaged files (see KNOWN-ISSUES.md). Start from
`--reset` unless you need what is on the old disc.

## 7. Without a window: headless and batch runs

    tools/runhpux.sh --multiuser --headless

This runs with the console on this terminal's output. Type into it with a
`FOCUS_TYPE` script, or put it on TCP or a pty with `--console`. The
multiplexer ports work as in section 4. ^C stops the run cleanly. MAME
prints `Warning: -video none doesn't make much sense without
-seconds_to_run` when it starts; ignore it.

For a console you can type at, with no window at all:

    tools/runhpux.sh --multiuser --console tcp      # in one terminal
    tools/hpuxterm.py console                       # in another

Long output on that console lags behind HP-UX (KNOWN-ISSUES.md); the six
terminal ports do not.

    tools/runhpux.sh --seconds 60 ...

This stops after 60 emulated seconds, running as fast as the host allows.
A typed session in single-user mode, as a batch job:

    FOCUS_TYPE='\n;ls /\n' FOCUS_TYPEAT=30,40 tools/runhpux.sh --seconds 60

The first `\n` answers the shell's `TERM =` prompt (at about 24 s), and
then `ls /` runs at 40 s. A `;` in `FOCUS_TYPE` ends one script and
starts the next, each typed at its own `FOCUS_TYPEAT` time: write `\x3b`
for a semicolon HP-UX should see (`'\n;date\x3b who\n'`). Give each
script time to be typed before the next one starts on the same terminal,
or their characters interleave. In multiuser mode, `@<terminal>=`
addresses one terminal (`console`, or `2:0` … `2:5` for tty01 … tty06):

    FOCUS_TYPE='@2:0=root\n;@2:0=\n;@2:0=\n;@2:0=who\n' FOCUS_TYPEAT=100,110,118,126 \
    FOCUS_TAP=2:0 tools/runhpux.sh --multiuser --seconds 140

`FOCUS_HELP=1 tools/runhpux.sh --seconds 1` lists every `FOCUS_*` setting
the emulator understands. `tools/runhpux.sh --help` lists the script's
options, and `--dry-run` prints the MAME command line it would run.

## 8. BASIC and HP-UX 5.01 on the 520

    tools/runbasic.sh        BASIC 2.0 from its two boot floppies, in a window
    tools/runbasic-hd.sh     BASIC from the 520's fixed disc (setup.sh built software/hd/basic.img)

- **MAME's red warning comes first.** The 520 is still flagged "not
  working" in MAME, so each windowed run opens on MAME's warning screen,
  which says `Press any key to continue`. (The 540 has no such screen.)
  **Tap the key, do not hold it**, or press Shift, which types nothing:
  the machine powers on the moment the warning goes, and a character key
  that is still down then (held 150 ms or more, in our runs) reaches the
  520 during its power-up and stops the boot. If the screen is still black
  five seconds after the warning has gone, that is what happened: press
  Scroll Lock and then F3, which resets the machine.
- **`BASIC Ready for use.`** appears after about 55 emulated seconds from
  the floppies, and within a few seconds from the fixed disc.
- **Type in capitals.** BASIC does not take its keywords in lower case:
  `disp 7` answers `IMPROPER EXPRESSION`. Press Caps Lock once (MAME
  remembers it from run to run), or hold Shift for the letters. With Caps
  Lock on, Shift gives lower case, as on the 520's own keyboard.
- **Return is ENTER, Ctrl-J is EXECUTE.** The 520's keyboard has both
  keys. Return (ENTER) stores a numbered program line. **Ctrl-J**, or the
  Insert key, is EXECUTE: it runs a command such as `DISP 7`, `LIST` or
  `RUN`. A command ended with Return answers `IMPROPER LINE NUMBER` and
  stays on the input line: **Esc** clears it. So, to type and run a
  program:

      10 FOR I=1 TO 3      Return
      20 PRINT I*I         Return
      30 NEXT I            Return
      40 END               Return
      RUN                  Ctrl-J

  A program needs its `END`; without it RUN stops with `ERROR 5`. Scroll
  Lock switches MAME's own keys on and off, as in section 3; typing,
  Ctrl-J and Insert work the same with them on.
- **The fixed disc is the better BASIC.** It has the error texts and the
  I/O and mass-storage options installed, so `CAT`, `CREATE`, `STORE` and
  `LOAD` work on it, and HP's `CAT_BOOT` and `BUILD_BOOT` utilities are on
  it. From the floppies BASIC comes up without options: errors are bare
  numbers and `CREATE` is an `IMPROPER COMMAND`. What you store on the
  fixed disc reaches the image file when MAME exits cleanly (close the
  window, or ^C once); a crash or a kill loses that session's changes.
- **An older `software/hd/basic.img`** (built before this alpha's disc-tool
  fixes) boots, but BASIC reports the wrong AVAILABLE SPACE and cannot
  CREATE or STORE on it (`ERROR 455`). `tools/setup.sh` keeps an existing
  image, and both it and `tools/runbasic-hd.sh` warn when it is such a
  one: delete it and run `tools/setup.sh` again.
- **Flexible discs are not saved.** What BASIC writes to a disc in a
  flexible drive stays in memory and is gone at exit. Keep work on the
  fixed disc.

A session without a window: `\r` is ENTER and `\n` is EXECUTE in
`FOCUS_TYPE`, `FOCUS_DIMDUMP=1` prints the screen memory as the run goes,
and `tools/screen.py` turns the last of it into the screen. Leave about
five seconds between lines.

    FOCUS_DIMDUMP=1 FOCUS_TYPE='10 A=3\r;20 B=4\r;30 DISP A*B+2\r;40 END\r;RUN\n' \
    FOCUS_TYPEAT=60,66,72,78,84 tools/runbasic-hd.sh -- -video none -str 95 \
      | tools/screen.py

This ends with ` 14` on the screen.

**The 9020 diagnostics.** `tools/setup.sh` also builds
`software/hd/diag.img` (`tools/mkdiagdisc.sh` rebuilds it), a fixed disc
holding HP's diagnostics environment. Boot it with the test disc in the
flexible drive:

    tools/runbasic-hd.sh software/hd/diag.img -- -mpb3 1m -mpb4 1m \
        -flop1 "$PWD/software/test/500MFTST.TD0"

At the BASIC prompt, `LOAD "TEST"` and `RUN`, each ended with Ctrl-J,
bring up `Welcome to the HP 9000 Test Supervisor`
(`docs/DIAGNOSTICS.md`).

HP-UX 5.01 runs on the same machine:

    tools/runhpux.sh --release 5.01 --headless

This boots HP-UX 5.01's kernel, from the damaged release tape, on the 520.
It runs for 100 emulated seconds and shows the screen: the copyright
banner and the terminal's softkey labels. That is as far as 5.01 gets,
because the tape's userland is lost (`docs/HPUX.md` section 11). The 5.01
run mounts the pristine 5.30 disc read-only. (Sent to a file or a pipe,
the output is the raw frame-buffer dump: pipe it through
`tools/screen.py`.)

## 9. Troubleshooting

**Include this in a bug report:** the output of `tools/smoketest.sh
--twice`, `tools/runhpux.sh --status`, and `git rev-parse HEAD`. Also say
whether the boot was `--fresh`.

- **`the emulator is not built`:** run `tools/setup.sh`. If the build
  failed, the end of `var/setup/build.log` says why. Errors about warnings
  from a newer compiler: try `tools/setup.sh -- NOWERROR=1`.
- **The patch "neither applies nor is applied":** `mame/` holds another
  MAME revision, or someone edited the two patched files. Run
  `git -C mame checkout -- scripts/src/cpu.lua src/mame/mame.lst` and
  `tools/setup.sh` again.
- **`Could not initialize SDL No available video device`:** there is no
  display for MAME's window, as in an ssh session. Run without one:
  `tools/runhpux.sh --multiuser --console tcp` and, in another terminal,
  `tools/hpuxterm.py console` (section 7).
- **The 520's window stays on a red MAME warning:** tap any key
  (section 8). The 540 shows no warning.
- **The 520's window stays black after the warning:** the key you
  dismissed the warning with was still down when the machine powered on.
  Press Scroll Lock, then F3 (section 8).
- **BASIC answers `IMPROPER EXPRESSION` or `IMPROPER COMMAND` to
  everything:** you are typing in lower case. Press Caps Lock (section 8).
- **BASIC answers `IMPROPER LINE NUMBER` to a command:** you ended it with
  Return. Press Esc, type it again and press Ctrl-J (EXECUTE) instead
  (section 8).
- **`tools/hpuxterm.py: Permission denied`:** the file lost its executable
  bit (some ways of copying a tree drop it). `chmod +x tools/*.py
  tools/*.sh`, or run it as `python3 tools/hpuxterm.py 1`.
- **`hpuxterm: cannot connect to 127.0.0.1:4001: Connection refused`:**
  no multiuser run is listening there. The ports open a few seconds after
  `tools/runhpux.sh --multiuser` starts. If the run was given
  `--port-base N`, give `hpuxterm.py` `--base N`.
- **Keys typed in the window go missing:** the computer is too busy to run
  the emulator at full speed, and the window samples the keyboard once a
  frame. Type more slowly, or use a terminal port (section 4), which
  loses nothing.
- **^C does not stop a run:** if you are in MAME's debugger (`-debug`), it
  holds the machine where no stop request is seen; let it run first. A
  port you attached yourself as a raw MAME socket can also block the
  emulator (KNOWN-ISSUES.md). Otherwise ^C stops a run whether it is
  running or paused; a second ^C kills it.
- **`cannot fork: no swap space`:** the disc is full, and HP-UX swaps into
  its free space. Stop the emulator, start it again and delete files
  before doing anything else (KNOWN-ISSUES.md).
- **No `login:` after two minutes:** run `tools/smoketest.sh`. If it
  passes, your working machine's state is the difference: try
  `tools/runhpux.sh --scratch --multiuser`, and `--reset` if that boots.
  On a heavily loaded computer the window can simply be slow: the logins
  come at about 40 *emulated* seconds, and several people logging in at
  once can each wait half a minute between `You have mail.` and the
  `TERM =` prompt.
- **`another run (pid N) is using var/hpux530`:** a run is still going.
  Stop it, or use `--scratch` or `--state DIR`. If no such process
  exists, the lock is stale and is ignored automatically.
- **`cannot listen on 127.0.0.1:4001`:** something else holds the port,
  perhaps another run. Use `--port-base`.
- **Stray characters like `1 1 1` after `TERM =`:** these are HP terminal
  escape sequences that `tset` sends. The emulated terminals are not HP
  262x terminals. They are harmless.
- **The first characters typed after `login:` are lost:** getty flushes
  its input just after printing the prompt, as it did on the real
  machine. Wait a second.
- **`error: ... hpux530.bin has changed since it was assembled`:** the
  pristine disc was modified. Delete it as the message says. The next run
  assembles it again from the tapes.

## 10. macOS

macOS is untested for this alpha. What should differ:

- **Prerequisites:** macOS 14.5 or later with Xcode 16.2 or later, as MAME
  asks. Run `xcode-select --install`, then `brew install sdl2 sdl2_ttf
  python3`. `tools/setup.sh` checks for the Xcode tools and SDL2. It does
  not use pkg-config there.
- **The scripts:** they are written for macOS's bash 3.2 and its BSD
  tools. They use `sysctl` where Linux has `nproc`, `cp -c` (an APFS
  clone) where Linux has `cp --reflink`, and Python for checksums. None of
  this has been run on a Mac.
- **Python:** the Xcode tools' `python3` (3.9) is new enough: the tools
  need 3.8 or later.
- **Pseudo terminals** are named `/dev/ttysNNN`.
- **Scroll Lock:** Mac keyboards have no Scroll Lock key. Reassign MAME's
  "UI Toggle" in its Input menu (Tab) if you need it.
- **Not checked at all:** whether the FOCUS driver itself builds with
  Apple clang.
