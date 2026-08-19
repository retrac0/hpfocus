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
	bool log_fr = false;       // trace every register access
	// Where display characters go: the panic reporter (and, on the
	// same evidence so far, the console output path) writes them to
	// FR0 with 0x70 in the high byte; a host that renders a screen
	// hooks this.
	std::function<void(u8)> on_char;
	u32 status() override { return 1; }   // status line asserted
	u32 flag() override { return 1; }     // done
	u32 latch12 = 0;           // last R12 write: RTC location / NVM address
	// All-zero powers up as "midnight, clock never set", which the
	// driver accepts. Control register 10's bit 7 is a busy bit the
	// driver polls until CLEAR before each rollover-consistent read
	// burst (trap seg 0x484f8: it spins on FR12=0x0a / ~FR9 while bit 7
	// is set), so it must stay 0 here.
	u8 rtc[16] = {};
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
	u8 nvm[2048] = {};
	u32 keycode = 0;           // holding register behind R10
	u32 read_fr(unsigned n) override
	{
		u32 v = 0;
		switch (n)
		{
		case 0:  v = fr0; fr0 = 0; break;
		// Status: bit 7 selects the direct RTC read flavor in the
		// driver's exchange helper (trap seg 0x48066); bit 0 is the
		// keyboard-interrupt-pending bit from the manual.
		case 1:  v = 0x80 | (fr0 ? 1 : 0); break;
		case 3:  v = (was_reset && reset_clears_id) ? 0 : id; break;
		// RTC data, complemented on the bus; reads auto-increment the
		// location (manual 2-34). The rollover triple-read never relies
		// on the increment because it re-latches every field.
		case 9:
			v = u32(~rtc[latch12 & 15]) & 0xff;
			latch12 = (latch12 & ~15u) | ((latch12 + 1) & 15);
			break;
		case 10: v = keycode; keycode = 0; break;
		case 13: v = nvm[(~latch12) & 2047]; break;
		default: break;
		}
		if (log_fr)
		{
			std::printf("[kbd R fr%u=%04x]", n, v & 0xffff);
			std::fflush(stdout);
		}
		return v;
	}
	bool input_ready() const { return fr0 != 0; }
	// DMA: the display's frame data arrives this way (the 520 manual:
	// the DIM frame buffer is written "in DMA or direct I/O modes", and
	// the boot starts PA 0 DMA with SDC). The card is always ready.
	bool service_request() override { return true; }
	void dma_write(u8 c) override
	{
		if (on_char)
			on_char(c);
		std::printf("%c", (c >= 32 && c < 127) ? c : '.');
		std::fflush(stdout);
	}
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
	void type(u8 c)
	{
		fr0 = c;
		keycode = c;
		ard = false;       // a fresh keystroke re-asserts ASYNC
	}
	void write_fr(unsigned n, u32 v) override
	{
		if (log_fr)
		{
			std::printf("[kbd W fr%u=%04x]", n, v & 0xffff);
			std::fflush(stdout);
		}
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
		
		switch (n)
		{
		case 9:            // RTC data write to the latched location,
			// complemented like the reads (the OS's clock-set sweep at
			// trap seg ~0x480fc writes every location through here).
			rtc[latch12 & 15] = u8(~v);
			latch12 = (latch12 & ~15u) | ((latch12 + 1) & 15);
			return;
		case 12:           // RTC location / complemented NVM address
			latch12 = v & 0xffff;
			out.push_back(char(v & 0xff));
			if (log_fr)
			{
				std::printf("[%u:%02x]", pa, v & 0xff);
				std::fflush(stdout);
			}
			if (iop)
				iop->raise(pa);
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
	bool log = false;
	bool was_reset = false;
	u32 fr[16] = {};
	u32 status() override { return 0; }
	u32 flag() override { return 1; }
	u32 read_fr(unsigned n) override
	{
		// The powerup scan's presence test: FR3 must read zero after the
		// WIF1/WIF8 reset, then it writes a bit pattern to FR0 and
		// expects FR1 to read back nonzero. An interface that echoes
		// satisfies it. An identity of 9 is the one the scan resets, so
		// --probe 5 9 is the way to get a card registered and watch what
		// the drivers then do with it.
		u32 v;
		if (n == 3)      v = was_reset ? 0 : id;
		else if (n == 1) v = fr[0];
		else             v = fr[n & 15];
		if (log)
			std::printf("[probe%u R fr%u = %04x]\n", pa, n, v);
		return v;
	}
	void write_fr(unsigned n, u32 v) override
	{
		if (n == 1 || n == 8)
			was_reset = true;
		fr[n & 15] = v & 0xffff;
		if (log)
			std::printf("[probe%u W fr%u = %04x]\n", pa, n, v & 0xffff);
	}
};

} // namespace focus

#endif // MAME_IOP_FOCUSCARDS_H
