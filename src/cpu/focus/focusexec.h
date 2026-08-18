// license:BSD-3-Clause
// copyright-holders:
/***************************************************************************

    HP FOCUS execution core

    An instruction-at-a-time interpreter over the register model, addressing
    unit and Data Segment Table walk. Kept free of MAME dependencies so it can
    be driven from unit tests; the CPU device will wrap it.

    Reference: Focus Machine Instruction Set ERS, A-1FE1-3020-B, 12/06/82,
    section 6.2 (sheets 54-129). Bit numbering is the ERS convention
    throughout: bit 0 is the most significant.

    THE STACK. S holds the absolute address of the top-of-stack word and its
    two low bits are always 1, so S addresses the *last byte* of that word.
    The word therefore lives at S-3. ERS pseudocode counts the stack in words,
    so its (S-1) is the word below the top, at byte S-7.

    NOT YET IMPLEMENTED. Floating point, decimal, the move/scan/string group,
    I/O, the privileged and debug-aid instructions, and the trap and interrupt
    sequences. Procedure calls handle local program pointers only; external
    pointers need the Code Segment Table walk. Unimplemented opcodes report
    trap::unimplemented rather than failing silently.

***************************************************************************/

#ifndef MAME_CPU_FOCUS_FOCUSEXEC_H
#define MAME_CPU_FOCUS_FOCUSEXEC_H

#pragma once

#include "focusdst.h"

#include <cstdint>
#include <cstring>

namespace focus {

using s64 = std::int64_t;
using u64 = std::uint64_t;

// Extends the addressing traps with execution-time ones.
enum class exec_trap : u8
{
	none = 0,
	unimplemented,       // opcode not yet modelled by this core
	bounds_violation,
	code_bounds,
	divide_by_zero,
	address_error,       // from the addressing unit; see last_addr_trap()
	stack_overflow,
	pointer_conversion,  // CPTR produced a misaligned pointer
	trapped,             // a trap fired but no handler is installed
	halted               // STOP executed
};

// Internal interrupt and trap causes. The value is the Segment Transfer Table
// entry number within code segment 1, which is reserved for these handlers
// (ERS section 5.2.2, sheets 45-52). The ERS numbers them in octal.
enum class trap_cause : u8
{
	bounds_violation          = 001,
	check_trap                = 002,
	machine_instruction       = 004,
	string_trap               = 005,
	reset                     = 010,
	page_table_violation      = 011,
	inconsistent_registers    = 012,
	pointer_conversion        = 016,
	external_program_pointer  = 017,
	unimplemented_instruction = 020,
	stt_violation             = 021,
	cst_violation             = 022,
	dst_violation             = 023,
	stack_overflow            = 024,
	stack_underflow           = 025,
	privileged_mode           = 026,
	privileged_mode_data      = 027,
	unexpected_pointer_type   = 030,
	user_trap                 = 031,   // 1 = integer overflow, 2 = divide by zero
	illegal_decimal           = 032,
	exponent_size             = 033,
	fp_operand                = 034,
	fp_result                 = 035,
	unexpected_eds_type       = 036,
	absent_page               = 040,
	uncallable_procedure      = 041,
	absent_data_segment       = 042,
	absent_page_table         = 043,
	start_of_line             = 044,
	variable_trace            = 045,
	start_of_procedure        = 046,
	end_of_procedure          = 047,
	start_of_subroutine       = 050,
	end_of_subroutine         = 051,
	code_segment_violation    = 052,
	branch_violation          = 053,
	message_trap              = 054,
	instruction_sequencing    = 055,
	start_of_line_check       = 056,
	data_segment_write        = 057
};

class cpu_core
{
public:
	cpu_core(memory_interface &mem)
		: m_mem(mem), m_dst(r, mem), m_au(r, mem, m_dst), m_io(&m_null_io) { }

	// Attach an IOP. Without one the I/O instructions report an illegal
	// opcode, which is what a machine with no IOP would do.
	void set_io(io_interface *io) { m_io = io ? io : &m_null_io; }

	registers r;

	void reset()
	{
		r = registers();
		m_cir = 0;
		m_halted = false;
	}

	bool halted() const { return m_halted; }

	// Traps vector through code segment 1 only once a Code Segment Table is
	// in place. Until then faults are reported to the caller instead, which
	// is what unit tests want and what a cold machine has to do anyway.
	void enable_traps(bool on) { m_traps_enabled = on; }
	bool traps_enabled() const { return m_traps_enabled; }
	trap_cause last_trap() const { return m_last_trap; }
	trap last_addr_trap() const { return m_addr_trap; }
	u32 cir() const { return m_cir; }

	//----------------------------------------------------------------
	//  stack access
	//----------------------------------------------------------------

	// Word `depth` below the top of stack; depth 0 is the top.
	u32 stk(unsigned depth) const { return m_mem.read32(r.s - 3 - 4 * depth); }
	void set_stk(unsigned depth, u32 v) { m_mem.write32(r.s - 3 - 4 * depth, v); }

	void push(u32 v) { r.s += 4; set_stk(0, v); }
	u32 pop() { const u32 v = stk(0); r.s -= 4; return v; }

	// The ERS pseudocode uses two conventions. A top-of-stack style pointer
	// such as S or Q ends in 11 and addresses the last byte of its word, so
	// "(S-n)" is n words below it. A base pointer such as SB is word aligned
	// and "(SB+n)" is n words above it.
	u32 word_below(u32 sptr, unsigned n) const
	{
		return m_mem.read32(sptr - 3 - 4 * n);
	}
	void set_word_below(u32 sptr, unsigned n, u32 v)
	{
		m_mem.write32(sptr - 3 - 4 * n, v);
	}
	u32 sb_word(unsigned n) const { return m_mem.read32(r.sb + 4 * n); }
	// Read-only view of word_below for tests and the debugger.
	u32 word_below_pub(u32 sptr, unsigned n) const { return word_below(sptr, n); }
	void set_sb_word(unsigned n, u32 v) { m_mem.write32(r.sb + 4 * n, v); }

	//----------------------------------------------------------------
	//  execution
	//----------------------------------------------------------------

	exec_trap step()
	{
		if (m_halted)
			return exec_trap::halted;

		m_addr_trap = trap::none;
		m_branched = false;

		if (!m_au.in_code(r.p))
			return exec_trap::code_bounds;

		const u16 w = read16(r.p);
		const u8 op1 = (w >> 13) & 7;
		const bool is32 = (op1 == 7);

		// 16-bit instructions sit right justified in CIR with zero in the
		// left halfword (ERS sheet 57).
		m_cir = is32 ? m_mem.read32(r.p) : u32(w);
		m_next = r.p + (is32 ? 4 : 2);

		exec_trap t = is32 ? exec_fmt6() : exec_16(op1, w);

		// Faults the core detects become architectural traps once a handler
		// segment exists.
		if (t != exec_trap::none && t != exec_trap::halted && m_traps_enabled)
			t = vector_fault(t);

		if (t == exec_trap::none && !m_branched)
			r.p = m_next;
		return t;
	}

	// Map an execution fault onto the architectural trap that represents it.
	exec_trap vector_fault(exec_trap t)
	{
		switch (t)
		{
		case exec_trap::unimplemented:
			return take_trap(trap_cause::unimplemented_instruction, 0);
		case exec_trap::code_bounds:
			return take_trap(trap_cause::instruction_sequencing, r.p);
		case exec_trap::bounds_violation:
			return take_trap(trap_cause::bounds_violation, r.p);
		case exec_trap::divide_by_zero:
			return take_trap(trap_cause::user_trap, 2);
		case exec_trap::pointer_conversion:
			return take_trap(trap_cause::pointer_conversion, 0);
		case exec_trap::address_error:
			return take_trap(addr_trap_cause(), 0);
		default:
			return t;
		}
	}

	trap_cause addr_trap_cause() const
	{
		switch (m_addr_trap)
		{
		case trap::eds_pointer_violation:    return trap_cause::unexpected_pointer_type;
		case trap::dst_violation:            return trap_cause::dst_violation;
		case trap::eds_bounds_violation:     return trap_cause::bounds_violation;
		case trap::data_seg_write_violation: return trap_cause::data_segment_write;
		case trap::priv_mode_data_violation: return trap_cause::privileged_mode_data;
		case trap::absent_segment:           return trap_cause::absent_data_segment;
		case trap::absent_page_table:        return trap_cause::absent_page_table;
		case trap::absent_page:              return trap_cause::absent_page;
		case trap::page_table_violation:     return trap_cause::page_table_violation;
		case trap::unexpected_eds_type:      return trap_cause::unexpected_eds_type;
		default:                             return trap_cause::bounds_violation;
		}
	}

	// Run until halted, a trap, or the step budget runs out. Returns the
	// trap that stopped it.
	exec_trap run(unsigned max_steps, unsigned *executed = nullptr)
	{
		unsigned n = 0;
		for (; n < max_steps; n++)
		{
			const exec_trap t = step();
			if (t != exec_trap::none)
			{
				if (executed) *executed = n;
				return t;
			}
		}
		if (executed) *executed = n;
		return exec_trap::none;
	}

private:
	u16 read16(u32 addr) const
	{
		const u32 word = m_mem.read32(addr & ~3u);
		return (addr & 2) ? u16(word & 0xffff) : u16(word >> 16);
	}

	// Effective address, reporting failures through m_addr_trap.
	bool ea(base_reg base, u32 offset, bool ind, bool idx, bool modify,
			u32 &out)
	{
		const ea_result e = m_au.effective_address(base, offset, ind, idx,
				modify);
		m_addr_trap = e.t;
		out = e.addr;
		return e.ok();
	}

	void branch_to(u32 target) { r.p = target; m_branched = true; }

	// Condition code, CCA interpretation, on a signed value.
	void cca(u32 v) { r.set_cca(s32(v)); }

	//----------------------------------------------------------------
	//  floating point
	//----------------------------------------------------------------
	//
	// The ERS targets IEEE 754 Draft 5.11, whose single and double basic
	// formats are the same as the ratified standard's binary32 and binary64
	// (ERS Appendix F), so the host's own types can carry the values. Only
	// the four rounding modes and the inexact flag differ in their handling,
	// and neither is modelled yet -- see the note in HARDWARE.md 7.1.
	//
	// A double occupies two stack words, high word below low, matching the
	// order LDD pushes them in.

	static double to_double(u64 b) { double d; std::memcpy(&d, &b, 8); return d; }
	static u64 from_double(double d) { u64 b; std::memcpy(&b, &d, 8); return b; }
	static float to_float(u32 b) { float f; std::memcpy(&f, &b, 4); return f; }
	static u32 from_float(float f) { u32 b; std::memcpy(&b, &f, 4); return b; }

	double pop_double()
	{
		const u32 low = pop(), high = pop();
		return to_double((u64(high) << 32) | low);
	}

	void push_double(double d)
	{
		const u64 b = from_double(d);
		push(u32(b >> 32));
		push(u32(b));
	}

	void set_fp_cc(double v)
	{
		r.put(sf::CC, (v < 0) ? CCL : (v == 0) ? CCE : CCG);
	}

	//----------------------------------------------------------------
	//  16-bit formats
	//----------------------------------------------------------------

	exec_trap exec_16(u8 op1, u16 w)
	{
		if (op1 == 0)
			return exec_fmt234(w);
		if (op1 == 1)
			return exec_fmt5(w);
		return exec_fmt1(op1, w);
	}

	// Format 1: OP1(0:3) I(3) X(4) BASE(5:3) OFFSET(8:8)
	exec_trap exec_fmt1(u8 op1, u16 w)
	{
		const bool ind = (w >> 12) & 1, idx = (w >> 11) & 1;
		const base_reg base = base_reg((w >> 8) & 7);
		const u32 raw = w & 0xff;

		switch (op1)
		{
		case 2:     // LDS  -- word offset
		{
			u32 a;
			if (!ea(base, raw * 4, ind, idx, false, a))
				return exec_trap::address_error;
			push(m_mem.read32(a));
			cca(stk(0));
			return exec_trap::none;
		}
		case 3:     // LRAS -- load relative address, byte offset
		{
			u32 a;
			if (!ea(base, raw, ind, idx, false, a))
				return exec_trap::address_error;
			push(a);
			return exec_trap::none;
		}
		case 4:     // LDDS -- load double, word offset
		{
			u32 a;
			if (!ea(base, raw * 4, ind, idx, false, a))
				return exec_trap::address_error;
			push(m_mem.read32(a));
			push(m_mem.read32(a + 4));
			return exec_trap::none;
		}
		case 5:     // STS  -- store, word offset
		{
			u32 a;
			if (!ea(base, raw * 4, ind, idx, true, a))
				return exec_trap::address_error;
			m_mem.write32(a, pop());
			return exec_trap::none;
		}
		case 6:     // BCCS -- see ERS sheet 90
		{
			const u8 ccf = (w >> 9) & 7;
			const u32 off = w & 0xff;
			if (cc_taken(ccf))
			{
				const s32 delta = (((w >> 8) & 1) ? -s32(off) : s32(off)) * 2;
				branch_to(u32(r.p + delta));
			}
			return exec_trap::none;
		}
		}
		return exec_trap::unimplemented;
	}

	// CCF(0:1) selects CCG, CCF(1:1) CCE, CCF(2:1) CCL (ERS sheet 90).
	bool cc_taken(u8 ccf) const
	{
		const u32 cc = r.condition_code();
		if (cc == CCG) return (ccf & 4) != 0;
		if (cc == CCE) return (ccf & 2) != 0;
		return (ccf & 1) != 0;              // CCL, either encoding
	}

	// Formats 2, 3 and 4: OP1 == 0.
	exec_trap exec_fmt234(u16 w)
	{
		const bool b3 = (w >> 12) & 1;
		const u8 op2 = (w >> 6) & 0x3f;
		const u8 operand = w & 0x3f;

		if (op2 == 0)
			return exec_fmt3(operand, b3);
		if (op2 == 1)
			return exec_fmt4(operand, b3);
		return exec_fmt2(op2, operand, b3);
	}

	// Format 3: stack operations.
	exec_trap exec_fmt3(u8 op3, bool b3)
	{
		switch (op3)
		{
		case 000: return exec_trap::none;                     // NOP
		case 001: {                                           // DELB
			const u32 a = pop();                              // save top
			pop();                                            // drop B
			push(a);
			return exec_trap::none;
		}
		case 002: pop(); pop(); return exec_trap::none;        // DDEL
		case 005: {                                            // PSHN
			// Pushes X copies of either zero or the current top of stack
			// (ERS sheet 67).
			u32 fill = 0;
			if (b3)
				fill = pop();
			while (r.x > 0) { push(fill); r.x--; }
			return exec_trap::none;
		}
		case 006: push(0); return exec_trap::none;             // ZERO
		case 007: push(0); push(0); return exec_trap::none;    // DZRO
		case 016: {                                            // DXCH
			const u32 a = stk(0), b = stk(1), c = stk(2), d = stk(3);
			set_stk(0, c); set_stk(1, d); set_stk(2, a); set_stk(3, b);
			return exec_trap::none;
		}
		case 017: {                                            // CMP
			const s32 b = s32(stk(1)), a = s32(stk(0));
			r.s -= 8;
			r.put(sf::CC, (b < a) ? CCL : (b == a) ? CCE : CCG);
			return exec_trap::none;
		}
		case 020: {                                            // ADD
			const u32 a = pop(), b = pop();
			const u64 sum = u64(b) + u64(a) + (b3 && r.test(sf::C) ? 1 : 0);
			set_overflow_add(b, a, u32(sum));
			r.put(sf::C, (sum >> 32) & 1);
			push(u32(sum));
			cca(u32(sum));
			return exec_trap::none;
		}
		case 021: {                                            // SUB
			const u32 a = pop(), b = pop();
			const u64 diff = u64(b) - u64(a) - (b3 && r.test(sf::C) ? 1 : 0);
			set_overflow_sub(b, a, u32(diff));
			r.put(sf::C, (diff >> 32) & 1);
			push(u32(diff));
			cca(u32(diff));
			return exec_trap::none;
		}
		case 022: {                                            // MPY
			const s32 a = s32(pop()), b = s32(pop());
			const s64 prod = s64(a) * s64(b);
			// The doubleword product: high half at (S-1), low at (S).
			push(u32(u64(prod) >> 32));
			push(u32(u64(prod)));
			if (!b3)
			{
				// Overflow if the most significant 33 bits are not all the
				// same, then the high half is discarded (ERS sheet 74).
				const u32 hi = stk(1);
				const bool sign = (stk(0) >> 31) & 1;
				r.put(sf::O, !((hi == 0 && !sign) || (hi == 0xffffffff && sign)));
				const u32 low = pop();
				pop();
				push(low);
			}
			cca(stk(0));
			return exec_trap::none;
		}
		case 023: {                                            // DIV
			const s32 a = s32(pop()), b = s32(pop());
			if (a == 0)
				return exec_trap::divide_by_zero;
			push(u32(b / a));
			cca(stk(0));
			return exec_trap::none;
		}
		case 024: {                                            // NEG
			const u32 v = u32(-s32(pop()));
			push(v); cca(v);
			return exec_trap::none;
		}
		case 025: cca(stk(0)); return exec_trap::none;         // TEST
		case 026: cca(stk(0)); return exec_trap::none;         // TSTD
		case 027: r.put(sf::CC, (stk(0) & 0xff) ? CCG : CCE);
			return exec_trap::none;                            // TSTB
		case 032: {                                            // XCH
			const u32 a = stk(0), b = stk(1);
			set_stk(0, b); set_stk(1, a);
			return exec_trap::none;
		}
		case 033: {                                            // DIVS
			// Quotient replaces (S-1), remainder replaces (S); with bit 3
			// set the remainder is forced positive (ERS sheet 78).
			const s32 divisor = s32(stk(0)), dividend = s32(stk(1));
			if (divisor == 0)
				return exec_trap::divide_by_zero;
			s32 quot = dividend / divisor, rem = dividend % divisor;
			if (b3 && rem < 0) { rem = -rem; }
			set_stk(1, u32(quot));
			set_stk(0, u32(rem));
			return exec_trap::none;
		}
		case 035: {                                            // XAX
			const u32 a = stk(0);
			set_stk(0, r.x); r.x = a;
			return exec_trap::none;
		}
		case 036: r.x += stk(0); return exec_trap::none;       // ADAX
		case 037: { const u32 v = stk(0) + r.x; set_stk(0, v); cca(v);
			return exec_trap::none; }                          // ADXA
		case 040: pop(); return exec_trap::none;               // DEL
		case 043: r.x = pop(); return exec_trap::none;         // STAX
		case 044: push(r.x); cca(r.x); return exec_trap::none; // LDXA
		case 045: push(stk(0)); return exec_trap::none;        // DUP
		case 046: { const u32 a = stk(0), b = stk(1); push(b); push(a);
			return exec_trap::none; }                          // DDUP
		case 056: {                                            // CAB
			const u32 a = stk(0), b = stk(1), c = stk(2);
			set_stk(0, c); set_stk(1, a); set_stk(2, b);
			return exec_trap::none;
		}
		case 003: set_fp_cc(to_double((u64(stk(1)) << 32) | stk(0)));
			return exec_trap::none;                            // FTST
		case 004: set_fp_cc(to_float(stk(0)));
			return exec_trap::none;                            // FTSS
		case 041: { const double d = pop_double();             // FIXD
			const u32 v = u32(s32(d)); push(v); cca(v);
			return exec_trap::none; }
		case 042: { const s32 v = s32(pop()); push_double(double(v));
			return exec_trap::none; }                          // FLTD
		case 050: {                                            // FCMP
			const double y = pop_double();
			const double x = to_double((u64(stk(1)) << 32) | stk(0));
			r.put(sf::CC, (x < y) ? CCL : (x == y) ? CCE : CCG);
			if (b3) r.s -= 8;
			return exec_trap::none;
		}
		case 051: { const double y = pop_double(), x = pop_double();
			push_double(x + y); set_fp_cc(x + y);
			return exec_trap::none; }                          // FADD
		case 052: { const double y = pop_double(), x = pop_double();
			push_double(x - y); set_fp_cc(x - y);
			return exec_trap::none; }                          // FSUB
		case 053: { const double y = pop_double(), x = pop_double();
			push_double(x * y); set_fp_cc(x * y);
			return exec_trap::none; }                          // FMPY
		case 054: { const double y = pop_double(), x = pop_double();
			push_double(x / y); set_fp_cc(x / y);
			return exec_trap::none; }                          // FDIV
		case 055: { const double x = pop_double(); push_double(-x);
			set_fp_cc(-x); return exec_trap::none; }           // FNEG
		case 070: { const s32 v = s32(pop()); push(from_float(float(v)));
			return exec_trap::none; }                          // FLTS
		case 071: { const u32 v = u32(s32(to_float(pop()))); push(v);
			cca(v); return exec_trap::none; }                  // FIXS
		case 072: { const float y = to_float(pop()), x = to_float(pop());
			push(from_float(x + y)); set_fp_cc(x + y);
			return exec_trap::none; }                          // FADS
		case 073: { const float y = to_float(pop()), x = to_float(pop());
			push(from_float(x - y)); set_fp_cc(x - y);
			return exec_trap::none; }                          // FSBS
		case 074: { const float y = to_float(pop()), x = to_float(pop());
			push(from_float(x * y)); set_fp_cc(x * y);
			return exec_trap::none; }                          // FMPS
		case 075: { const float y = to_float(pop()), x = to_float(pop());
			push(from_float(x / y)); set_fp_cc(x / y);
			return exec_trap::none; }                          // FDVS
		case 076: { const float x = to_float(pop()); push(from_float(-x));
			set_fp_cc(-x); return exec_trap::none; }           // FNGS
		case 077: {                                            // FCPS
			const float y = to_float(pop()), x = to_float(stk(0));
			r.put(sf::CC, (x < y) ? CCL : (x == y) ? CCE : CCG);
			if (b3) r.s -= 4;
			return exec_trap::none;
		}
		case 057: {                                            // LCMP
			const u32 a = stk(0), b = stk(1);
			r.s -= 8;
			r.put(sf::CC, (b < a) ? CCL : (b == a) ? CCE : CCG);
			return exec_trap::none;
		}
		case 060: {                                            // LADD
			const u32 a = pop(), b = pop();
			const u64 sum = u64(b) + u64(a);
			r.put(sf::C, (sum >> 32) & 1);
			push(u32(sum)); cca(u32(sum));
			return exec_trap::none;
		}
		case 061: {                                            // LSUB
			const u32 a = pop(), b = pop();
			const u64 diff = u64(b) - u64(a);
			r.put(sf::C, (diff >> 32) & 1);
			push(u32(diff)); cca(u32(diff));
			return exec_trap::none;
		}
		case 062: {                                            // LMPY
			const u64 prod = u64(pop()) * u64(pop());
			push(u32(prod >> 32)); push(u32(prod));
			return exec_trap::none;
		}
		case 064: { const u32 v = ~pop(); push(v); cca(v);
			return exec_trap::none; }                          // NOT
		case 065: { const u32 a = pop(), b = pop(); const u32 v = b | a;
			push(v); cca(v); return exec_trap::none; }         // OR
		case 066: { const u32 a = pop(), b = pop(); const u32 v = b ^ a;
			push(v); cca(v); return exec_trap::none; }         // XOR
		case 067: { const u32 a = pop(), b = pop(); const u32 v = b & a;
			push(v); cca(v); return exec_trap::none; }         // AND
		}
		return exec_trap::unimplemented;
	}

	// Format 4: specials. Bit 3 of the instruction is CIR(19:1), which most
	// of these use as their index flag.
	exec_trap exec_fmt4(u8 op3, bool b3)
	{
		// The privileged absolute-address group: the operand is an absolute
		// address rather than a segment-relative one, optionally indexed
		// (ERS sheets 60, 65).
		const u32 xx = b3 ? r.x : 0;

		switch (op3)
		{
		case 002: {                                             // LBIT
			// The top of stack is a bit offset: bits 0-28 give the byte
			// index, which goes to X, and bits 29-31 the bit within that
			// byte, counted from the most significant (ERS sheet 61).
			const u32 spec = stk(0);
			const u32 bitno = spec & 7;
			r.x = u32(s32(spec) >> 3);
			const u32 ptr = stk(1);
			r.s -= 8;
			const ea_result e = m_au.resolve_data_pointer(ptr, true, false);
			m_addr_trap = e.t;
			if (!e.ok()) return exec_trap::address_error;
			const u32 v = (m_mem.read8(e.addr) >> (7 - bitno)) & 1;
			push(v); cca(v);
			return exec_trap::none;
		}
		case 003: {                                             // SBIT
			const u32 spec = stk(0);
			const u32 bitno = spec & 7;
			r.x = u32(s32(spec) >> 3);
			const u32 ptr = stk(1);
			const u32 bit = stk(2) & 1;
			r.s -= 12;
			const ea_result e = m_au.resolve_data_pointer(ptr, true, true);
			m_addr_trap = e.t;
			if (!e.ok()) return exec_trap::address_error;
			const u8 mask = u8(1u << (7 - bitno));
			const u8 old = m_mem.read8(e.addr);
			m_mem.write8(e.addr, bit ? (old | mask) : (old & u8(~mask)));
			return exec_trap::none;
		}
		case 005: return stack_indirect(b3, 4, false);          // LSI
		case 006: return stack_indirect(b3, 1, false);          // LBSI
		case 007: return stack_indirect(b3, 8, false);          // LDSI
		case 010: return stack_indirect(b3, 2, false);          // LHSI
		case 025: return stack_indirect(b3, 4, true);           // SSI
		case 026: return stack_indirect(b3, 1, true);           // SBSI
		case 027: return stack_indirect(b3, 8, true);           // SDSI
		case 030: return stack_indirect(b3, 2, true);           // SHSI

		case 047: {                                             // PLBA
			if (!r.privileged()) return exec_trap::unimplemented;
			const u32 v = m_mem.read8(stk(0) + xx);
			set_stk(0, v); cca(v);
			return exec_trap::none;
		}
		case 050: {                                             // PSBA
			if (!r.privileged()) return exec_trap::unimplemented;
			const u32 v = pop(), a = pop();
			m_mem.write8(a + xx, u8(v));
			return exec_trap::none;
		}
		case 051: {                                             // PLHA
			if (!r.privileged()) return exec_trap::unimplemented;
			const u32 v = m_mem.read16(stk(0) + xx);
			set_stk(0, v); cca(v);
			return exec_trap::none;
		}
		case 052: {                                             // PSHA
			if (!r.privileged()) return exec_trap::unimplemented;
			const u32 v = pop(), a = pop();
			m_mem.write16(a + xx, u16(v));
			return exec_trap::none;
		}
		case 064: {                                             // PLDA
			if (!r.privileged()) return exec_trap::unimplemented;
			const u32 v = m_mem.read32(stk(0) + xx);
			set_stk(0, v); cca(v);
			return exec_trap::none;
		}
		case 065: {                                             // PSTA
			if (!r.privileged()) return exec_trap::unimplemented;
			const u32 v = pop(), a = pop();
			m_mem.write32(a + xx, v);
			return exec_trap::none;
		}

		case 011: r.put(sf::I, b3 ? 1 : 0);                     // INT
			return exec_trap::none;

		case 040: {                                             // TRY
			// Creates or deletes a pseudo stack marker for the high level
			// language TRY feature (ERS sheet 126). The frame has the same
			// four-word shape as a real stack marker, but its first slot
			// chains to the previous frame instead of holding X. The chain
			// head lives at (SB+2) as an SB-relative byte offset.
			if (b3)
			{
				r.s += 12;                          // three more words
				set_word_below(r.s, 0, r.s - r.q);              // delta Q
				set_word_below(r.s, 1, r.status);
				set_word_below(r.s, 2, word_below(r.s, 3));     // recovery addr
				set_word_below(r.s, 3, sb_word(2));             // previous head
				set_sb_word(2, r.s - r.sb);
				return exec_trap::none;
			}
			const u32 temp = r.sb + sb_word(2);
			if (!(r.q + 16 < temp && temp < r.s))
				return exec_trap::bounds_violation;
			set_sb_word(2, word_below(temp, 3));
			r.s = temp - 16;
			set_sb_word(4, 0);
			return exec_trap::none;
		}

		case 041: {                                             // ESCP
			// Unwinds to the innermost TRY frame by pointing Q at it and
			// then performing an EXIT 0, which restores STATUS and the
			// recovery address from the pseudo marker (ERS sheet 127).
			bool flag;
			if (b3)
				flag = sb_word(4) != 0;
			else
			{
				set_sb_word(3, stk(0));
				r.s -= 4;
				flag = true;
			}
			if (!flag)
				return exec_trap::none;

			set_sb_word(4, 0xffffffffu);
			const u32 temp = r.sb + sb_word(2);
			if (!(r.sb + 48 < temp && temp < r.s))
				return exec_trap::bounds_violation;
			set_sb_word(2, word_below(temp, 3));
			r.q = temp;
			return do_exit(0);
		}

		case 070: {                                             // CPTR
			// Adds a byte offset to a data segment pointer, yielding a new
			// pointer to the same segment (ERS sheet 124). The external
			// data segment path, taken when bit 3 is set and the pointer's
			// top bit is clear, is not modelled yet.
			const u32 delta = stk(0);
			const u32 ptr = stk(1);
			if (b3 && ers32::get(ptr, 0, 1) == 0)
				return exec_trap::unimplemented;
			const u32 offset = ers32::get(ptr, 2, 30) + delta;
			if (ers32::get(offset, 0, 2) != 0)
				return exec_trap::pointer_conversion;
			r.s -= 4;
			set_stk(0, ers32::set(ptr, 2, 30, offset));
			return exec_trap::none;
		}

		case 077: m_halted = true; return exec_trap::halted;    // STOP
		}
		return exec_trap::unimplemented;
	}

	// The stack-indirect family: the top of stack holds a data segment
	// pointer, which is evaluated and then either loaded from or stored to
	// (ERS sheets 58-59, 63-64). `width` selects byte, halfword, word or
	// doubleword.
	exec_trap stack_indirect(bool indexed, unsigned width, bool store)
	{
		const u32 ptr = pop();
		const ea_result e = m_au.resolve_data_pointer(ptr, indexed, store);
		m_addr_trap = e.t;
		if (!e.ok())
			return exec_trap::address_error;

		if (!store)
		{
			switch (width)
			{
			case 1: push(m_mem.read8(e.addr)); break;
			case 2: push(m_mem.read16(e.addr)); break;
			case 4: push(m_mem.read32(e.addr)); break;
			case 8: push(m_mem.read32(e.addr));
				push(m_mem.read32(e.addr + 4)); break;
			}
			cca(stk(0));
			return exec_trap::none;
		}

		switch (width)
		{
		case 1: m_mem.write8(e.addr, u8(pop())); break;
		case 2: m_mem.write16(e.addr, u16(pop())); break;
		case 4: m_mem.write32(e.addr, pop()); break;
		case 8: { const u32 low = pop(), high = pop();
			m_mem.write32(e.addr, high); m_mem.write32(e.addr + 4, low); } break;
		}
		return exec_trap::none;
	}

	void set_overflow_add(u32 a, u32 b, u32 res)
	{
		r.put(sf::O, (~(a ^ b) & (a ^ res)) >> 31);
	}
	void set_overflow_sub(u32 a, u32 b, u32 res)
	{
		r.put(sf::O, ((a ^ b) & (a ^ res)) >> 31);
	}

	// Format 2: 6-bit operand -- shifts and short branches.
	exec_trap exec_fmt2(u8 op2, u8 operand, bool b3)
	{
		// Short branches. Backward variants have bit 2 of the opcode set.
		auto rel = [&]() { branch_to(u32(r.p + ((op2 & 4) ? -s32(operand)
				: s32(operand)) * 2)); };

		switch (op2)
		{
		case 002: { u32 v = stk(0) << operand; set_stk(0, v); cca(v);
			return exec_trap::none; }                            // LSL
		case 003: { u32 v = stk(0) >> operand; set_stk(0, v); cca(v);
			return exec_trap::none; }                            // LSR
		case 004: { const unsigned n = operand & 31;              // CSL
			const u32 x = stk(0);
			const u32 v = n ? ((x << n) | (x >> (32 - n))) : x;
			set_stk(0, v); cca(v); return exec_trap::none; }
		case 005: { const unsigned n = operand & 31;              // CSR
			const u32 x = stk(0);
			const u32 v = n ? ((x >> n) | (x << (32 - n))) : x;
			set_stk(0, v); cca(v); return exec_trap::none; }
		case 006: { u32 v = stk(0) << operand; set_stk(0, v); cca(v);
			return exec_trap::none; }                            // ASL
		case 007: { u32 v = u32(s32(stk(0)) >> operand); set_stk(0, v);
			cca(v); return exec_trap::none; }                    // ASR
		case 010: r.x <<= operand; return exec_trap::none;        // LSXL
		case 051: {                                               // MVB
			// Move Bytes (ERS sheet 101). The stack holds a byte count on
			// top, then a target pointer, then a source pointer. Bit 3 of
			// the instruction picks the direction; within the 6-bit
			// operand, bit 3 says the source is a data segment pointer
			// rather than a local program pointer, and bits 4-5 give how
			// many words to pop at the end.
			const int d = b3 ? -1 : 1;
			const unsigned sdec = operand & 3;
			const bool src_is_ds = ((operand >> 2) & 1) != 0;

			const s32 count = s32(stk(0));
			const u32 tptr = stk(1);
			const u32 sptr = stk(2);

			u32 sba;
			if (src_is_ds)
			{
				const ea_result e = m_au.resolve_data_pointer(sptr, false, false);
				m_addr_trap = e.t;
				if (!e.ok()) return exec_trap::address_error;
				sba = e.addr;
			}
			else
			{
				// A local program pointer is a byte offset from PB, bounds
				// checked against PB and PL (ERS convention note 18).
				sba = r.pb + sptr;
				if (!m_au.in_code(sba))
					return exec_trap::bounds_violation;
			}

			const ea_result te = m_au.resolve_data_pointer(tptr, false, true);
			m_addr_trap = te.t;
			if (!te.ok()) return exec_trap::address_error;
			u32 tba = te.addr;

			s32 x = 0;
			while (count > d * x)
			{
				m_mem.write8(tba, m_mem.read8(sba));
				tba = u32(s32(tba) + d);
				sba = u32(s32(sba) + d);
				x += d;
			}
			r.x = u32(x);
			set_stk(0, u32(count - d * x));
			r.s -= 4 * sdec;
			return exec_trap::none;
		}
		case 037: {                                               // TCC
			// Pushes 1 if the condition code matches the mask, else 0
			// (ERS sheet 123). The mask is CIR(29:3), the low three bits
			// of the operand.
			const u32 v = cc_taken(operand & 7) ? 1u : 0u;
			push(v);
			return exec_trap::none;
		}

		case 040: case 044:                                       // BIR
			if (r.test(sf::IR)) { r.put(sf::IR, 0); rel(); }
			return exec_trap::none;
		case 041: case 045: rel(); return exec_trap::none;        // BUN
		case 060: case 064:                                       // BCY
			if (r.test(sf::C)) rel();
			return exec_trap::none;
		case 061: case 065:                                       // BNC
			if (!r.test(sf::C)) rel();
			return exec_trap::none;
		case 062: case 066:                                       // BOV
			if (r.test(sf::O)) { r.put(sf::O, 0); rel(); }
			return exec_trap::none;
		case 063: case 067:                                       // BNO
			if (!r.test(sf::O)) rel(); else r.put(sf::O, 0);
			return exec_trap::none;
		case 070: case 074:                                       // BRZ
			if (pop() == 0) rel();
			return exec_trap::none;
		case 071: case 075:                                       // BRE
			if ((pop() & 1) == 0) rel();
			return exec_trap::none;
		case 072: case 076:                                       // BRO
			if ((pop() & 1) != 0) rel();
			return exec_trap::none;
		}
		(void)b3;
		return exec_trap::unimplemented;
	}

	// Format 5: 8-bit operand.
	exec_trap exec_fmt5(u16 w)
	{
		const u8 op2 = (w >> 8) & 0x1f;
		const u8 imm = w & 0xff;

		switch (op2)
		{
		// Debug aids. Each traps only when its enable bit is set in STATUS;
		// with tracing off they are the cheapest instruction there is, which
		// is why compilers emit SOL at every source line (ERS sheet 121).
		case 000: return r.test(sf::SL)
				? take_trap(trap_cause::start_of_line, imm)
				: exec_trap::none;                               // SOL
		case 001: return r.test(sf::EP)
				? take_trap(trap_cause::start_of_procedure, imm)
				: exec_trap::none;                               // SOP
		case 016: return r.test(sf::SS)
				? take_trap(trap_cause::start_of_subroutine, imm)
				: exec_trap::none;                               // SOS
		case 031: return r.test(sf::SL)
				? take_trap(trap_cause::start_of_line_check, imm)
				: exec_trap::none;                               // SOLC
		case 002: {                                             // CMPN
			const s32 a = s32(pop());
			const s32 b = -s32(u32(imm));
			r.put(sf::CC, (a < b) ? CCL : (a == b) ? CCE : CCG);
			return exec_trap::none;
		}
		case 003: {                                             // CMPI
			const s32 a = s32(pop());
			r.put(sf::CC, (a < s32(imm)) ? CCL : (a == s32(imm)) ? CCE : CCG);
			return exec_trap::none;
		}
		case 004: { const u32 v = stk(0) + imm; set_stk(0, v); cca(v);
			return exec_trap::none; }                            // ADDI
		case 005: { const u32 v = stk(0) - imm; set_stk(0, v); cca(v);
			return exec_trap::none; }                            // SUBI
		case 006: { const u32 v = u32(s32(stk(0)) * s32(u32(imm)));
			set_stk(0, v); cca(v); return exec_trap::none; }     // MPYI
		case 007: {                                              // DIVI
			if (imm == 0) return exec_trap::divide_by_zero;
			const u32 v = u32(s32(stk(0)) / s32(u32(imm)));
			set_stk(0, v); cca(v); return exec_trap::none;
		}
		case 012: r.x = imm; return exec_trap::none;             // LDXI
		case 013: r.x = u32(-s32(u32(imm))); return exec_trap::none; // LDXN
		case 014: push(imm); cca(imm); return exec_trap::none;   // LDIS
		case 015: { const u32 v = u32(-s32(u32(imm))); push(v); cca(v);
			return exec_trap::none; }                            // LNIS
		case 017: return do_io(io_kind::write, imm);             // IOW
		case 020: return do_io(io_kind::read, imm);              // IOR
		case 021: return do_io(io_kind::control, imm);           // IOC
		case 022: return do_call(4u * imm);                      // PCLS
		case 023: return do_exit(imm);                           // EXIT
		case 024: return do_exit(imm);                           // SXIT
		case 026: r.x += imm; return exec_trap::none;            // ADXI
		case 027: r.x -= imm; return exec_trap::none;            // SBXI
		case 032: r.s += 4u * imm; return exec_trap::none;       // ADDS
		case 033: r.s -= 4u * imm; return exec_trap::none;       // SUBS
		case 035: { const u32 v = stk(0) | imm; set_stk(0, v); cca(v);
			return exec_trap::none; }                            // ORI
		case 036: { const u32 v = stk(0) ^ imm; set_stk(0, v); cca(v);
			return exec_trap::none; }                            // XORI
		case 037: { const u32 v = stk(0) & imm; set_stk(0, v); cca(v);
			return exec_trap::none; }                            // ANDI
		}
		return exec_trap::unimplemented;
	}

	// Format 6: 32-bit.
	exec_trap exec_fmt6()
	{
		const u32 d = m_cir;
		const u8 op2 = (d >> 24) & 0x1f;
		const bool ind = (d >> 23) & 1, idx = (d >> 22) & 1;
		const base_reg base = base_reg((d >> 19) & 7);
		const u32 off = d & 0x7ffff;

		switch (op2)
		{
		case 000: {                                               // EXF
			// J = CIR(27:5) start bit, K = CIR(22:5) = 32 - width; bit 8
			// sign extends, bit 9 indexes (ERS sheet 116).
			u32 fields = d & 0x3ff;
			if (idx) fields += r.x;
			const u32 j = fields & 0x1f, k = (fields >> 5) & 0x1f;
			const u32 len = 32 - k;
			const u32 src = pop();
			u32 v = (len >= 32) ? src : ((src << j) >> (32 - len));
			if (ind && len < 32 && (v >> (len - 1)) & 1)
				v |= ~((u32(1) << len) - 1);      // sign extend
			push(v); cca(v);
			return exec_trap::none;
		}
		case 001: {                                               // DPF
			u32 fields = d & 0x3ff;
			if (idx) fields += r.x;
			const u32 j = fields & 0x1f, k = (fields >> 5) & 0x1f;
			const u32 len = 32 - k;
			const u32 src = pop();
			u32 dst = stk(0);
			if (len < 32)
			{
				const u32 mask = ((u32(1) << len) - 1) << (32 - j - len);
				dst = (dst & ~mask) | ((src << (32 - j - len)) & mask);
			}
			else
				dst = src;
			set_stk(0, dst); cca(dst);
			return exec_trap::none;
		}
		case 003: return do_call(off);                            // PCL
		case 006: push(off); cca(off); return exec_trap::none;    // LDI
		case 007: { const u32 v = u32(-s32(off)); push(v); cca(v);
			return exec_trap::none; }                             // LNI
		case 010: {                                               // LD
			u32 a;
			if (!ea(base, off, ind, idx, false, a))
				return exec_trap::address_error;
			push(m_mem.read32(a)); cca(stk(0));
			return exec_trap::none;
		}
		case 002: {                                               // CHEK
			// Range check: traps if the top of stack lies outside the two
			// immediate bounds (ERS sheet 128).
			const s32 v = s32(stk(0));
			const s32 lo = s32(u32((d >> 12) & 0x3ff));
			const s32 hi = s32(u32(d & 0xfff));
			if (v < lo || v > hi)
				return exec_trap::bounds_violation;
			return exec_trap::none;
		}
		case 013: {                                               // ST
			u32 a;
			if (!ea(base, off, ind, idx, true, a))
				return exec_trap::address_error;
			m_mem.write32(a, pop());
			return exec_trap::none;
		}
		case 014: {                                               // LDB
			u32 a;
			if (!ea(base, off, ind, idx, false, a))
				return exec_trap::address_error;
			const u32 v = m_mem.read8(a);
			push(v); cca(v);
			return exec_trap::none;
		}
		case 015: {                                               // CMPM
			u32 a;
			if (!ea(base, off, ind, idx, false, a))
				return exec_trap::address_error;
			const s32 x = s32(pop()), y = s32(m_mem.read32(a));
			r.put(sf::CC, (x < y) ? CCL : (x == y) ? CCE : CCG);
			return exec_trap::none;
		}
		case 020: {                                               // LDD
			u32 a;
			if (!ea(base, off, ind, idx, false, a))
				return exec_trap::address_error;
			push(m_mem.read32(a)); push(m_mem.read32(a + 4));
			return exec_trap::none;
		}
		case 023: case 024: {                                     // DECM/INCM
			u32 a;
			if (!ea(base, off, ind, idx, true, a))
				return exec_trap::address_error;
			const u32 v = m_mem.read32(a) + ((op2 == 024) ? 1u : u32(-1));
			m_mem.write32(a, v); cca(v);
			return exec_trap::none;
		}
		case 022: {                                               // LDH
			u32 a;
			if (!ea(base, off, ind, idx, false, a))
				return exec_trap::address_error;
			const u32 v = m_mem.read16(a);
			push(v); cca(v);
			return exec_trap::none;
		}
		case 025: {                                               // STH
			u32 a;
			if (!ea(base, off, ind, idx, true, a))
				return exec_trap::address_error;
			m_mem.write16(a, u16(pop()));
			return exec_trap::none;
		}
		case 026: {                                               // LDX
			u32 a;
			if (!ea(base, off, ind, idx, false, a))
				return exec_trap::address_error;
			r.x = m_mem.read32(a);
			return exec_trap::none;
		}
		case 030: {                                               // BR
			u32 a;
			if (!ea(base, off, ind, idx, false, a))
				return exec_trap::address_error;
			branch_to(a);
			return exec_trap::none;
		}
		case 031: {                                               // BCC
			const u8 ccf = (d >> 20) & 7;
			if (cc_taken(ccf))
			{
				const s32 delta = ((d >> 19) & 1) ? -s32(off) : s32(off);
				branch_to(u32(r.p + delta));
			}
			return exec_trap::none;
		}
		case 027: case 032: {                                     // INCF/DECF
			u32 a;
			if (!ea(base, off, ind, idx, true, a))
				return exec_trap::address_error;
			const u32 v = m_mem.read32(a) + ((op2 == 027) ? 1u : u32(-1));
			m_mem.write32(a, v); cca(v);
			return exec_trap::none;
		}
		case 034: {                                               // STB
			u32 a;
			if (!ea(base, off, ind, idx, true, a))
				return exec_trap::address_error;
			m_mem.write8(a, u8(pop()));
			return exec_trap::none;
		}
		case 035: {                                               // STD
			u32 a;
			if (!ea(base, off, ind, idx, true, a))
				return exec_trap::address_error;
			const u32 low = pop(), high = pop();
			m_mem.write32(a, high); m_mem.write32(a + 4, low);
			return exec_trap::none;
		}
		case 036: {                                               // LRA
			u32 a;
			if (!ea(base, off, ind, idx, false, a))
				return exec_trap::address_error;
			push(a);
			return exec_trap::none;
		}
		}
		return exec_trap::unimplemented;
	}

	//----------------------------------------------------------------
	//  I/O (FOCUS I/O ERS section 6)
	//----------------------------------------------------------------

	enum class io_kind { write, read, control };

	exec_trap do_io(io_kind kind, u8 opcode)
	{
		// A write takes its data from the top of stack and the interface
		// address from the word beneath; a read or control takes the
		// interface address from the top.
		u32 data = 0;
		if (kind == io_kind::write)
			data = pop();
		const u32 iface = pop();

		io_interface::target t;
		t.opcode  = opcode;
		t.ic      = u8(ers32::get(iface, 22, 4));
		t.channel = u8(ers32::get(iface, 26, 3));
		t.pa      = u8(ers32::get(iface, 29, 3));

		bool ok = false;
		switch (kind)
		{
		case io_kind::write:   ok = m_io->io_write(t, data); break;
		case io_kind::read:    ok = m_io->io_read(t, data); break;
		case io_kind::control: ok = m_io->io_control(t); break;
		}

		if (!ok)
		{
			// Message register bit 7 is "illegal I/O opcode" (ERS sheet 13).
			r.msg |= ers32::mask(7, 1);
			return take_trap(trap_cause::message_trap, opcode);
		}

		if (kind == io_kind::read)
		{
			push(data);
			cca(data);
		}
		return exec_trap::none;
	}

	//----------------------------------------------------------------
	//  internal interrupts and traps (ERS sheets 44-45)
	//----------------------------------------------------------------

	// Code segment 1 is reserved for trap handlers and is assumed resident,
	// so the usual trap conditions are not retested while entering it.
	//
	// The sequence, per sheet 44:
	//   1  clear the condition; form an external program pointer to segment
	//      1, STT entry = the cause
	//   2  push a stack marker
	//   3  adjust the code segment use counts
	//   4  push a parameter; if none is needed it is the program pointer
	//   5  set PB and PL from CST entry 1
	//   6  clear STATUS except the mode bit and the code segment number
	//   7  set P from the STT entry
	//
	// Step 3 is not modelled: use counts exist for the operating system's
	// benefit and nothing in the CPU depends on them yet.
	exec_trap take_trap(trap_cause cause, u32 parameter)
	{
		m_last_trap = cause;
		if (!m_traps_enabled)
			return exec_trap::trapped;

		// CST entry 1, from the system Code Segment Table pointer at
		// absolute location 0 (ERS Appendix D).
		const u32 cst = m_mem.read32(0);
		const u32 entry = cst + 16;
		const u32 len_m1 = m_mem.read32(entry + 4);
		const u32 location = m_mem.read32(entry + 8);

		const u32 ret = m_next - r.pb;
		const u32 old_q = r.q;

		r.s += 16;
		set_stk(3, r.x);
		set_stk(2, ret);
		set_stk(1, r.status);
		set_stk(0, r.s - old_q);
		r.q = r.s;

		push(parameter);

		r.pb = location;
		// LENGTH-1 always ends in 11 and PL must end in 00, so the last
		// word of the segment starts three bytes lower.
		r.pl = location + len_m1 - 3;

		// Only the mode bit and the code segment number survive.
		const u32 keep = ers32::mask(14, 1) | ers32::mask(20, 12);
		r.status &= keep;

		const u32 stt = m_mem.read32(r.pl - 4 * u32(cause));
		branch_to(r.pb + stt);
		return exec_trap::none;
	}

	//----------------------------------------------------------------
	//  procedure call and return (ERS sheets 92, 96)
	//----------------------------------------------------------------

	// PCL/PCLS. `offset` is a byte offset back from PL naming the Segment
	// Transfer Table entry; zero means the pointer is on the stack.
	//
	// Only local program pointers are handled: the pointer is a byte offset
	// from PB (ERS convention note 18). External pointers additionally need
	// the Code Segment Table walk and the U-bit privilege check.
	exec_trap do_call(u32 offset)
	{
		u32 ptr;
		if (offset == 0)
			ptr = pop();
		else
			ptr = m_mem.read32(r.pl - offset);

		const u32 ret = m_next - r.pb;      // PB-relative return address
		const u32 old_q = r.q;

		r.s += 16;                          // four words
		set_stk(3, r.x);
		set_stk(2, ret);
		set_stk(1, r.status);
		set_stk(0, r.s - old_q);            // delta Q
		r.q = r.s;

		const u32 target = r.pb + ptr;
		if (!m_au.in_code(target))
			return exec_trap::code_bounds;
		branch_to(target);
		return exec_trap::none;
	}

	// EXIT/SXIT. `n` counts words of parameters to drop beyond the marker.
	exec_trap do_exit(u8 n)
	{
		r.s = r.q;
		const u32 delta = stk(0);
		r.q = r.q - delta;
		r.status = stk(1);
		const u32 ret = stk(2);
		r.x = stk(3);
		r.s -= 16 + 4u * n;

		const u32 target = r.pb + ret;
		if (!m_au.in_code(target))
			return exec_trap::code_bounds;
		branch_to(target);
		return exec_trap::none;
	}

	memory_interface &m_mem;
	dst_walker m_dst;
	addressing_unit m_au;
	null_io m_null_io;
	io_interface *m_io;

	u32 m_cir = 0;
	u32 m_next = 0;
	bool m_branched = false;
	bool m_halted = false;
	bool m_traps_enabled = false;
	trap m_addr_trap = trap::none;
	trap_cause m_last_trap = trap_cause::reset;
};

} // namespace focus

#endif // MAME_CPU_FOCUS_FOCUSEXEC_H
