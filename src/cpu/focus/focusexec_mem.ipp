// license:BSD-3-Clause
// copyright-holders:
/***************************************************************************

    HP FOCUS execution core -- memory reference and the MPB

    Formats 2, 5 and 6: shifts and short branches, the 8-bit immediate group,
    the 32-bit long forms, relative addresses, and the writes that reach
    another processor across the Memory Processor Bus.

    Included into the body of focus::cpu_core; see focusexec.h. Not a
    standalone header: it has no include guard and cannot be compiled on
    its own.

***************************************************************************/

private:
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
				knob::alarm("[core] PMVA count %d at P=%08x "
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

			byte_cursor src, dst;
			{
				const exec_trap t1 = cursor_open(src, stk(2), src_is_ds, false, d, sba);
				if (t1 != exec_trap::none) return t1;
				const exec_trap t2 = cursor_open(dst, stk(1), true, false, d);
				if (t2 != exec_trap::none) return t2;
			}

			s32 x = 0;
			u8 sb = 0, tb = 0;
			while (count > d * x)
			{
				sb = m_mem.read8(src.phys);
				tb = m_mem.read8(dst.phys);
				if (sb != tb)
					break;
				x += d;
				if (count > d * x)
				{
					exec_trap t = cursor_step(src);
					if (t == exec_trap::none)
						t = cursor_step(dst);
					if (t != exec_trap::none) return t;
				}
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

			byte_cursor dst;
			{
				const exec_trap t2 = cursor_open(dst, stk(1), true, true, d);
				if (t2 != exec_trap::none) return t2;
			}

			s32 x = 0;
			while (count > d * x)
			{
				m_mem.write8(dst.phys, m_mem.read8(tblba + m_mem.read8(dst.phys)));
				x += d;
				if (count > d * x)
				{
					const exec_trap t = cursor_step(dst);
					if (t != exec_trap::none)
					{
						// Translation is in place, so a restart from the
						// original pointer would put bytes already done
						// through the table a second time. Leave the
						// resumable state MVB leaves: the remainder, the
						// target advanced past the work, X the progress.
						r.x = u32(x);
						set_stk(0, u32(count - d * x));
						set_stk(1, u32(s32(stk(1)) + x));
						return t;
					}
				}
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
			if (stack_would_overflow(news))
				return stack_grow_trap();
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
				if (stack_would_overflow(news))
					return stack_grow_trap();
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
			// The S form's value is the offset of S BEFORE the push:
			// the ERS writes it as "S:=S+1; (S):=S-SB-1", both S's being
			// the incremented one, so the -1 undoes the increment rather
			// than biasing the offset. In the word units of that
			// pseudocode one word is 1; here S is a byte address, so
			// subtracting a literal 1 took a byte off the offset. It
			// cancelled against the same mistake in SETR whenever a
			// PSHR/SETR pair carried S back to itself, which is why it
			// survived -- but AP's procedure prologue pushes the S form
			// and restores it as the Q form (PSHR 1 / SETR 2, to open a
			// frame at the current top), and there the missing byte came
			// straight through: Q landed one byte below the word grid,
			// and the EXIT that unwound it read its delta from a
			// misaligned marker and trapped as a bounds violation.
			if (operand & 1)  push(r.s - r.sb);
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
			if (operand & 1)  { r.s = stk(0) + r.sb; }
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
				byte_cursor dst;
				{
					const exec_trap t2 = cursor_open(dst, tptr, true, true, 1);
					if (t2 != exec_trap::none) return t2;
				}

				s32 x = 0;
				while (count > x)
				{
					m_mem.write8(dst.phys, fill);
					x++;
					if (count > x)
					{
						const exec_trap t = cursor_step(dst);
						if (t != exec_trap::none) return t;
					}
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
			if (r.test(sf::C)) { r.put(sf::C, 0); rel(); }
			return exec_trap::none;
		case 061: case 065:                                       // BNC
			if (!r.test(sf::C)) rel(); else r.put(sf::C, 0);
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
			// value. In range gives CCE and falls through, out of range
			// gives CCG and branches -- or takes a Check Trap when the
			// branch field is zero, which is how a CASE with no default
			// reports an unmatched selector.
			//
			// The bounds are inclusive. The ERS pseudocode as scanned reads
			// "If (S-1) < (S-2) < (S)", strict at both ends, and BASIC
			// settles it: three sites in the boot image push 48 and 57 --
			// '0' and '9' -- around a character and branch on the result,
			// which is a digit test that the strict reading would narrow
			// to '1'..'8'. Others push 34 and 90 ('"' and 'Z'), and -24
			// and 24. tools/focusdis.py over the boot image lists them.
			//
			// The pop cannot be hoisted above the test. The bounds go when
			// the value is in range and when the branch is taken, but
			// "they are not popped for the trap case" (sheet 93) -- a
			// Check Trap handler is entitled to still find them there.
			const s32 hi = s32(stk(0)), lo = s32(stk(1)), v = s32(stk(2));
			if (v >= lo && v <= hi)
			{
				r.s -= 8;
				r.put(sf::CC, CCE);
				return exec_trap::none;
			}
			r.put(sf::CC, CCG);
			if (operand == 0)
				return take_trap(trap_cause::check_trap, m_cir);
			r.s -= 8;
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
		// is why compilers emit SOL at every source line (ERS sheet 100).
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
			// SXIT, Subroutine Exit (ERS sheet 97). Nothing like EXIT:
			// there is no stack marker. Subroutines are called with an
			// LRA/BR pair which stacks only a PB-relative return address,
			// so the exit pops that and drops `imm` further words of
			// parameters.
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
		// ADDS and SUBS each have two forms, and only the fixed one was
		// implemented: a zero operand means the count is on the stack
		// instead ("If the 8-bit operand is zero, the content of the TOS
		// is added to S-1", ERS sheets 87-88), which is how a routine
		// reserves a buffer whose size it only learns at run time. Both
		// halves silently did nothing, and they have to be fixed
		// together -- SDF_DISC reserves 725 words this way on every
		// call, so ADDS alone grows a stack that is never given back
		// and the boot drowns in stack-overflow traps within a second
		// and a half. The pair also explains how the old build survived
		// at all: with neither the allocation nor the release happening,
		// the routine's buffer simply sat in the unclaimed words above
		// S, where it worked until something else pushed over it.
		case 032: {                                              // ADDS
			const u32 news = imm ? r.s + 4u * imm
					: (r.s - 4) + 4u * stk(0);
			if (stack_would_overflow(news))
				return stack_grow_trap();
			r.s = news;
			return exec_trap::none;
		}
		case 033: {                                              // SUBS
			// The released count "must be positive"; a zero or negative
			// one releases nothing and only the count itself goes.
			if (imm)
				r.s -= 4u * imm;
			else
			{
				const s32 n = s32(stk(0));
				r.s -= 4;
				if (n > 0)
					r.s -= 4u * u32(n);
			}
			return exec_trap::none;
		}
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
			// Deposit Field (ERS sheet 74). J = CIR(27:5) is the leftmost
			// destination bit, K = CIR(22:5) is 32 minus the width, and
			// the L low bits of the TOS land in bits J..J+L-1 of the word
			// beneath.
			u32 fields = d & 0x3ff;
			if (idx) fields += r.x;
			const u32 j = fields & 0x1f, k = (fields >> 5) & 0x1f;
			const u32 len = 32 - k;
			const u32 src = pop();
			u32 dst = stk(0);
			// "All bits greater than 31 are lost": when J + L runs past
			// the end of the word the field is truncated there, keeping
			// the leading bits of the source. Computing the shift as
			// 32 - J - L and using it unguarded goes negative exactly
			// then -- an undefined shift -- and the boot image really
			// does contain such sites, J=20 K=16 and J=2 K=1 among them.
			const u32 eff = std::min(len, 32 - j);
			if (eff >= 32)
				dst = src;
			else
			{
				const u32 mask = ((u32(1) << eff) - 1) << (32 - j - eff);
				const u32 val = (src >> (len - eff)) << (32 - j - eff);
				dst = (dst & ~mask) | (val & mask);
			}
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
			dcca(stk(1), stk(0));
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
			//
			// And branches PRE-index, alone among the instructions.
			// Sheet 17: "For all instructions except branch instructions
			// postindexing is used... Branch instructions use
			// preindexing so that high-level language CASE statements
			// can be executed efficiently. In this case, the base,
			// offset and index register select one of a set of indirect
			// pointers which is then evaluated (without using the index
			// register) to determine the branch target." Sheet 108 says
			// it again: "In all cases preindexing is performed."
			//
			// The general addressing unit post-indexes, correctly, for
			// everything else -- so an indirect branch cannot be left to
			// it. Post-indexing a CASE dispatch reads the table entry
			// for arm ZERO and then adds the index to the address that
			// entry resolves to, which lands a few bytes into the wrong
			// arm. That is how HP-UX's console driver came to call a
			// routine that takes a lock the caller was already holding:
			// the state machine at CORE SYSTEM +0x3ffe dispatched arm 1
			// into the middle of arm 0, and the machine spun on its own
			// lock with interrupts disabled.
			if (ind)
			{
				u32 pa;
				if (!ea(base, off, false, idx, false, pa))
					return exec_trap::address_error;
				const u32 ptr = m_mem.read32(pa);
				u32 target;
				if (base == BASE_P_PLUS || base == BASE_P_MINUS)
					target = u32(s32(pa) + s32(ptr));  // self-relative
				else if (ers32::get(ptr, 0, 1))
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
			if (!ea(base, off, false, idx, false, a))
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

	// LRA / LRAS (ERS sheet 61). This does not push an absolute address: in
	// direct mode it computes a *relative, tagged* pointer and makes no
	// memory reference at all, so there is nothing to bounds check.
	//
	//   P-relative        -> E - PB, untagged
	//   DB or DL relative -> base + offset - DB, tagged 10
	//   stack relative    -> base + offset - SB, tagged 11
	//
	// For the two tagged forms indexing is applied to the resulting pointer
	// rather than to the address; the P-relative form takes it in E, before
	// PB is subtracted. Either way the index lands in the pushed value.
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
				// Sheet 61 gives this arm as (S+1) := E - PB, and E is the
				// INDEXED effective address -- the prose says "a pointer to
				// the computed address (using byte indexing)" and section
				// 2.2.4 defines the computed address as base + offset + X.
				// The tagged arms below add X to the pointer they build
				// because they never form E; this arm has to add it to the
				// address, and did not.
				//
				// The cost of the omission was every indexed P-relative LRAS
				// pointing at element ZERO of its table. BASIC's compiler
				// picks the code template for each statement and each
				// operator with exactly this instruction (COMPILER +0x2090,
				// `LRAS P+14,X` over the pool at +0x209e), so every statement
				// it compiled was template 0 -- `LDIS #9; PCLS STT[9]`. That
				// is the "spurious literal 9" every operator printed, and the
				// missing `TRY,B` statement prologue behind the second
				// statement's trap 1, NEW's short frame and LET's absent
				// pointer.
				push(addr + (idx ? r.x : 0) - r.pb);
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
