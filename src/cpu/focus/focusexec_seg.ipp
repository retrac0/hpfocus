// license:BSD-3-Clause
// copyright-holders:
/***************************************************************************

    HP FOCUS execution core -- segmentation and procedure call

    Program and intermediate pointers, the Code Segment Table and its use
    counts, entering a segment, and the procedure call and return
    instructions (ERS sheets 62, 92, 96, 122).

    Included into the body of focus::cpu_core; see focusexec.h. Not a
    standalone header: it has no include guard and cannot be compiled on
    its own.

***************************************************************************/

private:

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
	//
	// Gaining the count is entering the segment, which is the access the
	// CST's reference bit records: "R = Reference bit - set to 1 when the
	// segment is accessed" (ERS sheet 27; word 0 bit 29, with M at 30 and
	// A at 31, the DST entry's layout). HP-UX ages code segments with the
	// same sweep as data segments (see focusdst.h): a user program
	// segment's word 0 is 0x00022908, which matches its "present, unused,
	// referenced" test only once R is set, and the kernel clears R again
	// each time it stamps the segment.
	void cst_use_count(u32 seg, int delta)
	{
		u32 ea;
		if (seg == 0 || seg == 1024 || seg == 3072)
			return;
		if (!cst_entry_addr(seg, ea))
			return;
		u32 w0 = m_mem.read32(ea);
		const u32 n = ers32::get(w0, 1, 12);
		if (delta > 0)
			w0 = ers32::set(w0, 29, 1, 1);
		m_mem.write32(ea, ers32::set(w0, 1, 12, u32(int(n) + delta) & 0xfff));
	}

	// An external data segment pointer advanced by a byte delta, for CPTR
	// and CPEP with the index bit ("a complete evaluation of the pointer
	// is performed", ERS sheet 120). While the sum stays inside the
	// pointer's nineteen offset bits that is the offset field and nothing
	// else. When it does not, a PAGED segment's pointer carries into the
	// segment number, as the evaluation on sheet 56 has it (TEMP := PTR +
	// X; dst_walker::carry_paged), and the result is a pointer into the
	// next segment: the OFFSET the instruction then tests fits, and there
	// is no Pointer Conversion Violation. An unpaged segment's pointer
	// has nowhere to carry to and traps as before.
	//
	// HP-UX needs the carry. Its kernel keeps a transfer's user address
	// as the base of the process's data space (user segment 11, offset 0)
	// and a byte offset, and forms the address with CPTR,B (IOS and DDMs
	// +0x4c18, before the MVB that copies) -- so the first read or write
	// touching a byte past 512 KB of a process's data, which lies in
	// segment 12, took cause 016 instead of the pointer 0x40600000 + n.
	exec_trap index_external_pointer(u32 ptr, u32 delta, u32 &out)
	{
		// Summed in 64 bits: the delta is whatever the program had in X,
		// and offset + 0x7fffffff is not a sum the host's int can hold. A
		// sum beyond 32 bits is beyond every table too; clamped, it still
		// is, and carry_paged refuses it as it would the true one.
		s64 noff = s64(ers32::get(ptr, 13, 19)) + s64(s32(delta));
		if (noff >= 0 && noff < (1 << 19))
		{
			out = ers32::set(ptr, 13, 19, u32(noff));
			return exec_trap::none;
		}
		noff = std::clamp<s64>(noff, INT32_MIN, INT32_MAX);
		u32 seg = ers32::get(ptr, 2, 11), off = u32(s32(noff));
		trap t = trap::none;
		const int c = m_dst.carry_paged(ers32::get(ptr, 1, 1) == 0, seg, off, t);
		if (c == 0)
			return exec_trap::pointer_conversion;
		if (c < 0)
		{
			m_addr_trap = t;
			return exec_trap::address_error;
		}
		out = ers32::set(ers32::set(ptr, 2, 11, seg), 13, 19, off);
		return exec_trap::none;
	}

	// Adjust a data segment's use count -- DST entry word 0 bits 1-12,
	// "the number of tasks currently using this segment". Saturating
	// rather than wrapping, as MUC does: a miscounted release must not
	// make a pinned segment look free. Entry zero is the table header.
	void dst_use_count(u32 entry_addr, int delta)
	{
		const u32 w0 = m_mem.read32(entry_addr);
		u32 n = ers32::get(w0, 1, 12);
		if (delta > 0) { if (n != 0xfff) n++; }
		else           { if (n != 0) n--; }
		m_mem.write32(entry_addr, ers32::set(w0, 1, 12, n));
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
	// What an external program pointer resolves to, worked out without
	// touching a register: the PCL flowchart (sheet 138) raises traps 17,
	// 21, 22, 37 and 41 BEFORE it pops the pointer and pushes the stack
	// marker, and every one of them returns to the PCL to be retried.
	// Validating after the pop and the push, as this used to, left the
	// retry with a marker already on the stack and the pointer gone: it
	// popped a word of the marker as its pointer -- a local offset, so
	// the call landed at PB+0x58 of the wrong segment -- and each retry
	// leaked sixteen bytes. HP-UX's init, whose program segment traps
	// 37 the first time it is entered, walked into the kernel that way
	// and then spun on trap 41 until its stack was 137 KB deep.
	struct segment_target
	{
		u32 seg = 0, w0 = 0, pb = 0, pl = 0, slot = 0, target = 0;
	};
	bool check_external(u32 ptr, segment_target &e, exec_trap &out)
	{
		const u32 stt_off = ers32::get(ptr, 1, 19);
		const u32 seg = ers32::get(ptr, 20, 12);
		e.seg = seg;

		// "An external program pointer contains a code segment number of
		// zero, 1024, or 3072" (sheet 47, cause 17). do_exit had the EXIT
		// half of this; the pointer half was missing. 1024 and 3072 are
		// the sharable and user sub-table headers, and 0 is the pre-CST
		// absolute environment -- none of them is callable.
		if (seg == 0 || seg == 1024 || seg == 3072)
		{
			out = take_trap(trap_cause::external_program_pointer, seg);
			return false;
		}

		u32 ea;
		if (!cst_entry_addr(seg, ea))
		{
			out = take_trap(trap_cause::cst_violation, seg);
			return false;
		}

		e.w0 = m_mem.read32(ea);
		// The absence bit is word 0 bit 31, as in the DST entry: the two
		// tables share one layout in their last three bits -- R at 29, M
		// at 30, A at 31 -- and HP-UX keeps them in one table. Reading A
		// at bit 29 took the R bit the loader sets on a freshly written
		// program segment for absence, and every free entry (word 0 = 1
		// or 3, A set) for present.
		if (ers32::get(e.w0, 31, 1))        // A: segment absent
		{
			// "Absent Code Segment. The absence bit in the CST entry for
			// the referenced segment is 1. The CST entry number is passed
			// as a parameter. If this trap occurs on the ICS, the
			// parameter is negated" (sheet 49, cause 37) -- and the trap
			// returns to the instruction, so the OS can bring the
			// segment in and let the call proceed. This is how HP-UX
			// loads a program: exec leaves the process's code segment
			// absent and the first PCL into it asks for the load.
			// Reported as CST Violation (cause 22) instead, the call was
			// routed to the fault handler, which signalled the process
			// and let it run on -- unprivileged, in kernel code.
			out = take_trap(trap_cause::absent_code_segment,
					m_ics_flag ? u32(-s32(seg)) : seg);
			return false;
		}

		e.pb = m_mem.read32(ea + 8);
		e.pl = e.pb + m_mem.read32(ea + 4);   // length-1, low bits 11

		// Cause 21's length test is "in the referenced segment" (sheet
		// 48), and the referenced segment is this one, not the caller's.
		// do_call bounds the first lookup against its own PL; this is the
		// second lookup, into the STT of the segment being entered, and
		// it was going unchecked.
		const u32 limit = ers32::get(m_mem.read32(e.pl), 13, 19);
		if (stt_off > limit)
		{
			out = take_trap(trap_cause::stt_violation, stt_off);
			return false;
		}

		// The STT entry in the called segment must be a local pointer.
		// This is STT Violation condition 2 -- "the STT number in an
		// external program pointer points to an external program pointer
		// in the referenced segment" (sheet 48) -- and not cause 17,
		// which is about a pointer naming a reserved segment number.
		e.slot = m_mem.read32(e.pl - stt_off);
		if (ers32::get(e.slot, 0, 1))
		{
			out = take_trap(trap_cause::stt_violation, e.slot);
			return false;
		}
		if (ers32::get(e.slot, 1, 1) && !r.privileged())
		{
			out = take_trap(trap_cause::uncallable_procedure, e.slot);
			return false;
		}
		e.target = e.pb + ers32::get(e.slot, 2, 30);
		out = exec_trap::none;
		return true;
	}

	// The register side of entering the segment check_external resolved.
	void commit_external(const segment_target &e, u32 ptr)
	{
		const u32 stt_off = ers32::get(ptr, 1, 19);
		// Every transfer between segments comes through here, so this is
		// the one place that sees the whole call graph. docs/SUNOS.md has
		// the static version, read out of the boot image.
		static knob::channel segcall("FOCUS_SEGCALL", knob::sync::buffered);
		if (segcall)
			segcall("[segcall %u -> %u entry %u  p=%08x]\n",
					r.get(sf::CSN), e.seg, stt_off / 4, r.p);

		cst_use_count(r.get(sf::CSN), -1);
		r.pb = e.pb;
		r.pl = e.pl;
		r.put(sf::CSN, e.seg);
		if (ers32::get(e.w0, 30, 1))        // M: executes privileged
			r.put(sf::M, 1);
		// This CPU joins the use count, and cst_use_count marks the
		// segment referenced (word 0 bit 29) as it does. (This used to
		// say the reference bit was bit 31 and must not be written, as
		// the DST layout's absence bit; bit 31 is the absence bit in
		// BOTH layouts, and R is 29 in both.)
		cst_use_count(e.seg, +1);
	}

	bool enter_segment(u32 ptr, u32 &target, exec_trap &out)
	{
		segment_target e;
		if (!check_external(ptr, e, out))
			return false;
		commit_external(e, ptr);
		target = e.target;
		return true;
	}

	// PCL/PCLS. `offset` is a byte offset back from PL naming the Segment
	// Transfer Table entry; zero means the pointer is on the stack. A
	// local pointer is a byte offset from PB (ERS convention note 18); an
	// external one walks the Code Segment Table.
	exec_trap do_call(u32 offset)
	{
		static const knob::number calllog_p("FOCUS_CALLLOG", 0, 16);
		u32 ptr;
		if (offset == 0)
			ptr = stk(0);       // popped below, once the call is known good
		else
		{
			// The entry must lie within the STT, whose byte length is in
			// the word at PL -- the same check LPP makes. This is not
			// defensive tidiness but a load-bearing OS interface: BASIC
			// compiles a program into a heap code segment with a SHORT
			// transfer table and emits calls to entries beyond it; the
			// STT-violation trap is how the OS learns it must link the
			// entry before the call can proceed. Reading past the table
			// instead found a zero, and the call landed at the segment's
			// own constant pool -- RUN executed floats until an 0xffff.
			const u32 limit = ers32::get(m_mem.read32(r.pl), 13, 19);
			if (offset > limit)
				return take_trap(trap_cause::stt_violation, offset);
			ptr = m_mem.read32(r.pl - offset);
		}
		static knob::channel calllog("FOCUS_CALLLOG");
		if (calllog_p && r.p == calllog_p)
			calllog("[call p=%08x pl=%08x offset=%02x ptr=%08x]\n",
					r.p, r.pl, offset, ptr);

		// Resolve and check the target first: the flowchart's traps come
		// before the pointer is popped and the marker pushed, so a trap
		// returns to a PCL that can simply run again (see check_external).
		const bool external = ers32::get(ptr, 0, 1) != 0;
		segment_target e;
		u32 target;
		if (external)
		{
			exec_trap t;
			if (!check_external(ptr, e, t))
				return t;
			target = e.target;
		}
		else
		{
			target = r.pb + ers32::get(ptr, 2, 30);
			if (!m_au.in_code(target))
				return exec_trap::code_bounds;
		}

		if (offset == 0)
			pop();

		const u32 ret = m_next - r.pb;      // PB-relative return address
		const u32 old_q = r.q;

		r.s += 16;                          // four words
		set_stk(3, r.x);
		set_stk(2, ret);
		set_stk(1, r.status);
		set_stk(0, r.s - old_q);            // delta Q
		r.q = r.s;

		if (external)
		{
			commit_external(e, ptr);
			// Cause 55 is tested after the transfer (sheet 50): "the
			// segment number and delta P in the stack marker are
			// probably in error" presumes the marker is there.
			if (!m_au.in_code(target))
				return exec_trap::code_bounds;
		}
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
		// "A routine executing in unprivileged mode may not EXIT to
		// privileged mode, may not change the interrupt bit, and may not
		// set the instruction pending bit in the Status register" (sheet
		// 96) -- Privileged Mode Violation, cause 26, whose parameter
		// tells the three conditions apart: "2) An attempt is made to
		// EXIT or Branch from an unprivileged procedure to a privileged
		// procedure. 3) An attempt is made in an unprivileged EXIT
		// instruction to change the interrupt bit or to set the
		// instruction pending bit" (sheet 47). Note 2: nothing has been
		// popped and the marker points at this EXIT.
		//
		// A marker is ordinary stack memory, and "STATUS := (S-1)" loaded
		// whatever a user program had written there: sixteen bytes on
		// its own stack and an EXIT made it privileged, in any segment
		// including segment 0's absolute addressing (the test below reads
		// the mode bit the marker has just supplied) -- the same door
		// SETR's field limits and the privilege checks on STOP and INT
		// were closed against. And since the string instructions now
		// resume through STATUS.P, a P set this way is no longer inert:
		// SLD, SAS and SCON skip their descriptors and move whatever
		// three words are on the stack, from whatever X says.
		//
		// Both tests are on the EXECUTING mode, so a handler's EXIT, the
		// dispatcher's and IXIT's (all privileged) load STATUS whole, as
		// HP-UX's signal and single-step paths need them to.
		if (!r.privileged())
		{
			const u32 st = m_mem.read32(r.q - 7);
			if (ers32::get(st, 14, 1))
				return take_trap(trap_cause::privileged_mode, 2);
			if (ers32::get(st, 13, 1) != ers32::get(r.status, 13, 1)
					|| (ers32::get(st, 19, 1) && !r.test(sf::P)))
				return take_trap(trap_cause::privileged_mode, 3);
		}
		// Returning into a code segment that is absent is cause 37, as a
		// call into one is (see enter_segment): the CST entry number is
		// the parameter, the trap returns to this EXIT, and the marker
		// must still be here for the retry -- so the test comes before
		// anything is popped. The marker's STATUS word sits just under Q.
		{
			const u32 st = m_mem.read32(r.q - 7);
			const u32 seg = ers32::get(st, 20, 12);
			u32 ea;
			if (!m_exit_from_ixit
					&& seg != r.get(sf::CSN) && seg != 0 && seg != 1024
					&& seg != 3072 && cst_entry_addr(seg, ea)
					&& ers32::get(m_mem.read32(ea), 31, 1))
				return take_trap(trap_cause::absent_code_segment,
						m_ics_flag ? u32(-s32(seg)) : seg);
		}
		r.s = r.q;
		const u32 delta = stk(0);
		r.q = r.q - delta;
		const u32 old_seg = r.get(sf::CSN);
		r.status = status_from_marker(stk(1));
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
				cst_use_count(old_seg, -1);
				cst_use_count(seg, +1);
				if (m_exit_from_ixit && ers32::get(m_mem.read32(ea), 31, 1))
				{
					// IXIT returning a task to a code segment that was
					// swapped out while it waited. The dispatcher pins a
					// task's stack and global segments before choosing it
					// (MUC,B on both) but nothing pins its code, and an
					// IXIT cannot be retried from the task's side the way
					// an EXIT is retried above: by now the registers are
					// the task's, the ICS has been left, and a trap that
					// returned to the IXIT would run the dispatcher's
					// instruction in the task's context -- which is what
					// happened, unwinding the task's own frame as if it
					// were an interrupt's and loading DB = 0 from a word
					// that was not a pointer, so that the shell died of
					// "Unexpected memory fault" (seven logins at once
					// swapped enough out to get there). Taken on the ICS
					// instead, with the parameter negated, the kernel's
					// handler halts on Inconsistent Registers. What is
					// left is the task's own view: it was returning into
					// segment `seg` at `ret`, and that segment is absent
					// -- exactly the trap a branch into it would raise,
					// taken on the task's stack with the task's status in
					// the marker, so the handler's EXIT brings the
					// segment in and lands on the instruction the task
					// was about to execute.
					r.pb = 0;
					r.pl = 0xfffffffc;
					r.p = ers32::get(ret, 2, 30);
					return take_trap(trap_cause::absent_code_segment, seg);
				}
				r.pb = m_mem.read32(ea + 8);
				r.pl = r.pb + m_mem.read32(ea + 4);
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
