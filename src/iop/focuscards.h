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
#include <string>

namespace focus {

// The console. The error reporter writes characters to FR12 at select
// code 0; the console driver does the same at select code 6, then polls
// STATUS/FLAG and FR9 for completion. Always ready, never busy, and each
// completed character raises the card's interrupt so the driver's wait
// queues drain.
struct console_card : public iop_interface
{
	std::string out;
	std::string report;    // panic text, see write_fr
	// The powerup device scan (image offset 0x656) reads FR3 and only
	// counts a card present when it reads zero. A card answering exactly
	// 9 first gets a reset -- WIF1 then WIF8, opcodes 012 and 0102 -- and
	// has to read zero afterwards, so 9 is the one identity this powerup
	// knows how to quiesce. Answering 4, nothing is registered at all.
	// --consoleid drives that experiment; see the comment at the flag.
	u32 id = 4;
	bool was_reset = false;
	u32 fr0 = 0;               // pending input: 0 = none, else keycode
	iop_core *iop = nullptr;
	unsigned pa = 0;
	u32 status() override { return 1; }   // status line asserted
	u32 flag() override { return 1; }     // done
	// The 520 service manual (sheet C-1 error codes, "Theory of
	// Operation" 2-32) names what lives behind these registers on the
	// keyboard interface: bus test latches whose input and output are
	// both on the bus, exercised at powerup by writing FR12 and reading
	// the value back through FR9 -- the error codes for this are "FR12
	// failed loopback" and "FR9 failed loop back". The visible powerup
	// "character ramp" is that test, not display output.
	u32 latch12 = 0;
	// Response digits for FR9 reads (--resp9): each read consumes the
	// next digit of the string; the last repeats. The driver reads a
	// response as two decimal digits, d1*10+d2, and routes on the value
	// (trap seg 0x480ce), so a mixed value needs a sequence.
	std::string resp9 = "0";
	size_t resp9_i = 0;
	u32 read_fr(unsigned n) override
	{
		switch (n)
		{
		case 0:  { const u32 v = fr0; fr0 = 0; return v; }
		case 1:  return 0x80;   // ready to accept a character
		case 3:  return was_reset ? 0 : id;
		// FR9 is the command-response register of the SCM-style console
		// protocol (trap segment ~0x48060): the driver reads it, takes
		// ~FR9 & 15 as one decimal digit of a two-digit response value.
		// 0xff is digit 0. The powerup loopback ramp through FR12
		// tolerates any value; an echo instead loops the command
		// exchanges forever on their first command.
		case 9: {
			const u32 d = u32(resp9[resp9_i] - '0');
			if (resp9_i + 1 < resp9.size())
				resp9_i++;
			return (~d) & 0xff;
		}
		default: return 0;
		}
	}
	bool input_ready() const { return fr0 != 0; }
	bool attention() override { return fr0 != 0 && !ard; }
	bool ard = false;      // attention request disable, set by the IOP's poll
	void type(u8 c)
	{
		fr0 = c;
		ard = false;       // a fresh keystroke re-asserts ASYNC
	}
	void write_fr(unsigned n, u32 v) override
	{
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
				std::printf("%c", (c >= 32 && c < 127) ? c : '.');
				std::fflush(stdout);
			}
			return;
		}
		if (n == 9)
			return;             // command parameter; acted on silently
		
		if (n != 12)
			return;
		latch12 = v & 0xff;
		const char c = char(v & 0xff);
		out.push_back(c);
		std::printf("[%u:%02x]%c", pa, v & 0xff,
				(c >= 32 && c < 127) ? c : '.');
		std::fflush(stdout);
		if (iop)
			iop->raise(pa);
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
