// license:BSD-3-Clause
// copyright-holders:
/***************************************************************************

    HP FOCUS execution core -- the dispatcher

    Dedicated memory, the Interrupt Control Stack, the message and stack
    overflow traps, external interrupts, and DISP and IXIT -- the
    flowcharts on ERS sheets 140-142.

    Included into the body of focus::cpu_core; see focusexec.h. Not a
    standalone header: it has no include guard and cannot be compiled on
    its own.

***************************************************************************/

private:

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
		{
			static knob::channel dispdbg("FOCUS_DISPDBG");
			if (dispdbg)
				dispdbg("[dispatch tcb=%08x entw=%08x dst=%08x stackseg=%u loc=%08x len=%08x sb=%08x sl=%08x s=%08x]\n",
						ctcbp, entw, r.dst, ers32::get(entw, 4, 12), loc, len, sb, sl, s);
			return take_trap(trap_cause::inconsistent_registers, 0);
		}
		r.sb = sb;
		r.sl = sl;
		r.s = s;
		r.q = s;
		m_ics_flag = false;
		m_exit_from_ixit = true;
		const exec_trap t = do_exit(0);
		m_exit_from_ixit = false;
		return t;
	}

	// Unwind the dispatcher-entry frame at Q on the ICS: (Q-1) status,
	// (Q-2) PB-relative P, (Q-3) X, with the global data segment pointer
	// at (Q+1). Shared by DISP, IXIT cases 7 and 8 and PSEB's restart.
	//
	// Every one of them ends "DISP FLAG := 1" (DISP's pseudocode, sheet
	// 98; IXIT and PSEB enter the dispatcher the same way): from here on
	// the dispatcher is what runs. The flag used to be set only by IXIT's
	// case 5, so a dispatcher entered by DISP -- every blocking DOWN
	// enters it that way -- ran with it clear, and two things went wrong.
	// An interrupt taken over it pushed an unflagged marker, so the
	// service routine's IXIT returned to the dispatcher as to any
	// interrupted routine even when the routine had asked for a dispatch
	// (case 8 never happened); and the dispatcher's own IXIT to its
	// chosen task was taken for the last interrupt frame on the ICS,
	// which "restored" MASK from the stale word at QI+2 -- whatever the
	// last interrupted task had -- instead of leaving the dispatcher's.
	exec_trap launch_ics_frame()
	{
		m_disp_flag = true;
		set_qword(0, 0);
		const exec_trap t = set_dbdl_from_pointer(qword(1));
		if (t != exec_trap::none)
			return t;
		m_mem.write32(r.sb, qword(1));

		const u32 sta = qword(-1);
		const u32 csn = ers32::get(sta, 20, 12);
		if (csn != r.get(sf::CSN))
		{
			// The dispatcher's segment is entered like any other: the
			// interrupted segment gives up its use count and the new
			// one gains it, each on its own account (segment 0 has no
			// count; see take_trap). HP-UX's dispatcher is segment 0, so
			// skipping both whenever either side was 0 left every task
			// that blocked holding a count on its segment, and gaining
			// another each time it was dispatched back.
			cst_use_count(r.get(sf::CSN), -1);
			cst_use_count(csn, +1);
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
		r.status = status_from_marker(sta);
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

	// The frame every ICS-run entry shares (ERS sheets 141-142): a marker
	// on the interrupted stack, S saved at (SB-1) and a hop to the ICS on
	// first entry, the interrupted global pointer at (Q+1) when already
	// there, then the previous mask at (Q+2) and the cause's parameter at
	// (Q+3). External interrupts, message traps and Stack Overflow all
	// arrive this way; only what they vector through differs.
	void enter_ics(u32 parameter)
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
		set_qword(3, parameter);
	}

	// Vector into code segment 1 through its Segment Transfer Table, the
	// way every trap enters its handler. The entry is a program pointer:
	// offset in bits 2:30, flags above -- HP-UX's carry the uncallable
	// bit, so the offset has to be extracted rather than used whole.
	void vector_cs1(unsigned cause)
	{
		const u32 cst = m_mem.read32(0);
		const u32 entry = cst + 16;
		const u32 len_m1 = m_mem.read32(entry + 4);
		const u32 location = m_mem.read32(entry + 8);
		r.pb = location;
		r.pl = location + len_m1 - 3;
		// Counted like any other trap entry (ERS 5.2.1 step 3), each
		// side on its own account; see take_trap.
		cst_use_count(r.get(sf::CSN), -1);
		cst_use_count(1, +1);
		r.status = ers32::mask(14, 1);
		r.put(sf::CSN, 1);
		const u32 vector = m_mem.read32(r.pl - 4 * cause);
		branch_to(r.pb + ers32::get(vector, 2, 30));
	}

	// Stack Overflow (cause 024, ERS sheets 45-51). The condition is
	// tested between instructions, not inside one: every stack segment
	// carries an overflow area of at least 64 words above SL "so that
	// instructions can run to completion even after causing S to become
	// larger than SL and that all pending traps and interrupts can be
	// processed". The service routine runs on the ICS -- BASIC's asks the
	// system to grow the stack segment and leaves through IXIT, which is
	// why this builds the interrupt frame rather than the in-stack marker
	// take_trap makes. On the ICS there is nobody to ask, and the
	// microcode simply raises SL by 64 words itself.
	//
	// Without this, a stack that outgrows its segment is never noticed:
	// the pushes keep working, and the first instruction that does test
	// against SL -- TRY deleting its frame -- reports a bounds violation
	// instead, tens of thousands of instructions after the fact. That is
	// what stopped RUN, and it is why LIST died on its second statement.
	// "ADDS, PSHN, SLD, and SCaN clear STAT[9:1] before trapping" (ERS
	// sheet 51). Those four are the instructions that move S by an amount
	// the program chose, so unlike every other instruction they test SL
	// themselves instead of leaving S past it for the end-of-instruction
	// check to find. That distinction is not cosmetic: the check between
	// instructions can only report a deficit already incurred, and BASIC
	// starts a program by allocating its whole variable area in one PSHN
	// -- 3,330 words for the 9020 test driver. Run to completion, that
	// put S 6 KB beyond SL, outside the stack segment altogether, and the
	// handler grew the stack by just enough to cover where S had already
	// landed; the next call over-ran the fresh limit and the second
	// overflow was fatal. Trapping at the limit instead keeps S inside
	// the segment and asks for the growth before the damage.
	//
	// STAT[9:1] is EM, the machine-instruction trap enable. Clearing it
	// is what makes the sequence resumable: the trap returns to the
	// instruction, not past it, so it must not re-arm on the way in.
	exec_trap stack_grow_trap()
	{
		r.put(sf::EM, 0);
		return stack_overflow_trap();
	}

	// Would moving S to `news` cross SL? Guarded exactly as the
	// end-of-instruction test is, so the two agree about when the
	// condition exists at all -- a boot running with traps disabled, or
	// before SL means anything, has no stack limit to cross.
	bool stack_would_overflow(u32 news) const
	{
		return m_traps_enabled && r.sl != 0 && news > r.sl;
	}

	exec_trap stack_overflow_trap()
	{
		if (on_trap)
			on_trap(trap_cause::stack_overflow, 0);
		if (m_ics_flag)
		{
			r.sl += 256;                    // 64 words
			return exec_trap::none;
		}
		enter_ics(0);
		vector_cs1(unsigned(trap_cause::stack_overflow));
		return exec_trap::none;
	}

	exec_trap message_trap(u32 bits)
	{
		enter_ics(bits);
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
		vector_cs1(054);
		return exec_trap::none;
	}

	exec_trap external_interrupt(u32 device)
	{
		enter_ics(device);

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
			// "The interrupt handler decrements the use count for the
			// current code segment, and increments the use count for the
			// new segment. For the special case of code segment 0 the use
			// count is neither incremented nor decremented" (sheet 43):
			// segment 0's own count, that is -- it has none. The other
			// side of the transfer is counted (see take_trap).
			cst_use_count(r.get(sf::CSN), -1);
			cst_use_count(seg, +1);
			r.status = ers32::mask(14, 1);   // privileged, all else clear
			r.put(sf::CSN, seg);
			branch_to(r.pb + ers32::get(slot, 2, 30));
		}
		else
		{
			// A local pointer runs absolute, the pre-CST convention:
			// segment 0, which the interrupted segment's count is given
			// up for like any other (the IXIT's EXIT takes it back).
			cst_use_count(r.get(sf::CSN), -1);
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
		return launch_ics_frame();          // DISP FLAG := 1
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
			else if (!m_ics_flag && m_mem.read32(qi - 3) == 0
					&& m_mem.read32(qi - 3 - 16) == 0)
			{
				// Only OFF the ICS. On it, a flagged marker with (QI)
				// clear and dispatching enabled is the commonest case 5
				// of all: a service routine returning to a dispatcher it
				// interrupted at its idle STOP. Once DISP set the
				// dispatcher flag as it should, that return would come
				// through here and dispatch through whatever TCB was
				// current -- the idle one -- instead of resuming the
				// dispatcher (exectest's interrupted-dispatcher case; a
				// reviewer's first build without this test halted at
				// 1.3 s). The handoff's marker is on the powerup
				// module's own stack, never the ICS.
				// The frame at QI is empty AND dispatching is not
				// pseudo-disabled: there is no interrupted dispatcher to
				// return to, only a dispatcher to enter.
				//
				// The delta word alone does not say "empty". It is zero
				// for the bottom frame on the ICS, which is equally true
				// of a dispatcher that really was interrupted -- and
				// taking this door out of one of those threw the
				// interrupted context away and dispatched through a TCB
				// that had never been filled in, restoring SB, SL, Q and
				// S as 0x35 apiece and trapping stack underflow inside
				// the IXIT. The pseudo-disable count is what separates
				// the two, exactly as it does in the deferred-DISP case
				// just above: the boot handoff leaves it zero (the word
				// is never written and memory starts clear), and a
				// running system that has disabled dispatching around a
				// critical section leaves it set. With the count set,
				// fall through to cases 4 and 6 and return to whatever
				// was interrupted.
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
