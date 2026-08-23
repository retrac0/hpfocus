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

#include <cmath>
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
	// Instruction timing. The Hardware Technical Data sheet gives four
	// measured figures for the Series 500 and no instruction table:
	//
	//     load register from memory     550 ns
	//     64-bit floating point add    1.17 us
	//     64-bit floating point mpy    1.28 us
	//     32-bit integer multiply      2.92 us
	//
	// At the 18 MHz clock those are 10, 21, 23 and 53 cycles. Charging
	// one cycle per instruction -- as this core used to -- makes emulated
	// time run about ten times faster than the hardware could, so every
	// emulated second demands ten times the instructions the real machine
	// executed. On a workload that idles in STOP, like BASIC at its
	// prompt, that is invisible. On one that spins, like HP-UX, it is the
	// difference between a machine that keeps up and one that crawls.
	//
	// A real per-opcode table would need timings nobody documented; these
	// four anchor points and one typical cost for everything else are what
	// the manuals actually support.
	static constexpr unsigned CYCLES_TYPICAL = 10;   // 550 ns
	static constexpr unsigned CYCLES_FADD    = 21;   // 1.17 us
	static constexpr unsigned CYCLES_FMPY    = 23;   // 1.28 us
	static constexpr unsigned CYCLES_MPY     = 53;   // 2.92 us

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
		static const knob::flag msg10_trap("FOCUS_MSG10TRAP");
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
			// FOCUS_INTLOG reports each message bit the I/O service
			// vectors through, with the DRT entry it took and how many
			// times -- the count is the point. A device that is being
			// serviced thousands of times a second is not being
			// serviced: it is stuck at the head of its level's list.
			// One line per 2000 services of a bit: the count is the
			// point, so the channel is uncapped and the sampling is here.
			static knob::channel intlog("FOCUS_INTLOG");
			if (intlog)
			{
				static unsigned n[32] = {};
				if ((++n[bit] % 2000) == 1)
					intlog("[int bit %u serviced %u times, head=%08x "
							"pgm=%08x]\n", bit, n[bit], head,
							m_mem.read32(head + 8));
			}
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

	// What the last instruction cost in 18 MHz clocks; see CYCLES_TYPICAL.
	unsigned cycles = CYCLES_TYPICAL;

	exec_trap step()
	{
		if (m_halted)
			return exec_trap::halted;

		m_addr_trap = trap::none;
		m_branched = false;
		cycles = CYCLES_TYPICAL;

		// Stack Overflow is condition 6 of the end-of-instruction list
		// (ERS sheet 50), below the I/O interrupts the device loop has
		// already had its chance at, and it is tested again "just prior
		// to executing the first instruction of the trap or interrupt
		// service routine" -- so between instructions is where it goes.
		// Detected here, the marker's return offset points at the next
		// instruction, which is what makes the sequence resumable once
		// the handler has grown the stack.
		if (m_traps_enabled && r.sl != 0 && r.s > r.sl)
			return stack_overflow_trap();

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
		static knob::channel flagbr("FOCUS_FLAGBR");
		if (target >= 0x1000000u && flagbr)
			flagbr.err("[core] branch to %08x from P=%08x "
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

	// FIXD and FIXS, whose ERS entries (sheets 87-88) agree on both
	// details this core used to miss.
	//
	// "Indicators = CCA, Overflow" -- a value the 32-bit result cannot
	// hold sets Overflow, and it is read back. HP-UX's MATH LIBRARY has a
	// truncate-to-integral-double routine that is written entirely around
	// it: clear the user-trap enable, FIXD, `BOV` to a slow path that
	// masks the fraction off by exponent instead, otherwise FLTD the fixed
	// value back. Leaving Overflow alone made every large value come back
	// as the saturated s32 cast: the kernel's 10 Hz callout daemon rounds
	// the microsecond clock up to the next 100 ms through that routine,
	// got -2147483647 * 100000 for its deadline, found the deadline
	// already past, never slept, and spun at task priority forever with
	// the rest of the boot behind it.
	//
	// "If CIR(19:1)=0 then the integer part is kept and the fractional
	// part truncated. If CIR(19:1)=1 then this number is rounded (by
	// adding 0.5 to its magnitude)" -- that is the ",B" bit, which was
	// ignored; both forms truncated.
	//
	// The result on overflow is not specified. The low 32 bits of the
	// whole integer is what a machine that converts and then narrows
	// produces, and it is at least deterministic; a value too large even
	// for 64 bits, or a NaN, has no such answer and gives zero.
	u32 fix(double d, bool round)
	{
		const double t = round ? ((d < 0) ? std::ceil(d - 0.5)
						: std::floor(d + 0.5))
				: std::trunc(d);
		const bool fits = t >= -2147483648.0 && t <= 2147483647.0;
		r.put(sf::O, fits ? 0 : 1);
		if (fits)
			return u32(s32(t));
		return (t > -9.2233720368547758e18 && t < 9.2233720368547758e18)
				? u32(u64(s64(t))) : 0u;
	}
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

	//----------------------------------------------------------------
	//  the instruction set
	//----------------------------------------------------------------
	//
	// The six format dispatchers and the machinery behind them, split by
	// subject into fragments included here. They are part of this class,
	// not headers in their own right -- keeping the core header-only is
	// what lets src/test build it without a MAME tree, and the split is
	// for reading, not for separate compilation.

#include "focusexec_alu.ipp"
#include "focusexec_mem.ipp"
#include "focusexec_trap.ipp"
#include "focusexec_str.ipp"
#include "focusexec_seg.ipp"
#include "focusexec_disp.ipp"

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
	std::function<void(bool, u32, u32, u32)> sem_hook;
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
