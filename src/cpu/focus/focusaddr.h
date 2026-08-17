// license:BSD-3-Clause
// copyright-holders:
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

#include "focusstate.h"

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
// path with its page table -- is specified on ERS sheets 30-36 and 55-56 and
// needs the DST entry bit layout, which is not yet transcribed. This is the
// seam where that goes; until then a test double stands in.
class eds_resolver
{
public:
	virtual ~eds_resolver() = default;

	// `system_dst` selects the system rather than user table, per PTR(1:1).
	// Returns false and sets `t` on a violation.
	virtual bool resolve(bool system_dst, u32 segment, u32 offset,
			bool modify, u32 &addr, trap &t) = 0;
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
	bool ok() const { return t == trap::none; }
};

class addressing_unit
{
public:
	addressing_unit(registers &r, memory_interface &mem, eds_resolver &eds)
		: m_r(r), m_mem(mem), m_eds(eds) { }

	// Compute the effective address of a base-relative reference.
	//
	// `modify` marks a modify-memory instruction, which matters for the write
	// permission check and the dirty bit on external data segments.
	//
	// Post-indexing is the default (ERS sheet 57): with both indirect and
	// indexed set, the pointer is resolved first and X is applied to the
	// result. Figure 2.1 confirms this -- LOAD P+20,I,X follows the pointer
	// to P-40 and only then adds X to reach P-20.
	ea_result effective_address(base_reg base, u32 offset,
			bool indirect, bool indexed, bool modify = false)
	{
		ea_result out;
		const u32 direct = base_address(base, offset);

		if (!indirect)
		{
			out.addr = indexed ? (direct + m_r.x) : direct;
			check(segment_of(base), out.addr, out.t);
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
			out.addr = u32(s32(direct) + s32(ptr));
			if (indexed)
				out.addr += m_r.x;
			check(segment::code, out.addr, out.t);
			return out;
		}

		return resolve_data_pointer(ptr, indexed, modify);
	}

	// Evaluate a data segment pointer, as ERS sheets 55-56 specify.
	ea_result resolve_data_pointer(u32 ptr, bool indexed, bool modify)
	{
		ea_result out;
		const u32 tag = ers32::get(ptr, 0, 2);

		if (tag == 3)                       // 11: stack segment pointer
		{
			out.addr = m_r.sb + ers32::get(ptr, 2, 30);
			if (indexed)
				out.addr += m_r.x;
			check(segment::stack, out.addr, out.t);
			return out;
		}

		if (tag == 2)                       // 10: global data segment pointer
		{
			out.addr = m_r.db + ers32::get(ptr, 2, 30);
			if (indexed)
				out.addr += m_r.x;
			check(segment::global, out.addr, out.t);
			return out;
		}

		// Bit 0 clear: external data segment pointer. Bit 1 selects the
		// system rather than the user DST; the segment number is (2:11) and
		// the offset within the segment is (13:19). Indexing adds to the
		// offset, not to the final address.
		const bool system_dst = ers32::get(ptr, 1, 1) == 0;
		const u32 segnum = ers32::get(ptr, 2, 11);
		u32 offset = ers32::get(ptr, 13, 19);
		if (indexed)
			offset += m_r.x;

		if (segnum == 0)
		{
			out.t = trap::eds_pointer_violation;
			return out;
		}

		m_eds.resolve(system_dst, segnum, offset, modify, out.addr, out.t);
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
};

} // namespace focus

#endif // MAME_CPU_FOCUS_FOCUSADDR_H
