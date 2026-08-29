// license:BSD-3-Clause
// copyright-holders:
/***************************************************************************

    FOCUS emulator run-time knobs

    Every FOCUS_* environment variable the emulator reads, declared in one
    place. These are the project's instruments: this is a reverse-engineering
    effort as much as an emulator, and most of what the README reports was
    found by switching one of these on and reading what came out.

    There are two kinds, and keeping them apart is half the point of this
    file:

      * a LOG CHANNEL produces diagnostic output and changes nothing. The
        machine runs exactly as it would with the channel off.
      * an OVERRIDE changes what the machine is or does -- how much RAM is
        poisoned, what identity a card answers, which keys get typed. A run
        with an override set is an experiment, not an observation.

    The catalogue below carries both, with a one-line description each, and
    FOCUS_HELP=1 prints it. Before this existed the only index of the ~100
    knobs was the source, and the only way to find the right one was grep.

    No MAME dependency, like its siblings, so the standalone harnesses in
    src/test build against it unchanged.

***************************************************************************/

#ifndef MAME_CPU_FOCUS_FOCUSKNOBS_H
#define MAME_CPU_FOCUS_FOCUSKNOBS_H

#pragma once

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace focus::knob {

//**************************************************************************
//  THE CATALOGUE
//**************************************************************************

enum class group
{
	cpu,        // the execution core
	iop,        // the I/O processor and its channels
	cards,      // what the cards on the backplane answer
	machine,    // memory, boot, media, persistence
	watch,      // breakpoints, watchpoints, tracing, searching
	input,      // the keyboard and scripted typing
};

struct spec
{
	const char *name;
	const char *syntax;     // "" for a plain on/off switch
	group       g;
	bool        overrides;  // true when setting it changes the machine
	const char *help;
};

// Kept alphabetical within each group so the help listing reads as a
// reference rather than as a history of what got added when.
inline constexpr spec CATALOGUE[] = {

// ---- the execution core -------------------------------------------------
{ "FOCUS_CALLLOG", "<hex P>", group::cpu, false,
  "Report every PCL/PCLS taken at that program address, with the segment "
  "and STT entry it resolves to." },
{ "FOCUS_FLAGBR", "", group::cpu, false,
  "Report a branch to a target outside real memory, naming the instruction "
  "still in CIR -- by the time the fetch fails the trail is cold." },
{ "FOCUS_INTLOG", "", group::cpu, false,
  "Report each message bit the I/O service vectors through, with its DRT "
  "entry and a running count. A device serviced thousands of times a second "
  "is not being serviced; it is stuck at the head of its level's list." },
{ "FOCUS_MSG10TRAP", "", group::cpu, true,
  "Deliver message bit 10, the OS doorbell, as a message trap instead of "
  "poll-only. An experiment: BASIC masks the bit and HP-UX does not." },
{ "FOCUS_NOLABELDATA", "", group::cpu, true,
  "Disable the external-program-label fallback in pointer resolution, so a "
  "pointer that fails its DB-relative bounds check traps instead. Turns off "
  "a hypothesis under test (BASIC's DISP output path)." },
{ "FOCUS_MVBLOG", "", group::cpu, false,
  "Report every MVB whose source and target overlap, with the direction, "
  "the gap between them, the count and the caller. The one place the two "
  "readings of the instruction disagree: FOCUS.md says byte-at-a-time in "
  "the chosen direction, which makes a target of source+n a self-propagating "
  "fill. See FOCUS_MVBMEMMOVE." },
{ "FOCUS_MVBMEMMOVE", "", group::cpu, true,
  "Make an overlapping MVB behave like memmove instead of copying byte at a "
  "time in the chosen direction. That was the default for a while, on a "
  "diagnosis that turned out to name the wrong instruction; it breaks the "
  "self-propagating fill HP's display self test depends on. Kept as the A/B "
  "control -- see the comment on do_mvb, and FOCUS_MVBLOG." },
{ "FOCUS_SEGCALL", "", group::cpu, false,
  "Log every inter-segment transfer as caller segment, target segment and "
  "STT entry. The static form of the same information is in the boot image "
  "(tools/stt.py, docs/SUNOS.md), so the two can be diffed." },
{ "FOCUS_SEMALL", "", group::cpu, false,
  "Log every UP and DOWN with its raw operand and resolved address -- the "
  "census that catches a wake landing on the wrong word, which the "
  "block/wake hook cannot see." },
{ "FOCUS_SEMALLMAX", "<n>", group::cpu, false,
  "How many FOCUS_SEMALL lines to print (default 400)." },
{ "FOCUS_SEMALLSKIP", "<n>", group::cpu, false,
  "Drop the first n FOCUS_SEMALL events, which is how the boot's own "
  "traffic gets off the front of a census." },
{ "FOCUS_TRYLOG", "", group::cpu, false,
  "Log every TRY and ESCP with the frame chain head, for following the "
  "pseudo stack markers BASIC's statement loop builds and eats." },
{ "FOCUS_TRYNOP", "", group::cpu, true,
  "Make a TRY delete on an empty chain a no-op instead of a fault. An "
  "experiment, not an emulation claim." },

// ---- the I/O processor --------------------------------------------------
{ "FOCUS_ICEILOG", "", group::iop, false,
  "Log the ICEI command lists opcode by opcode -- the walk, each entry, "
  "and where a list suspends or stops. FOCUS_IOPLOG carries them too." },
{ "FOCUS_IOALL", "", group::iop, false,
  "Log every MPB command the CPU issues, not just the ones that do "
  "something. The full I/O dialogue, and it is voluminous." },
{ "FOCUS_IOPDBG", "", group::iop, false,
  "Log the IOP's internal decisions -- channel selection, poll rounds, "
  "suspension and resumption." },
{ "FOCUS_IOPREV", "<hex>", group::iop, true,
  "The microcode release date the IOP answers RIRV with, YYYYMMDD in hex "
  "(default 19820920, the I/O ERS's own date). HP-UX 5.30 will not open a "
  "device on the HP-IB card unless it is later than 19810630." },
{ "FOCUS_IOPPOLL", "<us>", group::iop, true,
  "How often the IOP polls the interfaces, in microseconds. The ERS has it "
  "polling continuously; this is the emulated stand-in, and it has to be "
  "short compared with what the CPU does between two entries of a command "
  "list or the two processors race over the list itself. Default 400 us, "
  "the middle of a measured band: above ~700 the console open loses its "
  "hand-offs again, below ~200 the 520 stops reaching its banner." },
{ "FOCUS_IOPPARKALWAYS", "", group::iop, true,
  "Suspend a command list on every ICEI, even when the interface is already "
  "answering the poll. The A/B control for the continuous-poll model: with "
  "it on, a list resumes no sooner than the next millisecond tick, which is "
  "long enough for the CPU to run a whole interrupt service between two "
  "entries of one transaction." },
{ "FOCUS_IOPDEFER", "", group::iop, true,
  "Make a start command arm its transfer for the next poll round instead of "
  "moving the data inside the command. Closer to real timing, and it breaks "
  "drivers that spin for a completion." },
{ "FOCUS_IOPLOG", "", group::iop, false,
  "Log DMA transfers and channel state changes." },
{ "FOCUS_IRQLOG", "", group::iop, false,
  "Log every interrupt raise with the device's enable and both latches, for "
  "telling 'the card keeps interrupting' apart from 'the line never fell'." },
{ "FOCUS_IOP_CHANNEL", "<n>", group::iop, true,
  "Which MPB channel bootprobe's IOP answers to (default 2)." },

// ---- what the cards answer ----------------------------------------------
{ "FOCUS_ASICFGEVENT", "<hex>", group::cards, true,
  "The answer to an FR9 write on the serial card; the console driver tests "
  "only its high nibble." },
{ "FOCUS_ASICLCRTS", "<n>", group::cards, true,
  "The RTS opcode the card reports for a CLC order." },
{ "FOCUS_ASIDBG", "", group::cards, false,
  "Log the serial card's FR-register traffic and its internal state." },
{ "FOCUS_ASIEVDELAY", "<ms>", group::cards, true,
  "How many milliseconds of card time pass before a completion event is "
  "announced." },
{ "FOCUS_ASIEVENT", "<n>", group::cards, true,
  "The event code the card announces on completion." },
{ "FOCUS_ASIEVENT2", "<n>", group::cards, true,
  "The event code carried under AES in Read Transparent Status." },
{ "FOCUS_ASIFR1", "<hex>", group::cards, true,
  "Force the serial card's FR1 status word." },
{ "FOCUS_ASIID", "<n>", group::cards, true,
  "The serial card's identity; 5 is the 27130A eight-channel multiplexer, "
  "which HP-UX drives with the same module." },
{ "FOCUS_ASIIDENT", "<hex>", group::cards, true,
  "The serial card's identity halfword." },
{ "FOCUS_ASILCN", "<n>", group::cards, true,
  "The logical channel number the serial card reports." },
{ "FOCUS_ASILOG", "", group::cards, false,
  "Log the serial card's transactions at the driver's level." },
{ "FOCUS_ASILOGMAX", "<n>", group::cards, false,
  "Raise the FOCUS_ASILOG line cap (default 2000): a driver polling status "
  "outruns it long before the interesting part of a dialogue." },
{ "FOCUS_ASIMPX", "<0|1|2>", group::cards, true,
  "The multiplexing capability in the identity block: 0 subchannel only, "
  "1 in-channel only, 2 both (27128A 4-67; the ASI ships configured with 1)." },
{ "FOCUS_ASIPAUSE", "<ms>", group::cards, true,
  "How long the ASI's Pause order pauses before the card asks for its next "
  "order. The driver's idle loop is PAU then read transparent status, so "
  "this is what paces it; 1 ms reproduces the ~1000 status reads a second "
  "the real driver makes." },
{ "FOCUS_ASINOASYNC", "", group::cards, true,
  "Silence the asynchronous events the serial card's firmware senses -- "
  "'transmit buffer is empty', which the tty subsystem's drain waits for, "
  "and 'data available'. The A/B control for the console drain: with it on "
  "the boot stops after its banner, exactly as it did before the events "
  "were modelled." },
{ "FOCUS_ASIRTS", "<n>", group::cards, true,
  "The opcode the card reports through Read Transparent Status." },
{ "FOCUS_ASISWITCHES", "<hex>", group::cards, true,
  "The card's configuration switch byte, which the driver reads with "
  "subfunction 253." },
{ "FOCUS_DISCBUSY", "<n>", group::cards, true,
  "How many poll rounds the internal disc reports busy before completing." },
{ "FOCUS_DISCFR3", "<hex>", group::cards, true,
  "Force the internal disc controller's FR3 answer." },
{ "FOCUS_DISCFR3B", "<hex>", group::cards, true,
  "Force the internal disc controller's FR3 answer while unit 1 is selected." },
{ "FOCUS_DISCID", "<n>", group::cards, true,
  "The halfword the identify transaction answers; 1..6 map to internal "
  "device types, anything else types the slot 29." },
{ "FOCUS_DISCLOG", "", group::cards, false,
  "Log the internal disc controller's registers and transfers." },
{ "FOCUS_FIXEDDEV", "<6 hex digits>", group::cards, true,
  "The CS/80 packed-decimal device number the fixed disc reports "
  "(default 007908)." },
{ "FOCUS_HPIBADDR", "<n>", group::cards, true,
  "The HP-IB card's own bus address." },
{ "FOCUS_HPIBDISC", "<file>", group::cards, true,
  "Hand a raw disc image to an HP-IB card without going through MAME's "
  "media system." },
{ "FOCUS_HPIBIDENT", "<hex>", group::cards, true,
  "The HP-IB card's identity halfword." },
{ "FOCUS_CS80ID", "<hex>", group::cards, true,
  "The two bytes the CS/80 drive answers an AMIGO identify with (default "
  "020a, a 7914). HP-UX's boot-device probe tests the first byte for 02 "
  "to choose its CS80 driver." },
{ "FOCUS_HPIBDBG", "", group::cards, false,
  "Log the HP-IB card's internal decisions: orders, the bus dialogue an "
  "interface message runs, transaction completions and the events it "
  "queues." },
{ "FOCUS_HPIBSC", "<0|1>", group::cards, true,
  "Whether the HP-IB card is the bus's System Controller (switch S6, UP "
  "from the factory). FR1 bit 5 reports it, and the HP27110 driver refuses "
  "request 13 with escape 127 without it." },
{ "FOCUS_HPIBFR1", "<hex>", group::cards, true,
  "Force the HP-IB card's FR1 status word. The sweep control for the "
  "configuration pass's request 13, which the HP27110 driver refuses with "
  "escape 127 unless bit 5 is set and 129 unless bit 6 is." },
{ "FOCUS_HPIBLOG", "", group::cards, false,
  "Log the HP-IB card's transactions and bus activity." },
{ "FOCUS_GFXDUMP", "<n>", group::cards, false,
  "Dump the graphics memory as text every n frames, the way FOCUS_DIMDUMP "
  "dumps the alphanumeric one. tools/gfxscreen.py turns the log back into a "
  "picture." },
{ "FOCUS_GFXNOMEM", "", group::cards, true,
  "Keep the graphics interface fitted but give it no memory and no command "
  "decode -- the acknowledge-and-log card the emulator had before the "
  "command set was read off PWP SELF TEST. The fine-grained A/B control; "
  "FOCUS_NOGFX removes the slot entirely and is too blunt to bisect with." },
{ "FOCUS_GFXORIGIN", "<hex>", group::cards, true,
  "The word offset within a plane that the visible raster starts at. Zero "
  "until something says otherwise." },
{ "FOCUS_GFXPGM", "<file>", group::cards, false,
  "Write the graphics plane to a PPM image on the same schedule as "
  "FOCUS_GFXDUMP, or every 60 frames on its own -- for looking at rather "
  "than diffing. tools/gfxscreen.py --ppm makes the same picture from a log." },
{ "FOCUS_GFXPLANES", "<n>", group::cards, true,
  "How many memory planes the display has: 3 for the 98770A colour display, "
  "1 for the 98780A monochrome one. Pairs with the class nibble of "
  "FOCUS_SC1FR1, which is what picks the type-16 or type-17 self test." },
{ "FOCUS_GFXSTRIDE", "<n>", group::cards, true,
  "Words per raster line: 36 for the colour display (576 pixels, 560 of "
  "them displayed) and 35 for the monochrome one. The one number the raster "
  "interpretation hangs on, so it is sweepable." },
{ "FOCUS_NOGFX", "", group::cards, true,
  "Leave select code 1 empty, the way the emulator was before the graphics "
  "interface was modelled. The A/B control for anything the display changes." },
{ "FOCUS_SC1FR1", "<hex>", group::cards, true,
  "The graphics interface's FR1 status word. Four separate gates read it -- "
  "the powerup scan, the kernel's device typing, its ready poll and its "
  "bring-up -- and 0x04c0 is the value that passes all four; see "
  "src/iop/focusgfx.h." },
{ "FOCUS_SC1ID", "<hex>", group::cards, true,
  "The graphics interface's FR3. The powerup scan requires the low byte to "
  "read zero." },
{ "FOCUS_SC1LOG", "", group::cards, false,
  "Trace every register access to the graphics interface, with the segment "
  "and PC that made it." },
{ "FOCUS_MUXAESDELAY", "<ms>", group::cards, true,
  "How long the 6-channel MUX waits between a character reaching its silo "
  "and the data-available event (default 5; the ERS allows up to 10)." },
{ "FOCUS_MUXAESEVENT", "<hex>", group::cards, true,
  "The event code the 6-channel MUX announces for data available (default "
  "11, the ERS's ARQ,AES); a modem-line change is that plus 3." },
{ "FOCUS_MUXCSCEVENT", "<hex>", group::cards, true,
  "The event the 6-channel MUX answers the FR9 Connect Subchannel command "
  "with (default 30, the code the kernel's slot scan accepts from the ASI)." },
{ "FOCUS_MUXDBG", "", group::cards, false,
  "Log the 6-channel MUX's own decisions: orders, blocks, subfunctions, "
  "the download, and the events it queues." },
{ "FOCUS_MUXDOWNLOADED", "", group::cards, true,
  "Start the 6-channel MUX in its downloaded personality -- six ports, "
  "firmware ID 3 -- without waiting for the host to download it." },
{ "FOCUS_MUXEVDELAY", "<ms>", group::cards, true,
  "Milliseconds of card time before the 6-channel MUX announces a "
  "completion (default 2)." },
{ "FOCUS_MUXEVENT", "<hex>", group::cards, true,
  "The completion event code the 6-channel MUX announces after each "
  "transfer (default 10)." },
{ "FOCUS_MUXFR1", "<hex>", group::cards, true,
  "Force the 6-channel MUX's FR1 status word." },
{ "FOCUS_MUXIDENT", "<hex>", group::cards, true,
  "The 6-channel MUX's identify halfword: card ID in the high byte (7 "
  "types the slot 32), EPROM firmware ID in the low (default 0702)." },
{ "FOCUS_MUXLOG", "", group::cards, false,
  "Log the 6-channel MUX's register accesses and blocks at the driver's "
  "level." },
{ "FOCUS_MUXLOGMAX", "<n>", group::cards, false,
  "Raise the FOCUS_MUXLOG line cap (default 2000)." },
{ "FOCUS_MUXNOASYNC", "", group::cards, true,
  "Silence the 6-channel MUX's asynchronous events -- data available and "
  "modem line changes -- leaving the host to poll." },
{ "FOCUS_MUXPER", "<hex>", group::cards, true,
  "The base of the 6-channel MUX's protocol-error event codes (default "
  "70; the ERS's PER low nibble is added)." },
{ "FOCUS_MUXPERORDER", "<n>", group::cards, true,
  "The PER low nibble the 6-channel MUX answers an unsupported order "
  "with (default 2, 'unimplemented or invalid order')." },
{ "FOCUS_MUXPOLL", "<0|1|2>", group::cards, true,
  "What the 6-channel MUX reports to the interface poll: 0 nothing, 1 "
  "that it wants its next order (default), 2 whenever it could move data." },
{ "FOCUS_MUXTAP", "<sc>:<port>|<sc>:all", group::cards, false,
  "Print what a 6-channel MUX port transmits, as the console path prints "
  "the console, and let FOCUS_TRIP watch it." },
{ "FOCUS_PRINTER", "", group::cards, true,
  "Fit the internal thermal printer, which shares select code 6 with the "
  "keyboard (520 Service 2-43). An option on the machine and off by default "
  "here, since fitting it registers another device." },
{ "FOCUS_PRINTLOG", "", group::cards, false,
  "Trace the printer: every register access, the commands written to R5 and "
  "their R7 parameters, control codes, escape sequences and completed "
  "lines. This is how the undocumented escape set gets enumerated." },
{ "FOCUS_PRINTPRESENT", "<0|1>", group::cards, true,
  "Whether the printer announces itself in R1 bit 15, which is what makes "
  "the powerup self test run its printer test at all. R1 is shared with the "
  "keyboard, so this is the control for telling the two apart." },
{ "FOCUS_PRINTTEXT", "<file>", group::cards, true,
  "Write the printer's completed lines to a file as plain text. Explicitly "
  "lossy -- it drops the attributes, the graphics dumps and the upper half "
  "of the character set. The raw byte stream is what -printer captures." },
{ "FOCUS_PRINTSTATUS", "<hex>", group::cards, true,
  "Override the printer's status byte, whose bits the CE Handbook names "
  "PSFL DSTF MSTF OOPP HDFL VHLO PTTH HDTH -- 0x10 is out of paper. Zero is "
  "a healthy printer and the default." },
{ "FOCUS_PROBE", "<sc>[:<id>][,...]", group::cards, true,
  "Park a logging card in a slot, which is how what a driver expects of a "
  "controller we do not have gets read off a run. Applied after the real "
  "cards, so a probe can stand in for one of them. With FOCUS_PROBELOG the "
  "log names the segment and PC behind each access, which is what separates "
  "one caller's use of a register from another's." },
{ "FOCUS_PROBEFR0", "<hex>", group::cards, true,
  "What a probe card answers to FR0." },
{ "FOCUS_PROBEFR1", "<hex>", group::cards, true,
  "What a probe card answers to FR1." },
{ "FOCUS_PROBELOG", "", group::cards, false,
  "Log every register access a probe card sees." },
{ "FOCUS_RTCBATFAIL", "[6|7|both]", group::cards, true,
  "Force the battery-fail bit: R2 bit 5 on the SCM, R1 bit 3 on the "
  "keyboard board. The manuals say the OS restores its configuration when "
  "it sees this. NOTE that the 520's power-up self test FAILS on the "
  "keyboard bit -- \"Self test error 1: IO address 006,0002\", escape 4, "
  "\"Battery low condition detected\" (520 Service, table C-1) -- which is "
  "why a missing battery file does not raise it there by itself." },
{ "FOCUS_RTCWINDOW", "", group::cards, true,
  "Clear the battery-fail bit four seconds after power-up, as both manuals "
  "require it to be read within (530/540 Service 2-30, 520 Service 2-34). "
  "Off by default: the emulator has always latched it for the whole run." },
{ "FOCUS_SC0FR1", "<hex>", group::cards, true,
  "Force the display card's FR1 status word." },
{ "FOCUS_SC0ID", "<n>", group::cards, true,
  "The display card's identity. The alphanumeric driver binds only to 1 "
  "or 6." },
{ "FOCUS_SC0LOG", "", group::cards, false,
  "Log the display card's FR-register traffic." },
{ "FOCUS_SC6FR1", "<hex>", group::cards, true,
  "Force the keyboard board's FR1 status word." },
{ "FOCUS_UNIT1", "", group::cards, false,
  "Report any access to the internal disc while unit 1 -- the bootstrap "
  "loader card, whose ROM carries the machine ID -- is selected, so a "
  "licence check would show itself." },
{ "FOCUS_DIMCELLS", "", group::cards, false,
  "Log every character cell written to the display's frame buffer." },
{ "FOCUS_DIMDUMP", "", group::cards, false,
  "Dump the display's frame buffer as text at the end of the run." },

// ---- the machine --------------------------------------------------------
{ "FOCUS_BOOTLOAD", "<hex addr>", group::machine, true,
  "Where the emulated loader places the boot image, overriding the address "
  "chosen to fit the fitted RAM." },
{ "FOCUS_BOOTOPT", "<file>[,<file>...]", group::machine, true,
  "Append option binaries to the boot image, which is what a configured "
  "machine actually runs. Stands in for HP's BUILD_BOOT while the emulated "
  "media is read-only." },
{ "FOCUS_BOOTPB", "<hex base>", group::machine, true,
  "The code base the powerup module runs with, for testing what the loader "
  "really hands over." },
{ "FOCUS_CSTMAX", "<n>", group::machine, false,
  "How many Code Segment Table entries a state report lists (default 128). "
  "BASIC's CST has 300, and the scaffold segments live above the old cut." },
{ "FOCUS_FLOPPY", "<image>[,<image>...]", group::machine, true,
  "The raw disc images in the flexible drive, in the order an operator "
  "would feed them. A boot image is a set: BASIC 2.0 is two volumes, the "
  "diagnostics three." },
{ "FOCUS_FORCETICK", "<ms>[:<start s>]", group::machine, true,
  "Raise the interval-timer message on a fixed beat whether or not the OS "
  "armed the timer. The start delay exists because injecting during powerup "
  "panics a machine that is not ready." },
{ "FOCUS_KBDNV", "<file>", group::machine, true,
  "The battery behind the KEYBOARD board's clock and configuration memory "
  "(select code 6), the counterpart of FOCUS_SCMNV for the 520. Without it "
  "the 520's clock is volatile, so what BASIC's SET TIMEDATE writes dies "
  "with the run." },
{ "FOCUS_HDNOSAVE", "", group::machine, true,
  "Do not write the fixed disc back to its file at exit. A run that must "
  "not change what the next run starts from." },
{ "FOCUS_NVLOG", "", group::machine, false,
  "Report every battery load, save and battery-fail decision, with the "
  "file's header fields." },
{ "FOCUS_NVMFILL", "<hex byte>", group::machine, true,
  "Poison the configuration memory on both boards that carry one -- the "
  "520's keyboard board and the 530/540's SCM -- the counterpart of "
  "FOCUS_RAMFILL for non-volatile storage." },
{ "FOCUS_NVSAVE", "<seconds>", group::machine, true,
  "How often a changed clock or configuration memory is written back, so a "
  "run that is killed does not lose it (default 30). 0 saves only at exit, "
  "which is what it used to do." },
{ "FOCUS_RAMFILL", "<hex byte>", group::machine, true,
  "Poison all of RAM before the boot, standing in for the garbage a real "
  "machine powers up with. A boot that behaves differently under poison is "
  "depending on a read of memory nothing initialised." },
{ "FOCUS_RTCBATTERY", "off|on|<days>", group::machine, true,
  "Let the battery advance the clock over the time the emulator was NOT "
  "running, from the save time in the battery file. 'on' is the manuals' "
  "ten days worst case; past the limit the battery is treated as flat and "
  "the battery-fail bit goes up instead. Default off, because it makes a "
  "run's output depend on when it was started." },
{ "FOCUS_RTCTIME", "none|now|<ISO 8601>|<epoch>", group::machine, true,
  "Seed the clock at power-up. Default none: it powers up all-zero, "
  "\"never set\", which is the path HP-UX's first boot takes (error 941) "
  "and what BASIC reports as \"Clock and date not set\". 'now' takes the "
  "host's wall clock; a fixed moment is what a reproducible run wants. "
  "This seeds the HARDWARE: HP-UX 5.30 still clears the century and year "
  "during bring-up and sets its own, and the clock check byte that "
  "probably gates that is not known." },
{ "FOCUS_RTCUTC", "", group::machine, true,
  "Take FOCUS_RTCTIME=now as UTC rather than as the host's local time." },
{ "FOCUS_SCMNV", "<file>", group::machine, true,
  "The battery behind the SCM's clock and configuration memory (select code "
  "7): loaded at start, written back at exit and every FOCUS_NVSAVE "
  "seconds. Without it every boot is a first boot, and HP-UX treats an "
  "unset clock as reason to abandon its device bring-up. Files written "
  "before the format carried a header are still read." },

// ---- watching a run -----------------------------------------------------
{ "FOCUS_BREAK", "<hex addr>", group::watch, false,
  "Report the full processor state the first time execution reaches that "
  "instruction, with a window of the current stack frame -- enough to read "
  "the arguments a routine was called with." },
{ "FOCUS_BREAKFROM", "<seconds>", group::watch, false,
  "Ignore FOCUS_BREAK hits before that emulated moment." },
{ "FOCUS_DISPDBG", "", group::watch, false,
  "Report the registers the dispatcher computed when it refuses to resume "
  "a task -- the stack segment, its base, limit and saved S -- rather than "
  "just the inconsistent-registers trap." },
{ "FOCUS_DUMP", "<hex addr>[:<words>][,...]", group::watch, false,
  "Memory windows to print with every state report, for looking at tables "
  "the system built." },
{ "FOCUS_EATRACE", "<hex addr>[:<n>]", group::watch, false,
  "Print the next n data addresses the processor computes once it reaches "
  "that instruction, with what it found there." },
{ "FOCUS_EATRACEFROM", "<seconds>", group::watch, false,
  "Arm FOCUS_EATRACE late, so a shared routine can be traced on a chosen "
  "later visit -- the compile-time call and the run-time call of the same "
  "helper are different questions." },
{ "FOCUS_EAWATCH", "<lo>:<hi>", group::watch, false,
  "Log every data address the processor computes inside a range, with the "
  "instruction that did it: the definitive record of which words of a "
  "structure are actually touched." },
{ "FOCUS_ESCLOG", "", group::watch, false,
  "Log every ESCP escape with its value." },
{ "FOCUS_ESCVAL", "<hex>", group::watch, false,
  "Log only that escape value, uncapped; otherwise the first 400." },
{ "FOCUS_FIND", "<hex bytes>|\"<text>\"", group::watch, false,
  "Scan all of memory for a byte string -- an instruction pair to locate a "
  "site in the loaded copy, or a piece of text to find out whether the "
  "system composed it at all and where it put it." },
{ "FOCUS_FINDAT", "<seconds>", group::watch, false,
  "Run the FOCUS_FIND search at a chosen moment instead of at a "
  "breakpoint, which is how the question gets asked of a machine that is "
  "simply sitting there." },
{ "FOCUS_PCPROF", "", group::watch, false,
  "Bucket the program counter by 4 KB and print the busiest buckets every "
  "40M instructions, so 'where is the machine spending its time' is "
  "answered by measurement rather than by reading code." },
{ "FOCUS_POKE", "<addr>:<val>[:<s>][,...]", group::watch, true,
  "Write a word at a chosen moment -- hand a waiting task the event nobody "
  "produces and watch whether the machine sails on." },
{ "FOCUS_POKEPC", "<pc>:<addr>:<val>[,...]", group::watch, true,
  "Write a word the first time execution reaches an instruction, which is "
  "the only way to reach a moment two events share." },
{ "FOCUS_QCHECK", "", group::watch, false,
  "Report the first instruction that runs with a misaligned Q or an "
  "impossible CSN -- the wreckage of a corrupted stack marker, caught at "
  "the moment of restoration rather than at the later crash." },
{ "FOCUS_RUNAWAY", "", group::watch, false,
  "Report the moment the processor leaves real memory, with the last "
  "instructions it executed before it did." },
{ "FOCUS_RWATCH", "<lo>:<hi>", group::watch, false,
  "Report every read of a byte-address range, including the ones the "
  "effective-address hook cannot see. Instruction fetches come through "
  "here too, so keep the range to data." },
{ "FOCUS_RWFROM", "<seconds>", group::watch, false,
  "Arm FOCUS_RWATCH at that emulated moment." },
{ "FOCUS_SEMLOG", "", group::watch, false,
  "Trace the hardware semaphores: which one a task blocks on and which one "
  "wakes it. An idle kernel that has stopped short says so here." },
{ "FOCUS_STATEAT", "<seconds>[,...]", group::watch, false,
  "Report the full processor state, the segment table and the last "
  "instructions at each of those emulated moments." },
{ "FOCUS_TRACEN", "<n>", group::watch, false,
  "List the next n instructions after a FOCUS_BREAK hit, which is how a "
  "call gets followed into whatever it reaches." },
{ "FOCUS_TRAPFROM", "<seconds>", group::watch, false,
  "Skip the boot's own faults, which otherwise eat the whole FOCUS_TRAPLOG "
  "budget before the interesting one arrives." },
{ "FOCUS_TRAPLOG", "[<n>]", group::watch, false,
  "Report each fault as it is taken, with the state the processor is in, "
  "which names the code that faulted. A number sets how many (default 6); "
  "'1' is the historical on-switch, not a budget of one." },
{ "FOCUS_TRIP", "<text>", group::watch, false,
  "Report the processor state and a walk back up the stack markers the "
  "moment that text reaches the screen, which is how the code behind a "
  "message gets found." },
{ "FOCUS_WATCH", "<lo>:<hi>", group::watch, false,
  "Report the first instructions executed inside an address range, which "
  "answers whether a driver is ever entered." },
{ "FOCUS_WATCHFROM", "<seconds>", group::watch, false,
  "Ignore everything before that moment, which is how activity from a "
  "typed command gets separated from the boot's own." },
{ "FOCUS_WATCHN", "<n>", group::watch, false,
  "How many FOCUS_WATCH hits to report." },
{ "FOCUS_WPFROM", "<seconds>", group::watch, false,
  "Arm FOCUS_WPOINT late: heap memory is recycled, and a write seen before "
  "the interesting object exists belongs to some earlier tenant." },
{ "FOCUS_WPOINT", "<hex addr>", group::watch, false,
  "Poll a word and report every change, with the instruction that made it." },
{ "FOCUS_WPSTACK", "", group::watch, false,
  "Add the call chain to each FOCUS_WPOINT report, which is how the code "
  "that asked for a write gets named when the writer is a shared helper." },
{ "FOCUS_WWATCH", "<lo>:<hi>", group::watch, false,
  "Report every store into a byte-address range, including a value "
  "overwritten with itself, which the FOCUS_WPOINT change-poller cannot "
  "see." },
{ "FOCUS_WWFROM", "<seconds>", group::watch, false,
  "Arm FOCUS_WWATCH at that emulated moment." },

// ---- input --------------------------------------------------------------
{ "FOCUS_KBDLOG", "", group::input, false,
  "Log the keyboard board's registers and the keycodes it delivers." },
{ "FOCUS_KBDST", "<hex>", group::input, true,
  "Force the keyboard board's status register." },
{ "FOCUS_KEYOR", "<hex>", group::input, true,
  "Add flag bits above the 8-bit host byte, for probing the holding "
  "register's shift/control encoding." },
{ "FOCUS_RAWKEYS", "", group::input, true,
  "Make the host's byte the keycode itself, which is how the keycode table "
  "was read off the machine." },
{ "FOCUS_TYPE", "<text>[;<text>...]", group::input, true,
  "Keystrokes to feed at the FOCUS_TYPEAT moments, with \\n for EXECUTE, "
  "\\r for STORE and \\xNN to press KEYCODE NN -- the only way to reach a "
  "key that puts nothing on the screen, such as the one that clears the "
  "input line (0x70). A ';' starts a further script, which is what a "
  "command that takes seconds to finish needs." },
{ "FOCUS_TYPEAT", "<seconds>[,...]", group::input, true,
  "When to type each FOCUS_TYPE script." },
{ "FOCUS_TYPERATE", "<ms>", group::input, true,
  "Pace the injected keystrokes. The default mimics fast human typing; the "
  "display driver drops characters pushed faster than it redraws." },
{ "FOCUS_TYPEPORT", "<sc>:<port>", group::input, true,
  "Send the FOCUS_TYPE scripts into a port of the 6-channel MUX at that "
  "select code instead of the console." },
};

inline constexpr unsigned COUNT = sizeof(CATALOGUE) / sizeof(CATALOGUE[0]);

inline const spec *find(const char *name)
{
	for (const spec &s : CATALOGUE)
		if (!std::strcmp(s.name, name))
			return &s;
	return nullptr;
}

//**************************************************************************
//  THE HELP LISTING
//**************************************************************************

inline bool help_requested() { return std::getenv("FOCUS_HELP") != nullptr; }

inline void print_help(std::FILE *out = stdout)
{
	static const struct { group g; const char *title; } SECTIONS[] = {
		{ group::cpu,     "execution core" },
		{ group::iop,     "I/O processor" },
		{ group::cards,   "cards" },
		{ group::machine, "machine" },
		{ group::watch,   "watching a run" },
		{ group::input,   "input" },
	};

	std::fprintf(out,
		"FOCUS emulator knobs -- %u environment variables.\n"
		"A knob marked (!) changes what the machine does; the rest only\n"
		"report. Set any of them in the environment:\n"
		"\n"
		"    FOCUS_TRAPLOG=1 FOCUS_STATEAT=19 tools/runbasic-hd.sh -- -str 20\n"
		"\n", COUNT);

	for (const auto &sec : SECTIONS)
	{
		std::fprintf(out, "%s\n", sec.title);
		for (const spec &s : CATALOGUE)
		{
			if (s.g != sec.g)
				continue;
			char lhs[80];
			std::snprintf(lhs, sizeof lhs, "%s%s%s", s.name,
					*s.syntax ? "=" : "", s.syntax);
			// 2 + 3 + 38 puts every description at column 43.
			static constexpr int INDENT = 43, WRAP = 34;
			std::fprintf(out, "  %-3s%-38s", s.overrides ? "(!)" : "", lhs);
			// A name longer than its column pushes the description to a
			// line of its own rather than shunting it out of alignment.
			if (std::strlen(lhs) > 38)
				std::fprintf(out, "\n%*s", INDENT, "");
			unsigned col = 0;
			for (const char *p = s.help; *p; )
			{
				const char *e = std::strchr(p, ' ');
				const unsigned n = e ? unsigned(e - p) : unsigned(std::strlen(p));
				if (col && col + n + 1 > WRAP)
				{
					std::fprintf(out, "\n%*s", INDENT, "");
					col = 0;
				}
				std::fprintf(out, "%s%.*s", col ? " " : "", int(n), p);
				col += n + (col ? 1 : 0);
				p = e ? e + 1 : p + n;
			}
			std::fprintf(out, "\n");
		}
		std::fprintf(out, "\n");
	}
	std::fflush(out);
}

// Called once from each program's start-up. Returns true when the caller
// should exit rather than run: FOCUS_HELP is a query, not a setting.
inline bool handle_help()
{
	if (!help_requested())
		return false;
	print_help();
	return true;
}

//**************************************************************************
//  READING A KNOB
//**************************************************************************

// Every reader goes through here, so a knob that is read but not catalogued
// says so once rather than hiding. It is a developer error, not a user one:
// the catalogue is what FOCUS_HELP prints, and a knob missing from it is a
// knob nobody can find.
inline const char *lookup(const char *name)
{
	if (!find(name))
		std::fprintf(stderr,
				"[knob] %s is read but not in the catalogue "
				"(focusknobs.h)\n", name);
	return std::getenv(name);
}

// FOCUS_X -- set or not.
class flag
{
public:
	explicit flag(const char *name) : m_on(lookup(name) != nullptr) {}
	explicit operator bool() const { return m_on; }
	bool on() const { return m_on; }
private:
	bool m_on;
};

// FOCUS_X=<n>. The base matters: addresses are written in hex without an
// 0x, counts in decimal, so each site says which it means.
class number
{
public:
	number(const char *name, unsigned long dflt, int base = 0)
		: m_set(false), m_v(dflt)
	{
		if (const char *e = lookup(name))
		{
			m_set = true;
			m_v = std::strtoul(e, nullptr, base);
		}
	}
	bool set() const { return m_set; }
	unsigned long get() const { return m_v; }
	operator unsigned long() const { return m_v; }
private:
	bool m_set;
	unsigned long m_v;
};

// FOCUS_X=<seconds>, the arming time shared by the watch family.
class seconds
{
public:
	seconds(const char *name, double dflt)
		: m_v(dflt)
	{
		if (const char *e = lookup(name))
			m_v = std::strtod(e, nullptr);
	}
	double get() const { return m_v; }
	operator double() const { return m_v; }
private:
	double m_v;
};

// FOCUS_X=<string>; get() is null when unset.
class text
{
public:
	explicit text(const char *name) : m_v(lookup(name)) {}
	const char *get() const { return m_v; }
	explicit operator bool() const { return m_v != nullptr; }
private:
	const char *m_v;
};

// FOCUS_X=<lo>:<hi>, hex byte addresses. Off unless both are given and the
// range is non-empty -- the whole watch family reads its range this way.
class range
{
public:
	explicit range(const char *name) : m_on(false), m_lo(0), m_hi(0)
	{
		if (const char *e = lookup(name))
		{
			m_lo = std::strtoul(e, nullptr, 16);
			if (const char *colon = std::strchr(e, ':'))
				m_hi = std::strtoul(colon + 1, nullptr, 16);
			m_on = m_hi >= m_lo && m_hi != 0;
		}
	}
	explicit operator bool() const { return m_on; }
	bool holds(unsigned long a) const { return m_on && a >= m_lo && a <= m_hi; }
	unsigned long lo() const { return m_lo; }
	unsigned long hi() const { return m_hi; }
private:
	bool m_on;
	unsigned long m_lo, m_hi;
};

//**************************************************************************
//  LOG CHANNELS
//**************************************************************************

// A log channel is a flag, a line budget and a counter, which is what every
// one of these sites was open-coding. Declare one as a function-local
// static and the environment is read once:
//
//     static knob::channel try_log("FOCUS_TRYLOG", 20000);
//     if (try_log) try_log("[TRY p=%08x sb=%08x]\n", r.p, r.sb);
//
// The budget counts printed lines. `skip_env` drops that many EVENTS off
// the front first, so a census can start past the boot's own traffic, and
// seq() is the event number for a site that prints it.
// Whether each line is flushed as it is written. It matters twice: a
// report that shares stdout with the emulated machine's own output has to
// be flushed to land in the right place, but the high-volume I/O channels
// run to tens of thousands of lines and flushing every one of them costs
// more than the ordering is worth. Each channel says which it is.
enum class sync { flush, buffered };

class channel
{
public:
	channel(const char *name, unsigned cap = 0,
			const char *cap_env = nullptr, const char *skip_env = nullptr)
		: m_on(lookup(name) != nullptr)
		, m_cap(cap_env ? unsigned(number(cap_env, cap)) : cap)
		, m_skip(skip_env ? unsigned(number(skip_env, 0)) : 0)
	{
	}

	channel(const char *name, sync s, unsigned cap = 0)
		: m_on(lookup(name) != nullptr)
		, m_cap(cap)
		, m_sync(s)
	{
	}

	// True while the channel is on and has budget left. Guarding the call
	// with this keeps a site from computing arguments it will not print.
	explicit operator bool() const
	{
		return m_on && (!m_cap || m_printed < m_cap);
	}

	bool on() const { return m_on; }
	unsigned seq() const { return m_events; }
	unsigned printed() const { return m_printed; }
	unsigned left() const { return m_cap ? m_cap - m_printed : ~0u; }

	// Counts the event, prints it if it is past the skip and inside the
	// budget, and flushes -- these reports interleave with the emulated
	// machine's own output, so an unflushed line lands in the wrong place.
	void operator()(const char *fmt, ...)
#if defined(__GNUC__)
			__attribute__((format(printf, 2, 3)))
#endif
	{
		const unsigned seq = m_events++;
		if (!m_on || seq < m_skip || (m_cap && m_printed >= m_cap))
			return;
		m_printed++;
		std::va_list ap;
		va_start(ap, fmt);
		std::vprintf(fmt, ap);
		va_end(ap);
		if (m_sync == sync::flush)
			std::fflush(stdout);
	}

	// Some sites report to stderr, because their point is to survive a
	// stdout that the emulated machine is also writing to.
	void err(const char *fmt, ...)
#if defined(__GNUC__)
			__attribute__((format(printf, 2, 3)))
#endif
	{
		const unsigned seq = m_events++;
		if (!m_on || seq < m_skip || (m_cap && m_printed >= m_cap))
			return;
		m_printed++;
		std::va_list ap;
		va_start(ap, fmt);
		std::vfprintf(stderr, fmt, ap);
		va_end(ap);
		std::fflush(stderr);
	}

private:
	bool m_on;
	unsigned m_cap;
	unsigned m_skip = 0;
	sync m_sync = sync::flush;
	unsigned m_events = 0;
	unsigned m_printed = 0;
};

//**************************************************************************
//  ALARMS
//**************************************************************************

// A report with no switch, because it means the emulator has caught itself
// doing something impossible -- an operand it cannot have been handed, a
// state it cannot have reached. Distinct from a log channel on purpose: a
// channel is off until someone asks a question, an alarm is always on
// because nobody would think to ask.
//
// On stderr, and flushed, so it survives a run whose stdout is being piped
// somewhere and whose tail is never read.
inline void alarm(const char *fmt, ...)
#if defined(__GNUC__)
		__attribute__((format(printf, 1, 2)))
#endif
;

inline void alarm(const char *fmt, ...)
{
	std::va_list ap;
	va_start(ap, fmt);
	std::vfprintf(stderr, fmt, ap);
	va_end(ap);
	std::fflush(stderr);
}

//**************************************************************************
//  DEVICE LOGS
//**************************************************************************

// A card's log, as distinct from a call site's channel.
//
// The channels above are declared where they print and read their own
// switch from the environment. A card's log differs in two ways. It is
// enabled from outside -- the driver reads FOCUS_ASILOG and hands the
// answer to the card, because it is the driver that knows which card is in
// which slot. And its traffic is repetitive: a driver polling a status
// register runs to hundreds of thousands of accesses in a few emulated
// seconds, so identical consecutive lines are collapsed into a repeat
// count and the whole log stops after a budget.
//
// The card supplies the key -- whatever distinguishes one line from the
// next -- and formats its own text:
//
//     if (m_log.gate(key))
//         m_log("[asi%u R fr%u = %04x]\n", pa, n, v);
//
// A card with no repetition to collapse just tests it and prints:
//
//     if (m_log) m_log("[disc select %u]\n", unit);
//
// This replaced two near-identical copies of the collapsing logic, one in
// the serial card and one in the HP-IB card, which had drifted to the
// point of printing different text when they hit the same cap.
class device_log
{
public:
	// Named once by the card that owns it. The tag and unit appear in the
	// log's own notes, so they read as coming from the card they describe.
	void name(const char *tag, unsigned unit)
	{
		m_tag = tag;
		m_unit = unit;
	}

	// A card there is only one of -- the internal disc, the display --
	// wants no unit number after its tag.
	void name(const char *tag) { m_tag = tag; m_unit = NO_UNIT; }

	void enable(bool on) { m_on = on; }
	void set_cap(unsigned lines) { m_cap = lines; }

	explicit operator bool() const { return m_on; }
	bool on() const { return m_on; }
	unsigned cap() const { return m_cap; }

	// True when the caller should print this line. Collapses a run of
	// identical keys, and stops at the budget -- saying so once, because
	// a log that simply ends looks like a machine that stopped.
	bool gate(unsigned long key)
	{
		if (!m_on)
			return false;
		if (key == m_last)
		{
			m_repeat++;
			return false;
		}
		if (m_repeat)
		{
			note("... x%u more", m_repeat);
			m_repeat = 0;
		}
		m_last = key;
		if (m_lines++ >= m_cap)
		{
			if (m_lines == m_cap + 1)
				note("log capped at %u lines", m_cap);
			return false;
		}
		return true;
	}

	// Print one line. Flushed: these interleave with the emulated
	// machine's own output, and a run killed at the interesting moment
	// still has to say what it saw.
	void operator()(const char *fmt, ...)
#if defined(__GNUC__)
			__attribute__((format(printf, 2, 3)))
#endif
	{
		if (!m_on)
			return;
		std::va_list ap;
		va_start(ap, fmt);
		std::vprintf(fmt, ap);
		va_end(ap);
		std::fflush(stdout);
	}

private:
	// The log's own remarks about itself, tagged like the card's lines.
	void note(const char *fmt, ...)
#if defined(__GNUC__)
			__attribute__((format(printf, 2, 3)))
#endif
	{
		if (m_unit == NO_UNIT)
			std::printf("[%s ", m_tag);
		else
			std::printf("[%s%u ", m_tag, m_unit);
		std::va_list ap;
		va_start(ap, fmt);
		std::vprintf(fmt, ap);
		va_end(ap);
		std::printf("]\n");
		std::fflush(stdout);
	}

	static constexpr unsigned NO_UNIT = ~0u;

	const char *m_tag = "card";
	unsigned m_unit = NO_UNIT;
	bool m_on = false;
	unsigned m_cap = 2000;
	unsigned long m_last = ~0ul;
	unsigned m_repeat = 0;
	unsigned m_lines = 0;
};

} // namespace focus::knob

#endif // MAME_CPU_FOCUS_FOCUSKNOBS_H
