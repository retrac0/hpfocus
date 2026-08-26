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
    handshake.

    ---------------------------------------------------------------------

    THE COMMAND SET, read off PWP SELF TEST.

    HP's power-up self test for the display is segment 13 of the boot
    image -- 3,712 bytes, the smallest segment there is, and the machine's
    own specification of hardware no manual in the collection describes.
    Its local entries 17-32 are the display test; `tools/mkdisasm.sh`
    extracts it, and a local at `PB+X` is at `X - 0x24` in the extracted
    .bin. Three one-line primitives carry every access:

      local 17  PB+0x6e8   write FR1 -- the command          escape 10
      local 18  PB+0x704   write FR0 -- its data             escape 11
      local 19  PB+0x720   read  FR0 -- the read-back        escape 12
      local 20  PB+0x73c   fill a buffer, capped at 12,288 bytes

    and every command below is an `LDI` constant sitting immediately in
    front of a `PCLS STT[17]`:

      0x100  0 data   reset, phase 1                  entry 21
      0x180  0 data   reset, phase 2                  entry 21
      0x181  4 data   region: (a, x, y0|f, y1|f)      local 23 PB+0x830
      0x186  1 data   set mode, type 17               local 29
      0x187  2 data   write mask, type 17 (0, 0)      local 25
      0x188  -        WRITE memory, type 17 (DMA out) local 27
      0x189  -        READ memory, type 17 (DMA in)   locals 26, 29
      0x18a  1 data   plane select, 0x38 | 1<<plane   locals 29, 31
      0x18d  0 data   [unknown] before a read         local 30
      0x1a0  -        WRITE memory, type 16 (DMA out) local 27 PB+0x91c
      0x1a1  -        READ memory, type 16 (DMA in)   local 26, entry 32
      0x1a8  1 data   write mask A, type 16 (-1)      local 25 PB+0x8b0
      0x1a9  1 data   write mask B, type 16 (-1)      local 25
      0x1ab  1 data   set mode, type 16               local 31
      0x1ad  4 data   window: 1023, 1023, 2047, 2031  local 31

    The 16/17 pairing is systematic -- 0x186 to 0x1ab, 0x187 to
    0x1a8+0x1a9, 0x188 to 0x1a0, 0x189 to 0x1a1 -- so bit 5 of the low
    byte reads as the colour display's flavour of the same order.
    [inferred: four pairs, no counter-example, but nothing states it]

    THE RESET, entry 21 (PB+0x764), instruction for instruction. Note the
    order: the interface poll is checked AFTER the first wait, and the
    escape codes run 1, 2, 4, 3, 5:

      RIST odd                             else escape 1
      FR1 = 0    then RIFG even            else escape 2
      FR1 = 0x100 then wait (kernel 19)    else escape 4
      IFPL this PA's bit clear             else escape 3
      FR1 = 0x180 then wait (kernel 21)    else escape 5

    So FR1 = 0 is a reset that drops the flag and a command raises it
    again; the interface poll is separate from the service request, which
    is where the emulator's default (poll chained to service) had the card
    asking for attention forever. Completion is an INTERRUPT: the kernel's
    wait arms the device record and blocks on a semaphore that only the
    interrupt service posts, so a card that never raises leaves every
    command timing out.

    HOW BIG THE MEMORY IS. Two length constants, one per display type,
    and both come out exact:

      type 17, the 98780A mono display   LDI #31850 = 560 * 455 / 8
                                         one plane, 35 words per line
      type 16, the 98770A colour display LDI #98304 = 3 * 32,768
                                         three planes of 16,384 words;
                                         455 lines of 36 words is 16,380,
                                         four words spare

    36 words is 576 pixels of which 560 are displayed -- the same 576x455
    the 97060A documents (HARDWARE.md 2.7.5), and the 16-pixel margin
    0x1ad's 2031 = 2047 - 16 also names. Three planes is the 9020C's
    "8 pure" colours.

    WHERE THE ADDRESS POINTER COMES FROM. The self test issues 0x1a0 and
    0x1a1 with no address of any kind, and then transfers exactly the
    whole memory -- 98,304 bytes for three planes, 31,850 for one. So the
    read/write command itself rewinds the pointer to the start, and every
    word transferred advances it. Measured by the sizes agreeing to the
    byte, not assumed.

    Planes are interleaved a word at a time rather than stored one after
    another, which entry 32 forces: see the test-pattern note below.
    [inferred from the read order; the DMA totals fit either layout]

    THE MEMORY TEST AND ITS PATTERNS. This is entry 32 (PB+0xab4), the
    check that escapes 8, and it does something a plain memory does not
    answer:

      reset; DMA 98,304 bytes out and back (0x5555AAAA, then inverted)
      DMA 98,304 bytes of ZERO out
      0x18a/0x1ab/0x1ad for plane 0 mode n=0, plane 1 n=1, plane 2 n=3
      0x1a8 = 0x1a9 = -1
      0x1a1, then three FR0 reads: 0xAAAA, 0xCCCC, 0xF0F0

    Memory was just filled with zero, so those three words are generated,
    not stored -- which is the display's own self test, entered "by
    pushbutton S1 or by software" and emitting a test pattern
    (HARDWARE.md 2.7.3). Two things disambiguate what selects the
    pattern, and both come from the type-17 test, local 29 (PB+0x960),
    which holds the plane at 0 and varies the rest:

      0x181 x = 1, mode n = 0  ->  0x5555     the x offset ROTATES it
      0x181 x = 0, mode n = 0  ->  0xAAAA
      0x181 x = 0, mode n = 1  ->  0xCCCC     the mode SELECTS it
      0x181 x = 0, mode n = 3  ->  0xF0F0

    so the pattern is a function of the mode alone and the region's x
    rotates it. Entry 32 then reads three words after setting three
    planes to three modes and expects one pattern from each, in plane
    order, which is what says the read walks planes before addresses.
    The mode's low five bits are 17 in every one of these; HP98770, the
    real driver, never issues 0x1ab or 0x18a at all, so a display in
    ordinary use is never in this mode and reads its real memory.

    THE DRIVER'S OWN COMMANDS. PWP SELF TEST never issues any of these;
    they were read off HP98770 (segment 43) and HUMAN INTERFACE (segment
    7) driving a live machine, with FOCUS_SC1LOG naming the caller:

      0x12a  1 data   pen colour, a plane mask: 0x3f is all three
      0x12b  1 data   background colour, 0x38 is none
      0x12d  2 data   a point of a polyline -- see vector() below
      0x280  0 data   the driver's idle poll, once every few frames
      0x402  1 data   0x400, and 0x403 with 0x3b0, are the pair HUMAN
      0x403  1 data   INTERFACE writes at boot before its first reset

    and 0x12c, 0x12e, 0x12f and 0x400 go by at boot carrying nothing.

    WHAT IS STILL UNREAD. 0x1ad's 1023/1023/2047/2031 fit a window, a
    clip rectangle and a pan origin equally well; 0x1ab's mode is only
    ever seen carrying 17; 0x402/0x403's two numbers are unaccounted for;
    and nothing anywhere touches a colour map. Those are named and
    logged, not interpreted. A command that is not in the table at all
    raises a knob::alarm, so the next one to appear says so itself.

***************************************************************************/

#ifndef MAME_IOP_FOCUSGFX_H
#define MAME_IOP_FOCUSGFX_H

#pragma once

#include "focusiop.h"

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>

namespace focus {

struct gfx_card : public iop_interface
{
	// The 98770A's raster, which is what HP98770's own bind plants as the
	// maximum X and Y (559 and 454) before it computes the aspect ratio.
	static constexpr unsigned WIDTH  = 560;
	static constexpr unsigned HEIGHT = 455;

	// The memory behind it, measured off the self test's two transfer
	// lengths. STRIDE_WORDS * 16 is 576 pixels, sixteen more than are
	// displayed; the mono display is one plane of 35 words instead.
	static constexpr unsigned MAX_PLANES  = 3;
	static constexpr unsigned PLANE_WORDS = 16384;      // 32 KB
	static constexpr unsigned STRIDE_WORDS = 36;        // 576 px
	static constexpr unsigned MONO_STRIDE_WORDS = 35;   // 560 px

	// What FR1 answers. See the header comment: this single word is read
	// by four separate gates and 0x04c0 is the value that passes all of
	// them -- class 4 (device type 16) in bits 8-11, ready in bit 6, and
	// bit 7 set.
	u32 fr1 = 0x04c0;
	u32 id = 0;             // FR3. The powerup scan needs the low byte zero.
	unsigned pa = 1;
	iop_core *iop = nullptr;
	knob::device_log log;

	// How many planes are fitted. Three is the 98770A colour display,
	// which is what FR1's class nibble above claims to be; one is the
	// 98780A monochrome one, whose self test wants 31,850 bytes instead.
	unsigned planes = MAX_PLANES;
	unsigned stride = STRIDE_WORDS;
	unsigned origin = 0;    // word offset the visible raster starts at

	// The card the emulator had before the command set was read: every
	// command acknowledged and logged, no memory behind any of it. Kept
	// as the A/B control for anything the memory changes.
	bool nomem = false;

	u16 vram[MAX_PLANES][PLANE_WORDS] = {};

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
			v = get_word();
		log("[gfx R fr%u = %04x%s]\n", n, v, site().c_str());
		return v;
	}

	void write_fr(unsigned n, u32 v) override
	{
		v &= 0xffff;
		log("[gfx W fr%u = %04x%s]\n", n, v, site().c_str());
		if (n == 1)
		{
			// Every command the self test and the driver issue has bit 8
			// set. FR1 = 0 is the reset that drops the flag; the kernel's
			// escape path writes FR1 = 6 and FR1 = 1, and those are
			// neither commands nor completions -- raising on them, which
			// the acknowledge-everything card did, would post an
			// interrupt for a hand-off that never happened.
			if (v == 0)
				reset();
			else if (v >= 0x100)
				command(v);
		}
		else if (n == 0)
			put_word(v);
	}

	// Bulk transfers arrive the way the alphanumeric card's text does.
	// The IOP picks byte or halfword from the channel's status word, so
	// both land in the same word stream; a byte transfer fills the high
	// half first, which is the order the halfword one would have used.
	void dma_write16(u16 v) override { m_dma++; put_word(v); }
	u16 dma_read16() override { m_dma++; return u16(get_word()); }

	void dma_write(u8 v) override
	{
		if (m_half)
			put_word(u16((m_byte << 8) | v));
		else
			m_byte = v;
		m_half = !m_half;
		m_dma++;
	}

	u8 dma_read() override
	{
		if (!m_half)
			m_byte = u16(get_word());
		m_half = !m_half;
		m_dma++;
		return u8(m_half ? (m_byte >> 8) : m_byte);
	}

	bool service_request() override { return true; }

	// The interface poll is separate from the service request, and the
	// self test is explicit about it: straight after the reset it reads
	// IFPL and escapes with code 3 unless this card's bit is CLEAR. So a
	// quiet card does not poll -- the default, which chains poll_request
	// to service_request, had it asking for attention forever.
	bool poll_request() override { return m_poll; }

	u32 last_command() const { return m_command; }
	unsigned dma_words() const { return m_dma; }
	unsigned plane_words() const { return planes * PLANE_WORDS; }
	// How far the current transfer has got, in words. The only measure of
	// a leg the register traffic gives.
	unsigned pointer() const { return m_addr; }

	// What the screen shows: one pixel's plane bits, most significant bit
	// of a word leftmost. Outside the raster is black rather than
	// whatever the sixteen undisplayed columns of each line hold.
	unsigned pixel(unsigned x, unsigned y) const
	{
		if (x >= WIDTH || y >= HEIGHT)
			return 0;
		const unsigned w = origin + y * stride + (x >> 4);
		if (w >= PLANE_WORDS)
			return 0;
		const unsigned bit = 15 - (x & 15);
		unsigned v = 0;
		for (unsigned p = 0; p < planes; p++)
			v |= ((vram[p][w] >> bit) & 1) << p;
		return v;
	}

private:
	enum class phase { idle, params, writing, reading };

	// One row per command the self test or the driver issues. `params` is
	// how many FR0 data words follow; `next` is what the card is doing
	// afterwards. Keeping it a table rather than a switch is deliberate:
	// this list IS the specification, and it should read like the one in
	// the header comment.
	struct cmdspec { u16 code; const char *name; u8 params; phase next; };

	static const cmdspec *lookup(u16 code)
	{
		static const cmdspec TABLE[] = {
			{ 0x100, "reset1",   0, phase::idle    },
			{ 0x12a, "pen",      1, phase::idle    },
			{ 0x12b, "bg",       1, phase::idle    },
			{ 0x12c, "cmd12c",   0, phase::idle    },
			{ 0x12d, "vector",   2, phase::idle    },
			{ 0x12e, "cmd12e",   0, phase::idle    },
			{ 0x12f, "cmd12f",   0, phase::idle    },
			{ 0x180, "reset2",   0, phase::idle    },
			{ 0x181, "region",   4, phase::idle    },
			{ 0x186, "mode17",   1, phase::idle    },
			{ 0x187, "mask17",   2, phase::idle    },
			{ 0x188, "write17",  0, phase::writing },
			{ 0x189, "read17",   0, phase::reading },
			{ 0x18a, "plane",    1, phase::idle    },
			{ 0x18d, "cmd18d",   0, phase::idle    },
			{ 0x1a0, "write16",  0, phase::writing },
			{ 0x1a1, "read16",   0, phase::reading },
			{ 0x1a8, "maskA",    1, phase::idle    },
			{ 0x1a9, "maskB",    1, phase::idle    },
			{ 0x1ab, "mode16",   1, phase::idle    },
			{ 0x1ad, "window",   4, phase::idle    },
			{ 0x280, "poll",     0, phase::idle    },
			{ 0x400, "cmd400",   0, phase::idle    },
			{ 0x402, "cmd402",   1, phase::idle    },
			{ 0x403, "cmd403",   1, phase::idle    },
		};
		for (const cmdspec &c : TABLE)
			if (c.code == code)
				return &c;
		return nullptr;
	}

	void reset()
	{
		m_ready = false;
		m_phase = phase::idle;
		m_params_left = 0;
		m_addr = 0;
		m_half = false;
		// The self test's own order says this is where the display's
		// test-pattern mode goes away: entry 21 writes FR1 = 0 before
		// anything, and entry 32 then sets the three planes' modes and
		// reads patterns from them. HP98770 never issues a mode command
		// at all, so a mode left set would have the driver's own seven
		// reads answering with patterns for the rest of the run.
		for (u16 &m : m_mode)
			m = 0;
	}

	void command(u32 v)
	{
		// How far the pointer got is the only measure of a transfer that
		// the register traffic gives, and it is the one that says whether
		// a leg moved what it was asked to.
		if (log && (m_phase == phase::writing || m_phase == phase::reading))
			log("[gfx   %s ended at word %u]\n",
					m_phase == phase::writing ? "write" : "read", m_addr);
		m_command = v;
		m_ready = true;
		const cmdspec *c = lookup(u16(v));
		if (!c)
		{
			// An undocumented interface is exactly where the emulator
			// should say so rather than quietly acknowledging: a command
			// nobody has seen is the next thing to read off the listing.
			static unsigned complained = 0;
			if (complained < 16)
			{
				complained++;
				knob::alarm("[gfx] unknown command %04x%s\n",
						v, site().c_str());
			}
			m_phase = phase::idle;
			m_params_left = 0;
		}
		else
		{
			m_spec = c;
			m_params_left = c->params;
			m_phase = c->params ? phase::params : c->next;
			// A read or write command rewinds the pointer: the self test
			// issues one and then transfers the whole memory, and the
			// two lengths only come out exact if it starts at zero.
			if (c->next != phase::idle)
			{
				m_addr = 0;
				m_half = false;
			}
			if (log)
				log("[gfx   %s%s]\n", c->name, site().c_str());
		}
		if (iop)
			iop->raise(pa);
	}

	// The parameter words of a command, then the data of a transfer.
	void put_word(u32 v)
	{
		v &= 0xffff;
		m_data = v;
		if (m_params_left)
		{
			const unsigned k = m_spec->params - m_params_left;
			m_params_left--;
			switch (m_spec->code)
			{
			case 0x181: m_region[k] = u16(v); break;
			case 0x12a: m_pen = u16(v) & 7; break;
			case 0x12b: m_bg = u16(v) & 7; break;
			case 0x12d:
				m_coord[k] = u16(v);
				if (k == 1)
					vector(m_coord[0], m_coord[1]);
				break;
			case 0x1ad: m_window[k] = u16(v); break;
			case 0x186: case 0x1ab: m_mode[m_plane] = u16(v); break;
			case 0x18a:
				// 0x38 | 1<<plane. Keep the word as it came as well as
				// the index, because only three of its values have ever
				// been seen and the other bits are unaccounted for.
				m_plane_word = u16(v);
				for (unsigned p = 0; p < MAX_PLANES; p++)
					if (v & (1u << p))
						m_plane = p;
				break;
			case 0x187: case 0x1a8: case 0x1a9:
				m_wmask[k & 1] = u16(v);
				break;
			default: break;
			}
			if (!m_params_left)
				m_phase = m_spec->next;
			return;
		}
		if (nomem || m_phase != phase::writing)
			return;
		const unsigned p = m_addr % planes;
		const unsigned w = m_addr / planes;
		if (w < PLANE_WORDS)
			vram[p][w] = u16(v);
		m_addr++;
		if (log && m_addr <= 8)
			log("[gfx   write[%u] plane %u word %u = %04x]\n",
					m_addr - 1, p, w, v);
	}

	u32 get_word()
	{
		if (nomem || m_phase != phase::reading)
			return m_data;      // the echo a bare card always answered
		const unsigned p = m_addr % planes;
		const unsigned w = m_addr / planes;
		m_addr++;
		const u16 t = test_pattern(m_mode[p]);
		const u16 v = t ? t : (w < PLANE_WORDS ? vram[p][w] : u16(0));
		// The head of a transfer, which is where a leg that has gone
		// wrong shows it.
		if (log && m_addr <= 8)
			log("[gfx   read[%u] plane %u word %u = %04x]\n",
					m_addr - 1, p, w, v);
		return v;
	}

	// One point of a polyline. Both words are the one's complement of a
	// field whose low ten bits are the coordinate and whose bit 10 lifts
	// the pen, and HP98770 emits it from three places that between them
	// pin the encoding down (offsets are into the extracted .bin):
	//
	//   +0x7ea  NOT(((pen_up ^ 1) << 10) | coord)   the caller's own flag
	//   +0x1c12 NOT(coord)                          bit 10 always 0
	//   +0x1a60 1023 - coord                        bit 10 always 1
	//
	// so with u = ~w, the coordinate is u & 0x3ff and the pen is bit 10 of
	// u, set meaning up. FRAME reads out of that as move (454, 0), draw
	// (0, 0), (0, 559), (454, 559), (454, 0) -- the four sides of the
	// 560 x 455 raster, closed -- which is what says the first word is Y
	// and the second X.
	//
	// [inferred] Y grows downward here. The frame HP draws is symmetric
	// and says nothing about it, and nothing in the collection does
	// either; the first asymmetric picture settles it.
	void vector(u16 wy, u16 wx)
	{
		const unsigned uy = u16(~wy), ux = u16(~wx);
		const unsigned y = uy & 0x3ff, x = ux & 0x3ff;
		const bool up = ((uy | ux) & 0x400) != 0;
		if (log)
			log("[gfx   %s %u,%u]\n", up ? "move" : "draw", x, y);
		if (!up && !nomem)
			line(m_x, m_y, x, y);
		m_x = x;
		m_y = y;
	}

	void plot(unsigned x, unsigned y)
	{
		if (x >= WIDTH || y >= HEIGHT)
			return;
		const unsigned w = origin + y * stride + (x >> 4);
		if (w >= PLANE_WORDS)
			return;
		const u16 bit = u16(1u << (15 - (x & 15)));
		for (unsigned p = 0; p < planes; p++)
		{
			if (m_pen & (1u << p))
				vram[p][w] |= bit;
			else
				vram[p][w] &= u16(~bit);
		}
	}

	// Bresenham. What the display's own vector generator does between the
	// endpoints is not observable from the register traffic -- only that
	// it joins them -- so this is the emulator's line, not HP's.
	void line(unsigned x0, unsigned y0, unsigned x1, unsigned y1)
	{
		int x = int(x0), y = int(y0);
		const int dx = std::abs(int(x1) - x), sx = x < int(x1) ? 1 : -1;
		const int dy = -std::abs(int(y1) - y), sy = y < int(y1) ? 1 : -1;
		int err = dx + dy;
		for (;;)
		{
			plot(unsigned(x), unsigned(y));
			if (x == int(x1) && y == int(y1))
				break;
			const int e2 = 2 * err;
			if (e2 >= dy) { err += dy; x += sx; }
			if (e2 <= dx) { err += dx; y += sy; }
		}
	}

	// The display's own self test emits a pattern instead of memory. The
	// mode selects which and the region's x rotates it; see the header
	// comment for the four readings that separate those two.
	u16 test_pattern(u16 mode) const
	{
		if ((mode & 0x1f) != 17)
			return 0;
		u16 pat;
		switch ((mode >> 5) & 7)
		{
		case 0: pat = 0xaaaa; break;
		case 1: pat = 0xcccc; break;
		case 3: pat = 0xf0f0; break;
		default: return 0;      // no reading for the other five
		}
		const unsigned r = m_region[1] & 15;
		return u16((pat >> r) | (pat << (16 - r)));
	}

	u32 m_command = 0;
	u32 m_data = 0;
	unsigned m_dma = 0;
	bool m_ready = true;
	bool m_poll = false;

	const cmdspec *m_spec = nullptr;
	phase m_phase = phase::idle;
	unsigned m_params_left = 0;
	unsigned m_addr = 0;        // the word pointer, plane-interleaved
	u16 m_byte = 0;             // the half of a word a byte transfer holds
	bool m_half = false;

	unsigned m_x = 0, m_y = 0;   // where the pen is
	unsigned m_pen = 7;          // 0x12a, a plane mask: 0x3f is all three
	unsigned m_bg = 0;           // 0x12b
	u16 m_coord[2] = {};

	unsigned m_plane = 0;
	u16 m_plane_word = 0;
	u16 m_mode[MAX_PLANES] = {};
	u16 m_wmask[2] = {};
	u16 m_region[4] = {};
	u16 m_window[4] = {};
};

} // namespace focus

#endif // MAME_IOP_FOCUSGFX_H
