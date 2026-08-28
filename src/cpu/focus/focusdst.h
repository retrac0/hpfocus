// license:BSD-3-Clause
// copyright-holders:
/***************************************************************************

    HP FOCUS Data Segment Table

    External data segment resolution: the DST entry, segment chaining, and the
    paged path with its page table. Implements the procedure given in the ERS
    at sheets 55-56, using the entry formats from section 3.2.1 (sheets 32-34).

    Plugs into the addressing unit through the eds_resolver interface.

***************************************************************************/

#ifndef MAME_CPU_FOCUS_FOCUSDST_H
#define MAME_CPU_FOCUS_FOCUSDST_H

#pragma once

#include "focusaddr.h"

namespace focus {

//**************************************************************************
//  TABLE ENTRIES
//**************************************************************************

// DST entry, four words (ERS sheet 32):
//
//   word 0  |0| USE COUNT |  OS  | rsv |PAGESIZE|P|W|D|R|M|A|
//            0  1......12  13..17 18.21 22....25 26 ....... 31
//   word 1  LENGTH - 1
//   word 2  LOCATION
//   word 3  |C|  LINK  |        OS use          |
//            0  1...12   13....................31
//
// Bits 13-17 of word 0 and bits 13-31 of word 3 are available to the
// operating system; bits 18-21 of word 0 are reserved.
struct dst_entry
{
	u32 w0 = 0, w1 = 0, w2 = 0, w3 = 0;

	u32 use_count() const { return ers32::get(w0, 1, 12); }
	u32 pagesize() const  { return ers32::get(w0, 22, 4); }
	bool paged() const    { return ers32::get(w0, 26, 1) != 0; }  // P
	bool writable() const { return ers32::get(w0, 27, 1) != 0; }  // W
	bool dirty() const    { return ers32::get(w0, 28, 1) != 0; }  // D
	bool referenced() const { return ers32::get(w0, 29, 1) != 0; }// R
	bool priv_only() const { return ers32::get(w0, 30, 1) != 0; } // M
	bool absent() const   { return ers32::get(w0, 31, 1) != 0; }  // A

	void set_referenced() { w0 = ers32::set(w0, 29, 1, 1); }
	void set_dirty()      { w0 = ers32::set(w0, 28, 1, 1); }

	// The field holds LENGTH - 1; its two low bits must both be 1, so the
	// actual length is always a multiple of four.
	u32 length_minus_1() const { return w1; }
	u32 length() const { return w1 + 1; }

	u32 location() const { return w2; }

	bool consecutive() const { return ers32::get(w3, 0, 1) != 0; }  // C
	u32 link() const { return ers32::get(w3, 1, 12); }
};

// Page table entry, two words (ERS sheet 34):
//
//   word 0  |0| USE COUNT |  OS  |        |1|W|D|R|M|A|
//            0  1......12  13..17          26 ....... 31
//   word 1  LOCATION       (low `pagesize` bits must be zero)
struct page_entry
{
	u32 w0 = 0, w1 = 0;

	u32 use_count() const { return ers32::get(w0, 1, 12); }
	bool writable() const { return ers32::get(w0, 27, 1) != 0; }
	bool dirty() const    { return ers32::get(w0, 28, 1) != 0; }
	bool referenced() const { return ers32::get(w0, 29, 1) != 0; }
	bool priv_only() const { return ers32::get(w0, 30, 1) != 0; }
	bool absent() const   { return ers32::get(w0, 31, 1) != 0; }

	void set_referenced() { w0 = ers32::set(w0, 29, 1, 1); }
	void set_dirty()      { w0 = ers32::set(w0, 28, 1, 1); }

	u32 location() const { return w1; }
};

//**************************************************************************
//  RESOLVER
//**************************************************************************

class dst_walker : public eds_resolver
{
public:
	// Entry zero of the system DST is assumed to start at memory location
	// 2200 octal (ERS sheet 32); the user table's base is in the DST
	// register.
	static constexpr u32 SYSTEM_DST_BASE = 02200;

	// Each entry is four words, so entry N sits at base + 16*N -- which is
	// also why entry zero's first word holds 16 times the entry count.
	static constexpr u32 ENTRY_BYTES = 16;

	dst_walker(registers &r, memory_interface &mem) : m_r(r), m_mem(mem) { }

	// Address of the word carrying the use count of whatever the last
	// successful resolve() landed on -- the DST entry for an unpaged
	// segment, the page table entry for a paged one. MUC is the only
	// instruction that needs it, and it needs it for the same walk it has
	// just done, so recording it here beats a second traversal.
	u32 last_use_count_slot() const { return m_use_slot; }

	// The parameter the trap sequence pushes for the last failed resolve:
	// "Absent Data Segment ... The DST entry number is passed as a
	// parameter"; "Absent Page / Absent Page Table ... The EDS pointer
	// [indexed, if appropriate] is passed as a parameter" (ERS 5.2.2,
	// causes 040, 042, 043). The pointer is rebuilt from what the walk
	// was given, which is the pointer after indexing. HP-UX's handlers
	// begin by testing this word: zero, which every fault used to carry,
	// is the "on the ICS, negated" reading of nothing and halts.
	u32 last_trap_param() const override { return m_trap_param; }
	void last_run(u32 &fwd, u32 &back) const override
	{
		fwd = m_run_fwd;
		back = m_run_back;
	}

	// VPTR's view of an external data segment pointer (ERS 155.1): -1
	// when the pointer is invalid, out of bounds, or asks for a mode the
	// segment does not grant; 0 when the segment (or its page) is absent;
	// 1 when it is good. Absence is tested last, as the ERS lists it, so
	// an absent segment answers "E" rather than "L".
	int validate(bool system_dst, u32 segment, u32 offset,
			bool want_write, bool want_priv) const override
	{
		const u32 table = system_dst ? SYSTEM_DST_BASE : m_r.dst;
		const u32 tlength = m_mem.read32(table);
		if (segment == 0 || tlength < ENTRY_BYTES * segment)
			return -1;
		const u32 entry_addr = table + ENTRY_BYTES * segment;
		dst_entry e;
		e.w0 = m_mem.read32(entry_addr);
		e.w1 = m_mem.read32(entry_addr + 4);
		e.w2 = m_mem.read32(entry_addr + 8);
		e.w3 = m_mem.read32(entry_addr + 12);
		int v = 1;
		if (s32(offset) < 0)
			v = -1;
		else if (e.priv_only() && !want_priv)
			v = -1;
		else if (want_write && !e.writable())
			v = -1;
		else if (!e.paged())
		{
			// Chained segments are not followed here; a pointer past the
			// first link is rare enough that reading it as invalid is the
			// safer error.
			if (offset > e.length_minus_1())
				v = -1;
		}
		else
		{
			if (e.absent())
				return 0;
			const u32 pagenum = offset >> e.pagesize();
			if (8 * pagenum + 4 > e.length_minus_1())
				return -1;
			page_entry pe;
			pe.w0 = m_mem.read32(e.location() + 8 * pagenum);
			if (pe.priv_only() && !want_priv)
				return -1;
			if (want_write && !pe.writable())
				return -1;
			if (pe.absent())
				return 0;
			return 1;
		}
		if (e.absent())
			return 0;
		return v;
	}

	bool resolve(bool system_dst, u32 segment, u32 offset, bool modify,
			u32 &addr, trap &t) override
	{
		m_use_slot = 0;
		m_trap_param = 0;
		const u32 eds_ptr = (system_dst ? 0u : 0x40000000u)
				| ((segment & 0x7ff) << 19) | (offset & 0x7ffff);
		const u32 table = system_dst ? SYSTEM_DST_BASE : m_r.dst;

		// Entry zero's first word is 16 times the number of entries,
		// excluding entry zero itself.
		const u32 tlength = m_mem.read32(table);

		if (segment == 0)
		{
			t = trap::eds_pointer_violation;
			return false;
		}

		bool linked = false;

		// Chained segments walk this loop; the bound stops a corrupt table
		// from spinning forever.
		for (unsigned hops = 0; hops < MAX_HOPS; hops++)
		{
			if (tlength < ENTRY_BYTES * segment)
			{
				t = trap::dst_violation;
				return false;
			}

			const u32 entry_addr = table + ENTRY_BYTES * segment;
			dst_entry e;
			e.w0 = m_mem.read32(entry_addr);
			e.w1 = m_mem.read32(entry_addr + 4);
			e.w2 = m_mem.read32(entry_addr + 8);
			e.w3 = m_mem.read32(entry_addr + 12);

			if (!e.paged())
			{
				// A negative offset -- X can be negative -- is out of bounds
				// before anything else is considered.
				if (s32(offset) < 0)
				{
					t = trap::eds_bounds_violation;
					return false;
				}

				// Sheet 18 gives the valid range as start .. start+length-1,
				// and the field holds length-1, so the test is inclusive.
				if (offset <= e.length_minus_1())
				{
					if (modify && !e.writable())
					{
						t = trap::data_seg_write_violation;
						return false;
					}
					if (!m_r.privileged() && e.priv_only())
					{
						t = trap::priv_mode_data_violation;
						return false;
					}
					if (e.absent())
					{
						// "The DST entry number" as the pointer names
						// it: the segment number with the U bit above
						// it -- the top thirteen bits of an EDS pointer.
						// HP-UX's handler sorts on exactly that: a
						// number below 2048 is looked up in the system
						// DST at absolute 0, anything else has its low
						// ten bits looked up in the process's own table.
						// A bare user segment number was taken for a
						// system one, and the fault repeated for ever.
						t = trap::absent_segment;
						m_trap_param = (system_dst ? 0u : 0x800u) | (segment & 0x7ff);
						return false;
					}

					// The referenced and dirty bits are NOT maintained.
					// SUNOS keeps one table serving as both CST and DST,
					// and the DST reference bit (word 0 bit 29) is the
					// CST layout's absence bit: setting it on a data
					// access to a code segment would make every later
					// procedure call into it trap as absent. The powerup
					// module initialises flags with bits 28-29 clear and
					// nothing in the OS clears them again.

					m_use_slot = entry_addr;
					addr = e.location() + offset;
					m_run_fwd = e.length() - offset;
					m_run_back = offset + 1;
					return true;
				}

				// Past the end of this segment: follow the chain
				// (ERS section 3.2.2, sheet 36).
				linked = true;
				if (!e.consecutive())
				{
					if (e.link() == 0)
					{
						t = trap::eds_bounds_violation;
						return false;
					}
					offset -= e.length();
					segment = e.link();
				}
				else
				{
					// Consecutively linked: the segments are contiguous DST
					// entries, so the entry can be indexed directly.
					const u32 len = e.length();
					segment += offset / len;
					offset %= len;
					if (segment > e.link())
					{
						t = trap::eds_bounds_violation;
						return false;
					}
				}
				continue;
			}

			// ---- paged ----------------------------------------------
			if (linked)
			{
				t = trap::unexpected_eds_type;
				return false;
			}
			if (e.absent())
			{
				t = trap::absent_page_table;
				m_trap_param = eds_ptr;
				return false;
			}

			const u32 ps = e.pagesize();
			const u32 pagenum = offset >> ps;
			const u32 page_offset = offset & ((u32(1) << ps) - 1);

			if (8 * pagenum + 4 > e.length_minus_1())
			{
				t = trap::page_table_violation;
				return false;
			}

			const u32 pte_addr = e.location() + 8 * pagenum;
			page_entry pe;
			pe.w0 = m_mem.read32(pte_addr);
			pe.w1 = m_mem.read32(pte_addr + 4);

			if (modify && !pe.writable())
			{
				t = trap::data_seg_write_violation;
				return false;
			}
			if (!m_r.privileged() && pe.priv_only())
			{
				t = trap::priv_mode_data_violation;
				return false;
			}
			if (pe.absent())
			{
				t = trap::absent_page;
				m_trap_param = eds_ptr;
				return false;
			}

			pe.set_referenced();
			if (modify)
				pe.set_dirty();
			m_mem.write32(pte_addr, pe.w0);

			m_use_slot = pte_addr;

			// LOCATION is page aligned, so OR and add are equivalent; the
			// ERS writes it as OR.
			addr = pe.location() | page_offset;
			// A page is the unit of physical contiguity: the next page
			// of the same segment can be anywhere in memory (or absent),
			// so a byte walk that reaches the boundary has to resolve
			// again rather than run on. HP-UX's pager copies a 3 KB
			// swap read into a process's data segment with one MVB; run
			// on past the first page and the bytes land in whichever
			// frames happen to follow physically -- the frames of other
			// pages of the same segment, in the order the allocator
			// handed them out -- so the image came back permuted and
			// the exec's relocation pass read a pointer out of the
			// wrong page and gave up with 315.
			m_run_fwd = (u32(1) << ps) - page_offset;
			m_run_back = page_offset + 1;
			return true;
		}

		t = trap::eds_bounds_violation;
		return false;
	}

private:
	static constexpr unsigned MAX_HOPS = 4096;   // one per possible DST entry

	registers &m_r;
	memory_interface &m_mem;
	u32 m_use_slot = 0;
	u32 m_trap_param = 0;
	u32 m_run_fwd = 0x7fffffffu;
	u32 m_run_back = 0x7fffffffu;
};

} // namespace focus

#endif // MAME_CPU_FOCUS_FOCUSDST_H
