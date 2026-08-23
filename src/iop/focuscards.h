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
// RTC locations (manual 3-16): 0 second, 1 clock check byte, 2 minute,
// 3 stack size, 4 hour, 5 century, 6 day of week, 7 day of month,
// 8 month, 9 year, 10-13 control registers, 14 timer resolution,
// 15 timeout scan resolution. The BASIC console open reads exactly
// 0,2,4,7,6,8,9,5,1 three times over (a rollover-consistent read),
// then control register 10 -- and requires the recovered value's
// bit 7 set there before it continues. Data returns complemented:
// the driver undoes the inversion with NOT (trap seg 0x48072).
struct console_card : public iop_interface
{
	std::string out;
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
	u32 flag() override { return 1; }
	u32 latch12 = 0;           // last R12 write: RTC location / NVM address
	// All-zero powers up as "midnight, clock never set", which the
	// driver accepts. Control register 10's bit 7 is a busy bit the
	// driver polls until CLEAR before each rollover-consistent read
	// burst (trap seg 0x484f8: it spins on FR12=0x0a / ~FR9 while bit 7
	// is set), so it must stay 0 here.
	u8 rtc[64] = {};
	// The clock must actually run: the boot synchronises its software
	// clock to the next second boundary by polling the seconds location
	// between 100 ms sleeps (trap seg 0x476ec..0x47798), and a frozen
	// RTC parks the boot there forever. The host calls this once per
	// emulated second.
	// Values count in binary (the boot's clock-sync completed with a
	// binary-counting seconds field; a BCD experiment stalled it).
	void rtc_second_tick()
	{
		auto bump = [this](unsigned loc, u8 wrap) -> bool {
			rtc[loc] = u8(rtc[loc] + 1);
			if (rtc[loc] < wrap)
				return false;
			rtc[loc] = 0;
			return true;
		};
		if (!bump(0, 60)) return;         // second
		if (!bump(2, 60)) return;         // minute
		if (!bump(4, 24)) return;         // hour
		rtc[6] = u8((rtc[6] + 1) % 7);    // day of week
		if (!bump(7, 31)) return;         // day of month (approximate)
		if (!bump(8, 12)) return;         // month
		bump(9, 100);                     // year
	}
	// The board's non-volatile memory. The boot reads it out in full at
	// powerup -- four thousand accesses, descending through complemented
	// addresses -- so this is where its configuration lives. What the
	// system expects to find is not known; an erased part reads 0xff.
	u8 nvm[2048] = {};
	void fill_nvm(u8 v) { for (u8 &b : nvm) b = v; }
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
		switch (n)
		{
		case 0:  v = fr0; fr0 = 0; break;
		// Status: bit 7 selects the direct RTC read flavor in the
		// driver's exchange helper (trap seg 0x48066); bit 0 is the
		// keyboard-interrupt-pending bit from the manual.
		case 1:  v = fr1_base | (fr0 ? 1 : 0); break;
		case 3:  v = (was_reset && reset_clears_id) ? 0 : id; break;
		// RTC data, complemented on the bus; reads auto-increment the
		// location (manual 2-34). The rollover triple-read never relies
		// on the increment because it re-latches every field.
		case 9:
			v = u32(~rtc[latch12 & 63]) & 0xff;
			latch12 = (latch12 & ~63u) | ((latch12 + 1) & 63);
			break;
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
		case 13: v = nvm[(~latch12) & 2047]; break;
		default: v = scratch[n & 15]; break;
		}
		log_fr("[sc%u R fr%u=%04x]\n", pa, n, v & 0xffff);
		return v;
	}
	bool input_ready() const { return fr0 != 0; }
	// DMA: the display's frame data arrives this way (the 520 manual:
	// the DIM frame buffer is written "in DMA or direct I/O modes", and
	// the boot starts PA 0 DMA with SDC). The card is always ready.
	bool service_request() override { return true; }
	// The interface poll must be quiet on this board: the self test
	// checks that nothing is requesting service between its steps
	// (escape 7). Transfers are a separate line, above.
	bool poll_request() override { return false; }
	void dma_write(u8 c) override
	{
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
		log_fr("[sc%u W fr%u=%04x]\n", pa, n, v & 0xffff);
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
		if (n == 9)
			return;             // command parameter; acted on silently
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
			rtc[latch12 & 63] = u8(~v);
			latch12 = (latch12 & ~63u) | ((latch12 + 1) & 63);
			return;
		case 12:           // RTC location / complemented NVM address.
			// Just an address latch: it does NOT interrupt. (A legacy
			// raise here kept a stale request queued at the keyboard's
			// level, and the real keystroke's request then made no fresh
			// edge for the host to forward.)
			latch12 = v & 0xffff;
			out.push_back(char(v & 0xff));
			log_fr("[%u:%02x]", pa, v & 0xff);
			return;
		case 13:           // NVM data byte
			nvm[(~latch12) & 2047] = u8(v);
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
		log("[probe%u R fr%u = %04x]\n", pa, n, v);
		return v;
	}
	void write_fr(unsigned n, u32 v) override
	{
		if (n == 0)
			m_fr1_reads = 0;
		if (n == 1 || n == 8)
			was_reset = true;
		fr[n & 15] = v & 0xffff;
		log("[probe%u W fr%u = %04x]\n", pa, n, v & 0xffff);
	}
};

} // namespace focus

#endif // MAME_IOP_FOCUSCARDS_H
