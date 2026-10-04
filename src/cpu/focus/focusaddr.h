// license:GPL-2.0+
// copyright-holders:Joel Heikkila
/***************************************************************************

    HP FOCUS addressing unit

    Effective address computation and segment bounds checking, per section 2.2
    of the Focus Machine Instruction Set ERS (sheets 14-20).

    Every memory reference is bounds-checked by the microcode, so the checking
    lives here in the CPU rather than in an external MMU.

    Kept free of MAME dependencies: memory and external-data-segment lookup
    are abstract interfaces, so this unit tests against a flat array and the
    CPU device plugs in an address_space.

***************************************************************************/

#ifndef MAME_CPU_FOCUS_FOCUSADDR_H
#define MAME_CPU_FOCUS_FOCUSADDR_H

#pragma once

#include "focusknobs.h"
#include "focusstate.h"

#include <algorithm>

#include <cstdlib>
#include <functional>

#include <cstdint>

namespace focus {

using s32 = std::int32_t;

//**************************************************************************
//  TRAPS
//**************************************************************************

// Named for the traps the ERS raises during address evaluation
// (sheets 18, 55-56). The full internal trap list is section 5.2.2.
enum class trap : u8
{
	none = 0,
	bounds_violation,            // outside PB..PL, SB..S or DB..DL
	eds_pointer_violation,       // external data segment number of zero
	dst_violation,               // segment number beyond the DST
	eds_bounds_violation,        // offset outside the external data segment
	data_seg_write_violation,    // store to a segment with W clear
	priv_mode_data_violation,    // unprivileged access to a segment with M set
	absent_segment,
	absent_page_table,
	absent_page,
	page_table_violation,
	unexpected_eds_type
};

//**************************************************************************
//  ABSTRACT INTERFACES
//**************************************************************************

class memory_interface
{
public:
	virtual ~memory_interface() = default;
	virtual u32 read32(u32 addr) = 0;
	virtual void write32(u32 addr, u32 data) = 0;

	// Sub-word access, defaulted in terms of the word accessors so existing
	// implementations need not change. FOCUS is big-endian: byte 0 of a word
	// is its most significant.
	virtual u8 read8(u32 addr)
	{
		return u8(read32(addr & ~3u) >> (8 * (3 - (addr & 3))));
	}

	virtual void write8(u32 addr, u8 data)
	{
		const unsigned sh = 8 * (3 - (addr & 3));
		const u32 w = read32(addr & ~3u);
		write32(addr & ~3u, (w & ~(u32(0xff) << sh)) | (u32(data) << sh));
	}

	virtual u16 read16(u32 addr)
	{
		const u32 w = read32(addr & ~3u);
		return u16((addr & 2) ? (w & 0xffff) : (w >> 16));
	}

	virtual void write16(u32 addr, u16 data)
	{
		const u32 w = read32(addr & ~3u);
		write32(addr & ~3u, (addr & 2) ? ((w & 0xffff0000u) | data)
				: ((w & 0x0000ffffu) | (u32(data) << 16)));
	}
};

// Resolution of an external data segment reference to an absolute address.
//
// The full walk -- Data Segment Table entry, segment chaining, and the paged
// path with its page table -- is specified on ERS sheets 30-36 and 55-56;
// dst_walker (focusdst.h) implements it, and unit tests can plug a double
// in here instead.
class eds_resolver
{
public:
	virtual ~eds_resolver() = default;

	// `system_dst` selects the system rather than user table, per PTR(1:1).
	// Returns false and sets `t` on a violation.
	virtual bool resolve(bool system_dst, u32 segment, u32 offset,
			bool modify, u32 &addr, trap &t) = 0;
	// The parameter an absent-segment/page fault carries (ERS 5.2.2).
	virtual u32 last_trap_param() const { return 0; }
	// How far the last successful resolve() can be walked byte by byte
	// before the physical address stops following the logical one: to
	// the end of the page for a paged segment, to the end of the segment
	// for an unpaged one, counting the resolved byte itself; `back` is
	// the same toward lower addresses.
	virtual void last_run(u32 &fwd, u32 &back) const { fwd = back = 0x7fffffffu; }
	// VPTR's validation of an external pointer: -1 invalid, 0 absent, 1 good.
	virtual int validate(bool, u32, u32, bool, bool) const { return 1; }
};

// The CPU side of the Memory Processor Bus I/O path (FOCUS I/O ERS section 6,
// sheet 43). IOW, IOR and IOC each carry an 8-bit I/O operation code in the
// instruction, which becomes bits 24-31 of the MPB channel address. The rest
// of the address comes from an interface-address word on the stack:
//
//     bits 22-25  IC, the interface command field
//     bits 26-28  the IOP channel number
//     bits 29-31  PA, the peripheral address
//
// A WRITE pops a data word and sends it; a READ pushes the word the IOP
// returns; a CONTROL sends a zero data word which the IOP ignores.
//
// Implementations return false when no IOP answers for that channel, which
// the CPU reports as an illegal I/O opcode.
class io_interface
{
public:
	virtual ~io_interface() = default;

	struct target
	{
		u8 opcode;    // from the instruction
		u8 channel;   // interface address bits 26-28
		u8 pa;        // interface address bits 29-31
		u8 ic;        // interface address bits 22-25
	};

	virtual bool io_write(const target &t, u32 data) = 0;
	virtual bool io_read(const target &t, u32 &data) = 0;
	virtual bool io_control(const target &t) = 0;
};

// Stands in when no IOP is attached, so the core can run without one.
class null_io : public io_interface
{
public:
	virtual bool io_write(const target &, u32) override { return false; }
	virtual bool io_read(const target &, u32 &) override { return false; }
	virtual bool io_control(const target &) override { return false; }
};

//**************************************************************************
//  ADDRESSING
//**************************************************************************

// The BASE field, identical in formats 1 and 6 (ERS sheet 53). The offset is
// always positive; the base selects the direction.
enum base_reg : u8
{
	BASE_P_PLUS = 0, BASE_P_MINUS = 1,
	BASE_DB_PLUS = 2, BASE_DL_MINUS = 3,
	BASE_Q_PLUS = 4, BASE_Q_MINUS = 5,
	BASE_SB_PLUS = 6, BASE_S_MINUS = 7
};

// Which segment a base register addresses, and therefore which bounds apply
// (ERS sheet 18: code PB..PL, stack SB..S, global data DB..DL).
enum class segment : u8 { code, stack, global };

constexpr segment segment_of(base_reg b)
{
	return (b == BASE_P_PLUS || b == BASE_P_MINUS) ? segment::code
			: (b == BASE_DB_PLUS || b == BASE_DL_MINUS) ? segment::global
			: segment::stack;
}

struct ea_result
{
	u32 addr = 0;
	trap t = trap::none;
	// Bytes from `addr` (inclusive) that stay physically contiguous in
	// each direction; see eds_resolver::last_run. A code, stack or
	// global operand is one piece of memory, and its run is the distance
	// to the segment's own bound (addressing_unit::check): the byte
	// after it is not a byte of the segment.
	u32 run_fwd = 0x7fffffffu;
	u32 run_back = 0x7fffffffu;
	bool ok() const { return t == trap::none; }
};

class addressing_unit
{
public:
	addressing_unit(registers &r, memory_interface &mem, eds_resolver &eds)
		: m_r(r), m_mem(mem), m_eds(eds) { }

	// What the last external-data-segment fault should hand its trap
	// handler; see dst_walker::last_trap_param().
	u32 last_trap_param() const { return m_eds.last_trap_param(); }
	int validate(bool system_dst, u32 segment, u32 offset, bool want_write,
			bool want_priv) const
	{ return m_eds.validate(system_dst, segment, offset, want_write, want_priv); }

	// Compute the effective address of a base-relative reference.
	//
	// `modify` marks a modify-memory instruction, which matters for the write
	// permission check and the dirty bit on external data segments.
	//
	// Post-indexing is the default (ERS sheet 57): with both indirect and
	// indexed set, the pointer is resolved first and X is applied to the
	// result. Figure 2.1 confirms this -- LOAD P+20,I,X follows the pointer
	// to P-40 and only then adds X to reach P-20.
	//
	// `extra` is a byte displacement applied to the operand the way an
	// index is -- to the offset within an external data segment, not to
	// the address it resolved to. It is how the second word of a
	// doubleword is reached when the first is the last word of a page:
	// the next page is a reference of its own (see ea_double).
	ea_result effective_address(base_reg base, u32 offset,
			bool indirect, bool indexed, bool modify = false, u32 extra = 0)
	{
		ea_result out;
		const u32 direct = base_address(base, offset);

		if (!indirect)
		{
			out.addr = (indexed ? (direct + m_r.x) : direct) + extra;
			check(segment_of(base), out);
			return out;
		}

		// The pointer itself must lie in bounds before it can be fetched.
		if (!check(segment_of(base), direct, out.t))
			return out;

		const u32 ptr = m_mem.read32(direct);

		// Self-relative pointers are used exclusively with code segments
		// (ERS sheet 16); elsewhere the word is a data segment pointer.
		if (segment_of(base) == segment::code)
		{
			// 32-bit two's complement byte offset from the pointer's own
			// location.
			out.addr = u32(s32(direct) + s32(ptr)) + extra;
			if (indexed)
				out.addr += m_r.x;
			check(segment::code, out);
			return out;
		}

		return resolve_data_pointer(ptr, indexed, modify, extra);
	}

	// Evaluate a data segment pointer, as ERS sheets 55-56 specify.
	// `extra`: see effective_address.
	ea_result resolve_data_pointer(u32 ptr, bool indexed, bool modify,
			u32 extra = 0)
	{
		ea_result out;
		const u32 tag = ers32::get(ptr, 0, 2);

		if (tag == 3)                       // 11: stack segment pointer
		{
			out.addr = m_r.sb + ers32::get(ptr, 2, 30) + extra;
			if (indexed)
				out.addr += m_r.x;
			check(segment::stack, out);
			return out;
		}

		if (tag == 2)                       // 10: global data segment pointer
		{
			out.addr = m_r.db + ers32::get(ptr, 2, 30) + extra;
			if (indexed)
				out.addr += m_r.x;
			check(segment::global, out);
			// HYPOTHESIS UNDER TEST (BASIC's console output path). The
			// word can also be an external program label -- bit 0 set,
			// STT byte offset in (1:19), code segment in (20:12) -- and
			// BASIC's DISP path builds exactly that with LPP and then
			// increments through it (user seg 4 offset 0x140: LPP
			// STT[11]; INCM S-3,I). Read as DB-relative it is far out of
			// bounds, so when the DB reading fails its bounds check the
			// label reading is tried: the address is the referenced
			// PL-relative SLOT itself in the named code segment --
			// per-segment static storage addressed through PL, the way
			// the STT is. If this is wrong the bounds trap stands.
			static const knob::flag no_fb("FOCUS_NOLABELDATA");
			if (!no_fb && out.t != trap::none && ers32::get(ptr, 0, 1) == 1 && m_cst)
			{
				u32 base = 0, len = 0;
				if (m_cst(ers32::get(ptr, 20, 12), base, len))
				{
					out.t = trap::none;
					out.addr = base + len - ers32::get(ptr, 1, 19) - 3 + extra;
					if (indexed)
						out.addr += m_r.x;
				}
			}
			return out;
		}

		// Bit 0 clear: external data segment pointer. Bit 1 selects the
		// system rather than the user DST; the segment number is (2:11) and
		// the offset within the segment is (13:19). Indexing adds to the
		// offset, not to the final address.
		const bool system_dst = ers32::get(ptr, 1, 1) == 0;
		const u32 segnum = ers32::get(ptr, 2, 11);
		u32 offset = ers32::get(ptr, 13, 19) + extra;
		if (indexed)
			offset += m_r.x;

		if (segnum == 0)
		{
			out.t = trap::eds_pointer_violation;
			return out;
		}

		m_eds.resolve(system_dst, segnum, offset, modify, out.addr, out.t);
		if (out.ok())
			m_eds.last_run(out.run_fwd, out.run_back);
		return out;
	}

	// Bounds, per ERS sheet 18.
	bool in_code(u32 a) const { return a >= m_r.pb && a <= m_r.pl; }
	bool in_stack(u32 a) const { return a >= m_r.sb && a <= m_r.s; }
	bool in_global(u32 a) const { return a >= m_r.db && a <= m_r.dl; }

private:
	u32 base_address(base_reg base, u32 offset) const
	{
		switch (base)
		{
		case BASE_P_PLUS:   return m_r.p + offset;
		case BASE_P_MINUS:  return m_r.p - offset;
		case BASE_DB_PLUS:  return m_r.db + offset;
		case BASE_DL_MINUS: return m_r.dl - offset;
		case BASE_Q_PLUS:   return m_r.q + offset;
		case BASE_Q_MINUS:  return m_r.q - offset;
		case BASE_SB_PLUS:  return m_r.sb + offset;
		case BASE_S_MINUS:  return m_r.s - offset;
		}
		return 0;
	}

	// The bounds test for an operand, and how far the operand can run
	// before the next byte is outside the segment. "The microcode detects
	// all attempts to access memory locations outside of the following
	// ranges: Code Segments PB to PL, Stack Segments SB to S, Global Data
	// Segments DB to DL" (ERS 2.2.5) -- ALL attempts, so a doubleword's
	// second word and every byte a string instruction walks to are tested
	// like the first. They were not: only the first address was, and the
	// run was left at "one piece of memory, no end". A byte move or fill
	// that began inside the global segment ran on past DL (or, on the
	// stack, past S) into whatever was physically next -- another
	// segment, another process's or the kernel's -- for as many bytes as
	// its count said, where "a bounds violation will cause premature
	// termination of the instruction and a Bounds Violation Trap" (MVB,
	// sheet 101). The last byte of the code segment is PL+3: PL addresses
	// its last word.
	bool check(segment seg, ea_result &out) const
	{
		if (!check(seg, out.addr, out.t))
			return false;
		const u32 lo = (seg == segment::code) ? m_r.pb
				: (seg == segment::stack) ? m_r.sb : m_r.db;
		const u32 hi = (seg == segment::code) ? m_r.pl + 3
				: (seg == segment::stack) ? m_r.s : m_r.dl;
		const std::uint64_t fwd = (hi >= out.addr) ? std::uint64_t(hi) - out.addr + 1 : 1;
		const std::uint64_t back = std::uint64_t(out.addr) - lo + 1;
		out.run_fwd = u32(std::min<std::uint64_t>(fwd, 0x7fffffffu));
		out.run_back = u32(std::min<std::uint64_t>(back, 0x7fffffffu));
		return true;
	}

	bool check(segment seg, u32 addr, trap &t) const
	{
		const bool ok = (seg == segment::code) ? in_code(addr)
				: (seg == segment::stack) ? in_stack(addr)
				: in_global(addr);
		if (!ok)
			t = trap::bounds_violation;
		return ok;
	}

	registers &m_r;
	memory_interface &m_mem;
	eds_resolver &m_eds;
public:
	// CST lookup for the label-as-data fallback above: (segment) ->
	// (base, LENGTH-1), false if no such segment. Supplied by the core,
	// which owns the three-table CST walk.
	std::function<bool(u32, u32 &, u32 &)> m_cst;
};

} // namespace focus

#endif // MAME_CPU_FOCUS_FOCUSADDR_H
