// license:BSD-3-Clause
// copyright-holders:
/***************************************************************************

    HP FOCUS execution core -- byte strings

    The byte string group (ERS sheets 101-107): descriptor validation, the
    moves, and the scan and compare instructions.

    Included into the body of focus::cpu_core; see focusexec.h. Not a
    standalone header: it has no include guard and cannot be compiled on
    its own.

***************************************************************************/

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
			knob::alarm("[core] MVB count %d at P=%08x\n", count, r.p);
		const u32 tptr = stk(1);
		const u32 sptr = stk(2);

		u32 sba;
		const exec_trap st = string_source(sptr, src_is_ds, sba);
		if (st != exec_trap::none) return st;

		const ea_result te = m_au.resolve_data_pointer(tptr, false, true);
		m_addr_trap = te.t;
		if (!te.ok()) return exec_trap::address_error;
		u32 tba = te.addr;

		// The move must behave like memmove, not memcpy: BASIC's line
		// editor slides a stack frame sixteen bytes upward over itself
		// (SETR 2 points Q at the new spot, then MVB 7 with count 0x108
		// moves the frame -- source SB+0xb0, target SB+0xc0 -- so the
		// stack marker lands at the new Q for the EXIT that follows). A
		// naive ascending copy smears the first sixteen bytes across the
		// whole region, the EXIT restores a marker made of the smear, and
		// the machine dies two statements into any stored program. When
		// the regions overlap so that a straight copy would read bytes
		// already written, the copy runs from the far end instead.
		s32 x = 0;
		static const knob::flag no_memmove("FOCUS_NOMEMMOVE");
		const bool overlap = !no_memmove && (d > 0)
				? (tba > sba && tba < sba + u32(count))
				: (tba < sba && sba < tba + u32(-count));
		if (overlap)
		{
			// Same bytes, opposite order: start at the last element and
			// walk back toward the first.
			u32 tb2 = u32(s32(tba) + d * (count - d));
			u32 sb2 = u32(s32(sba) + d * (count - d));
			while (count > d * x)
			{
				m_mem.write8(tb2, m_mem.read8(sb2));
				tb2 = u32(s32(tb2) - d);
				sb2 = u32(s32(sb2) - d);
				x += d;
			}
		}
		else
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
