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

		const u32 w0 = m_mem.read32(ea);
		if (ers32::get(w0, 29, 1))          // A: segment absent
		{
			out = take_trap(trap_cause::cst_violation, seg);
			return false;
		}

		const u32 pb = m_mem.read32(ea + 8);
		const u32 pl = pb + m_mem.read32(ea + 4);   // length-1, low bits 11

		// Cause 21's length test is "in the referenced segment" (sheet
		// 48), and the referenced segment is this one, not the caller's.
		// do_call bounds the first lookup against its own PL; this is the
		// second lookup, into the STT of the segment being entered, and
		// it was going unchecked.
		const u32 limit = ers32::get(m_mem.read32(pl), 13, 19);
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
		const u32 slot = m_mem.read32(pl - stt_off);
		if (ers32::get(slot, 0, 1))
		{
			out = take_trap(trap_cause::stt_violation, slot);
			return false;
		}
		if (ers32::get(slot, 1, 1) && !r.privileged())
		{
			out = take_trap(trap_cause::uncallable_procedure, slot);
			return false;
		}

		// Every transfer between segments comes through here, so this is
		// the one place that sees the whole call graph. docs/SUNOS.md has
		// the static version, read out of the boot image.
		static knob::channel segcall("FOCUS_SEGCALL", knob::sync::buffered);
		if (segcall)
			segcall("[segcall %u -> %u entry %u  p=%08x]\n",
					r.get(sf::CSN), seg, stt_off / 4, r.p);

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
		static const knob::number calllog_p("FOCUS_CALLLOG", 0, 16);
		u32 ptr;
		if (offset == 0)
			ptr = pop();
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
