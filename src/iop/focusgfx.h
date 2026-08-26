// license:BSD-3-Clause
// copyright-holders:
/***************************************************************************

    The Series 500 graphics display interface, select code 1.

    Nothing in the collection documents this interface -- the DIM passes
    graphics data straight through to the display (HARDWARE.md 2.7.3), so
    the register meanings had to be read off HP's own software. What is
    modelled here is what the boot demands before a display driver will
    bind, and every number in it was measured rather than assumed.

    The one register that matters at bring-up is FR1, read three times by
    three different pieces of system software, each wanting something
    different from the same word:

      SYSTEM POWERUP +0x525a   requires FR1 & 0x0F30 == 0 before it will
                               treat the slot as a generic interface. A
                               display FAILS this deliberately: the scan
                               skips it and leaves its bit CLEAR in the
                               present bitmap at DB+152, which is exactly
                               what the kernel's typing pass then keys on.
                               A card that echoes the scan's FR0 pattern
                               instead gets registered, its bit set, and
                               is skipped by the typing pass forever --
                               which is why a logging card parked here
                               never bound a driver no matter what
                               identity it answered.
      SUN OS KERNEL  +0x1df6   types the device. Bits 4-5 pick a four-arm
                               dispatch; on arm 0, bits 8-11 are a device
                               CLASS: 4 and 8 give device type 16, 10
                               gives 17.
      SUN OS KERNEL  +0x28d6   bit 6 is READY. Clear spins ~598,700 times
                               and times out.
      SUN OS KERNEL  +0x240e   bit 7 must be SET, or the kernel resets the
                               interface (FR1 = 6, FR1 = 1) and escapes.

    0x04c0 satisfies all four at once, and HP98770 -- the internal display
    driver, segment 43 of the 500ENV diagnostics environment -- binds only
    on device type 16 or 17.

    Traffic to the display itself is FR0 and FR1 with the interface
    address's IC field selecting between them (the kernel's register
    primitives, kernel entries 17 and 18, build their operand as
    `select code | register << 6`), so a command goes to FR1 and its data
    to FR0, with RIFG -- the interface flag -- as the completion
    handshake. The command set is not modelled yet; every command is
    logged and acknowledged so that the conversation can be read off a
    run.

***************************************************************************/

#ifndef MAME_IOP_FOCUSGFX_H
#define MAME_IOP_FOCUSGFX_H

#pragma once

#include "focusiop.h"

#include <cstdio>
#include <functional>
#include <string>

namespace focus {

struct gfx_card : public iop_interface
{
	// The 98770A's raster, which is what HP98770's own bind plants as the
	// maximum X and Y (559 and 454) before it computes the aspect ratio.
	static constexpr unsigned WIDTH  = 560;
	static constexpr unsigned HEIGHT = 455;

	// What FR1 answers. See the header comment: this single word is read
	// by four separate gates and 0x04c0 is the value that passes all of
	// them -- class 4 (device type 16) in bits 8-11, ready in bit 6, and
	// bit 7 set.
	u32 fr1 = 0x04c0;
	u32 id = 0;             // FR3. The powerup scan needs the low byte zero.
	unsigned pa = 1;
	iop_core *iop = nullptr;
	knob::device_log log;

	// Where the processor was, for the protocol log. An undocumented
	// interface is read by knowing which routine asked, not which
	// register it asked for.
	std::function<std::string()> where;
	std::string site() const
	{ return (where && log) ? where() : std::string(); }

	u32 status() override { return 1; }
	// RIST, the interface status. HP's own display self test (PWP SELF
	// TEST local entry 21) reads it first and escapes with code 1 unless
	// it is ODD.
	u32 status_reg() override { return 1; }

	// RIFG, the interface flag, is the command handshake, and the self
	// test pins down both of its states in four instructions: it writes
	// FR1 = 0 and requires the flag to read EVEN straight afterwards
	// (escape 2 if not), then writes a real command and waits for it.
	// So FR1 = 0 is a reset that drops the flag, and a command raises it
	// again when the card has taken it -- which, with nothing to
	// simulate, is immediately.
	u32 flag() override { return m_ready ? 1 : 0; }

	u32 read_fr(unsigned n) override
	{
		u32 v = 0;
		if (n == 1)
			v = fr1;
		else if (n == 3)
			v = id;
		else if (n == 0)
			v = m_data;
		log("[gfx R fr%u = %04x%s]\n", n, v, site().c_str());
		return v;
	}

	void write_fr(unsigned n, u32 v) override
	{
		v &= 0xffff;
		log("[gfx W fr%u = %04x%s]\n", n, v, site().c_str());
		if (n == 1)
		{
			m_command = v;      // the command port
			m_ready = v != 0;   // 0 is the reset; see flag()
			// Completion is an interrupt, not a poll: the kernel's wait
			// (entry 21) arms the device record and blocks on a
			// semaphore that only the interrupt service posts, so a card
			// that never raises leaves the self test timing out. The
			// reset is not a command and does not complete.
			if (m_ready && iop)
				iop->raise(pa);
		}
		else if (n == 0)
			m_data = v;         // its data
	}

	// Bulk transfers arrive the way the alphanumeric card's text does.
	// Nothing interprets them yet; the count is what says whether the
	// driver got as far as sending pixels.
	void dma_write16(u16 v) override { m_data = v; m_dma++; }
	void dma_write(u8 v) override { m_data = v; m_dma++; }
	bool service_request() override { return true; }

	// The interface poll is separate from the service request, and the
	// self test is explicit about it: straight after the reset it reads
	// IFPL and escapes with code 3 unless this card's bit is CLEAR. So a
	// quiet card does not poll -- the default, which chains poll_request
	// to service_request, had it asking for attention forever.
	bool poll_request() override { return m_poll; }

	u32 last_command() const { return m_command; }
	unsigned dma_words() const { return m_dma; }

private:
	u32 m_command = 0;
	u32 m_data = 0;
	unsigned m_dma = 0;
	bool m_ready = true;
	bool m_poll = false;
};

} // namespace focus

#endif // MAME_IOP_FOCUSGFX_H
