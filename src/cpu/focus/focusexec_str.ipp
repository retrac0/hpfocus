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

	// A byte position inside a string operand. Code operands and the
	// stack and global segments are one piece of memory, but an external
	// data segment is only contiguous within a page (or, chained, within
	// one segment of the chain), so a byte walk keeps the logical pointer
	// and resolves it again whenever the physical run it was given is
	// used up. Every string instruction that steps through a data
	// segment pointer walks through one of these.
	struct byte_cursor
	{
		u32 ptr = 0;        // the operand pointer, as the instruction had it
		bool ds = false;    // data segment pointer (else code, contiguous)
		bool modify = false;
		int dir = 1;
		u32 phys = 0;       // physical address of the current byte
		u32 left = 0;       // bytes contiguous from phys in `dir`, this one included
		s32 disp = 0;       // bytes walked from the original pointer
	};

	exec_trap cursor_resolve(byte_cursor &c)
	{
		const ea_result e = m_au.resolve_data_pointer(u32(s32(c.ptr) + c.disp),
				false, c.modify);
		m_addr_trap = e.t;
		if (!e.ok())
			return exec_trap::address_error;
		c.phys = e.addr;
		c.left = c.dir > 0 ? e.run_fwd : e.run_back;
		return exec_trap::none;
	}

	exec_trap cursor_open(byte_cursor &c, u32 ptr, bool is_ds, bool modify,
			int dir, u32 code_phys = 0)
	{
		c.ptr = ptr;
		c.ds = is_ds;
		c.modify = modify;
		c.dir = dir;
		c.disp = 0;
		if (!is_ds)
		{
			c.phys = code_phys;
			c.left = 0x7fffffffu;
			return exec_trap::none;
		}
		return cursor_resolve(c);
	}

	// Step to the next byte; resolves afresh at the end of a run.
	exec_trap cursor_step(byte_cursor &c)
	{
		c.disp += c.dir;
		if (c.left > 1)
		{
			c.left--;
			c.phys = u32(s32(c.phys) + c.dir);
			return exec_trap::none;
		}
		if (!c.ds)
		{
			c.phys = u32(s32(c.phys) + c.dir);
			return exec_trap::none;
		}
		return cursor_resolve(c);
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

		byte_cursor src, dst;
		{
			const exec_trap t1 = cursor_open(src, sptr, src_is_ds, false, d, sba);
			if (t1 != exec_trap::none) return t1;
			const exec_trap t2 = cursor_open(dst, tptr, true, true, d);
			if (t2 != exec_trap::none) return t2;
		}

		// An overlapping move runs BYTE AT A TIME in the chosen
		// direction, which is what FOCUS.md says of this instruction and
		// what makes a target of source+n a self-propagating fill: the
		// bytes just written are read again, and a four-byte pattern
		// replicates through the whole region. HP's software uses that
		// on purpose -- PWP SELF TEST (segment 13, local 20) fills its
		// DMA buffer with exactly this idiom before the display's memory
		// test sends it, and nothing else in the collection needs an
		// overlapping move at all.
		//
		// This read `memmove` for a while, on the diagnosis that BASIC's
		// line editor slid a stack frame sixteen bytes over itself and a
		// naive ascending copy smeared it, killing any stored program
		// two statements in. That was the wrong instruction: the real
		// cause was `LRAS P+n,X` dropping its index, so the compiler
		// built every statement from code template 0, and the sixteen
		// bytes were a missing `TRY,B`'s four words. Fixing LRAS fixed
		// BASIC, and FOCUS_MVBLOG says the rest -- across a whole BASIC
		// boot and a stored program that runs correctly, the ONLY
		// overlapping moves are the four the display self test makes.
		s32 x = 0;
		static const knob::flag as_memmove("FOCUS_MVBMEMMOVE");
		const bool clash = (d > 0)
				? (tba > sba && tba < sba + u32(count))
				: (tba < sba && sba < tba + u32(-count));
		// Which way an overlapping move has to run is the one place the
		// two readings of this instruction disagree, so every instance
		// is reportable: FOCUS.md says byte-at-a-time in the chosen
		// direction, which makes a target of source+n a self-propagating
		// fill, and that is what HP's display self test relies on.
		static knob::channel mvblog("FOCUS_MVBLOG");
		if (clash && mvblog)
			mvblog("[mvb %s src=%08x dst=%08x n=%d gap=%+d p=%08x seg=%u]\n",
					d > 0 ? "fwd" : "back", sba, tba, count,
					int(s32(tba) - s32(sba)), r.p - r.pb,
					r.get(sf::CSN));
		const bool overlap = clash && bool(as_memmove);
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
				m_mem.write8(dst.phys, m_mem.read8(src.phys));
				x += d;
				if (count > d * x)
				{
					exec_trap t = cursor_step(src);
					if (t == exec_trap::none)
						t = cursor_step(dst);
					if (t != exec_trap::none)
					{
						// Resumable: the count and X say how far it got.
						r.x = u32(x);
						set_stk(0, u32(count - d * x));
						return t;
					}
				}
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
		byte_cursor src;
		{
			const exec_trap t1 = cursor_open(src, stk(2), src_is_ds, false, d, sba);
			if (t1 != exec_trap::none) return t1;
		}

		s32 x = 0;
		u8 ch = 0;
		bool scanning = true;
		while (scanning && count > d * x)
		{
			ch = m_mem.read8(src.phys);
			ctrl = ers32::set(ctrl, 0, 8, ch);
			ctrl = ers32::set(ctrl, 8, 8, u8(ch ^ terminal));
			const bool go = until ? (ch != test && ch != terminal)
								  : (ch == test);
			if (!go)
			{
				scanning = false;
				break;
			}
			x += d;
			if (count > d * x)
			{
				const exec_trap t = cursor_step(src);
				if (t != exec_trap::none) return t;
			}
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
