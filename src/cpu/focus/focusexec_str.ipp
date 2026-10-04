// license:GPL-2.0+
// copyright-holders:Joel Heikkila
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

	// Descriptor arithmetic wraps as the machine's 32-bit adder does; a
	// signed overflow in the host's `int` is undefined, and descriptors
	// are whatever the program stacked.
	static s32 wadd(s32 a, s32 b) { return s32(u32(a) + u32(b)); }

	// Adding a byte offset to a string pointer must not disturb the two
	// bits that say which kind of pointer it is (section 5.2.2, error 8).
	// The stricter rule the ERS gives for external data segment pointers
	// into unpaged segments -- a trap if any of the top thirteen bits
	// change -- needs the segment's paged flag to apply safely, so only
	// the tag check is made here.
	bool string_offset(u32 ptr, s32 delta, bool target, u32 &out,
			exec_trap &t)
	{
		out = ptr + u32(delta);
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
		else if (d.len >= 0 && s64(d.first) + d.len - 1 > d.maxlen) err = 4;
		else if (d.curlen < 0)                                     err = 5;
		else if (d.curlen > d.maxlen)                              err = 6;
		else if (d.first > s64(d.curlen) + 1)                      err = 7;
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
		const ea_result e = m_au.resolve_data_pointer(c.ptr + u32(c.disp),
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
			// A local program pointer: bounded by PB and by the last
			// byte of the segment's last word (convention note 18).
			c.phys = code_phys;
			c.left = (dir > 0) ? (r.pl + 3 - code_phys + 1)
					: (code_phys - r.pb + 1);
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
			c.phys += u32(c.dir);
			return exec_trap::none;
		}
		// The run is used up. For a code operand the next byte is past
		// PB or PL, and for a stack or global pointer (bit 0 set) past
		// the segment's bound -- said here, because stepping the pointer
		// itself below offset zero would change its tag and be reported
		// as some other pointer's fault. For an external data segment
		// pointer the next byte is a reference of its own: another page,
		// or outside the segment, which the evaluation reports.
		if (!c.ds)
			return exec_trap::bounds_violation;
		if (ers32::get(c.ptr, 0, 1))
		{
			m_addr_trap = trap::bounds_violation;
			return exec_trap::address_error;
		}
		return cursor_resolve(c);
	}

	// Where an interruptible byte-string instruction takes up its work.
	// The loop on every one of these sheets begins `X := 0`, and that is
	// all a first execution can do. One that a trap cut short comes back
	// with the Instruction Pending bit set: "P = Instruction Pending Bit
	// - set to 1 by an instruction if it gets interrupted. This bit is
	// cleared when the instruction is finally completed" (ERS sheet 11).
	// The instruction set it, the trap's marker saved it with the rest of
	// STATUS and X, the handler's EXIT restored both -- and the
	// instruction goes on from X, with its stack exactly as the program
	// wrote it. A progress count that cannot be one (the wrong sign, or
	// past the count) is not believed.
	s32 string_resume(int d, s32 count) const
	{
		if (!r.test(sf::P))
			return 0;
		// (In 64 bits: X is whatever the marker held, and -1 times -2^31
		// is not a 32-bit number.)
		const s32 x = s32(r.x);
		const s64 done = s64(d) * s64(x);
		if (done < 0 || done > s64(count))
			return 0;
		return x;
	}

	// The last step of SLD, SAS and SCON: "Execute MVB Instruction" on the
	// three parameters they have just stacked (ERS sheets 108-112). By then
	// the descriptors they were given are gone and the stack rearranged,
	// so a trap in the move -- the source or target running into an
	// absent page -- cannot send the instruction back to its beginning:
	// it would take the MVB parameters for a descriptor. It is pending
	// from here on instead, X counting the bytes moved, which is what the
	// three sheets' "the original content of X is destroyed early in the
	// instruction" is saying; the re-executed instruction sees STATUS.P
	// and finishes the move.
	exec_trap string_move(int d, bool src_is_ds)
	{
		r.x = 0;
		r.put(sf::P, 1);
		return do_mvb(d, 3, src_is_ds);
	}

	// The body of MVB, which SLD, SAS and SCON all finish by executing
	// with parameters of their own making (ERS sheets 108-112).
	exec_trap do_mvb(int d, unsigned sdec, bool src_is_ds)
	{
		const s32 count = s32(stk(0));
		if (count > 0x1000000 || count < -0x1000000)
			knob::alarm("[core] MVB count %d at P=%08x\n", count, r.p);
		// The pointers as the program stacked them, and how far along
		// them a move that was interrupted had got.
		s32 x = string_resume(d, count);
		const u32 tptr = stk(1) + u32(x);
		const u32 sptr = stk(2) + u32(x);
		const s32 rem = count - d * x;      // bytes still to move

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
		static const knob::flag as_memmove("FOCUS_MVBMEMMOVE");
		const bool clash = (d > 0)
				? (tba > sba && tba < sba + u32(rem))
				: (tba < sba && sba < tba + u32(-rem));
		// Which way an overlapping move has to run is the one place the
		// two readings of this instruction disagree, so every instance
		// is reportable: FOCUS.md says byte-at-a-time in the chosen
		// direction, which makes a target of source+n a self-propagating
		// fill, and that is what HP's display self test relies on.
		static knob::channel mvblog("FOCUS_MVBLOG");
		if (mvblog && count >= 1024)
			mvblog("[mvb start src=%08x(%08x) dst=%08x(%08x) n=%d p=%08x seg=%u x_in=%08x]\n",
					sptr, sba, tptr, tba, count, r.p - r.pb, r.get(sf::CSN), r.x);
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
			u32 tb2 = u32(s32(tba) + d * (rem - d));
			u32 sb2 = u32(s32(sba) + d * (rem - d));
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
						// Resumable: X holds the bytes done and STATUS.P
						// says the instruction is pending (see
						// string_resume), which is how the re-executed
						// instruction tells a continuation from a first
						// execution -- its loop begins `X := 0`, and HP's
						// code never clears X before an MVB. The stack is
						// left exactly as the program wrote it, so a move
						// that was interrupted ends as one that was not:
						// X the whole count, the count word reduced by
						// it, and any pointer an SDEC below 3 leaves on
						// the stack still the one the program pushed.
						//
						// This once reported the remainder in the count
						// word and left the pointers alone, which was the
						// bug that lost HP-UX's init its /etc/passwd
						// parser: the exec loader copies a 1 KB file
						// block into a paged data segment, the copy
						// crosses into an absent page after 712 bytes,
						// the kernel maps the page and re-executes, and
						// the 312 remaining bytes were copied to the
						// ORIGINAL target again while offsets 0x800..0x937
						// -- the C library's character-class table --
						// stayed zero. Pages 2, 4 and 8, the first of
						// each group the pager maps, all lost their first
						// 0x138 bytes the same way; getpwnam("root")
						// then found no digits in "0" and init went
						// single-user. The fix that followed rewrote the
						// count AND both pointer words on the stack,
						// which moved the right bytes but ended with X
						// short by the bytes moved before the trap and
						// with any pointer an SDEC < 3 keeps changed
						// under the program -- what the program sees
						// depended on whether a page happened to fault.
						if (mvblog)
							mvblog("[mvb TRAP src=%08x dst=%08x n=%d done=%d "
									"p=%08x seg=%u x_in=%08x]\n", stk(2), stk(1),
									count, x, r.p - r.pb, r.get(sf::CSN), r.x);
						r.x = u32(x);
						r.put(sf::P, 1);
						return t;
					}
				}
			}
		r.put(sf::P, 0);
		r.x = u32(x);
		set_stk(0, u32(count - d * x));
		r.s -= 4 * sdec;
		return exec_trap::none;
	}

	// `run`, when asked for, gets how many bytes from `out` are physically
	// contiguous (ea_result::run_fwd; a code operand is all one piece).
	exec_trap string_source(u32 ptr, bool is_ds, u32 &out, u32 *run = nullptr)
	{
		if (run)
			*run = 0x7fffffffu;
		if (is_ds)
		{
			const ea_result e = m_au.resolve_data_pointer(ptr, false, false);
			m_addr_trap = e.t;
			if (!e.ok()) return exec_trap::address_error;
			out = e.addr;
			if (run)
				*run = e.run_fwd;
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
