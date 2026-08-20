// license:BSD-3-Clause
// copyright-holders:
/***************************************************************************

    The Series 500 alphanumeric display, select code 0.

    What the boot does to this card, read off a running system:

      FR5   a register file written as (index, 0x100 | value) pairs. The
            values are a 6845 CRTC's: horizontal total 0x72, horizontal
            displayed 0x50 (80 columns), sync position 0x61, sync width
            7, vertical total 0x20, adjust 3, vertical displayed 0x1a
            (26 rows), sync position 0x1e, interlace 0, scan lines per
            character 0x0e, cursor start 0x4c and end 0x0c (blinking),
            display start 0, cursor address 0x0820.
      FR7   the frame buffer address the next transfer writes to,
            counted in character cells: the boot's screen updates set
            0x0730, 0x0640, 0x0780, 0x0690 -- rows 23, 20, 24 and 21 of
            an 80 column screen -- before each write.
      FR1   0x80 before a DMA transfer, 0 or 1 around direct writes.
      FR0   a character cell written directly, attribute in the high
            byte and code in the low one. The powerup clear writes 2400
            of them, which is the whole buffer, and the system's panic
            reporter writes its text the same way with 0x70 as the
            attribute.

    and the text itself arrives as halfword output DMA, which lands in
    the buffer at the FR7 address exactly as the direct writes do.

***************************************************************************/

#ifndef MAME_IOP_FOCUSDIM_H
#define MAME_IOP_FOCUSDIM_H

#pragma once

#include "focusiop.h"
#include "focusfont.h"

#include <cstdio>
#include <cstdlib>
#include <functional>

namespace focus {

struct dim_card : public iop_interface
{
	static constexpr unsigned COLUMNS = 80;
	static constexpr unsigned CELLS   = 4096;   // the address wrap

	u16 vram[CELLS] = {};
	u8 crtc[16] = {};
	u32 id = 6;             // the identity the alphanumeric driver binds
	unsigned pa = 0;
	bool log_fr = false;
	u32 fr1 = 0x80;         // what the powerup self test reads
	bool log_cells = false;
	// A host that wants to know which code put a particular message on
	// the screen watches every cell going by.
	std::function<void(u16)> on_cell;

	// The card powers up with a blank buffer; the boot clears it anyway.
	dim_card()
	{
		for (u16 &c : vram)
			c = 0x2020;
	}

	u32 status() override { return 1; }   // status line asserted
	u32 flag() override { return 1; }     // done
	// The display driver reads the interface status register between
	// setting the CRTC up and writing the screen, and goes no further
	// while it reads zero.
	u32 status_reg() override { return 1; }

	u32 read_fr(unsigned n) override
	{
		u32 v = 0;
		// The powerup scan writes a pattern to FR0 and takes a nonzero
		// FR1 as proof something is in the slot; the self test writes
		// FR0 and reads it back, which the frame buffer answers from
		// the cell the write landed in.
		if (n == 1)
			v = 0x80;
		else if (n == 3)
			v = id;

		if (log_fr)
			std::printf("[dim R fr%u=%04x]\n", n, v);
		return v;
	}

	void write_fr(unsigned n, u32 v) override
	{
		if (log_fr)
			std::printf("[dim W fr%u=%04x]\n", n, v & 0xffff);
		switch (n)
		{
		case 0:                     // a character cell, written directly
			put(u16(v));
			break;
		case 5:                     // CRTC: index, then 0x100 | value
			if (v & 0x100)
				crtc[m_index & 15] = u8(v);
			else
				m_index = u8(v);
			break;
		case 7:                     // where the next transfer writes
			m_addr = v & (CELLS - 1);
			break;
		default:
			break;                  // FR1 mode bits, FR8 reset, ...
		}
	}

	// Text arrives as halfword output DMA into the same write pointer.
	bool service_request() override { return true; }
	void dma_write16(u16 v) override { put(v); }
	void dma_write(u8 v) override { put(u16(0x2000 | v)); }

	// What the screen shows: the CRTC's display start (R12/R13) plus the
	// cell offset, and its cursor address (R14/R15).
	u16 cell(unsigned offset) const
	{ return vram[(start() + offset) & (CELLS - 1)]; }
	unsigned start() const { return ((crtc[12] << 8) | crtc[13]) & (CELLS - 1); }
	unsigned cursor() const { return ((crtc[14] << 8) | crtc[15]) & (CELLS - 1); }
	unsigned rows() const { return crtc[6] ? crtc[6] : 26; }
	// Cursor start register bit 5 blanks the cursor, bits 6-5 = 3 blinks
	// it; the shape is bits 0-4 of R10 through R11.
	bool cursor_visible() const { return (crtc[10] & 0x20) == 0 || (crtc[10] & 0x40) != 0; }

private:
	void put(u16 v)
	{
		if (log_cells)
		{
			std::printf("[dim %04x <- %04x %c]\n", m_addr, v,
					(v & 0xff) >= 32 && (v & 0xff) < 127 ? char(v & 0xff) : '.');
			std::fflush(stdout);   // MAME buffers stdout; keep the log ordered
		}
		if (on_cell)
			on_cell(v);
		vram[m_addr] = v;
		m_addr = (m_addr + 1) & (CELLS - 1);
	}

	unsigned m_addr = 0;
	u8 m_index = 0;
};

} // namespace focus

#endif // MAME_IOP_FOCUSDIM_H
