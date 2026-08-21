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
#include <cstdio>
#include <cstring>
#include <functional>

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
		: m_mem(mem), m_dst(r, mem), m_au(r, mem, m_dst), m_io(&m_null_io)
	{
		// The addressing unit's label-as-data fallback needs the CST walk,
		// which lives here.
		m_au.m_cst = [this](u32 seg, u32 &base, u32 &len) {
			u32 ea;
			if (!cst_entry_addr(seg, ea))
				return false;
			len = m_mem.read32(ea + 4);
			base = m_mem.read32(ea + 8);
			return base != 0;
		};
	}

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

	// Deliver an external interrupt between instructions. Honoured only
	// when STATUS.I permits; returns true when taken. A STOPped CPU
	// wakes to service it.
	bool interrupt(u32 device)
	{
		if (!r.test(sf::I))
			return false;
		m_halted = false;
		external_interrupt(device);
		return true;
	}

	// The control store release date RCS location 6 reports, as eight
	// hex digits YYYYMMDD. Software keys revision-dependent behaviour
	// off it, so it is worth being able to pose as another CPU.
	void set_control_store_date(u32 d) { m_control_store_date = d; }

	// A STOPped CPU resumes at the next instruction when an interrupt
	// request or MPB message arrives, even with STATUS.I clear -- the
	// dispatcher's idle loop is STOP followed by a branch back into the
	// scheduler, relying on exactly this.
	void resume() { m_halted = false; }

	// Raise a Message register bit (ERS sheet 13): 13 is the interval
	// timer, 15 the memory controller, 16-31 the I/O device interrupts,
	// one bit per IOP channel pair. A bit that MASK does not disable
	// wakes a STOPped CPU, and interrupts the running one when STATUS.I
	// is set.
	void raise_message(unsigned bit)
	{
		r.msg |= ers32::mask(bit, 1);
		if ((r.msg & ~r.mask) != 0)
			m_halted = false;
	}

	// The unmasked message bits, highest priority (lowest number) first.
	u32 pending_messages() const { return r.msg & ~r.mask; }

	// The front half of the microcode interrupt handler (ERS section
	// 5.1.3, flowchart sheet 142): pick the highest priority unmasked
	// I/O message bit, remove the first device from that level's list in
	// dedicated memory, clear the message bit when the list empties, and
	// vector through the device's DRT entry. The lists are the ones the
	// IOP builds: a (tail, head) pair of absolute pointers per level at
	// octal 2000, the link in the left halfword of each DRT entry's
	// first word (I/O ERS section 4.1.4).
	//
	// Returns true when an interrupt was taken. The host calls this
	// between instructions; conditions 1-4 of section 5.1.3 are exactly
	// the checks made here.
	bool service_interrupts()
	{
		if (!r.test(sf::I))
			return false;

		// The Message Trap comes before I/O interrupts in the priority
		// order (ERS sheet 43: message trap third, I/O fourth). Unmasked
		// bits in the 16 most significant bits of MSG with STATUS.I set
		// are cleared, and the handler runs on the ICS with a parameter
		// carrying the bits at Q+3 (section 5.2.2, cause 54).
		// Bit 10, the OS's own doorbell (SMSG,B to self), is poll-only:
		// the dispatcher consumes it lock-step at its STOP wake, and its
		// message-trap router reports an un-routable doorbell as system
		// error 054 and soft-halts. It wakes a STOPped CPU (raise_message
		// does that) but is never delivered as a trap.
		//
		// That reading came from BASIC, whose task mask (0x08A00000)
		// covers bit 10 anyway. HP-UX leaves it UNMASKED (0x08000000)
		// and parks a task on the doorbell semaphore at absolute 920, so
		// on that system the bit is set once and stands forever with
		// nothing to consume it. FOCUS_MSG10TRAP lets the mask alone
		// decide, which is what the machine documents.
		static const bool msg10_trap = std::getenv("FOCUS_MSG10TRAP") != nullptr;
		const u32 sysmsg = r.msg & ~r.mask & 0xffff0000u
				& (msg10_trap ? ~0u : ~ers32::mask(10, 1));
		if (sysmsg != 0)
		{
			// PSDB holds this trap off: "Pseudo Interrupt Disable" is
			// what its name says, and the dispatcher and the doorbell
			// enqueue paths bracket themselves in PSDB/PSEB precisely so
			// a pending doorbell bit cannot trap them mid-update. The
			// count lives at (QI - 4); this OS installs no handler at
			// STT[054] at all, so any delivery inside those brackets
			// vectors through garbage and panics -- the gate is not an
			// optimisation but the machine's advertised behaviour.
			const u32 qi = m_mem.read32(ded_base() + 8);
			if (qi != 0 && m_mem.read32(qi - 3 - 16) == 0
					&& message_handler_installed())
			{
				r.msg &= ~sysmsg;
				m_halted = false;
				message_trap(sysmsg);
				return true;
			}
		}

		const u32 pend = r.msg & ~r.mask;
		for (unsigned bit = 16; bit < 32; bit++)
		{
			if (!(pend & ers32::mask(bit, 1)))
				continue;
			const unsigned level = bit - 16;
			const u32 la = 1024 + 8 * level;          // octal 2000 + 2 words per level
			const u32 head = m_mem.read32(la + 4);
			if (head == 0 || head == 0xffffffffu)
			{
				// All devices at this level serviced: clear the bit.
				r.msg &= ~ers32::mask(bit, 1);
				continue;
			}
			// This OS never fills the DRT's program-pointer word: its
			// interrupt service is software, driven by RIRQ polling from
			// the woken dispatcher, with the DRT holding only per-device
			// data records. A zero program word therefore means "wake
			// only": clear the message bit and leave the device queued
			// for the OS to find, rather than vectoring through address
			// zero.
			if (m_mem.read32(head + 8) == 0)
			{
				r.msg &= ~ers32::mask(bit, 1);
				continue;
			}
			const u32 link = m_mem.read32(head) >> 16;
			if (link == 0)
			{
				m_mem.write32(la + 4, 0);
				m_mem.write32(la, 0);
				r.msg &= ~ers32::mask(bit, 1);        // list now empty
			}
			else
				m_mem.write32(la + 4, link);

			m_halted = false;
			external_interrupt(head);

			// "Mask current & lower int. levels" -- the service routine
			// runs shielded from its own level and everything below it,
			// and IXIT restores the mask from the frame at Q+2.
			for (unsigned b = bit; b < 32; b++)
				r.mask |= ers32::mask(b, 1);
			return true;
		}
		return false;
	}

	trap_cause last_trap() const { return m_last_trap; }
	// A host that wants to see where a fault came from hooks this; it is
	// called before the trap is routed, with the processor still on the
	// faulting instruction.
	std::function<void(trap_cause, u32)> on_trap;
	// Every ESCP, with the value it escapes with: the driver layers pass
	// their failures up this way, so this is where an error number that
	// never reaches the screen can still be caught.
	std::function<void(u32, u32)> on_escape;
	// Every data effective address as it is computed, with the
	// instruction that asked for it: the way to see which word a load
	// through a pointer actually reached.
	std::function<void(u32, u32)> on_ea;
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
		if (on_ea && e.ok())
			on_ea(r.p, e.addr);
		return e.ok();
	}

	void branch_to(u32 target)
	{
		// A target outside real memory is always a bug in this emulator,
		// and the instruction that produced it is the one still in CIR.
		// Reporting it here names the culprit; by the time the fetch
		// fails the trail is cold.
		if (target >= 0x1000000u && std::getenv("FOCUS_FLAGBR"))
			std::fprintf(stderr, "[core] branch to %08x from P=%08x "
					"PB=%08x CIR=%08x\n", target, r.p, r.pb, m_cir);
		r.p = target;
		m_branched = true;
	}

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
	// and neither is modelled yet -- see the note in FOCUS.md 10.8.
	//
	// A double occupies two stack words, high word below low, matching the
	// order LDD pushes them in.

	static double to_double(u64 b) { double d; std::memcpy(&d, &b, 8); return d; }
	static u64 from_double(double d) { u64 b; std::memcpy(&b, &d, 8); return b; }
	static float to_float(u32 b) { float f; std::memcpy(&f, &b, 4); return f; }
	static u32 from_float(float f) { u32 b; std::memcpy(&b, &f, 4); return b; }

	//----------------------------------------------------------------
	//  packed decimal
	//----------------------------------------------------------------
	//
	// A decimal word is eight BCD digits, most significant first. A digit
	// above nine is an illegal operand rather than something to normalise.

	static bool bcd_valid(u32 v)
	{
		for (unsigned i = 0; i < 8; i++)
			if (((v >> (4 * i)) & 0xf) > 9) return false;
		return true;
	}

	static u32 bcd_add(u32 a, u32 b, unsigned cin, unsigned &cout)
	{
		u32 out = 0;
		unsigned c = cin;
		for (unsigned i = 0; i < 8; i++)
		{
			unsigned d = ((a >> (4 * i)) & 0xf) + ((b >> (4 * i)) & 0xf) + c;
			c = (d > 9) ? 1 : 0;
			if (c) d -= 10;
			out |= d << (4 * i);
		}
		cout = c;
		return out;
	}

	static u32 bcd_sub(u32 a, u32 b, unsigned bin, unsigned &bout)
	{
		u32 out = 0;
		unsigned brw = bin;
		for (unsigned i = 0; i < 8; i++)
		{
			int d = int((a >> (4 * i)) & 0xf)
					- int((b >> (4 * i)) & 0xf) - int(brw);
			brw = (d < 0) ? 1u : 0u;
			if (brw) d += 10;
			out |= u32(d) << (4 * i);
		}
		bout = brw;
		return out;
	}

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
			return do_lra(base, raw, ind, idx);
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
		case 011:                                              // DLAD
		case 012: {                                            // DLSB
			// Decimal Logical Add and Subtract (ERS sheet 84). The top two
			// words are eight packed BCD digits each. Bit 3 chains a carry
			// in, which is how multiprecision decimal arithmetic is built,
			// and the same bit is why a zero result then leaves the
			// condition code alone: a zero limb must not erase the
			// "nonzero" that a previous limb reported.
			const u32 a = stk(0), b = stk(1);
			if (!bcd_valid(a) || !bcd_valid(b))
				return take_trap(trap_cause::illegal_decimal, m_cir);

			const bool sub = op3 == 012;
			// The ERS carries a borrow inverted -- DLSB subtracts
			// "1-Carry" -- so here Carry set means no borrow, and the
			// result is written back the same way.
			const unsigned cin = !b3 ? 0u
					: (sub ? 1u - r.get(sf::C) : r.get(sf::C));
			unsigned cout = 0;
			const u32 v = sub ? bcd_sub(b, a, cin, cout)
			                  : bcd_add(b, a, cin, cout);

			r.s -= 4;
			set_stk(0, v);
			r.put(sf::C, sub ? 1u - cout : cout);
			if (v != 0)   r.put(sf::CC, CCG);
			else if (!b3) r.put(sf::CC, CCE);
			return exec_trap::none;
		}
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
			// S := if CIR(19:1)=1 then S-2 else S-1 (ERS sheet 61): both
			// operands are deleted only with the b bit; plain CMP keeps
			// the second word -- the powerup module's device loops rely
			// on the running index surviving the compare.
			const s32 b = s32(stk(1)), a = s32(stk(0));
			r.s -= b3 ? 8 : 4;
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
			// Divide (ERS sheet 59): the DOUBLEWORD integer in (S-2,S-1)
			// divided by the singleword in (S); the doubleword quotient
			// replaces the dividend and the remainder replaces the
			// divisor -- the stack shape does not change. A singles-only
			// version collapsed the stack a word per division and walked
			// BASIC's decimal formatter off its digit buffer. Bit 3
			// forces the remainder positive, as in DIVS.
			const s32 divisor = s32(stk(0));
			if (divisor == 0)
				return exec_trap::divide_by_zero;
			const s64 dividend = (s64(u64(stk(2)) << 32)) | u64(stk(1));
			s64 quot = dividend / divisor;
			s32 rem = s32(dividend % divisor);
			if (b3 && rem < 0)
				rem = -rem;
			set_stk(2, u32(u64(quot) >> 32));
			set_stk(1, u32(u64(quot)));
			set_stk(0, u32(rem));
			cca(u32(u64(quot)));
			return exec_trap::none;
		}
		case 024: {                                            // NEG
			const u32 v = u32(-s32(pop()));
			push(v); cca(v);
			return exec_trap::none;
		}
		// The test group deletes its operand only with the b bit
		// (ERS sheets 33-34.5).
		case 025: cca(stk(0)); if (b3) pop();
			return exec_trap::none;                            // TEST
		case 026: {                                            // TSTD
			const s64 v = (s64(u64(stk(1)) << 32) | stk(0));
			r.put(sf::CC, (v < 0) ? CCL : (v == 0) ? CCE : CCG);
			if (b3) { pop(); pop(); }
			return exec_trap::none;
		}
		case 027: r.put(sf::CC, (stk(0) & 0xff) ? CCG : CCE);
			if (b3) pop();
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
		case 034: {                                            // ISC
			// Integer Size Check (ERS sheet 87). Sets Overflow when the
			// halfword format cannot hold the full word integer on top --
			// that is, when the top seventeen bits are neither all zero
			// nor all one, which is the sign-extension test. The operand
			// is left alone; the compiled form is FIXD,B / ISC / STH.
			//
			// The pseudocode shows only the branch that sets Overflow,
			// never one that clears it. Read literally that would make the
			// flag sticky and the check useless without an explicit clear,
			// and the indicator line names Overflow the way ADD's does --
			// which this core assigns. So it is assigned here too.
			const u32 v = stk(0);
			const u32 top = v >> 15;
			r.put(sf::O, (top != 0 && top != 0x1ffff) ? 1 : 0);
			cca(v);
			return exec_trap::none;
		}
		case 035: {                                            // XAX
			const u32 a = stk(0);
			set_stk(0, r.x); r.x = a;
			return exec_trap::none;
		}
		case 036: r.x += stk(0); if (b3) pop(); cca(r.x);
			return exec_trap::none;                            // ADAX
		case 037: { const u32 v = stk(0) + r.x; set_stk(0, v); cca(v);
			return exec_trap::none; }                          // ADXA
		case 040: pop(); return exec_trap::none;               // DEL
		case 043: r.x = stk(0); if (b3) pop(); cca(r.x);
			return exec_trap::none;                            // STAX
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
			if (b3) { pop(); pop(); }
			return exec_trap::none;                            // FTST
		case 004: set_fp_cc(to_float(stk(0)));
			if (b3) pop();
			return exec_trap::none;                            // FTSS
		case 030: { const double d = pop_double();             // CDSF
			const float f = float(d);
			push(from_float(f)); set_fp_cc(f);
			return exec_trap::none; }
		case 031: { const double d = double(to_float(pop())); // CSDF
			push_double(d); set_fp_cc(d);
			return exec_trap::none; }
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
			// Same operand-delete rule as CMP: both only with the b bit.
			const u32 a = stk(0), b = stk(1);
			r.s -= b3 ? 8 : 4;
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
			// Unlike LBIT, the operands run the other way up (ERS sheet
			// 66): "a 2's complement bit offset in the second word on the
			// stack and a data segment pointer in the third word... the
			// least significant bit of the TOS is stored". So the value
			// is on top, the offset beneath it and the pointer below
			// that, and all three are popped.
			const u32 bit = stk(0) & 1;
			const u32 spec = stk(1);
			const u32 bitno = spec & 7;
			r.x = u32(s32(spec) >> 3);
			const u32 ptr = stk(2);
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
		case 020:                                               // UP
		case 021: {                                             // DOWN
			// The semaphore signal/wait pair, named only by the shipped
			// assembler's table and executed only by the HP-UX kernels.
			// Semantics derived from the BASIC kernel's software
			// implementation of the same structure (routine at 0x45720
			// of the diagnostics environment, and the dispatcher that
			// consumes it): the operand names a two-word semaphore --
			// word 0 the wait-list head, doubling as the multiprocessor
			// lock (-1 while held), the count in word 1.
			// Both HP-UX sites confirm the fixed layout ({0, 1} each
			// time -- an unlocked, one-unit mutex); the X the software
			// implementation loads before its own DECM is that
			// routine's business, not an operand of these.
			//
			// Both halves are modelled, blocking included: the same
			// software implementation fixes the TCB conventions the
			// microcode must share, so a DOWN that takes the count
			// negative parks the task and enters the dispatcher, and an
			// UP with waiters readies the first of them. What is not
			// modelled is real multiprocessing -- a semaphore another
			// processor holds locked is handled by spinning on the
			// instruction, which on one CPU can only mean the OS left
			// the word locked itself.
			if (!r.privileged()) return exec_trap::unimplemented;
			// The operand is a data segment pointer, like SEML's: the
			// kernels feed both LRA-built tagged pointers (DB- and
			// SB-relative) and raw absolute addresses. A raw absolute
			// reads as an external pointer with segment number zero,
			// which is invalid as an EDS reference, so that encoding is
			// free to mean absolute -- the privileged instructions' own
			// convention.
			// How the operand names the semaphore is what CIR(19:1) --
			// the ",B" flag -- selects, and the kernels' own data settle
			// it. The plain form takes a DATA SEGMENT POINTER: HP-UX's
			// unit semaphores arrive as 0x00780008 and 0x00780028, which
			// are external pointers to segment 15, and segment 15 holds
			// exactly the three-block structure with handles 0x28000,
			// 0x38000 and 0x48000 that BASIC's kernel builds, counts
			// initialised to 1. The ",B" form takes an ABSOLUTE address:
			// HP-UX's first DOWN of the boot names 0x00ffe6f8, up among
			// the TCBs and stacks, which as a pointer would be segment
			// 31 -- a 32-byte segment based at zero -- at offset half a
			// megabyte.
			// Reading every operand one way or the other was wrong in
			// both directions: as pointers the boot panicked on the
			// first DOWN, and as addresses two tasks parked on words of
			// untouched memory that no UP would ever signal.
			const u32 ptr = stk(0);
			u32 a;
			if (b3)
				a = ptr;                     // absolute address
			else
			{
				const ea_result e = m_au.resolve_data_pointer(ptr, false, true);
				m_addr_trap = e.t;
				if (!e.ok()) return exec_trap::address_error;
				a = e.addr;
			}
			// FOCUS_SEMALL logs EVERY UP and DOWN with its raw operand
			// and resolved address -- the census that catches a wake
			// landing on the wrong word, which the block/wake hook
			// cannot see (an UP with no waiter is silent there).
			static const bool sem_all = std::getenv("FOCUS_SEMALL") != nullptr;
			if (sem_all)
			{
				static unsigned census = 0;
				if (census++ < 400)
				{
					std::printf("[%s%s ptr=%08x -> %08x count=%d p=%08x]\n",
							op3 == 021 ? "DOWN" : "UP", b3 ? ",B" : "",
							ptr, a, s32(m_mem.read32(a + 4)), r.p);
					std::fflush(stdout);
				}
			}
			const u32 head = m_mem.read32(a);
			if (head == 0xffffffffu)
			{
				// Another processor holds the word; spin by retrying
				// the instruction. Single-CPU, this only happens if
				// the OS itself left it locked.
				branch_to(r.p);
				return exec_trap::none;
			}
			const u32 ca = a + 4;
			const s32 count = s32(m_mem.read32(ca));
			const u32 ctcbp = m_mem.read32(ded_base() + 4);
			if (op3 == 021)
			{
				m_mem.write32(ca, u32(count - 1));
				if (b3) pop();
				if (count - 1 >= 0)
					return exec_trap::none;

				// Block. The BASIC kernel's software version fixes the
				// conventions the microcode must share: the TCB's wait
				// link node lives at CTCBP-8 ({forward, backward}, the
				// backward convention LDEL expects), its state word at
				// CTCBP-16, low halfword 2 meaning blocked -- the TCB
				// stays spliced in the dispatcher's priority chain, whose
				// LLSH walk simply skips anything whose state is not 1.
				// Then the task leaves through the dispatcher, exactly
				// the entry DISP performs.
				const u32 link = ctcbp - 8;
				const u32 old_head = head;
				m_mem.write32(link, old_head);
				m_mem.write32(link + 4, a + 4);
				if (old_head != 0)
					m_mem.write32(old_head + 4, link + 4);
				m_mem.write32(a, link);
				const u32 sw = ctcbp - 16;
				m_mem.write32(sw,
						(m_mem.read32(sw) & 0xffff0000u) | 2);
				if (sem_hook) sem_hook(true, a, ctcbp, r.p);
				return do_disp();
			}
			else
			{
				m_mem.write32(ca, u32(count + 1));
				if (count + 1 <= 0)
				{
					// Wake the first waiter: unlink its node from the
					// list, mark its TCB ready, and ring the doorbell
					// bit the dispatcher's idle mask always leaves
					// enabled, so a STOPped processor comes back to
					// find it.
					const u32 l = head;
					if (l != 0)
					{
						const u32 fwd = m_mem.read32(l);
						m_mem.write32(a, fwd);
						if (fwd != 0)
							m_mem.write32(fwd + 4, a + 4);
						const u32 sw = l - 8;
						m_mem.write32(sw,
								(m_mem.read32(sw) & 0xffff0000u) | 1);
						r.msg |= 0x00200000u;
						m_halted = false;
						if (sem_hook) sem_hook(false, a, l - 0x10, r.p);
					}
				}
				if (b3) pop();
				return exec_trap::none;
			}
		}

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
		case 063: {                                             // PSLA
			// Test-and-set: read the word, leave -1 behind. The ERS marks
			// the pair uninterruptable by other processors; this core is
			// single-threaded, so issuing them back to back is enough.
			if (!r.privileged()) return exec_trap::unimplemented;
			const u32 e = stk(0) + xx;
			const u32 v = m_mem.read32(e);
			m_mem.write32(e, 0xffffffffu);
			set_stk(0, v); cca(v);
			return exec_trap::none;
		}
		case 064: {                                             // PLDA
			if (!r.privileged()) return exec_trap::unimplemented;
			const u32 v = m_mem.read32(stk(0) + xx);
			set_stk(0, v); cca(v);
			return exec_trap::none;
		}
		case 066: {                                             // CPAA
			// Replaces a data segment pointer with the absolute address it
			// evaluates to, so privileged code can then use the P*A forms.
			const ea_result e = m_au.resolve_data_pointer(stk(0), false, false);
			if (!e.ok())
			{
				m_addr_trap = e.t;
				return exec_trap::address_error;
			}
			set_stk(0, e.addr); cca(e.addr);
			return exec_trap::none;
		}
		case 013: {                                             // MUC
			// Adjusts the use count of the segment or page a pointer names,
			// which is how the memory manager pins something it is about to
			// touch. Bit 3 selects increment. The condition code reports the
			// outcome rather than trapping: CCE if absent, CCL for any other
			// evaluation error, CCG on success. The pointer is POPPED on
			// every path (ERS sheet 127: "S := S-1 ... The pointer is
			// popped from the stack") -- leaving it shifted the V wake's
			// stack one word and made it unlock the semaphore's own
			// pointer minus eight instead of readying the waiter's TCB.
			const u32 ptr = pop();
			const ea_result e = m_au.resolve_data_pointer(ptr, false, false);
			if (!e.ok())
			{
				const bool absent = e.t == trap::absent_segment
						|| e.t == trap::absent_page
						|| e.t == trap::absent_page_table;
				r.put(sf::CC, absent ? CCE : CCL);
				return exec_trap::none;
			}
			// Only an external data segment pointer -- bit 0 clear -- goes
			// through the DST and so has a use count at all. Stack and
			// global pointers resolve without touching a table, and asking
			// for a slot after one of those would read a stale answer.
			const bool external = ers32::get(ptr, 0, 1) == 0;
			const u32 slot = external ? m_dst.last_use_count_slot() : 0;
			if (slot != 0)
			{
				const u32 w = m_mem.read32(slot);
				u32 n = ers32::get(w, 1, 12);
				// The field is 12 bits; saturate rather than wrap, so a
				// miscounted release cannot make a pinned page look free.
				if (b3) { if (n != 0xfff) n++; }
				else    { if (n != 0) n--; }
				m_mem.write32(slot, ers32::set(w, 1, 12, n));
			}
			r.put(sf::CC, CCG);
			return exec_trap::none;
		}
		case 065: {                                             // PSTA
			if (!r.privileged()) return exec_trap::unimplemented;
			const u32 v = pop(), a = pop();
			m_mem.write32(a + xx, v);
			return exec_trap::none;
		}

		case 037: {                                             // BRSI
			// Branch Stack Indirect (ERS sheet 108.1): a faster BR S-0,I
			// that also pops the indirect pointer. The pointer follows
			// the STT convention: bit 0 clear is a local program pointer,
			// a byte offset from PB with the uncallable bit ignored.
			const u32 ptr = pop();
			if (ptr & 0x80000000u)
				return exec_trap::unimplemented;    // external: needs CST
			const u32 target = r.pb + (ptr & 0x3fffffffu);
			if (!m_au.in_code(target))
				return exec_trap::code_bounds;
			branch_to(target);
			return exec_trap::none;
		}

		case 004: return r.test(sf::ES)                         // EOS
				? take_trap(trap_cause::end_of_subroutine, 0)
				: exec_trap::none;
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
			// The frame must lie between the marker and the stack limit.
			// The upper bound is SL, not S: the trap plumbing deletes a
			// frame after popping operands from above it, leaving S a few
			// words below the frame's top.
			const u32 temp = r.sb + sb_word(2);
			if (!(r.q + 16 <= temp && temp <= r.sl))
				return exec_trap::bounds_violation;
			set_sb_word(2, word_below(temp, 3));
			r.s = temp - 16;
			set_sb_word(4, 0);
			return exec_trap::none;
		}

		case 041: {                                             // ESCP
			if (on_escape)
				on_escape(r.p, b3 ? sb_word(3) : stk(0));
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
			if (!(r.sb + 48 < temp && temp <= r.sl))
				return exec_trap::bounds_violation;
			set_sb_word(2, word_below(temp, 3));
			r.q = temp;
			return do_exit(0);
		}

		case 067: {                                             // CPEP
			// Convert Pointer to External Data Segment Pointer (ERS sheet
			// 120). A stack or global pointer becomes an external one.
			// The segment numbers come from the current TCB rather than
			// from any register, which is why the ERS warns this must not
			// be executed while on the ICS.
			const u32 ptr = stk(0);

			// An external pointer is only touched when indexing is asked
			// for, and then it folds X into the offset as CPTR does.
			if (ers32::get(ptr, 0, 1) == 0)
			{
				if (!b3) return exec_trap::none;
				const s32 noff = s32(ers32::get(ptr, 13, 19)) + s32(r.x);
				if (noff < 0 || noff >= (1 << 19))
					return exec_trap::pointer_conversion;
				set_stk(0, ers32::set(ptr, 13, 19, u32(noff)));
				return exec_trap::none;
			}

			u32 offset = ers32::get(ptr, 2, 30);
			if (b3) offset += r.x;

			const u32 ctcbp = m_mem.read32(ded_base() + 4);
			const u32 w2 = m_mem.read32(ctcbp + 8);
			u32 seg;
			if (ers32::get(ptr, 0, 2) == 2)
				seg = ers32::get(w2, 20, 12);       // global segment
			else
			{
				// A stack pointer's offset is relative to SB, so it has
				// to be rebased onto the segment SB itself sits in.
				seg = ers32::get(w2, 4, 12);
				const u32 table = (seg >> 11)
						? r.dst : dst_walker::SYSTEM_DST_BASE;
				const u32 ea = table + 16 * (seg & 0x7ff);
				offset += r.sb - m_mem.read32(ea + 8);
			}

			if (ers32::get(offset, 0, 13) != 0)
				return exec_trap::pointer_conversion;

			u32 out = ers32::set(ptr, 0, 1, 0);
			out = ers32::set(out, 1, 12, seg);
			out = ers32::set(out, 13, 19, offset);
			set_stk(0, out);
			return exec_trap::none;
		}

		case 070: {                                             // CPTR
			// Adds a byte offset to a data segment pointer, yielding a new
			// pointer of the same type (ERS sheet 120). The delta lands in
			// X. With bit 3 set and an external pointer, the pointer is
			// fully evaluated: the delta folds into the 19-bit offset
			// field, trapping when it no longer fits. Otherwise the delta
			// simply adds to bits (2:30).
			const u32 delta = stk(0);
			const u32 ptr = stk(1);
			r.x = delta;
			if (b3 && ers32::get(ptr, 0, 1) == 0)
			{
				const s32 noff = s32(ers32::get(ptr, 13, 19)) + s32(delta);
				if (noff < 0 || noff >= (1 << 19))
					return exec_trap::pointer_conversion;
				r.s -= 4;
				set_stk(0, ers32::set(ptr, 13, 19, u32(noff)));
				return exec_trap::none;
			}
			const u32 offset = ers32::get(ptr, 2, 30) + delta;
			if (ers32::get(offset, 0, 2) != 0)
				return exec_trap::pointer_conversion;
			r.s -= 4;
			set_stk(0, ers32::set(ptr, 2, 30, offset));
			return exec_trap::none;
		}

		case 012: {                                             // GTSP
			// Get Task Segment Pointers (ERS sheet 122): pop a TCB
			// address into the CTCBP dedicated word, load the DST
			// register from TCB word 1, and push external data segment
			// pointers (offset 0) for the task's stack and global
			// segments from word 2, then this CPU's channel number.
			if (!r.privileged()) return exec_trap::unimplemented;
			const u32 tcb = pop();
			m_mem.write32(ded_base() + 4, tcb);
			const u32 w2 = m_mem.read32(tcb + 8);
			r.dst = m_mem.read32(tcb + 4);
			push(ers32::set(0, 1, 12, ers32::get(w2, 4, 12)));
			push(ers32::set(0, 1, 12, ers32::get(w2, 20, 12)));
			push(ers32::set(0, 29, 3, ers32::get(r.mask, 3, 3)));
			return exec_trap::none;
		}

		case 014: {                                             // XGDS
			// Exchange Global Data Segment (ERS sheet 68): the TOS holds
			// an external data segment pointer naming the new global
			// segment (offset ignored). DB and DL come from its DST
			// entry, and the pointer's table-select and segment fields
			// (1:12) are swapped with the TCB's global-segment field, so
			// the modified pointer on the stack can switch back later.
			if (!r.privileged()) return exec_trap::unimplemented;
			const u32 ptr = stk(0);
			const u32 seg = ers32::get(ptr, 2, 11);
			const u32 table = ers32::get(ptr, 1, 1)
					? r.dst : dst_walker::SYSTEM_DST_BASE;
			const u32 ea = table + 16 * seg;
			r.db = m_mem.read32(ea + 8);
			r.dl = r.db + m_mem.read32(ea + 4);
			const u32 ctcbp = m_mem.read32(ded_base() + 4);
			const u32 w = m_mem.read32(ctcbp + 8);
			const u32 old = ers32::get(w, 20, 12);
			m_mem.write32(ctcbp + 8,
					ers32::set(w, 20, 12, ers32::get(ptr, 1, 12)));
			set_stk(0, ers32::set(ptr, 1, 12, old));
			return exec_trap::none;
		}

		case 015: {                                             // PSDB
			// Pseudo Interrupt Disable: bump the dispatcher disable count
			// held at (QI-4) (ERS sheet 119). Privileged.
			if (!r.privileged()) return exec_trap::unimplemented;
			const u32 a = m_mem.read32(ded_base() + 8) - 3 - 16;
			m_mem.write32(a, m_mem.read32(a) + 1);
			return exec_trap::none;
		}
		case 016: return do_disp();                             // DISP
		case 017: {                                             // PSEB
			if (!r.privileged()) return exec_trap::unimplemented;
			const u32 a = m_mem.read32(ded_base() + 8) - 3 - 16;
			const u32 n = m_mem.read32(a);
			if (n == 0)
				return take_trap(trap_cause::inconsistent_registers, 0);
			m_mem.write32(a, n - 1);
			return exec_trap::none;
		}
		case 074: return do_ixit();                             // IXIT

		case 031: {                                             // RCS
			// Read Control Store. Location 6 holds the CPU release date as
			// eight hex digits YYYYMMDD (ERS Appendix G), which software
			// uses to decide which silicon bugs to work around -- and
			// which revision-dependent paths to take: SYSTEM POWERUP
			// compares it against 19810828 nine instructions in. The
			// default is CPU 4.0, the last revision the ERS describes.
			const u32 loc = stk(0);
			set_stk(0, (loc == 6) ? m_control_store_date : 0u);
			return exec_trap::none;
		}

		case 042:                                               // ALM
			m_address_lockout = b3;
			return exec_trap::none;

		case 043: {                                             // CCI
			// Convert Condition Code to Integer: -1, 0 or 1 for CCL, CCE
			// and CCG (ERS sheet 123).
			const u32 cc = r.condition_code();
			push(cc == CCE ? 0u : (cc == CCG ? 1u : u32(-1)));
			return exec_trap::none;
		}

		case 044: return r.test(sf::EP)                         // EOP
				? take_trap(trap_cause::end_of_procedure, 0)
				: exec_trap::none;

		case 057: {                                             // WTC
			// Write to Channel: the second stack word holds the 29-bit MPB
			// address, the top the data word (ERS sheet 125).
			//
			// Neither operand is popped. The ERS text does not say so
			// outright, but the MPB initialisation loop in BASIC's
			// SYSTEM POWERUP settles it: it keeps the address and data in
			// A and B, swapping them with XCH to bump the address by 4
			// each pass, which only works if WTC leaves the stack alone.
			mpb_write(stk(1), stk(0), b3);
			return exec_trap::none;
		}

		case 060: {                                             // SMSG
			// Send Message to an MPB channel; the low 29 bits of the top of
			// stack carry the channel address and the message itself. Like
			// WTC, the operand is left on the stack.
			mpb_message(stk(0), b3);
			return exec_trap::none;
		}

		case 062:                                               // RSC
			// Read Slave Channel: push the two MPB slave registers.
			push(r.sar);
			push(r.sdr);
			return exec_trap::none;

		case 055: {                                             // FLMO
			// Find Leftmost One (ERS sheet 128): the ERS bit number of
			// the highest set bit in the TOS, 32 when it is zero. The
			// result replaces the TOS, or is pushed when b is set.
			const u32 v = stk(0);
			u32 i = 32;
			if (v != 0)
			{
				i = 0;
				while (ers32::get(v, i, 1) == 0)
					i++;
			}
			if (b3)
				push(i);
			else
				set_stk(0, i);
			return exec_trap::none;
		}

		case 072: {                                             // LINS
			// List Insert (ERS sheet 116): doubly-linked-list insert over
			// absolute addresses. (S) is the forward-link word of an
			// existing entry, (S-1) the new entry's, (S-2) the byte
			// offset from a forward to its backward link word. Zero is
			// nil; inserting into an empty list makes the entry circular
			// and rewrites the TOS to name it. Nothing is popped: the
			// powerup module chains 2060-byte blocks with an
			// XCH / LINS / XCH pattern that reuses all three operands.
			if (!r.privileged()) return exec_trap::unimplemented;
			const u32 new_fwd = stk(1);
			const u32 off = stk(2);
			if (new_fwd == 0)
				return exec_trap::none;
			bool was_empty = false;
			if (stk(0) == 0)
			{
				set_stk(0, new_fwd);
				was_empty = true;
			}
			const u32 exist = stk(0);
			m_mem.write32(new_fwd, exist);
			const u32 bkw = m_mem.read32(exist + off);
			m_mem.write32(exist + off, new_fwd + off);
			if (!was_empty)
			{
				if (bkw != 0)
					m_mem.write32(bkw - off, new_fwd);
				m_mem.write32(new_fwd + off, bkw);
			}
			return exec_trap::none;
		}

		case 073: {                                             // LDEL
			// List Delete (ERS sheet 117): remove the entry whose forward
			// link word (S) addresses; (S-1) is the forward-to-backward
			// offset. CCE when the list is left empty -- the entry linked
			// only to itself -- else CCG.
			if (!r.privileged()) return exec_trap::unimplemented;
			const u32 entry = stk(0);
			const u32 off = stk(1);
			if (entry != 0)
			{
				const u32 fwd = m_mem.read32(entry);
				const u32 bkw = m_mem.read32(entry + off);
				if (bkw != 0)
					m_mem.write32(bkw - off, fwd);
				if (fwd != 0)
					m_mem.write32(fwd + off, bkw);
				r.put(sf::CC, fwd == entry ? CCE : CCG);
			}
			else
				r.put(sf::CC, CCE);
			return exec_trap::none;
		}

		case 076: {                                             // WAIT
			// A calibrated delay: decrement the top of stack to zero
			// (ERS sheet 95). Modelled as taking effect immediately.
			set_stk(0, 0);
			return exec_trap::none;
		}

		case 077:                                               // STOP
			// Halt until an interrupt or message; execution resumes at
			// the next instruction, so P moves past the STOP now. With
			// bit 3 the TOS is loaded into MASK first -- the dispatcher's
			// idle sequence computes the sleep-time interrupt mask on the
			// stack and hands it to STOP,B to arm atomically.
			if (b3)
				r.mask = pop();
			// I is left as the dispatcher set it (INT,B each loop pass):
			// a STOPped machine must vector an I/O interrupt the moment
			// its message wakes it, or the dispatcher's own PMPB reads
			// the bit away and the device request rots in its level
			// list. The doorbell (bit 10) staying poll-only is handled
			// by its exclusion from message-trap delivery.
			m_halted = true;
			r.p = m_next;
			return exec_trap::halted;
		}
		return exec_trap::unimplemented;
	}

	// The stack-indirect family (ERS sheets 58-59, 63-64). `width` selects
	// byte, halfword, word or doubleword. A load takes its data segment
	// pointer from the top of stack and replaces it with the value. A
	// store's pointer sits *beneath* the value -- SSI/SBSI/SHSI read it
	// from (S-1) and SDSI from (S-2) -- and everything is popped.
	exec_trap stack_indirect(bool indexed, unsigned width, bool store)
	{
		const u32 ptr = store ? stk(width == 8 ? 2 : 1) : stk(0);
		const ea_result e = m_au.resolve_data_pointer(ptr, indexed, store);
		m_addr_trap = e.t;
		if (!e.ok())
			return exec_trap::address_error;

		if (!store)
		{
			pop();
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
		pop();                              // the pointer
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
		// The shift group. The note under ASL (ERS sheet 72) governs all
		// of them and is easy to miss: "in all of the shift instructions
		// CIR(19:1) is an index bit and CNT = 6 least significant bits of
		// (CIR(26:6) + (if CIR(19:1)=1 then X else 0))". So bit 3 adds X
		// to the operand, and the count can reach 63 -- past the width of
		// what is being shifted, which the 32-bit forms have to saturate
		// rather than shift by.
		case 002: { const unsigned n = shift_count(operand, b3);  // LSL
			const u32 v = (n >= 32) ? 0 : (stk(0) << n);
			set_stk(0, v); cca(v); return exec_trap::none; }
		case 003: { const unsigned n = shift_count(operand, b3);  // LSR
			const u32 v = (n >= 32) ? 0 : (stk(0) >> n);
			set_stk(0, v); cca(v); return exec_trap::none; }
		case 004: { const unsigned n = shift_count(operand, b3) & 31;
			const u32 x = stk(0);                                 // CSL
			const u32 v = n ? ((x << n) | (x >> (32 - n))) : x;
			set_stk(0, v); cca(v); return exec_trap::none; }
		case 005: { const unsigned n = shift_count(operand, b3) & 31;
			const u32 x = stk(0);                                 // CSR
			const u32 v = n ? ((x >> n) | (x << (32 - n))) : x;
			set_stk(0, v); cca(v); return exec_trap::none; }
		case 006: { const unsigned n = shift_count(operand, b3);  // ASL
			const u32 x = stk(0);
			const u32 v = (n >= 32) ? 0 : (x << n);
			// "If the sign bit changes, Overflow is set."
			r.put(sf::O, ((x ^ v) >> 31) & 1);
			set_stk(0, v); cca(v); return exec_trap::none; }
		case 007: { const unsigned n = shift_count(operand, b3);  // ASR
			// Not an arithmetic shift: the ERS negates a negative TOS,
			// shifts the magnitude right filling with zeros, and negates
			// the result back, which rounds toward zero rather than down.
			const s32 x = s32(stk(0));
			const u32 mag = (x < 0) ? u32(-x) : u32(x);
			const u32 sh = (n >= 32) ? 0 : (mag >> n);
			const u32 v = (x < 0) ? u32(-s32(sh)) : sh;
			set_stk(0, v); cca(v); return exec_trap::none; }
		case 010: { const unsigned n = shift_count(operand, b3);  // LSXL
			r.x = (n >= 32) ? 0 : (r.x << n);
			cca(r.x); return exec_trap::none; }
		case 050: {                                               // PMVA
			// Privileged Move Bytes using Absolute Addresses (ERS sheet
			// 101). As MVB, but no bounds checking applies.
			//
			// "Absolute" describes the addressing, not the operand
			// encoding: SYSTEM POWERUP builds both operands with LRAS,
			// which yields a *tagged* relative pointer, and the one PMVA
			// the boot executes is exactly that -- 0xc0100200 to
			// 0xc0100114, the stack-tagged forms of Q-3 and Q-239.
			// Taking those as raw addresses put the copy above the top of
			// memory, where it was silently lost; the tag resolves them
			// to the intended words. Operands that carry no tag are
			// absolute addresses, which is what the sites that compute
			// their operands arithmetically pass.
			if (!r.privileged())
				return exec_trap::unimplemented;
			const int d = b3 ? -1 : 1;
			const unsigned sdec = operand & 3;
			const s32 count = s32(stk(0));
			u32 tba = absolute_or_pointer(stk(1));
			u32 sba = absolute_or_pointer(stk(2));
			// A count in the tens of megabytes is not a copy the OS
			// means: it stalls the emulator for the whole transfer
			// inside one instruction, which reads as a hang from
			// outside. Say so instead of silently grinding.
			if (count > 0x1000000 || count < -0x1000000)
				std::fprintf(stderr, "[core] PMVA count %d at P=%08x "
						"src=%08x dst=%08x\n", count, r.p, sba, tba);

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
		case 051:                                                 // MVB
			// Move Bytes (ERS sheet 101). The stack holds a byte count on
			// top, then a target pointer, then a source pointer. Bit 3 of
			// the instruction picks the direction; within the 6-bit
			// operand, bit 3 says the source is a data segment pointer
			// rather than a local program pointer, and bits 4-5 give how
			// many words to pop at the end.
			return do_mvb(b3 ? -1 : 1, operand & 3,
					((operand >> 2) & 1) != 0);
		case 052: return do_scan(operand, b3, false);             // SCW
		case 053: return do_scan(operand, b3, true);              // SCU

		case 054: {                                               // CMPB
			// Compare Bytes (ERS sheet 105). Source in (S-2), target in
			// (S-1), byte count on top. Stops on the first difference or
			// when the count runs out; the count left behind says which,
			// and the condition code reports the comparison.
			const int d = b3 ? -1 : 1;
			const unsigned sdec = operand & 3;
			const bool src_is_ds = ((operand >> 2) & 1) != 0;

			const s32 count = s32(stk(0));
			u32 sba;
			const exec_trap st = string_source(stk(2), src_is_ds, sba);
			if (st != exec_trap::none) return st;

			const ea_result te = m_au.resolve_data_pointer(stk(1), false, false);
			m_addr_trap = te.t;
			if (!te.ok()) return exec_trap::address_error;
			u32 tba = te.addr;

			s32 x = 0;
			u8 sb = 0, tb = 0;
			while (count > d * x)
			{
				sb = m_mem.read8(sba);
				tb = m_mem.read8(tba);
				if (sb != tb)
					break;
				sba = u32(s32(sba) + d);
				tba = u32(s32(tba) + d);
				x += d;
			}
			r.x = u32(x);
			set_stk(0, u32(count - d * x));
			// Running the count out means the areas match, and so does a
			// count that was never positive.
			if (count <= 0 || count == d * x)
				r.put(sf::CC, CCE);
			else
				r.put(sf::CC, (sb > tb) ? CCG : CCL);
			r.s -= 4 * sdec;
			return exec_trap::none;
		}

		case 056: {                                               // TRAN
			// Translate (ERS sheet 106). Each byte of the string in (S-1)
			// indexes the 256-byte table in (S-2), and the table entry
			// replaces it in place.
			const int d = b3 ? -1 : 1;
			const unsigned sdec = operand & 3;
			const bool tbl_is_ds = ((operand >> 2) & 1) != 0;

			const s32 count = s32(stk(0));
			u32 tblba;
			const exec_trap st = string_source(stk(2), tbl_is_ds, tblba);
			if (st != exec_trap::none) return st;

			const ea_result te = m_au.resolve_data_pointer(stk(1), false, true);
			m_addr_trap = te.t;
			if (!te.ok()) return exec_trap::address_error;
			u32 tba = te.addr;

			s32 x = 0;
			while (count > d * x)
			{
				m_mem.write8(tba, m_mem.read8(tblba + m_mem.read8(tba)));
				tba = u32(s32(tba) + d);
				x += d;
			}
			r.x = u32(x);
			set_stk(0, u32(count - d * x));
			r.s -= 4 * sdec;
			return exec_trap::none;
		}

		case 025: {                                               // SLD
			// String Load (ERS sheet 108). The descriptor in the top four
			// words is replaced by a string temp holding the substring it
			// names -- the bytes, a size word above them and a fresh
			// descriptor above that. The source string is not modified.
			const bool src_is_ds = ((operand >> 2) & 1) != 0;

			str_desc d;
			d.maxlen = s32(stk(3));
			d.ptr    = stk(2);
			d.first  = s32(stk(1));
			d.len    = s32(stk(0));

			exec_trap t;
			if (!string_validate(d, src_is_ds, b3, false, t))
				return t;

			// Byte FIRST of a string sits FIRST+3 past the pointer: one
			// word of current length, and FIRST counts from one.
			u32 sptr;
			if (!string_offset(d.ptr, d.first + 3, false, sptr, t))
				return t;

			// The temp starts where the descriptor did, and carries four
			// words of new descriptor plus a size word above its data.
			const u32 tempadr = r.s - 15;
			const s32 nwords = ((d.len + 3) / 4) + 1;
			const u32 news = tempadr + u32(nwords) * 4 + 19;
			if (news > r.sl)
				return take_trap(trap_cause::stack_overflow, 0);
			r.s = news;

			m_mem.write32(tempadr, u32(d.len));
			set_stk(4, u32(nwords));
			set_stk(3, u32(d.len));
			set_stk(2, stack_pointer_to(tempadr));
			set_stk(1, 1);
			set_stk(0, u32(-1));

			// Hand the copy itself to MVB, which pops the three words of
			// parameters it is given.
			r.s += 12;
			set_stk(2, sptr);
			set_stk(1, stack_pointer_to(tempadr + 4));
			set_stk(0, u32(d.len));
			return do_mvb(1, 3, src_is_ds);
		}

		case 026: {                                               // SVAL
			// String Validate (ERS sheet 107): check the descriptor in the
			// top four words, and optionally replace the count of bytes of
			// interest with the actual length the checks worked out.
			str_desc d;
			d.maxlen = s32(stk(3));
			d.ptr    = stk(2) & ~u32(3);
			d.first  = s32(stk(1));
			d.len    = s32(stk(0));
			set_stk(2, d.ptr);          // the alignment is written back

			exec_trap t;
			if (!string_validate(d, (operand >> 2) & 1, b3, false, t))
				return t;

			if ((operand >> 4) & 1) set_stk(0, u32(d.len));
			return exec_trap::none;
		}

		case 027: {                                               // SCON
			// String Concatenate (ERS sheets 111-112). A source substring
			// is appended to a target string temp, in place: the temp's
			// own space is reused and grows only if it has to.
			const bool src_is_ds = ((operand >> 2) & 1) != 0;
			const bool from_temp = (operand & 1) != 0;
			const bool tneg_ok   = ((operand >> 1) & 1) != 0;

			str_desc s;
			s.maxlen = s32(stk(3));
			s.ptr    = stk(2);
			s.first  = s32(stk(1));
			s.len    = s32(stk(0));

			exec_trap t;
			if (!string_validate(s, src_is_ds, b3, false, t))
				return t;

			str_desc g;
			if (from_temp)
			{
				const u32 tdesc = s.stringadr - 16;
				if (tdesc < r.q || tdesc > r.s - 31)
					return take_trap(trap_cause::bounds_violation, r.p);
				g.maxlen = s32(m_mem.read32(tdesc));
				g.ptr    = m_mem.read32(tdesc + 4);
				g.first  = s32(m_mem.read32(tdesc + 8));
				g.len    = s32(m_mem.read32(tdesc + 12));
			}
			else
			{
				g.maxlen = s32(stk(7));
				g.ptr    = stk(6);
				g.first  = s32(stk(5));
				g.len    = s32(stk(4));
			}
			if (!string_validate(g, true, tneg_ok, true, t, false))
				return t;

			// The target has to be a temp on this stack. The ERS writes
			// the upper bound as "S-14"; read as words, the way "(S-n)"
			// is read everywhere else and the way SAS's own "S-7" is,
			// that means fourteen words clear of the top.
			if (g.stringadr < r.q || g.stringadr > r.s - 3 - 4 * 14)
				return take_trap(trap_cause::bounds_violation, r.p);

			s32 tcurlen = g.curlen;
			if (g.len >= 0)
			{
				const s32 reach = g.first + g.len - 1;
				if (reach < tcurlen) tcurlen = reach;
			}

			u32 msrc, mdst;
			if (!string_offset(s.ptr, s.first + 3, false, msrc, t)) return t;
			if (!string_offset(g.ptr, tcurlen + 4, true, mdst, t))  return t;

			const s32 tmaxlen = tcurlen + s.len;
			s32 nwords = ((tmaxlen + 3) / 4) + 1;
			const u32 news = g.stringadr + u32(nwords) * 4 + 19;
			if (news < r.s)
			{
				// The result already fits in what the operands occupied,
				// so the temp simply claims all of it.
				nwords = s32((r.s - g.stringadr - 19) / 4);
			}
			else
			{
				if (news > r.sl)
					return take_trap(trap_cause::stack_overflow, 0);
				r.s = news;
			}

			m_mem.write32(g.stringadr, u32(tmaxlen));
			set_stk(4, u32(nwords));
			set_stk(3, u32(tmaxlen));
			set_stk(2, g.ptr);
			set_stk(1, u32(g.first));
			set_stk(0, u32(-1));

			r.s += 12;
			set_stk(2, msrc);
			set_stk(1, mdst);
			set_stk(0, u32(s.len));
			return do_mvb(1, 3, src_is_ds);
		}

		case 030: {                                               // SAS
			// String Assign (ERS sheets 109-111). A source descriptor sits
			// on top; the target descriptor is either the four words below
			// it, or -- when the source is a string temp -- the four words
			// immediately below that temp.
			const bool src_is_ds  = ((operand >> 2) & 1) != 0;  // CIR(29:1)
			const bool from_temp  = (operand & 1) != 0;         // CIR(31:1)
			const bool fill       = ((operand >> 3) & 1) != 0;  // CIR(28:1)
			const bool tneg_ok    = ((operand >> 1) & 1) != 0;  // CIR(30:1)

			str_desc s;
			s.maxlen = s32(stk(3));
			s.ptr    = stk(2);
			s.first  = s32(stk(1));
			s.len    = s32(stk(0));

			exec_trap t;
			if (!string_validate(s, src_is_ds, b3, false, t))
				return t;

			str_desc g;
			if (from_temp)
			{
				const u32 tdesc = s.stringadr - 16;
				if (tdesc < r.q || tdesc > r.s - 31)
					return take_trap(trap_cause::bounds_violation, r.p);
				g.maxlen = s32(m_mem.read32(tdesc));
				g.ptr    = m_mem.read32(tdesc + 4);
				g.first  = s32(m_mem.read32(tdesc + 8));
				g.len    = s32(m_mem.read32(tdesc + 12));
			}
			else
			{
				g.maxlen = s32(stk(7));
				g.ptr    = stk(6);
				g.first  = s32(stk(5));
				g.len    = s32(stk(4));
			}

			// The target pointer is always a data segment pointer, and its
			// count of bytes of interest stays as written.
			if (!string_validate(g, true, tneg_ok, true, t, false))
				return t;

			// How much moves, and what the target's length becomes.
			s32 mvlen, tcurlen;
			if (g.len < 0)
			{
				tcurlen = g.first + s.len - 1;
				if (tcurlen > g.maxlen) return string_trap(9, true);
				mvlen = s.len;
			}
			else
			{
				const s32 reach = g.first + g.len - 1;
				tcurlen = (g.curlen > reach) ? g.curlen : reach;
				mvlen = (s.len < g.len) ? s.len : g.len;
			}

			// Assigning a string to a later part of itself has to run
			// backwards. The condition code reports which way it went.
			const bool backward = (g.stringadr == s.stringadr)
					&& (g.first > s.first);
			r.put(sf::CC, backward ? CCL : CCG);

			u32 msrc, mdst;
			const s32 sadj = backward ? s.first + mvlen + 2 : s.first + 3;
			const s32 tadj = backward ? g.first + mvlen + 2 : g.first + 3;
			if (!string_offset(s.ptr, sadj, false, msrc, t)) return t;
			if (!string_offset(g.ptr, tadj, true, mdst, t))  return t;

			// Blank fill covers the tail of a target substring longer than
			// what was moved into it. SAS only stacks the parameters; a
			// following FILB does the work.
			u32 filltarget = 0;
			s32 filllen = 0;
			if (fill && g.len > mvlen)
			{
				if (!string_offset(g.ptr, g.first + mvlen + 3, true,
						filltarget, t))
					return t;
				filllen = g.len - mvlen;
			}

			r.s -= from_temp ? 16 : 32;
			if (fill)
			{
				r.s += 12;
				set_stk(2, 32);             // the blank itself
				set_stk(1, filltarget);
				set_stk(0, u32(filllen));
			}

			m_mem.write32(g.stringadr, u32(tcurlen));

			r.s += 12;
			set_stk(2, msrc);
			set_stk(1, mdst);
			set_stk(0, u32(mvlen));
			return do_mvb(backward ? -1 : 1, 3, src_is_ds);
		}

		case 031: {                                               // PMPB
			// Pushes the Mask and Message registers and the CPU channel
			// number, selected by CIR(29:3) -- the low three bits of the
			// operand (ERS sheet 114).
			if (operand & 1) push(r.mask);
			if (operand & 2) push(r.msg);
			if (operand & 4)
			{
				// MASK bits 3-5 hold this CPU's MPB channel number, pushed
				// right justified.
				push(ers32::get(r.mask, 3, 3));
			}
			return exec_trap::none;
		}
		case 032: {                                               // SMPB
			// Based on CIR(29:3), in this order (ERS sheet 141):
			//   4  MASK(3:3) := (S)(29:3) -- assign this CPU's channel;
			//      the powerup module renumbers processors with this
			//   2  clear MSG bits wherever the TOS has a 1
			//   1  set MASK *except* the channel field
			if (operand & 4) r.mask = ers32::set(r.mask, 3, 3, pop() & 7);
			if (operand & 2) r.msg &= ~pop();
			if (operand & 1)
			{
				const u32 v = pop();
				r.mask = ers32::set(v, 3, 3, ers32::get(r.mask, 3, 3));
			}
			return exec_trap::none;
		}
		case 033: {                                               // SRM
			// Set Rounding Mode from the operand's low two bits, or from
			// the top of stack when bit 3 is set (ERS sheet 88).
			if (b3) { r.put(sf::RM, stk(0) & 3); r.s -= 4; }
			else      r.put(sf::RM, operand & 3);
			return exec_trap::none;
		}
		case 034: {                                               // PSHR
			// Push Registers (ERS sheet 38/67). Within the 6-bit operand,
			// bit 5 selects S, 4 Q, 3 STATUS, 2 DST, 1 BRKPT and 0 FLAGS;
			// instruction bit 3 additionally pushes ALL SEVEN program and
			// data extent registers, "P, PB, PL, DB, DL, SB, SL ... in
			// the sequence shown". Pushing only four of them shifted the
			// kernel's stack-extent probe (seg1 STT[51]) three words and
			// sent the boot into the error-18 reporter.
			if (operand & 1)  push(r.s - r.sb - 1);
			if (operand & 2)  push(r.q - r.sb);
			if (operand & 4)  push(r.status);
			if (operand & 8)  push(r.dst);
			if (operand & 16) push(r.brkpt);
			if (operand & 32) push(u32(r.flags));
			if (b3)
			{
				push(r.p); push(r.pb); push(r.pl);
				push(r.db); push(r.dl); push(r.sb); push(r.sl);
			}
			return exec_trap::none;
		}
		case 035: {                                               // SETR
			// Set Registers, the inverse of PSHR and in reverse order.
			if (operand & 32) { r.flags = u8(stk(0)); r.s -= 4; }
			if (operand & 16) { r.brkpt = pop(); }
			if (operand & 8)  { r.dst = pop(); }
			if (operand & 4)  { r.status = pop(); }
			if (operand & 2)  { r.q = pop() + r.sb; }
			if (operand & 1)  { r.s = stk(0) + r.sb + 1; }
			return exec_trap::none;
		}
		case 036: {                                               // LLSH
			// Linked List Search (ERS sheet 114): walk absolute link
			// words from the TOS while the word at link + (S-2) stands
			// in the CIR(29:3)-selected relation to the test word (S-1),
			// decrementing X per step. CCL when the count ran out, CCE
			// when the comparison stopped it, CCG on a nil link. The
			// dispatcher walks its ready queues with this.
			if (!r.privileged()) return exec_trap::unimplemented;
			const u32 test = stk(1);
			const u32 off = stk(2);
			u32 link = stk(0);
			bool flag = true;
			// The comparison is SIGNED over the full word. The SUNOS heap
			// depends on this: block headers carry the allocated flag in
			// bit 0 (the free routine at kernel+0x45af4 guards a free with
			// LSR 31 / CMPI #1 and clears exactly that bit), so allocated
			// blocks read negative and the first-fit walk (LLSH 1, advance
			// while header < size) skips them, stopping only at a FREE
			// block large enough. Masking bit 0 out instead made the walk
			// split live allocations -- the boot's two TCBs landed on the
			// same 244 bytes and the unit-1 server task was destroyed.
			while (s32(r.x) > 0 && link != 0 && flag)
			{
				const s32 target = s32(m_mem.read32(link + off));
				const s32 t = s32(test);
				bool advance;
				if (target > t)
					advance = (operand & 4) != 0;
				else if (target == t)
					advance = (operand & 2) != 0;
				else
					advance = (operand & 1) != 0;
				if (advance)
				{
					link = m_mem.read32(link);
					r.x--;
				}
				else
					flag = false;
			}
			set_stk(0, link);
			r.put(sf::CC, s32(r.x) <= 0 ? CCL : (link == 0 ? CCG : CCE));
			return exec_trap::none;
		}

		case 057: {                                               // FILB
			// Fill Bytes (ERS sheet 105). The stack holds a byte count on
			// top, a target data segment pointer beneath it, and the fill
			// byte in the low byte of the third word.
			const unsigned sdec = operand & 3;
			const s32 count = s32(stk(0));

			if (count > 0)
			{
				const u32 tptr = stk(1);
				const u8 fill = u8(stk(2));
				const ea_result e = m_au.resolve_data_pointer(tptr, false, true);
				m_addr_trap = e.t;
				if (!e.ok())
					return exec_trap::address_error;

				u32 tba = e.addr;
				s32 x = 0;
				while (count > x)
				{
					m_mem.write8(tba++, fill);
					x++;
				}
				r.x = u32(x);
				set_stk(0, u32(count - x));
			}
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
		case 073: case 077: {                                     // CRB
			// Compare Range & Branch (ERS sheet 93). The value sits under
			// two bounds: (S) is the upper, (S-1) the lower, (S-2) the
			// value. The bounds are popped either way; in range gives CCE
			// and falls through, out of range gives CCG and branches -- or
			// takes a Check Trap when the branch field is zero, which is
			// how a CASE with no default reports an unmatched selector.
			//
			// The bounds are treated as inclusive. The ERS pseudocode as
			// scanned reads "If (S-1) < (S-2) < (S)", strict at both ends,
			// but that would reject the endpoints of every CASE range and
			// contradicts the sibling CHEK, whose own text calls its limits
			// bounds. Read as inclusive; revisit if real code disagrees.
			const s32 hi = s32(stk(0)), lo = s32(stk(1)), v = s32(stk(2));
			r.s -= 8;
			if (v >= lo && v <= hi)
			{
				r.put(sf::CC, CCE);
				return exec_trap::none;
			}
			r.put(sf::CC, CCG);
			if (operand == 0)
				return take_trap(trap_cause::check_trap, m_cir);
			rel();
			return exec_trap::none;
		}

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
		// 16-bit forms of LPP and LIA. LPPS scales its 8-bit field by four
		// because it names a whole STT entry; LIAS splits the field into
		// CIR(24:2) level and CIR(26:6) offset, the offset always positive.
		case 034: return do_lpp(4u * imm);                       // LPPS
		case 025: return do_lia(imm >> 6, s32(imm & 0x3f));      // LIAS

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
		// The X-immediate group sets CCA on the new X (ERS sheets
		// 28-30, 76-77); the powerup module's DMA reset loop exits on
		// the condition code SBXI leaves, not on a compare.
		case 012: r.x = imm; cca(r.x); return exec_trap::none;   // LDXI
		case 013: r.x = u32(-s32(u32(imm))); cca(r.x);
			return exec_trap::none;                              // LDXN
		case 014: push(imm); cca(imm); return exec_trap::none;   // LDIS
		case 015: { const u32 v = u32(-s32(u32(imm))); push(v); cca(v);
			return exec_trap::none; }                            // LNIS
		case 017: return do_io(io_kind::write, imm);             // IOW
		case 020: return do_io(io_kind::read, imm);              // IOR
		case 021: return do_io(io_kind::control, imm);           // IOC
		case 022: return do_call(4u * imm);                      // PCLS
		case 023: return do_exit(imm);                           // EXIT
		case 024: {                                              // SXIT
			// Subroutine Exit (ERS sheet 93). Nothing like EXIT: there is
			// no stack marker. Subroutines are called with an LRA/BR pair
			// which stacks only a PB-relative return address, so the exit
			// pops that and drops `imm` further words of parameters.
			const u32 ret = pop();
			r.s -= 4u * imm;
			const u32 target = r.pb + ret;
			if (!m_au.in_code(target))
				return exec_trap::code_bounds;
			branch_to(target);
			return exec_trap::none;
		}
		case 026: r.x += imm; cca(r.x); return exec_trap::none;  // ADXI
		case 027: r.x -= imm; cca(r.x); return exec_trap::none;  // SBXI
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
		case 004: return do_lpp(off);                             // LPP
		case 005: {                                               // LIA
			// Level is CIR(8:4); the offset is CIR(13:19) with its sign in
			// CIR(12:1), 0 for + and 1 for -.
			const u32 level = (d >> 20) & 0xf;
			const s32 sofs = ((d >> 19) & 1) ? -s32(off) : s32(off);
			return do_lia(level, sofs);
		}
		// The immediate for LDI and LNI is CIR(8:24) -- the whole 24 bits
		// after the opcode, not the 19-bit offset field (ERS sheet 62).
		// The kernel's ceiling divisions encode 0x80000 this way, which
		// the narrower field would truncate to zero.
		case 006: { const u32 v = d & 0xffffff; push(v); cca(v);
			return exec_trap::none; }                             // LDI
		case 007: { const u32 v = u32(-s32(d & 0xffffff)); push(v); cca(v);
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
			// immediate bounds (ERS sheet 128). Sheet 46 names the cause:
			// "Check Trap. This trap is generated by the CHEK and CRB
			// instructions. The Current Instruction register is passed as a
			// parameter." That is STT entry 2, not the bounds violation at
			// entry 1 that this used to raise.
			const s32 v = s32(stk(0));
			const s32 lo = s32(u32((d >> 12) & 0x3ff));
			const s32 hi = s32(u32(d & 0xfff));
			if (v < lo || v > hi)
				return take_trap(trap_cause::check_trap, m_cir);
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
			// "Right justified, sign extended, and pushed" (ERS sheet 4).
			// The powerup module's MPB inventory marks valid halfwords
			// with the sign bit, so zero extension hides every device.
			u32 a;
			if (!ea(base, off, ind, idx, false, a))
				return exec_trap::address_error;
			const u32 v = u32(s32(s16(m_mem.read16(a))));
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
			cca(r.x);
			return exec_trap::none;
		}
		case 030: {                                               // BR
			// ERS sheet 108: a P-relative indirect branch goes through a
			// SELF-relative pointer, but DB, DL, SB, Q and S-relative
			// indirect branches go through PROGRAM pointers "like those
			// stored in the STT", with the uncallable bit ignored -- a
			// local one is a PB-relative offset, an external one is
			// evaluated through the CST as PCL does. Running those words
			// through data-pointer resolution instead is what sent HP-UX
			// to PB + 0x4000a6c8: the U bit was still in the offset.
			if (ind && base != BASE_P_PLUS && base != BASE_P_MINUS)
			{
				u32 pa;
				if (!ea(base, off, false, idx, false, pa))
					return exec_trap::address_error;
				const u32 ptr = m_mem.read32(pa);
				u32 target;
				if (ers32::get(ptr, 0, 1))
				{
					exec_trap t;
					if (!enter_segment(ptr, target, t))
						return t;
				}
				else
					target = r.pb + ers32::get(ptr, 2, 30);
				if (!m_au.in_code(target))
					return exec_trap::code_bounds;
				branch_to(target);
				return exec_trap::none;
			}
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
			// Increment/Decrement Memory by Four (ERS sheets 81.2-81.3).
			// The F is four, not one: the powerup module counts a byte
			// checksum down with DECF while stepping X by 4.
			u32 a;
			if (!ea(base, off, ind, idx, true, a))
				return exec_trap::address_error;
			const u32 v = m_mem.read32(a) + ((op2 == 027) ? 4u : u32(-4));
			m_mem.write32(a, v); cca(v);
			return exec_trap::none;
		}
		case 033: {                                               // SEML
			// Semaphore Load: push the word at E and store -1 there, in one
			// operation no other processor can interrupt (ERS sheet 122).
			u32 a;
			if (!ea(base, off, ind, idx, true, a))
				return exec_trap::address_error;
			push(m_mem.read32(a));
			m_mem.write32(a, 0xffffffffu);
			cca(stk(0));
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
		case 036:                                                 // LRA
			return do_lra(base, off, ind, idx);
		}
		return exec_trap::unimplemented;
	}

	// LRA / LRAS (ERS sheet 58). This does not push an absolute address: in
	// direct mode it computes a *relative, tagged* pointer and makes no
	// memory reference at all, so there is nothing to bounds check.
	//
	//   P-relative        -> E - PB, untagged
	//   DB or DL relative -> base + offset - DB, tagged 10
	//   stack relative    -> base + offset - SB, tagged 11
	//
	// Indexing is applied to the resulting pointer, not to the address.
	// In indirect mode the word at base + offset is fetched and pushed as
	// it stands.
	exec_trap do_lra(base_reg base, u32 offset, bool ind, bool idx)
	{
		const u32 addr = base_addr(base, offset);
		const bool p_rel = (base == BASE_P_PLUS || base == BASE_P_MINUS);

		if (!ind)
		{
			if (p_rel)
			{
				push(addr - r.pb);
				return exec_trap::none;
			}
			const bool global = (base == BASE_DB_PLUS || base == BASE_DL_MINUS);
			u32 v = global ? (addr - r.db) : (addr - r.sb);
			v = ers32::set(v, 0, 2, global ? 2 : 3);
			if (idx)
				v += r.x;
			push(v);
			return exec_trap::none;
		}

		if (p_rel)
		{
			u32 a;
			if (!ea(base, offset, true, idx, false, a))
				return exec_trap::address_error;
			push(a - r.pb);
			return exec_trap::none;
		}

		u32 v = m_mem.read32(addr);
		if (idx)
			v += r.x;
		push(v);
		return exec_trap::none;
	}

	// The direct base-relative address, without any bounds check -- LRA
	// needs the arithmetic but makes no reference.
	u32 base_addr(base_reg base, u32 offset) const
	{
		switch (base)
		{
		case BASE_P_PLUS:   return r.p + offset;
		case BASE_P_MINUS:  return r.p - offset;
		case BASE_DB_PLUS:  return r.db + offset;
		case BASE_DL_MINUS: return r.dl - offset;
		case BASE_Q_PLUS:   return r.q + offset;
		case BASE_Q_MINUS:  return r.q - offset;
		case BASE_SB_PLUS:  return r.sb + offset;
		case BASE_S_MINUS:  return r.s - offset;
		}
		return 0;
	}

	// Channel-to-channel MPB traffic. WTC and SMSG address a processor on
	// the bus rather than an IOP command port; with only one CPU modelled
	// the traffic is recorded in the slave registers so software can see it
	// went somewhere, and MSG bit 12 is set as WTC is documented to do.
	void mpb_write(u32 addr, u32 data, bool secondary)
	{
		r.sar = addr;
		r.sdr = data;
		if (!secondary)
			r.msg |= ers32::mask(12, 1);
	}

	void mpb_message(u32 msg, bool broadcast)
	{
		r.sar = msg;
		// A broadcast "goes to all channels simultaneously" (ERS sheet
		// 125) -- and that includes the sender's own CPU, which is how
		// the operating system rings its own doorbell: the trap segment
		// posts work to the queue in low dedicated memory and then does
		// SMSG,B with 0x200000, raising MSG bit 10, the one bit the
		// dispatcher's idle mask always leaves enabled. The message bits
		// OR into the Message register; an unmasked result wakes a
		// STOPped CPU. A targeted message would go to one channel; with
		// no other processors modelled it is recorded and dropped.
		if (broadcast)
		{
			r.msg |= msg & 0x1fffffffu;
			if ((r.msg & ~r.mask) != 0)
				m_halted = false;
		}
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
	// An operand that may be either a tagged pointer or a plain absolute
	// address. A stack- or global-tagged pointer resolves against its
	// base; anything else is the address itself. The privileged
	// instructions use this convention -- PMVA's operands come from LRAS
	// and are tagged, while HP-UX hands DOWN absolute addresses like
	// 0x00ffe6f8, which no tag can describe.
	u32 absolute_or_pointer(u32 ptr) const
	{
		switch (ers32::get(ptr, 0, 2))
		{
		case 3:  return r.sb + ers32::get(ptr, 2, 30);
		case 2:  return r.db + ers32::get(ptr, 2, 30);
		default: return ptr;
		}
	}

	exec_trap take_trap(trap_cause cause, u32 parameter)
	{
		m_last_trap = cause;
		if (on_trap)
			on_trap(cause, parameter);
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
		const u32 old_pb = r.pb;

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

		// Only the mode bit survives, and the code segment number becomes
		// 1 -- the handler segment the machine now executes in. (The old
		// status, old CSN included, is in the marker; EXIT needs the
		// running CSN to say 1 or the return would never switch back.)
		r.status &= ers32::mask(14, 1);
		r.put(sf::CSN, 1);

		// Entering the handler segment confers its privilege, exactly as
		// calling into it does: the CST entry's M attribute (word 0 bit
		// 30) says the segment executes privileged, and CS1's is set.
		// Without this a trap taken in user mode ran the OS's own handler
		// unprivileged, so the first privileged instruction in it -- a
		// PLDA a few words in -- trapped as unimplemented, and that trap
		// entered the same handler the same way. The machine recursed
		// until the stack left memory and the processor jumped to -1.
		if (ers32::get(m_mem.read32(entry), 30, 1))
			r.put(sf::M, 1);

		if (m_trap_log_len < 16)
		{
			m_trap_log[m_trap_log_len][0] = u32(cause);
			m_trap_log[m_trap_log_len][1] = ret;
			m_trap_log[m_trap_log_len][2] = parameter;
			m_trap_log[m_trap_log_len][3] = old_pb;
			m_trap_log_len++;
		}

		// The vector is a program pointer like any other: flags in the
		// top two bits, byte offset in the low thirty. HP-UX's vectors
		// carry the flags -- BASIC's happen not to -- and taking the
		// word whole sent the machine to PB + 0x4000a53c and stopped it
		// dead on the first trap it ever took.
		const u32 stt = m_mem.read32(r.pl - 4 * u32(cause));
		branch_to(r.pb + ers32::get(stt, 2, 30));
		return exec_trap::none;
	}

public:
	// The last sixteen traps taken: octal cause, PB-relative return
	// address, parameter, and the PB it happened under. A debugging aid
	// for the boot harnesses.
	u32 m_trap_log[16][4] = {};
	unsigned m_trap_log_len = 0;
private:

	//----------------------------------------------------------------
	//  procedure call and return (ERS sheets 92, 96)
	//----------------------------------------------------------------

	//----------------------------------------------------------------
	//  the byte string group (ERS sheets 101-107)
	//----------------------------------------------------------------

	// Every instruction in this group takes its source through the same
	// choice: CIR(29:1), operand bit 2, says whether the third stack word is
	// a data segment pointer or a local program pointer. A local program
	// pointer is a byte offset from PB bounds checked against PB and PL
	// (ERS convention note 18), which is how a constant table compiled into
	// the code segment gets addressed.
	// Structured byte strings (ERS section 2.3.4). A descriptor is four
	// stack words -- maximum length, pointer, index of the first byte of
	// interest, and the number of bytes of interest. The string itself
	// carries its current length in the word the pointer addresses.
	struct str_desc
	{
		s32 maxlen = 0;
		u32 ptr = 0;
		s32 first = 0;
		s32 len = 0;        // replaced by the actual length on return
		u32 stringadr = 0;
		s32 curlen = 0;
	};

	// The String Trap parameter packs the error number from section 5.2.2
	// and which of the two strings failed alongside the instruction word.
	exec_trap string_trap(unsigned err, bool target)
	{
		u32 p = ers32::set(0, 11, 1, target ? 1 : 0);
		p = ers32::set(p, 12, 4, err);
		p = ers32::set(p, 16, 16, m_cir);
		return take_trap(trap_cause::string_trap, p);
	}

	// CNT for the shift group: the six-bit operand plus X when the
	// instruction's index bit is set, kept to six bits (ERS sheet 72).
	unsigned shift_count(u8 operand, bool indexed) const
	{
		return unsigned(operand + (indexed ? r.x : 0)) & 63;
	}

	// A stack segment pointer: tag 11 in the top two bits, the rest an
	// SB-relative byte offset (ERS sheet 55).
	u32 stack_pointer_to(u32 addr) const
	{
		return ers32::set(u32(3) << 30, 2, 30, addr - r.sb);
	}

	// Adding a byte offset to a string pointer must not disturb the two
	// bits that say which kind of pointer it is (section 5.2.2, error 8).
	// The stricter rule the ERS gives for external data segment pointers
	// into unpaged segments -- a trap if any of the top thirteen bits
	// change -- needs the segment's paged flag to apply safely, so only
	// the tag check is made here.
	bool string_offset(u32 ptr, s32 delta, bool target, u32 &out,
			exec_trap &t)
	{
		out = u32(s32(ptr) + delta);
		if (ers32::get(out, 0, 2) != ers32::get(ptr, 0, 2))
		{
			t = string_trap(8, target);
			return false;
		}
		t = exec_trap::none;
		return true;
	}

	// The fetch, validate and actual-length steps SVAL defines (ERS sheet
	// 107), which every other string instruction is specified in terms of.
	// neg_len says whether a negative count of bytes of interest is
	// allowed; the instructions take it from different bits for source and
	// target, so the caller passes it in already resolved.
	//
	// Returns false if the descriptor was rejected, and only then is `out`
	// meaningful. It cannot report that through `out` alone: a trap that
	// vectored to a handler leaves exec_trap::none behind, and the caller
	// must still abandon the instruction rather than carry on and write
	// over the parameter word the trap just pushed.
	// compute_len asks for the final step as well. SAS and SCON validate
	// their target descriptor but must see its count of bytes of interest
	// as it was written, negative and all, so they leave it out.
	bool string_validate(str_desc &d, bool is_ds, bool neg_len, bool target,
			exec_trap &out, bool compute_len = true)
	{
		d.ptr &= ~u32(3);                       // force word alignment

		out = string_source(d.ptr, is_ds, d.stringadr);
		if (out != exec_trap::none) return false;
		d.curlen = s32(m_mem.read32(d.stringadr));

		unsigned err = 0;
		if (d.maxlen < 0)                                          err = 1;
		else if (d.len < 0 && !neg_len)                            err = 2;
		else if (d.first <= 0)                                     err = 3;
		else if (d.len >= 0 && d.first + d.len - 1 > d.maxlen)     err = 4;
		else if (d.curlen < 0)                                     err = 5;
		else if (d.curlen > d.maxlen)                              err = 6;
		else if (d.first > d.curlen + 1)                           err = 7;
		if (err) { out = string_trap(err, target); return false; }

		if (compute_len && (d.len < 0 || d.len > d.curlen - d.first + 1))
			d.len = d.curlen - d.first + 1;
		out = exec_trap::none;
		return true;
	}

	// The body of MVB, which SLD, SAS and SCON all finish by executing
	// with parameters of their own making (ERS sheets 108-112).
	exec_trap do_mvb(int d, unsigned sdec, bool src_is_ds)
	{
		const s32 count = s32(stk(0));
		if (count > 0x1000000 || count < -0x1000000)
			std::fprintf(stderr, "[core] MVB count %d at P=%08x\n",
					count, r.p);
		const u32 tptr = stk(1);
		const u32 sptr = stk(2);

		u32 sba;
		const exec_trap st = string_source(sptr, src_is_ds, sba);
		if (st != exec_trap::none) return st;

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

	exec_trap string_source(u32 ptr, bool is_ds, u32 &out)
	{
		if (is_ds)
		{
			const ea_result e = m_au.resolve_data_pointer(ptr, false, false);
			m_addr_trap = e.t;
			if (!e.ok()) return exec_trap::address_error;
			out = e.addr;
			return exec_trap::none;
		}
		// A local program pointer keeps flags in its top two bits and the
		// byte offset in the low thirty, the same shape do_call reads out
		// of an STT entry. HP-UX 5.30 hands MVB a flagged one and the
		// whole word taken as an offset put the source at PB +
		// 0x4000a840, which is where the machine stopped for good.
		out = r.pb + ers32::get(ptr, 2, 30);
		if (!m_au.in_code(out))
			return exec_trap::bounds_violation;
		return exec_trap::none;
	}

	// SCW and SCU (ERS sheets 103-104). Both walk a source area comparing
	// each byte against characters packed into the second stack word:
	// (16:8) is the terminal character and (24:8) the test character. Each
	// step writes the character just seen into (0:8) and its XOR with the
	// terminal character into (8:8), so the caller can tell which one
	// stopped it. SCW continues while the byte equals the test character;
	// SCU continues until it hits either character.
	exec_trap do_scan(u8 operand, bool b3, bool until)
	{
		const int d = b3 ? -1 : 1;
		const unsigned sdec = operand & 3;
		const bool src_is_ds = ((operand >> 2) & 1) != 0;

		const s32 count = s32(stk(0));
		u32 ctrl = stk(1);
		const u8 terminal = u8(ers32::get(ctrl, 16, 8));
		const u8 test = u8(ers32::get(ctrl, 24, 8));

		u32 sba;
		const exec_trap st = string_source(stk(2), src_is_ds, sba);
		if (st != exec_trap::none) return st;

		s32 x = 0;
		u8 ch = 0;
		bool scanning = true;
		while (scanning && count > d * x)
		{
			ch = m_mem.read8(sba);
			ctrl = ers32::set(ctrl, 0, 8, ch);
			ctrl = ers32::set(ctrl, 8, 8, u8(ch ^ terminal));
			const bool go = until ? (ch != test && ch != terminal)
								  : (ch == test);
			if (!go)
			{
				scanning = false;
				break;
			}
			sba = u32(s32(sba) + d);
			x += d;
		}

		set_stk(1, ctrl);
		r.x = u32(x);
		set_stk(0, u32(count - d * x));
		// "Indicators = CC on the last character scanned only if the
		// original TOS > 0". The scan of that line is damaged in both
		// instructions ("ccn"/"eCB"); read as CCA on that character, which
		// is the only condition code form that fits a single byte.
		if (count > 0)
			cca(ch);
		r.s -= 4 * sdec;
		return exec_trap::none;
	}

	//----------------------------------------------------------------
	//  program and intermediate pointers (ERS sheets 62, 122)
	//----------------------------------------------------------------

	// LPP/LPPS. Pushes the Segment Transfer Table entry at PL-Offset. If that
	// entry is a *local* program pointer -- bit 0 clear -- it is rewritten as
	// an external one naming (this code segment, this STT offset), which is
	// the form a caller in another segment can use.
	exec_trap do_lpp(u32 offset)
	{
		// The word at PL carries the STT length in its low 19 bits.
		const u32 limit = ers32::get(m_mem.read32(r.pl), 13, 19);
		if (offset > limit)
			return take_trap(trap_cause::stt_violation, 0);

		u32 v = m_mem.read32(r.pl - offset);
		if (ers32::get(v, 0, 1) == 0)
		{
			v = ers32::set(0, 0, 1, 1);
			v = ers32::set(v, 1, 19, offset);
			v = ers32::set(v, 20, 12, r.get(sf::CSN));
		}
		push(v);
		return exec_trap::none;
	}

	// LIA/LIAS. Builds a pointer to a lexically enclosing stack frame by
	// walking `level` links of the static chain, then adds a byte offset.
	// "The word preceding each stack marker is interpreted as an access
	// link" (ERS sheet 157): the caller pushes its scope's pointer as a
	// hidden last argument, so it sits one word BELOW the four-word
	// marker -- word (Q-4), sixteen bytes under the marker-top address a
	// stack segment pointer evaluates to. Reading four bytes under it
	// instead landed on the marker's own STATUS word and yielded garbage
	// scope pointers (EDS segment zero, cause 030) the moment BASIC's
	// block-structured code took a level-1 LIA.
	exec_trap do_lia(u32 level, s32 offset)
	{
		bool via_external = false;

		// Start from Q as a stack segment pointer: tag 11, offset Q-SB.
		u32 ptr = ers32::set(0, 0, 2, 3);
		ptr = ers32::set(ptr, 2, 30, r.q - r.sb);

		for (u32 d = level; d > 0; d--)
		{
			if (ers32::get(ptr, 0, 1) == 0)
				via_external = true;
			const ea_result e = m_au.resolve_data_pointer(ptr, false, false);
			if (!e.ok())
			{
				m_addr_trap = e.t;
				return exec_trap::address_error;
			}
			ptr = m_mem.read32(e.addr - 16);
		}

		ptr += u32(offset);

		// A chain that passed through an external frame yields a pointer
		// into a segment this one cannot name locally; converting it needs
		// the external data segment path, which is not modelled yet.
		if (via_external)
			return exec_trap::unimplemented;

		push(ptr);
		return exec_trap::none;
	}

	// The absolute address of the CST entry for a code segment number.
	// The CST is three sub-tables of 4-word entries whose bases live in
	// dedicated memory: system segments 0-1023 at absolute location 0,
	// sharable segments 1024-3071 at location 4, and user segments
	// 3072-4095 in word 1 of the executing CPU's dedicated block at
	// 16 * channel (ERS sheets 25-27, Appendix D).
	bool cst_entry_addr(u32 seg, u32 &addr)
	{
		u32 base, index = seg;
		if (seg < 1024)
			base = m_mem.read32(0);
		else if (seg < 3072)
		{
			base = m_mem.read32(4);
			index -= 1024;
		}
		else
		{
			base = m_mem.read32(16 * ers32::get(r.mask, 3, 3));
			index -= 3072;
		}
		if (base == 0)
			return false;
		// Word 0 of entry zero holds sixteen times the entry count.
		if (index != 0 && 16 * index > m_mem.read32(base))
			return false;
		addr = base + 16 * index;
		return true;
	}

	// Adjust a code segment's use count -- CST entry word 0 bits 1-12,
	// counting the CPUs currently executing in the segment. Segment 0 and
	// the sub-table bases have no entry -- their slot 0 is the table
	// header -- so counting them would corrupt the table length.
	void cst_use_count(u32 seg, int delta)
	{
		u32 ea;
		if (seg == 0 || seg == 1024 || seg == 3072)
			return;
		if (!cst_entry_addr(seg, ea))
			return;
		const u32 w0 = m_mem.read32(ea);
		const u32 n = ers32::get(w0, 1, 12);
		m_mem.write32(ea, ers32::set(w0, 1, 12, u32(int(n) + delta) & 0xfff));
	}

	// Resolve an external program pointer -- bits 1-19 hold four times the
	// STT entry number, bits 20-31 the code segment number (ERS sheet 28)
	// -- switching PB, PL, CSN and the mode bit to the named segment. On
	// success `target` is the absolute entry address.
	// Returns false if the call was rejected, and only then is `out`
	// meaningful. As with string_validate, a trap that reached a
	// handler leaves exec_trap::none behind, so the caller cannot use
	// that value alone to decide whether to go on -- and going on here
	// would branch to an uninitialised target.
	bool enter_segment(u32 ptr, u32 &target, exec_trap &out)
	{
		const u32 stt_off = ers32::get(ptr, 1, 19);
		const u32 seg = ers32::get(ptr, 20, 12);

		u32 ea;
		if (!cst_entry_addr(seg, ea))
		{
			out = take_trap(trap_cause::cst_violation, seg);
			return false;
		}

		const u32 w0 = m_mem.read32(ea);
		if (ers32::get(w0, 29, 1))          // A: segment absent
		{
			out = take_trap(trap_cause::cst_violation, seg);
			return false;
		}

		const u32 pb = m_mem.read32(ea + 8);
		const u32 pl = pb + m_mem.read32(ea + 4);   // length-1, low bits 11

		// The STT entry in the called segment must be a local pointer.
		const u32 slot = m_mem.read32(pl - stt_off);
		if (ers32::get(slot, 0, 1))
		{
			out = take_trap(trap_cause::external_program_pointer, slot);
			return false;
		}
		if (ers32::get(slot, 1, 1) && !r.privileged())
		{
			out = take_trap(trap_cause::uncallable_procedure, slot);
			return false;
		}

		cst_use_count(r.get(sf::CSN), -1);
		r.pb = pb;
		r.pl = pl;
		r.put(sf::CSN, seg);
		if (ers32::get(w0, 30, 1))          // M: executes privileged
			r.put(sf::M, 1);
		// This CPU joins the use count. The CST reference bit (word 0
		// bit 31) is deliberately NOT set: SUNOS keeps one unified
		// segment table, and bit 31 of word 0 is the DST layout's absent
		// bit -- marking an executed segment "referenced" would make
		// every later data access to it trap as absent. The powerup
		// module initialises the flags with bit 31 clear and nothing in
		// the OS ever clears it, so the hardware evidently does not
		// write it either.
		cst_use_count(seg, +1);

		target = pb + ers32::get(slot, 2, 30);
		out = exec_trap::none;
		return true;
	}

	// PCL/PCLS. `offset` is a byte offset back from PL naming the Segment
	// Transfer Table entry; zero means the pointer is on the stack. A
	// local pointer is a byte offset from PB (ERS convention note 18); an
	// external one walks the Code Segment Table.
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

		u32 target;
		if (ers32::get(ptr, 0, 1))
		{
			exec_trap t;
			if (!enter_segment(ptr, target, t))
				return t;
		}
		else
			target = r.pb + ers32::get(ptr, 2, 30);
		if (!m_au.in_code(target))
			return exec_trap::code_bounds;
		branch_to(target);
		return exec_trap::none;
	}

	// EXIT/SXIT. `n` counts words of parameters to drop beyond the marker.
	exec_trap do_exit(u8 n)
	{
		// The flowchart's own guards (sheet 140): an unwound Q outside
		// SB..Q is trap 1, and a new S below it trap 25. The boot relies
		// on the first: SYSTEM POWERUP's final IXIT unwinds a frame whose
		// delta-Q word is the loader's MPB inventory, and the resulting
		// fault -- with the dispatcher flag freshly set -- is what routes
		// control into the trap system and from there to the dispatcher.
		{
			const u32 d = m_mem.read32(r.q - 3);
			const u32 qnew = r.q - d;
			if (qnew < r.sb || qnew > r.q)
				return take_trap(trap_cause::bounds_violation, r.q);
			if (r.q - 16 - 4u * n < qnew)
				return take_trap(trap_cause::stack_underflow, r.q);
		}
		r.s = r.q;
		const u32 delta = stk(0);
		r.q = r.q - delta;
		const u32 old_seg = r.get(sf::CSN);
		r.status = stk(1);
		const u32 ret = stk(2);
		r.x = stk(3);
		r.s -= 16 + 4u * n;

		// "Set PB and PL from Code Segment Number in STATUS" (sheet 121).
		// When the marker's CSN matches the current segment the registers
		// are already right, and no CST need exist -- which is what lets
		// single-segment programs run before any tables are built.
		const u32 seg = r.get(sf::CSN);
		if (seg != old_seg)
		{
			if (seg == 0 || seg == 1024 || seg == 3072)
			{
				// Segments 1024 and 3072 are sub-table headers and always
				// trap 17; segment 0 traps only in unprivileged mode
				// (sheet 47). A privileged EXIT to segment 0 returns to
				// the pre-CST absolute environment the machine boots in:
				// SYSTEM POWERUP runs with PB = 0 and CSN = 0 while it
				// builds the tables, and calls into freshly-relocated
				// segments expecting this return path to restore it.
				if (seg != 0 || !r.privileged())
					return take_trap(trap_cause::external_program_pointer, seg);
				r.pb = 0;
				r.pl = 0xfffffffc;
				cst_use_count(old_seg, -1);
			}
			else
			{
				u32 ea;
				if (!cst_entry_addr(seg, ea))
					return take_trap(trap_cause::cst_violation, seg);
				r.pb = m_mem.read32(ea + 8);
				r.pl = r.pb + m_mem.read32(ea + 4);
				cst_use_count(old_seg, -1);
				cst_use_count(seg, +1);
			}
		}

		// The return word is a program pointer, and a program pointer
		// keeps flags in its top two bits and the byte offset in the
		// low thirty -- which is how do_call reads the STT entry it
		// calls through (bits 2:30). A marker the OS builds by hand can
		// therefore carry a flagged label: HP-UX 5.30's does, and taking
		// the word whole sent the machine to PB + 0x4000a840 instead of
		// PB + 0xa840 and left it sitting there.
		const u32 target = r.pb + ers32::get(ret, 2, 30);
		if (!m_au.in_code(target))
			return exec_trap::code_bounds;
		branch_to(target);
		return exec_trap::none;
	}

	//----------------------------------------------------------------
	//  the dispatcher: DISP and IXIT (ERS sheets 97-98, flowcharts on
	//  sheets 140-141)
	//----------------------------------------------------------------

	// Words of the stack relative to Q, in the ERS's (Q+k) notation: the
	// word (Q) ends at byte Q, so (Q+k) ends at Q + 4k.
	u32 qword(int k) const { return m_mem.read32(r.q - 3 + 4 * k); }
	void set_qword(int k, u32 v) { m_mem.write32(r.q - 3 + 4 * k, v); }

	// This CPU's dedicated four-word block at 16 * channel (Appendix D):
	// user CST pointer, current TCB pointer, QI, SLI.
	u32 ded_base() const { return 16 * ers32::get(r.mask, 3, 3); }

	// "Set DB and DL from the Data Segment Pointer" (ERS sheet 41): an
	// external pointer takes location and length from the DST, offset
	// ignored. For a global or stack pointer the offset is the absolute
	// address for DB, with DL the maximum for global pointers and
	// DB + 1024 words for stack pointers.
	exec_trap set_dbdl_from_pointer(u32 ptr)
	{
		const u32 tag = ers32::get(ptr, 0, 2);
		if (tag == 2)
		{
			r.db = ers32::get(ptr, 2, 30);
			r.dl = 0xffffffff;
			return exec_trap::none;
		}
		if (tag == 3)
		{
			r.db = ers32::get(ptr, 2, 30);
			r.dl = r.db + 4095;
			return exec_trap::none;
		}
		const u32 seg = ers32::get(ptr, 2, 11);
		if (seg == 0)
		{
			m_addr_trap = trap::eds_pointer_violation;
			return exec_trap::address_error;
		}
		const u32 table = ers32::get(ptr, 1, 1)
				? r.dst : dst_walker::SYSTEM_DST_BASE;
		const u32 ea = table + 16 * seg;
		r.db = m_mem.read32(ea + 8);
		r.dl = r.db + m_mem.read32(ea + 4);
		return exec_trap::none;
	}

	// A data segment table entry's location and length-1, for the TCB's
	// stack and global segment numbers. Numbers below 2048 live in the
	// system table at its architectural base; the rest in the user table
	// the DST register points at.
	void dst_loc_len(u32 seg, u32 &loc, u32 &len_m1)
	{
		const u32 table = (seg < 2048)
				? dst_walker::SYSTEM_DST_BASE : r.dst;
		const u32 ea = table + 16 * (seg & 2047);
		len_m1 = m_mem.read32(ea + 4);
		loc = m_mem.read32(ea + 8);
	}

	// The TCB path of IXIT (cases 1-3): restore the task named by the
	// current Task Control Block -- DB, DL, the user CST pointer and the
	// DST register from the TCB, then SB, SL, S and Q from the head of
	// its stack segment -- and EXIT through the marker at the stack top.
	//
	// The stack segment starts with its segment-base-relative SB in word
	// 0 and the SB-relative SL in word 1; the SB-relative S is at (SB-1),
	// and the marker S points at supplies Q (sheet 40).
	exec_trap tcb_dispatch()
	{
		const u32 ded = ded_base();
		const u32 ctcbp = m_mem.read32(ded + 4);
		const u32 entw = m_mem.read32(ctcbp + 8);
		m_mem.write32(ded, m_mem.read32(ctcbp));
		r.dst = m_mem.read32(ctcbp + 4);

		u32 loc, len;
		dst_loc_len(ers32::get(entw, 20, 12), loc, len);
		r.db = loc;
		r.dl = loc + len;

		dst_loc_len(ers32::get(entw, 4, 12), loc, len);
		const u32 sb = loc + m_mem.read32(loc);
		const u32 sl = sb + m_mem.read32(loc + 4);
		const u32 s = sb + m_mem.read32(sb - 4);
		if (sb < loc || sl > loc + len || s < sb || s > sl)
			return take_trap(trap_cause::inconsistent_registers, 0);
		r.sb = sb;
		r.sl = sl;
		r.s = s;
		r.q = s;
		m_ics_flag = false;
		return do_exit(0);
	}

	// Unwind the dispatcher-entry frame at Q on the ICS: (Q-1) status,
	// (Q-2) PB-relative P, (Q-3) X, with the global data segment pointer
	// at (Q+1). Shared by DISP and IXIT cases 7 and 8.
	exec_trap launch_ics_frame()
	{
		set_qword(0, 0);
		const exec_trap t = set_dbdl_from_pointer(qword(1));
		if (t != exec_trap::none)
			return t;
		m_mem.write32(r.sb, qword(1));

		const u32 sta = qword(-1);
		const u32 csn = ers32::get(sta, 20, 12);
		if (csn != r.get(sf::CSN))
		{
			if (csn == 0)
			{
				r.pb = 0;
				r.pl = 0xfffffffc;
			}
			else
			{
				u32 ea;
				if (!cst_entry_addr(csn, ea))
					return take_trap(trap_cause::cst_violation, csn);
				r.pb = m_mem.read32(ea + 8);
				r.pl = r.pb + m_mem.read32(ea + 4);
			}
		}
		r.status = sta;
		r.x = qword(-3);
		// The marker's return word is a program pointer: flags in the top
		// two bits, byte offset in the low thirty, as do_call reads an STT
		// entry. HP-UX's frames carry a flagged one.
		const u32 target = r.pb + ers32::get(qword(-2), 2, 30);
		if (!m_au.in_code(target))
			return take_trap(trap_cause::instruction_sequencing, target);
		r.s = r.q + 12;                     // S := Q + 3 words
		branch_to(target);
		return exec_trap::none;
	}

	// The external interrupt entry (ERS sheet 142, "INTERRUPT HANDLER").
	// `device` is the interrupting device's number, which is also its
	// Device Reference Table address. Push a marker on the current
	// stack; on first entry save S at (SB-1) and switch to the ICS from
	// QI and SLI; nested, flag the marker if the dispatcher was
	// interrupted and save the running global pointer at (Q+1). Then
	// save the mask at (Q+2) and the device at (Q+3), load DB/DL and
	// PB/PL/P from the DRT entry's data and program pointers, and run
	// the service routine privileged in its segment.

	// Message Trap delivery (cause 054): the same ICS frame as an
	// external interrupt -- marker on the interrupted stack, hop to the
	// ICS, previous mask at Q+2, parameter at Q+3 -- but vectored through
	// code segment 1's Segment Transfer Table entry 054 rather than a
	// DRT entry. IXIT's CSN==1 path restores the mask on the way out.
	// Is there anywhere to deliver a message trap to? The vector lives in
	// CS1's STT at entry 054, and until the powerup module has built the
	// code segment table and relocated CS1 there is no table to read --
	// delivering then vectors through whatever happens to be in memory.
	// Beyond that, an OS can decline the trap entirely: BASIC leaves the
	// sentinel 0x00020001 in the entry, which the trap machinery treats
	// as "unhandled" (a garbage vector faults to cause 055, whose handler
	// reports the original cause). HP-UX installs a real handler for every
	// cause, so the same test tells the two systems apart without either
	// being special-cased.
	bool message_handler_installed()
	{
		const u32 cst = m_mem.read32(0);
		if (cst == 0)
			return false;
		const u32 entry = cst + 16;
		const u32 location = m_mem.read32(entry + 8);
		if (location == 0)
			return false;
		const u32 pl = location + m_mem.read32(entry + 4) - 3;
		const u32 vector = m_mem.read32(pl - 4 * 054);
		return vector != 0 && vector != 0x00020001u;
	}

	exec_trap message_trap(u32 bits)
	{
		const u32 ret = r.p - r.pb;
		const u32 old_q = r.q;
		r.s += 16;
		set_stk(3, r.x);
		set_stk(2, ret);
		set_stk(1, r.status);
		set_stk(0, r.s - old_q);
		r.q = r.s;

		if (!m_ics_flag)
		{
			m_mem.write32(r.sb - 4, r.s - r.sb);
			const u32 ded = ded_base();
			r.q = m_mem.read32(ded + 8);
			r.sl = m_mem.read32(ded + 12);
			r.sb = (r.q & ~3u) - 256;
			m_ics_flag = true;
		}
		else
		{
			if (m_disp_flag)
			{
				m_disp_flag = false;
				set_qword(0, qword(0) | 0x80000000u);
			}
			set_qword(1, m_mem.read32(r.sb));
		}

		set_qword(2, r.mask);
		r.s = r.q + 12;                     // S := Q + 3
		set_qword(3, bits);
		// Message delivery masks further messages until the handler's
		// IXIT restores the mask from (Q+2) -- the CS1 handler is not
		// reentrant (a second message mid-handler routed its parameter
		// to the unexpected-message reporter and panicked), matching
		// the external-interrupt entry's "mask current and lower".
		// Bits 3-5 stay put: they are the live interrupt-level field
		// the IXIT restore deliberately carries over.
		r.mask |= 0xffff0000u & ~ers32::mask(3, 3);

		// CST entry 1 carries the handler segment; its STT entry 054 the
		// routine, exactly as the in-stack traps use it.
		const u32 cst = m_mem.read32(0);
		const u32 entry = cst + 16;
		const u32 len_m1 = m_mem.read32(entry + 4);
		const u32 location = m_mem.read32(entry + 8);
		r.pb = location;
		r.pl = location + len_m1 - 3;
		r.status = ers32::mask(14, 1);
		r.put(sf::CSN, 1);
		// The vector is a program pointer: offset in bits 2:30, flags
		// above. This entry kept the flags where take_trap masks them,
		// and HP-UX's message vector carries the uncallable bit -- the
		// machine jumped to PB + 0x4000a6c8 the first time a floating
		// point instruction trapped.
		const u32 vector = m_mem.read32(r.pl - 4 * 054);
		branch_to(r.pb + ers32::get(vector, 2, 30));
		return exec_trap::none;
	}

	exec_trap external_interrupt(u32 device)
	{
		const u32 ret = r.p - r.pb;
		const u32 old_q = r.q;
		r.s += 16;
		set_stk(3, r.x);
		set_stk(2, ret);
		set_stk(1, r.status);
		set_stk(0, r.s - old_q);
		r.q = r.s;

		if (!m_ics_flag)
		{
			m_mem.write32(r.sb - 4, r.s - r.sb);
			const u32 ded = ded_base();
			r.q = m_mem.read32(ded + 8);
			r.sl = m_mem.read32(ded + 12);
			r.sb = (r.q & ~3u) - 256;
			m_ics_flag = true;
		}
		else
		{
			if (m_disp_flag)
			{
				m_disp_flag = false;
				set_qword(0, qword(0) | 0x80000000u);
			}
			set_qword(1, m_mem.read32(r.sb));
		}

		set_qword(2, r.mask);
		r.s = r.q + 12;                     // S := Q + 3
		set_qword(3, device);

		// The DRT entry (ERS sheet 43): the first word is the link
		// halfword plus operating system information, the second the
		// data segment pointer, the third the interrupt routine pointer.
		const u32 dsp = m_mem.read32(device + 4);
		const u32 pgm = m_mem.read32(device + 8);
		exec_trap t = set_dbdl_from_pointer(dsp);
		if (t != exec_trap::none)
			return t;
		m_mem.write32(r.sb, dsp);

		const u32 seg = ers32::get(pgm, 20, 12);
		if (ers32::get(pgm, 0, 1) && seg != 0)
		{
			u32 ea;
			if (!cst_entry_addr(seg, ea))
				return take_trap(trap_cause::cst_violation, seg);
			r.pb = m_mem.read32(ea + 8);
			r.pl = r.pb + m_mem.read32(ea + 4);
			const u32 slot = m_mem.read32(r.pl - ers32::get(pgm, 1, 19));
			r.status = ers32::mask(14, 1);   // privileged, all else clear
			r.put(sf::CSN, seg);
			branch_to(r.pb + ers32::get(slot, 2, 30));
		}
		else
		{
			// A local pointer runs absolute, the pre-CST convention.
			r.pb = 0;
			r.pl = 0xfffffffc;
			r.status = ers32::mask(14, 1);
			branch_to(ers32::get(pgm, 2, 30));
		}
		return exec_trap::none;
	}

	// DISP (sheet 97): enter the dispatcher, or defer it by flagging (QI)
	// when already on the ICS or pseudo-disabled. Entering pushes a
	// marker, saves S at (SB-1) of the interrupted stack, hops onto the
	// ICS from QI and SLI, and unwinds the dispatcher frame there.
	exec_trap do_disp()
	{
		if (!r.privileged())
			return take_trap(trap_cause::privileged_mode, 0);
		const u32 ded = ded_base();
		const u32 qi = m_mem.read32(ded + 8);

		if (m_ics_flag || m_mem.read32(qi - 3 - 16) != 0)
		{
			m_mem.write32(qi - 3, m_mem.read32(qi - 3) | 0x80000000u);
			r.put(sf::CC, CCL);
			return exec_trap::none;
		}

		const u32 ret = m_next - r.pb;
		const u32 old_q = r.q;
		r.s += 16;
		set_stk(3, r.x);
		set_stk(2, ret);
		set_stk(1, r.status);
		set_stk(0, r.s - old_q);
		m_mem.write32(r.sb - 4, r.s - r.sb);

		r.q = qi;
		r.sl = m_mem.read32(ded + 12);
		r.sb = (qi & ~3u) - 256;            // QI - 64 words
		m_ics_flag = true;
		return launch_ics_frame();
	}

	// IXIT (sheets 96-98, flowchart sheet 141): exit from trap handlers,
	// interrupt service and the dispatcher. The eight situations of
	// section 6.2.4 reduce to four paths.
	exec_trap do_ixit()
	{
		if (!r.privileged())
			return take_trap(trap_cause::privileged_mode, 0);

		// Case 1: the dispatcher exiting to the process it picked.
		if (m_disp_flag)
		{
			m_disp_flag = false;
			return tcb_dispatch();
		}

		// An interrupt or message frame on the ICS carries the previous
		// interrupt mask at (Q+2); IXIT restores it on the way out
		// whatever segment the service routine ran in -- external
		// interrupts vector into DRIVER segments (the keyboard ISR runs
		// in segment 7), and only restoring for CSN 1 leaked the ISR's
		// "current and lower levels masked" state into the idle STOP,
		// blocking every later I/O interrupt. Bits 3-5, the live level
		// field, carry over as before.
		if (m_ics_flag)
			r.mask = ers32::set(qword(2), 3, 3, ers32::get(r.mask, 3, 3));

		if (ers32::get(qword(0), 0, 1))
		{
			// The marker below was flagged by the microcode interrupt
			// handler: the dispatcher (or a deferred dispatch) is under
			// this frame.
			m_disp_flag = true;
			const u32 qi = m_mem.read32(ded_base() + 8);
			if (ers32::get(m_mem.read32(qi - 3), 0, 1))
			{
				// A DISP was deferred. With dispatching enabled, restart
				// the dispatcher from its frame at QI (cases 7, 8). The
				// flowchart leaves SB and SL alone because IXIT assumes
				// it is already executing on the ICS; setting them to
				// the ICS geometry is what that assumption means, and it
				// also covers the boot-time entry that arrives here from
				// the powerup module's stack.
				if (m_mem.read32(qi - 3 - 16) == 0)
				{
					r.q = qi;
					r.sb = (qi & ~3u) - 256;
					r.sl = m_mem.read32(ded_base() + 12);
					m_ics_flag = true;
					return launch_ics_frame();
				}
				// Pseudo-disabled: fall through as cases 3 and 6.
			}
			else if (m_mem.read32(qi - 3) == 0)
			{
				// The frame at QI is empty: there is no interrupted
				// dispatcher to return to, only a dispatcher to enter.
				// This is the boot handoff's own path -- all three
				// systems (BASIC 2.0 and both HP-UX kernels) end SYSTEM
				// POWERUP by writing CTCBP, QI and SLI into the
				// dedicated words and executing IXIT under a marker
				// whose (Q) has its top bit set (the MPB inventory's
				// first halfword, 0x9004, carries it by construction).
				// Case 5's return-to-interrupted-dispatcher would unwind
				// the loader's marker, whose delta word IS the
				// inventory; entering the dispatcher through the TCB is
				// what the empty QI frame means. This replaces the
				// harness hack that zeroed (Q) to reach the same
				// dispatch by a different door.
				r.q = qi;
				r.sb = (qi & ~3u) - 256;
				r.sl = m_mem.read32(ded_base() + 12);
				m_ics_flag = true;
				m_disp_flag = false;
				return tcb_dispatch();
			}
			else
			{
				// Case 5: return to the interrupted dispatcher below.
				set_qword(0, qword(0) & 0x7fffffffu);
				const exec_trap t = set_dbdl_from_pointer(qword(1));
				if (t != exec_trap::none)
					return t;
				m_mem.write32(r.sb, qword(1));
				return do_exit(0);
			}
		}

		if ((qword(0) & 0x7fffffffu) == 0)
		{
			// Cases 2 and 3: the last frame on the ICS; return to the
			// interrupted process through its TCB.
			m_disp_flag = false;
			return tcb_dispatch();
		}

		// Cases 4 and 6: return to an interrupted service routine.
		set_qword(0, qword(0) & 0x7fffffffu);
		const exec_trap t = set_dbdl_from_pointer(qword(1));
		if (t != exec_trap::none)
			return t;
		m_mem.write32(r.sb, qword(1));
		return do_exit(0);
	}

	memory_interface &m_mem;
	dst_walker m_dst;
	addressing_unit m_au;
	null_io m_null_io;
	io_interface *m_io;

	u32 m_cir = 0;
public:
	// Memory access on the core's own bus, for host-side boot plumbing
	// (the loader handoff and its nudge).
	u32 read_mem32(u32 a) { return m_mem.read32(a); }
	u8 read_mem8(u32 a) { return m_mem.read8(a); }
	void write_mem32(u32 a, u32 v) { m_mem.write32(a, v); }

	// Observation hook for UP/DOWN's slow paths: called on a blocking
	// DOWN (first argument true; then semaphore, CTCBP, P) and on a
	// waking UP (false; semaphore, woken TCB, P). For harness
	// instrumentation; never affects execution.
	void (*sem_hook)(bool, u32, u32, u32) = nullptr;
private:
	u32 m_next = 0;
	bool m_branched = false;
	bool m_halted = false;
	bool m_traps_enabled = false;
	bool m_address_lockout = true;    // the default mode (ERS sheet 126)
	u32 m_control_store_date = 0x19820319;   // CPU 4.0
	bool m_disp_flag = false;         // the microcode dispatcher flag
	bool m_ics_flag = false;          // executing on the interrupt stack
	trap m_addr_trap = trap::none;
	trap_cause m_last_trap = trap_cause::reset;
};

} // namespace focus

#endif // MAME_CPU_FOCUS_FOCUSEXEC_H
