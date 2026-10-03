// license:BSD-3-Clause
// copyright-holders:
/***************************************************************************

    Console and probe interface cards for the HP 9000 Series 500.

    These began life inside the bootrun harness and carry everything
    learned about the select code 6 console interface: the powerup
    bus-test loopback through FR12, the complemented command/response
    protocol on FR9, the ID-9 presence reset, and the system's own
    panic reporter, whose text arrives at select code 0 as FR0 writes
    with 0x70 in the high byte. The same models serve the MAME driver
    and the standalone harness.

***************************************************************************/

#ifndef MAME_IOP_FOCUSCARDS_H
#define MAME_IOP_FOCUSCARDS_H

#pragma once

#include "focusiop.h"
#include "focusprint.h"
#include "focusrtc.h"

#include <cstdio>
#include <deque>
#include <functional>
#include <string>

namespace focus {

// The select code 6 card: the 520's keyboard electronics board.
//
// The 520 service manual ("Theory of Operation" 2-32..2-34, contents
// list 3-16) documents the register map this models:
//   R1  read:  status -- bit 0 keyboard interrupt pending, bit 1 power
//              fail warning, bit 3 battery failure
//   R3  read:  keyboard ID register, data bus bit 1 set = present
//   R9  r/w:   RTC data for the location latched through R12
//   R10 read:  keycode holding register (7-bit keycode + shift/control)
//       write: LEDs and auto-repeat rate
//   R11 write: beeper divide-down (all 1's = off)
//   R12 write: RTC location latch (low 4 bits); the NVM instead takes
//              a 16-bit address "in complemented form"
//   R13 r/w:   NVM data byte for the address latched through R12
// plus the powerup bus-test latches exercised through the FR12 write /
// FR9 read pair (the visible powerup "character ramp" was that test,
// not display output -- the display lives at select code 0).
//
// The clock is the same sixteen-location DIGIT chip the SCM carries
// (focusrtc.h, "THE DIGIT CLOCK"): each location one BCD digit -- seconds
// at 2-3, minutes 4-5, hours 6-7, day 8-9, day of week 10, month 11-12 --
// and no year. The manual's table at 3-16 (0 second, 1 clock check byte,
// 2 minute ... 9 year) is the 64-byte CONTENTS map, whose every offset is
// an NVM byte on this board; the check byte, century and year the OS
// reads beside the chip live there, at 1, 5 and 9. BASIC and HP-UX 5.01
// latch the locations complemented (ffff, fffe, fffd... on R12) and undo
// the data's inversion with NOT (trap seg 0x48072), exactly as HP-UX 5.30
// drives the SCM. (An earlier reading took the table for byte-wide clock
// registers; no guest drives the board that way.)
struct console_card : public iop_interface
{
	std::string report;    // panic text, see write_fr
	// The powerup device scan (image offset 0x656) reads FR3 and only
	// counts a card present when it reads zero. A card answering exactly
	// 9 first gets a reset -- WIF1 then WIF8, opcodes 012 and 0102 -- and
	// has to read zero afterwards, so 9 is the one identity this powerup
	// knows how to quiesce. The real board answers 2 (manual: R3 "sets
	// data bus bit 1 to a 1"); whether the scan quiesces a 2 the same
	// way is testable with --consoleid.
	u32 id = 9;
	// The scan's reset (WIF1/WIF8) historically zeroed the ID; the
	// device binder later re-reads FR3 and types the card by it, so a
	// keyboard must keep answering its identity. reset_clears_id keeps
	// the old behaviour selectable while the scan gate is pinned down.
	bool reset_clears_id = false;
	bool was_reset = false;
	u32 fr0 = 0;               // pending input: 0 = none, else keycode
	iop_core *iop = nullptr;
	unsigned pa = 0;
	knob::device_log log_fr;   // trace every register access
	// Which routine touched the board. Select code 6 carries several
	// devices -- keyboard, RTC/NVM, beeper, and the thermal printer --
	// on one interface, so the register number alone does not say whose
	// access this is. The machine fills this in; without it, inert.
	std::function<std::string()> where;
	std::string site() const
	{ return (where && log_fr) ? where() : std::string(); }
	// The optional internal thermal printer sits on this same interface
	// (520 Service 2-43: it shares select code 6 and its bus drivers are
	// enabled for R0, R1, R2, R4, R5, R6 and R7). Null when none is
	// fitted, which is the machine HP shipped without the option.
	printer_device *printer = nullptr;
	unsigned writes = 0;       // register writes so far, for ordering logs
	u32 fr1_base = 0x80;       // the status bits above the pending flag
	// Where display characters go: the panic reporter (and, on the
	// same evidence so far, the console output path) writes them to
	// FR0 with 0x70 in the high byte; a host that renders a screen
	// hooks this.
	std::function<void(u8)> on_char;
	u32 status() override { return 1; }   // status line asserted
	// The interface status register the self test prints ("IO address
	// 006,0002 STATUS: 00000001" is select code 6, our identity 2, and
	// this word); a healthy board has nothing to report.
	u32 status_reg() override { return st; }
	// The powerup self test requires this to read EVEN on the keyboard
	// board (its escape 6). The old value of 1 was chosen when the
	// "STATUS:" the report prints was thought to be this register; it is
	// the test's own escape code.
	u32 st = 0;
	// RIFG, the interface flag: the board has finished what it was asked
	// to do. The self test's wait helper polls this, and a card that
	// never sets it makes that wait time out (escape 2).
	//
	// One interface, one flag line: with a printer fitted it is the
	// printer's PFLG, and the CE Handbook's printer failure 02 -- "flag
	// did not go false after reading FR0" -- is the requirement that
	// gives it its shape.
	u32 flag() override
	{ return printer ? (printer->flag() ? 1u : 0u) : 1u; }   // NOLINT
	// The clock and the 2 KB of configuration memory, shared with the
	// 530/540's SCM (focusrtc.h), and decoded the same way: R12 latches a
	// clock location or an NVM address, both complemented, R9 carries
	// clock data complemented, one digit to a location, and R13 NVM data
	// plain.
	//
	// All-zero powers up as a stopped clock that was never set, which the
	// drivers accept and report as CLOCK AND DATE NOT SET.
	//
	// The host calls rtc_second_tick() once per emulated second, and a
	// running chip counts in BCD digits. (The byte-wide, binary-counting
	// clock this board modelled until the alpha was a decode no guest
	// drives: R1 bit 7 below sends BASIC and HP-UX 5.01 down the digit
	// path, where their reads landed on cells that never held a date.)
	//
	// The NVM is read out in full at powerup -- four thousand accesses,
	// descending through complemented addresses -- so this is where the
	// board's configuration lives; the map in focusrtc.h says what HP put
	// where. An erased part reads 0xff.
	rtc_nvm nv{rtc_decode::keyboard()};
	void rtc_second_tick() { nv.tick_second(); }
	void fill_nvm(u8 v) { nv.fill_nvm(v); }
	// The 520's battery. The SCM's has stood in for one since FOCUS_SCMNV;
	// this board -- the one BASIC and the diagnostics actually drive -- had
	// none, so what SET TIMEDATE wrote died with the run.
	nv_load_result load_nv(const char *path) { return nv.load(path, 6); }
	bool save_nv(const char *path) { return nv.save(path, 6); }
	bool nv_dirty() const { return nv.dirty; }

	// A machine reset. Only the RUN state goes: not the identity, the
	// status bits or the printer the machine configured, and not the clock
	// or the configuration memory, which is what the battery is for. The
	// SCM has had this since it was written; the keyboard board never did,
	// so a reset left it holding whatever key and whatever panic text the
	// run before it had.
	void reset()
	{
		fr0 = 0;
		keycode = 0;
		pending.clear();
		ard = false;
		was_reset = false;
		writes = 0;
		report.clear();
		nv.power_on_reset();
	}

	// Registers the board implements but whose behaviour is not modelled
	// individually. The powerup self test walks a bit through R15 and
	// requires an exact read-back (its escape code 1 is the "Self test
	// error 1: IO address 006,0002 STATUS: 00000001" the boot has always
	// reported), so an unmodelled register has to at least remember what
	// was written to it.
	static constexpr u32 SELF_TEST_REPLY = 0x47;
	u32 scratch[16] = {};
	u32 keycode = 0;           // holding register behind R10
	// The board holds one keycode at a time; a host that types faster
	// than the system reads would lose the rest, so the extras wait
	// here and each read of R10 hands the next one over with a fresh
	// service request, which is what a person typing looks like.
	std::deque<u8> pending;
	u32 read_fr(unsigned n) override
	{
		u32 v = 0;
		// With a printer fitted, R0/R2/R4/R5/R6/R7 present its status
		// byte (520 Service 2-43). R0 is the one that overlaps: this
		// model uses it for the keyboard's pending keycode, which the
		// manual does not assign to the keyboard at all. It costs
		// nothing either way at powerup -- the only read of R0 in a
		// whole boot is the self test's, before any key is pressed, and
		// "no key pending" and "printer healthy" are both 0x00 -- but
		// the printer's claim is the documented one, so it wins when
		// there is a printer to make it.
		if (printer && printer_device::is_status_register(n) && n != 1)
			return printer->read_status(n);
		switch (n)
		{
		case 0:  v = fr0; fr0 = 0; break;
		// Status: bit 7 says the clock is a digit chip -- the drivers'
		// exchange helper (trap seg 0x48066) branches on it, as HP-UX
		// 5.30's does on the SCM's R1, and takes one BCD digit from each
		// location; bit 0 is the keyboard-interrupt-pending bit from the
		// manual.
		// R1 is shared, "with the printer driving only the upper 4
		// bits of R1" (520 Service 2-43).
		// Bit 3 is the battery failure the 520 manual assigns to this
		// register (2-32), the keyboard board's counterpart of the SCM's
		// R2 bit 5. Nothing raises it by itself -- HP's own power-up self
		// test reports escape 4, "Battery low condition detected", when it
		// is set -- so it is FOCUS_RTCBATFAIL that puts it up.
		case 1:  v = fr1_base | (fr0 ? 1 : 0)
				| (nv.battery_fail_visible() ? (1u << 3) : 0)
				| (printer ? printer->r1_bits() : 0); break;
		case 3:  v = (was_reset && reset_clears_id) ? 0 : id; break;
		// RTC data, complemented on the bus; reads auto-increment the
		// location (manual 2-34). The rollover triple-read never relies
		// on the increment because it re-latches every field.
		case 9:  v = nv.read_rtc(); break;
		case 10:
			// Taking the keycode clears the board's pending flag, which
			// is bit 0 of the status register: leaving it set reads as a
			// keyboard still holding a key down, and the system stops
			// treating what arrives afterwards as typed characters.
			v = keycode;
			keycode = 0;
			fr0 = 0;
			if (!pending.empty())
			{
				const u8 next = pending.front();
				pending.pop_front();
				keycode = next;
				fr0 = next;
				if (iop)
					iop->raise(pa);
			}
			break;
		case 13: v = nv.read_nvm(); break;
		default: v = scratch[n & 15]; break;
		}
		log_fr("[sc%u R fr%u=%04x%s]\n", pa, n, v & 0xffff, site().c_str());
		return v;
	}
	bool input_ready() const { return fr0 != 0; }
	// DMA: the display's frame data arrives this way (the 520 manual:
	// the DIM frame buffer is written "in DMA or direct I/O modes", and
	// the boot starts PA 0 DMA with SDC). The card is always ready --
	// except that a printer takes its transfer only once armed, and this
	// is the line the IOP asks.
	bool service_request() override
	{ return printer ? printer->accepts_data() : true; }
	void dma_complete(bool) override
	{ if (printer) printer->transfer_complete(); }
	// The interface poll must be quiet on this board: the self test
	// checks that nothing is requesting service between its steps
	// (escape 7). Transfers are a separate line, above. The printer, when
	// fitted, has its own say -- its self test requires the poll after
	// the loopback (its escape 5).
	bool poll_request() override
	{ return printer && printer->poll_request(); }
	void dma_write(u8 c) override
	{
		// A line of print arrives this way. The driver arms the printer
		// (R1 bit 12) and starts a transfer rather than writing the
		// bytes one at a time, so on a board with a printer fitted this
		// is the paper, not the console.
		if (printer)
		{
			printer->dma_byte(c);
			return;
		}
		if (on_char)
			on_char(c);
		// Not a log: this is the machine's own console output, which is
		// why it goes out unconditionally and why it is the one printf
		// left in this file. Everything diagnostic goes through a
		// knob::device_log.
		std::printf("%c", (c >= 32 && c < 127) ? c : '.');
		std::fflush(stdout);
	}
	// The DIM's halfword transfers carry an attribute in the high byte
	// and the character in the low one -- the same layout the panic
	// reporter uses on FR0 (0x70xx).
	void dma_write16(u16 v) override { dma_write(u8(v)); }
	// A pending keycode does not raise attention yet. The OS registers a
	// bit-14 handler (sysglobal DB+1700[14] -> trap seg STT[344]) and
	// writes an all-eligible attention mask, but its message router
	// validates the interrupted context (MUC over CTCBP's globals,
	// CS1 ~0x6df2..0x6e2c) and reports system error 054 when a routed
	// message lands on the idle anchor -- which is where every keystroke
	// lands until the partition/UI world exists to post keyboard reads.
	// Wire this to `fr0 != 0 && !ard` once a task owns the keyboard.
	bool attention() override { return false; }
	bool ard = false;      // attention request disable, set by the IOP's poll
	void type(u16 c)
	{
		if (keycode)       // the holding register is still full
		{
			pending.push_back(c);
			return;
		}
		fr0 = c;
		keycode = c;
		ard = false;       // a fresh keystroke re-asserts ASYNC
		// "The keyboard service request latch is set when the keyboard
		// microprocessor outputs a keycode ... Select code 6 is enabled
		// for keyboard and printer interrupts at all times" (520 service
		// manual 2-32, 2-30): a keycode raises the SC6 interrupt.
		if (iop)
			iop->raise(pa);
	}
	void write_fr(unsigned n, u32 v) override
	{
		writes++;
		log_fr("[sc%u W fr%u=%04x%s]\n", pa, n, v & 0xffff, site().c_str());
		// "Writes to R0, R2, R4, R5, R6 and R7 clock data from the IOP
		// into the data register. Interface commands are specified by
		// writes to R5 ... parameters are written to R7" (520 Service
		// 2-43). R1 stays the shared control register and is not the
		// printer's to take.
		if (printer && printer_device::is_status_register(n) && n != 1)
		{
			printer->write_register(n, v);
			return;
		}
		if (n == 1 && printer)
			printer->control(v);
		if (n == 1 || n == 8)   // the scan's reset pair, and the ARD
		{
			was_reset = true;
			if (n == 1 && (v & 1))
				ard = true;    // attention request disable from the poll
			return;
		}
		// The system's panic reporter writes its text a character at a
		// time to FR0 with 0x70 in the high byte, which is how a run
		// that dies says so in words:
		//   "System halted due to system error. TRAP:00000030 ..."
		// followed by the trap number and the register set. The scan
		// also writes FR0, without the marker, so the marker is what
		// separates report text from a plain register write.
		if (n == 0)
		{
			if ((v & 0xff00) == 0x7000)
			{
				const char c = char(v & 0xff);
				report.push_back(c);
				if (on_char)
					on_char(u8(c));
				std::printf("%c", (c >= 32 && c < 127) ? c : '.');
				std::fflush(stdout);
			}
			return;
		}
		if (n >= 14)
		{
			scratch[n & 15] = v & 0xffff;
			return;
		}
		if (n == 10)
		{
			// R10 is the holding register the keyboard microprocessor
			// answers through, and writing it is how the system talks to
			// that microprocessor. The powerup self test writes zero and
			// then waits for the board to report itself: it polls R1 for
			// the data-available bit and requires the byte that comes
			// back to be 71 (0x47) in its low seven bits, retrying until
			// it is and reporting "Self test error 1: IO address
			// 006,0002" with escape 3 if it never comes.
			if ((v & 0xffff) == 0)
			{
				keycode = SELF_TEST_REPLY;
				fr0 = SELF_TEST_REPLY;
				// The board interrupts when its answer is ready: the
				// test's wait arms the device interrupt (SIL 4, EINT)
				// and blocks, so a reply that only sat in the register
				// left it waiting until the timeout (escape 2).
				if (iop)
					iop->raise(pa);
			}
			return;
		}
		
		switch (n)
		{
		case 9:            // RTC data write to the latched location,
			// complemented like the reads (the OS's clock-set sweep at
			// trap seg ~0x480fc writes every location through here).
			//
			// This was dead code: an early return above this switch
			// swallowed register 9 as an unmodelled "command parameter",
			// so the 520's clock could not be set at all and BASIC's
			// SET TIME / SET TIMEDATE -- the remedy the CE Handbook
			// prescribes for "Clock and date not set" -- wrote nowhere.
			nv.write_rtc(v);
			return;
		case 12:           // RTC location / complemented NVM address.
			// Just an address latch: it does NOT interrupt. (A legacy
			// raise here kept a stale request queued at the keyboard's
			// level, and the real keystroke's request then made no fresh
			// edge for the host to forward.)
			nv.write_latch(v);
			log_fr("[%u:%02x]", pa, v & 0xff);
			return;
		case 13:           // NVM data byte
			nv.write_nvm(v);
			return;
		default:
			return;        // R10 LEDs/auto-repeat, R11 beeper: accepted
		}
	}
};

// A probe card: answers an identity and logs everything the drivers do
// to it, so an unknown controller's protocol can be read off a run.
struct probe_card : public iop_interface
{
	u32 id = 0;
	unsigned pa = 0;
	knob::device_log log;
	bool was_reset = false;
	u32 fr[16] = {};
	u32 status() override { return 0; }
	u32 flag() override { return 1; }
	// What FR1 answers the powerup self test, which writes 1<<pa to FR0
	// and reads here. The right answer is not known yet, so the shapes
	// worth trying are selectable.
	unsigned fr1_mode = 0;
	// Select code 7's scan asks FR0 for the identity rather than FR3.
	u32 fr0_answer = 0;
	unsigned m_fr1_reads = 0;
	u32 fr1_policy(u32 pattern) const
	{
		// Anything from 0x100 up is a constant answer, for finding which
		// status bit a driver is waiting on.
		if (fr1_mode >= 0x100)
			return fr1_mode & 0xffff;
		switch (fr1_mode)
		{
		case 1:  return ~pattern & 0xffff;
		case 2:  return 0;
		case 3:  return 0xffff;
		case 4:  return id;
		case 5:  return pattern << 8;
		default: return pattern;
		}
	}
	// Where the processor was when it did this. A card cannot see the
	// CPU, and for an interface nobody has documented, "which routine
	// asked" is most of the answer -- the FR number alone says almost
	// nothing. The machine fills this in; without it the hook is inert.
	std::function<std::string()> where;
	// Evaluated only when the log is on: an FR access happens on the
	// machine's hot path and building a string per access would be felt.
	std::string site() const
	{ return (where && log) ? where() : std::string(); }

	u32 read_fr(unsigned n) override
	{
		// The powerup scan's presence test: FR3 must read zero after the
		// WIF1/WIF8 reset, then it writes a bit pattern to FR0 and
		// expects FR1 to read back nonzero. An interface that echoes
		// satisfies it. An identity of 9 is the one the scan resets, so
		// --probe 5 9 is the way to get a card registered and watch what
		// the drivers then do with it.
		u32 v;
		// The scan resets a card (WIF1/WIF8) before typing it, and the
		// real boards keep answering their identity afterwards -- the
		// console card learned this the hard way -- so a probe must too,
		// or the device record it produces carries no identity at all.
		if (n == 3)      v = id;
		else if (n == 0) v = fr0_answer ? fr0_answer : fr[0];
		else if (n == 1)
		{
			// The presence test wants the pattern it just wrote to FR0
			// echoed back; only after that does the driver poll this as
			// a status register, so a constant answer starts there.
			v = (m_fr1_reads++ == 0) ? fr[0] : fr1_policy(fr[0]);
		}
		else             v = fr[n & 15];
		log("[probe%u R fr%u = %04x%s]\n", pa, n, v, site().c_str());
		return v;
	}
	void write_fr(unsigned n, u32 v) override
	{
		if (n == 0)
			m_fr1_reads = 0;
		if (n == 1 || n == 8)
			was_reset = true;
		fr[n & 15] = v & 0xffff;
		log("[probe%u W fr%u = %04x%s]\n", pa, n, v & 0xffff, site().c_str());
	}
};

} // namespace focus

#endif // MAME_IOP_FOCUSCARDS_H
