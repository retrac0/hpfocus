// license:BSD-3-Clause
// copyright-holders:
/***************************************************************************

    HP FOCUS execution core -- I/O and the trap sequence

    The I/O instructions (FOCUS I/O ERS section 6) and the internal trap
    sequence they and everything else vector through (ERS sheets 44-52).

    Included into the body of focus::cpu_core; see focusexec.h. Not a
    standalone header: it has no include guard and cannot be compiled on
    its own.

***************************************************************************/

private:

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

	// Notes 1 and 2 of the trap list (ERS sheet 52) split every cause in
	// two, and the difference is the whole of whether a trap is
	// recoverable: note 1 says "the trap condition is detected at the end
	// of an instruction and the offset in the stack marker points to the
	// NEXT instruction", note 2 that it "is detected during an instruction
	// and the offset ... points to THIS instruction". A note-2 handler
	// fixes what the instruction complained about and returns to it; the
	// instruction then runs and succeeds.
	//
	// Only the debug and arithmetic traps are note 1 -- the ones a
	// completed instruction reports about itself. Everything else is note
	// 2, so that is the default here and this list is the exception.
	static bool trap_points_at_next(trap_cause cause)
	{
		switch (cause)
		{
		case trap_cause::check_trap:                 // notes 1 and 3
		case trap_cause::machine_instruction:        // notes 1, 3 and 4
		case trap_cause::unimplemented_instruction:  // notes 1 and 3
		case trap_cause::user_trap:
		case trap_cause::fp_operand:
		case trap_cause::fp_result:
		case trap_cause::start_of_line:
		case trap_cause::variable_trace:
		case trap_cause::start_of_procedure:
		case trap_cause::end_of_procedure:
		case trap_cause::start_of_subroutine:
		case trap_cause::end_of_subroutine:
		case trap_cause::message_trap:
		case trap_cause::start_of_line_check:
			return true;
		default:
			return false;
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

		// Note 1 returns past the instruction, note 2 back to it. Every
		// trap used to return past, which is why BASIC could not run a
		// typed-in program: the scaffold calls STT[9] in a segment whose
		// transfer table has six entries, the STT-violation trap is how
		// BASIC is asked to link the ninth, and returning past the call
		// meant the linked entry was never reached -- execution carried
		// on from the instruction after a call that never happened.
		const u32 ret = (trap_points_at_next(cause) ? m_next : r.p) - r.pb;
		const u32 old_q = r.q;
		const u32 old_pb = r.pb;

		r.s += 16;
		set_stk(3, r.x);
		set_stk(2, ret);
		set_stk(1, r.status);
		set_stk(0, r.s - old_q);
		r.q = r.s;

		// "A parameter is pushed onto the current stack at Q+3" (ERS
		// 5.2.1 step 4) -- the same slot the external interrupt entry
		// and the message trap use, with (Q+1) and (Q+2) being the
		// interrupt frame's saved global pointer and mask, which an
		// internal trap has no use for. Every handler in HP-UX 5.30
		// that wants its parameter reads Q+12: the absent data segment,
		// absent page and absent page table handlers all begin `LDS
		// Q+12`. Pushed at Q+1, as this used to do, the parameter sat
		// two words short of where the handler looked, and the first
		// absent-segment fault of the boot -- the kernel building its
		// first process -- became a bounds violation inside the handler
		// and a halt.
		m_mem.write32(r.q + 4, 0);
		m_mem.write32(r.q + 8, 0);
		r.s = r.q + 12;
		m_mem.write32(r.s, parameter);

		r.pb = location;
		// LENGTH-1 always ends in 11 and PL must end in 00, so the last
		// word of the segment starts three bytes lower.
		r.pl = location + len_m1 - 3;

		// "The use count for the exited code segment is decremented, and
		// the use count for code segment 1 is incremented" (ERS 5.2.1
		// step 3). EXIT back out of the handler does the reverse, so a
		// trap that was not counted on the way in left the interrupted
		// segment one higher every time it was taken: the paging faults
		// alone put thousands on the kernel's own segments, and one on a
		// user library segment was enough to pin it -- the memory
		// manager's relocator refuses any segment with a non-zero count,
		// the compactor could not clear the head of the pool for the
		// pager's next 32 KB, and the shell's children died of their
		// page-ins with "Killed".
		{
			const u32 from = r.get(sf::CSN);
			if (from != 0)
			{
				cst_use_count(from, -1);
				cst_use_count(1, +1);
			}
		}

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
