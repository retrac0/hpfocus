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
- **Disc space:** about 3.5 GB free. A shallow MAME checkout needs about
  1.2 GB, its build tree about 700 MB, and the HP-UX machines in `var/`
  about 400 MB.
- **Time:** a first build compiles MAME's core, about 1,190 files. Expect
  20–40 minutes on an idle 8-core machine. On a busy 8-core machine at
  `-j4` it took 2 hours. Later builds take seconds.
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
  command for your distribution.

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
   - a fixed disc for BASIC, `software/hd/basic.img`.

Every step is idempotent. If anything stops it (the network, a missing
package, ^C), fix the cause and run `tools/setup.sh` again. It carries on
from where it stopped.

Useful options:

| option | what it does |
|---|---|
| `--check` | check the prerequisites only |
| `--no-build` | do everything except the build |
| `-j N` | build with N jobs. The default is one per CPU, reduced if memory is short: a MAME compile job can take 1.5 GB |
| `-- NOWERROR=1` | pass options to MAME's make. NOWERROR=1 helps when a newer compiler warns about MAME's code |
| `--mame-source DIR` | fetch MAME from a mirror or a local clone that has the pinned commit |

`git submodule update --init --depth 1 mame` also works now that the pin is
an upstream commit, but it downloads MAME's current master first. If you
fetch MAME that way, or put any checkout of `a3ee360d` in `mame/`, run
`tools/setup.sh` afterwards: it uses the tree that is already there.

## 3. First boot

Check that HP-UX comes up on your machine. This takes about a minute and
touches nothing of yours:

    tools/smoketest.sh

It should end with `PASS`. Then boot HP-UX with a window:

    tools/runhpux.sh --multiuser

What happens:

- **The window is the system console.** It opens black, with a cursor, and
  stays that way for the first ten seconds or so while the kernel loads.
  There is no MAME warning screen to dismiss on this machine. Then the
  kernel prints its copyright banner and `/etc/rc` runs. Within a minute
  of power-on (about 45 emulated seconds), `login:` appears on the console
  and on the six terminal ports.
- **Log in as `root`.** There is no password. Press Return at the two
  `TERM =` prompts: `/etc/profile` asks once and root's `.profile` asks
  again.
- **The prompt is `unrcad#`**, the name of the 1991 machine this disc
  came from. Try `who`, `ls -l /`, `ps -ef` and `df`.

Without `--multiuser`, `tools/runhpux.sh` boots to **single-user mode**.
There, root's csh runs on the console with no login: press Return at its
`TERM =` prompt and you get the prompt `gator#`. Once the machine has been
booted multiuser the shell is `/bin/sh` and the prompt `unrcad#`: the
multiuser set-up rewrites root's `/etc/passwd` entry (`tools/hpuxconfig.py`),
and that stays on the disc until `--reset`.

**Keys:** every key you press in the window goes to the emulated terminal,
Tab and Esc included. MAME's own keys are switched off when the machine
starts. Scroll Lock switches them on (Tab then opens MAME's menu, and Esc
quits MAME, cleanly) and off again.

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
  5001–5006. Then tell `hpuxterm.py` too: `tools/hpuxterm.py --base 5000 1`
  (the run prints the command for each port when it starts).
- **Pseudo terminals instead:** `--ports pty` fits a pty to each port.
  The build prints `cio2:mux6:port0: pseudo terminal /dev/pts/N`, and you
  attach with `tools/hpuxterm.py /dev/pts/N`, `screen /dev/pts/N 9600`
  or `picocom -b 9600 /dev/pts/N`.
- **A MAME window per port:** add `-cio2:mux6:port0 terminal`, and so on.
- **The console somewhere else:** `--console tcp` (port 4000,
  `tools/hpuxterm.py console`) or `--console pty`. Both need a build whose
  console card has an rs232 port. The script checks, and says so if yours
  does not.

## 5. Stopping cleanly

HP-UX in this alpha has no automatic shutdown and never checks its file
system at boot: `/etc/bcheckrc` on this disc is empty. To stop:

1. At a root shell, type `sync`.
2. Close the MAME window. For a run without a window, press ^C in the
   terminal that started it.

`tools/runhpux.sh` turns ^C, `kill` and a closed terminal into a clean exit
(`tools/mame/stopfile.lua`): MAME stops and the disc and battery files are
saved. It takes a second or two and ends with a line like `disc: system
disc written through to ...`. If MAME does not stop, a second ^C sends it
SIGTERM, and a third sends SIGKILL.

Avoid `kill -9` on MAME itself. On a build where disc writes reach the
image only at exit, it loses the whole session. (The storage changes in
this alpha write each disc block through as HP-UX writes it, but the
battery file is still saved at exit.)

HP-UX's own `/etc/shutdown` exists on the disc. It has not been tested on
the emulator.

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
keeps everything else on the disc.

Only one run can use a machine at a time. A second run on the same
`--state` is refused.

**The date.** The scripts set the clock from your computer at every boot,
in GMT (`FOCUS_RTCTIME=now`, `FOCUS_RTCUTC=1`). HP-UX shows it in Pacific
time, because that is the `TZ` this disc was set up with. Set `TZ` inside
HP-UX for your own zone. Set `FOCUS_RTCTIME=none` for an unset clock, or
an ISO date to pin one. On a build without the clock fixes of this alpha,
the seed is ignored and HP-UX reports `CLOCK AND DATE NOT SET` and a date
in 2036.

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

    tools/runhpux.sh --seconds 60 ...

This stops after 60 emulated seconds, running as fast as the host allows.
A typed session in single-user mode, as a batch job:

    FOCUS_TYPE='\n;ls /\n' FOCUS_TYPEAT=30,40 tools/runhpux.sh --seconds 60

The first `\n` answers csh's `TERM =` prompt (at about 24 s), and then
`ls /` runs at 40 s. A `;` in `FOCUS_TYPE` ends one script and starts the
next, each typed at its own `FOCUS_TYPEAT` time: write `\x3b` for a
semicolon HP-UX should see (`'\n;date\x3b who\n'`). In multiuser mode,
`@<terminal>=` addresses one terminal (`console`, or `2:0` … `2:5` for
tty01 … tty06):

    FOCUS_TYPE='@2:0=root\n;@2:0=\n;@2:0=\n;@2:0=who\n' FOCUS_TYPEAT=100,110,118,126 \
    FOCUS_TAP=2:0 tools/runhpux.sh --multiuser --seconds 140

`FOCUS_HELP=1 tools/runhpux.sh --seconds 1` lists every `FOCUS_*` setting
the emulator understands. `tools/runhpux.sh --help` lists the script's
options, and `--dry-run` prints the MAME command line it would run.

## 8. BASIC and HP-UX 5.01 on the 520

    tools/runbasic.sh        BASIC 2.0 from its two boot floppies, in a window
    tools/runbasic-hd.sh     BASIC from the 520's fixed disc (setup.sh built software/hd/basic.img)

- **MAME's warning screen comes first on the 520.** A red box says `THIS
  SYSTEM DOESN'T WORK` and `Press any key to continue`. Press a key. MAME
  says this about every driver flagged as unfinished, which the 520's is.
- **`BASIC Ready for use.`** appears after about 55 emulated seconds.
- **The keys.** The 520's keyboard has two keys where yours has Enter:
  - **Enter is STORE.** It enters a numbered program line (`10 A=3`).
  - **Ctrl-J is EXECUTE.** It runs a command: `RUN`, `LIST`, `DISP 2+3`.
    Pressing Enter after a command answers `IMPROPER LINE NUMBER`.
  - **Esc clears the input line**, which you need after an error: the
    line stays, with the cursor where BASIC stopped reading.
  - Scroll Lock switches MAME's own keys on and off, as in section 3.

A session without a window: `\r` is STORE and `\n` is EXECUTE in
`FOCUS_TYPE`, `FOCUS_DIMDUMP=1` prints the screen memory as the run goes,
and `tools/screen.py` turns the last of it into the screen. Leave about
five seconds between lines.

    FOCUS_DIMDUMP=1 FOCUS_TYPE='10 A=3\r;20 B=4\r;30 DISP A*B+2\r;40 END\r;RUN\n' \
    FOCUS_TYPEAT=60,66,72,78,84 tools/runbasic-hd.sh -- -video none -str 95 \
      | tools/screen.py

This ends with ` 14` on the screen.

`tools/mkdiagdisc.sh` builds `software/hd/diag.img`, which holds HP's 9020
diagnostics environment. Boot it with the test disc in the flexible
drive, then `LOAD "TEST"` and `RUN` (`docs/DIAGNOSTICS.md`):

    tools/runbasic-hd.sh software/hd/diag.img -- -mpb3 1m -mpb4 1m \
        -flop1 "$PWD/software/test/500MFTST.TD0"

    tools/runhpux.sh --release 5.01 --headless

This boots HP-UX 5.01's kernel, from the damaged release tape, on the 520.
It prints the copyright banner and stops there, because the tape's userland
is lost (`docs/HPUX.md` section 11). The 5.01 run mounts the pristine 5.30
disc read-only.

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
- **The window stays on a red MAME warning:** that is the 520 (BASIC, HP-UX
  5.01 in a window). Press any key (section 8). The 540 has none.
- **`tools/hpuxterm.py: Permission denied`:** the file lost its executable
  bit (some ways of copying a tree drop it). `chmod +x tools/*.py
  tools/*.sh`, or run it as `python3 tools/hpuxterm.py 1`.
- **`hpuxterm: cannot connect to 127.0.0.1:4001: Connection refused`:**
  no multiuser run is listening there. The ports open a few seconds after
  `tools/runhpux.sh --multiuser` starts. If the run was given
  `--port-base N`, give `hpuxterm.py` `--base N`.
- **No `login:` after two minutes:** run `tools/smoketest.sh`. If it
  passes, your working machine's state is the difference: try
  `tools/runhpux.sh --scratch --multiuser`, and `--reset` if that boots.
  Multiuser boots are known to be timing-sensitive in some
  configurations: see KNOWN-ISSUES.md.
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
