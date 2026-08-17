// license:BSD-3-Clause
// copyright-holders:
/***************************************************************************

    HP FOCUS processor state

    The machine-visible register file, the STATUS register's fields, and the
    top-of-stack register cache. Kept free of MAME dependencies so it can be
    unit tested on its own; the CPU device embeds it.

    Reference: Focus Machine Instruction Set ERS, A-1FE1-3020-B, 12/06/82,
    section 2 (sheets 10-14).

    BIT NUMBERING. The ERS numbers bits with 0 as the MOST significant, and
    writes a field as (N:M) meaning M bits starting at bit N. Every accessor
    here takes ERS numbering; ers_field does the conversion exactly once so
    nothing else in the codebase has to think about it.

***************************************************************************/

#ifndef MAME_CPU_FOCUS_FOCUSSTATE_H
#define MAME_CPU_FOCUS_FOCUSSTATE_H

#pragma once

#include <cstdint>

namespace focus {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;

//**************************************************************************
//  ERS BIT FIELDS
//**************************************************************************

// A field (start:width) in a container of CONTAINER_BITS bits, ERS numbering.
template <unsigned CONTAINER_BITS>
struct ers_field
{
	static constexpr u32 shift(unsigned start, unsigned width)
	{
		return CONTAINER_BITS - start - width;
	}

	static constexpr u32 mask(unsigned start, unsigned width)
	{
		return ((width >= 32) ? ~u32(0) : ((u32(1) << width) - 1))
				<< shift(start, width);
	}

	static constexpr u32 get(u32 word, unsigned start, unsigned width)
	{
		return (word >> shift(start, width))
				& ((width >= 32) ? ~u32(0) : ((u32(1) << width) - 1));
	}

	static constexpr u32 set(u32 word, unsigned start, unsigned width, u32 value)
	{
		const u32 m = mask(start, width);
		return (word & ~m) | ((value << shift(start, width)) & m);
	}
};

using ers32 = ers_field<32>;
using ers8 = ers_field<8>;

//**************************************************************************
//  STATUS REGISTER
//**************************************************************************

// Layout transcribed from the field diagram on ERS sheet 12. Every one of the
// seventeen fields described in the accompanying text lands in a distinct
// slot with no overlap, which is what makes the reading trustworthy -- the
// OCR of that diagram is unusable, so it was read from the page image.
//
//   bit  0  TV   trace variables trap enable
//        1  SL   start of line trap enable
//        2  EP   end of procedure trap enable
//        3  SS   start of subroutine trap enable
//        4  ES   end of subroutine trap enable
//        5  IR   inexact result
//        6  C    carry
//        7  O    overflow
//        8  T    user trap enable
//        9  EM   enable machine instruction trap
//       10  MI   machine instruction trap request
//    11-12  RM   rounding mode
//       13  I    message register interrupt enable
//       14  M    mode: 1 = privileged
//       15       reserved
//    16-17  CC   condition code
//       18       reserved
//       19  P    instruction pending
//    20-31  CSN  code segment number
//
// Bits 15 and 18 carry no label in the diagram and no description in the
// text. They are treated as reserved.
struct status_field
{
	unsigned start, width;
};

namespace sf {
constexpr status_field TV  { 0, 1};
constexpr status_field SL  { 1, 1};
constexpr status_field EP  { 2, 1};
constexpr status_field SS  { 3, 1};
constexpr status_field ES  { 4, 1};
constexpr status_field IR  { 5, 1};
constexpr status_field C   { 6, 1};
constexpr status_field O   { 7, 1};
constexpr status_field T   { 8, 1};
constexpr status_field EM  { 9, 1};
constexpr status_field MI  {10, 1};
constexpr status_field RM  {11, 2};
constexpr status_field I   {13, 1};
constexpr status_field M   {14, 1};
constexpr status_field CC  {16, 2};
constexpr status_field P   {19, 1};
constexpr status_field CSN {20, 12};
} // namespace sf

// Condition code values (ERS sheet 12). Hardware only ever writes 01 for CCL,
// but 11 must also be accepted as CCL when software sets it.
enum : u32 { CCG = 0, CCE = 2, CCL = 1, CCL_ALT = 3 };

// Rounding modes for binary floating point (ERS sheet 12).
enum : u32 { RM_NEAREST_EVEN = 0, RM_PLUS_INF = 1, RM_ZERO = 2, RM_MINUS_INF = 3 };

//**************************************************************************
//  TOP-OF-STACK CACHE
//**************************************************************************

// FOCUS names the four topmost stack words A, B, C and D, with A topmost
// (ERS sheet 57). Following the approach SIMH's hp3000 takes for the same
// architecture, they are held in registers and renamed rather than copied:
// pushing rotates an index instead of shuffling four values.
//
// This models only the register rotation. Spilling to and filling from the
// memory-resident remainder of the stack belongs with the memory interface
// and is not handled here.
class tos_cache
{
public:
	static constexpr unsigned SIZE = 4;

	void reset() { m_top = 0; m_valid = 0; for (auto &r : m_reg) r = 0; }

	unsigned valid() const { return m_valid; }
	bool full() const { return m_valid == SIZE; }
	bool empty() const { return m_valid == 0; }

	// depth 0 is A (topmost), 1 is B, 2 is C, 3 is D.
	u32 get(unsigned depth) const { return m_reg[index(depth)]; }
	void set(unsigned depth, u32 value) { m_reg[index(depth)] = value; }

	u32 a() const { return get(0); }
	u32 b() const { return get(1); }
	u32 c() const { return get(2); }
	u32 d() const { return get(3); }

	// Push rotates the window; the value displaced off the bottom is returned
	// so the caller can spill it to memory when the cache was already full.
	u32 push(u32 value)
	{
		const u32 displaced = full() ? m_reg[index(SIZE - 1)] : 0;
		m_top = (m_top + SIZE - 1) % SIZE;
		m_reg[m_top] = value;
		if (m_valid < SIZE)
			m_valid++;
		return displaced;
	}

	u32 pop()
	{
		const u32 value = m_reg[m_top];
		m_top = (m_top + 1) % SIZE;
		if (m_valid)
			m_valid--;
		return value;
	}

	// XCH: exchange A and B (format 3 opcode 032).
	void exchange() { swap_depths(0, 1); }

	// DXCH: exchange the double words AB and CD (format 3 opcode 016).
	void double_exchange() { swap_depths(0, 2); swap_depths(1, 3); }

	// CAB: rotate A, B and C so that C becomes the top (format 3 opcode 056).
	void rotate_abc()
	{
		const u32 a = get(0), b = get(1), c = get(2);
		set(0, c); set(1, a); set(2, b);
	}

private:
	unsigned index(unsigned depth) const { return (m_top + depth) % SIZE; }

	void swap_depths(unsigned x, unsigned y)
	{
		const u32 t = get(x);
		set(x, get(y));
		set(y, t);
	}

	u32 m_reg[SIZE] = {0, 0, 0, 0};
	unsigned m_top = 0;
	unsigned m_valid = 0;
};

//**************************************************************************
//  REGISTER FILE
//**************************************************************************

// The eighteen registers reachable from the machine instruction set
// (ERS sheets 10-14). All are 32 bits except FLAGS, which is 8.
struct registers
{
	// Code segment (sheet 10).
	u32 p = 0;    // program counter; LSB must be 0
	u32 pb = 0;   // program base; two LSBs must be 0
	u32 pl = 0;   // program limit; two LSBs must be 0

	// Stack segment (sheets 10-11).
	u32 sb = 0;   // stack base; two LSBs must be 0
	u32 sl = 0;   // stack limit; two LSBs must be 1
	u32 s = 0;    // stack pointer; two LSBs must be 1
	u32 q = 0;    // stack marker pointer; two LSBs must be 1

	// Data segment (sheet 11).
	u32 db = 0;   // data base; two LSBs must be 0
	u32 dl = 0;   // data limit; two LSBs must be 1
	u32 dst = 0;  // data segment table pointer; two LSBs must be 0

	// Other (sheets 11-14).
	u32 x = 0;       // index register, holds a BYTE offset
	u32 status = 0;
	u8  flags = 0;   // 8-bit debug aid flags
	u32 msg = 0;     // message register
	u32 mask = 0;    // message mask; bits 3-5 hold the CPU's MPB channel
	u32 brkpt = 0;
	u32 sar = 0;     // MPB slave address
	u32 sdr = 0;     // MPB slave data

	u32 get(status_field f) const { return ers32::get(status, f.start, f.width); }
	void put(status_field f, u32 v) { status = ers32::set(status, f.start, f.width, v); }
	bool test(status_field f) const { return get(f) != 0; }

	bool privileged() const { return test(sf::M); }
	u32 condition_code() const { return get(sf::CC); }

	// The condition code has three interpretations selected per instruction
	// (ERS sheet 12); CCA is the common one, set from a signed operand.
	void set_cca(std::int32_t v)
	{
		put(sf::CC, (v < 0) ? CCL : (v == 0) ? CCE : CCG);
	}

	// Alignment invariants the ERS states for each pointer register. Useful
	// as assertions while bringing the core up; a violation means a bug in
	// the emulator or genuinely corrupt guest state.
	bool alignment_ok() const
	{
		return !(p & 1)
			&& !(pb & 3) && !(pl & 3)
			&& !(sb & 3) && ((sl & 3) == 3) && ((s & 3) == 3) && ((q & 3) == 3)
			&& !(db & 3) && ((dl & 3) == 3) && !(dst & 3);
	}
};

} // namespace focus

#endif // MAME_CPU_FOCUS_FOCUSSTATE_H
