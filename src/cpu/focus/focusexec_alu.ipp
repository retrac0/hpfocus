// license:BSD-3-Clause
// copyright-holders:
/***************************************************************************

    HP FOCUS execution core -- arithmetic and the stack

    Formats 1, 3 and 4: loads and stores through a base register, the stack
    rearrangement group, integer and logical arithmetic, the X register,
    binary floating point and packed decimal.

    Included into the body of focus::cpu_core; see focusexec.h. Not a
    standalone header: it has no include guard and cannot be compiled on
    its own.

***************************************************************************/

private:
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
			u32 a, a2;          // a2: see ea_double, on the page boundary
			if (!ea_double(base, raw * 4, ind, idx, false, a, a2))
				return exec_trap::address_error;
			push(m_mem.read32(a));
			push(m_mem.read32(a2));
			dcca(stk(1), stk(0));
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
		case 6:     // BCCS -- see ERS sheet 91
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

	// CCF(0:1) selects CCG, CCF(1:1) CCE, CCF(2:1) CCL (ERS sheet 91).
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
			// (ERS sheet 67), stopping at SL -- see stack_grow_trap.
			// X counts down as the words go on, so the trap is resumable
			// where it stood: the IXIT comes back to this same PSHN and
			// it pushes the rest into the grown stack.
			u32 fill = 0;
			if (b3)
				fill = pop();
			const bool pushed_any = r.x > 0;
			while (r.x > 0)
			{
				if (stack_would_overflow(r.s + 4))
					return stack_grow_trap();
				push(fill);
				r.x--;
			}
			// "If original X>0 then CCA else unaffected": a PSHN that
			// pushed nothing leaves the code alone.
			if (pushed_any)
				cca(stk(0));
			return exec_trap::none;
		}
		case 006: push(0); cca(0); return exec_trap::none;      // ZERO
		case 007: push(0); push(0); return exec_trap::none;    // DZRO
		case 016: {                                            // DXCH
			const u32 a = stk(0), b = stk(1), c = stk(2), d = stk(3);
			set_stk(0, c); set_stk(1, d); set_stk(2, a); set_stk(3, b);
			dcca(stk(1), stk(0));
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
		// ADD and SUB (ERS sheets 74-75, items 56-57). The b bit makes each
		// the TOP limb of a multiple precision chain whose lower limbs are
		// LADD/LSUB: "(S-1)+(S)+Carry" and "(S-1)-(S)-(1-Carry)". The
		// borrow is carried inverted -- Carry set means no borrow, as LSUB
		// and DLSB have it -- so SUB,B takes away 1-Carry, not Carry, and
		// SUB leaves its own carry out the same way up. And the indicators
		// with b set are "CCA only if result#0": a zero top limb must not
		// erase the nonzero a lower limb reported, which is what lets a
		// doubleword compare branch on the chain's condition code.
		case 020: {                                            // ADD
			const u32 a = pop(), b = pop();
			const u64 sum = u64(b) + u64(a) + (b3 && r.test(sf::C) ? 1 : 0);
			set_overflow_add(b, a, u32(sum));
			r.put(sf::C, (sum >> 32) & 1);
			push(u32(sum));
			if (!b3 || u32(sum) != 0)
				cca(u32(sum));
			return exec_trap::none;
		}
		case 021: {                                            // SUB
			const u32 a = pop(), b = pop();
			const u64 diff = u64(b) - u64(a) - (b3 && !r.test(sf::C) ? 1 : 0);
			set_overflow_sub(b, a, u32(diff));
			r.put(sf::C, ((diff >> 32) & 1) ^ 1);              // set = no borrow
			push(u32(diff));
			if (!b3 || u32(diff) != 0)
				cca(u32(diff));
			return exec_trap::none;
		}
		case 022: { cycles = CYCLES_MPY;                       // MPY
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
			// The one doubleword quotient that will not fit -- -2^63 by
			// -1 -- and, unguarded, a division the host itself faults on:
			// x86-64 raises #DE for it, so any program, privileged or
			// not, that pushed 0x80000000, 0, -1 and divided ended the
			// whole emulator with SIGFPE. The quotient's overflow is the
			// ERS's own answer ("Indicators = CCA on quotient,
			// Overflow"); the dividend is left standing as the quotient
			// with a zero remainder, which is what DIVS already does for
			// its own -2^31 by -1.
			if (dividend == INT64_MIN && divisor == -1)
			{
				r.put(sf::O, 1);
				set_stk(0, 0);
				dcca(stk(2), stk(1));
				return exec_trap::none;
			}
			s64 quot = dividend / divisor;
			s32 rem = s32(dividend % divisor);
			// "If CIR(19:1)=1 then the remainder is always positive. In
			// both cases, dividend = quotient*divisor + remainder" (sheet
			// 76): a remainder forced positive takes the quotient with
			// it, one step away from zero. Negating the remainder alone
			// broke the identity and gave the wrong modulus besides: -7
			// by 3 came back as -2 remainder 1, not -3 remainder 2.
			if (b3 && rem < 0)
			{
				if (divisor > 0) { rem += divisor; quot -= 1; }
				else             { rem -= divisor; quot += 1; }
			}
			set_stk(2, u32(u64(quot) >> 32));
			set_stk(1, u32(u64(quot)));
			set_stk(0, u32(rem));
			cca(u32(u64(quot)));
			return exec_trap::none;
		}
		case 024: {                                            // NEG
			// Negated in unsigned arithmetic: -s32 of -2^31 is undefined
			// in C++. That one value is also the overflow the indicator
			// line ("CCA, Overflow") is there for -- it negates to itself.
			const u32 x = pop(), v = 0u - x;
			if (x == 0x80000000u)
				r.put(sf::O, 1);
			push(v); cca(v);
			return exec_trap::none;
		}
		// The test group deletes its operand only with the b bit
		// (ERS sheets 33-34.5).
		case 025: cca(stk(0)); if (b3) pop();
			return exec_trap::none;                            // TEST
		case 026: {                                            // TSTD
			dcca(stk(1), stk(0));
			if (b3) { pop(); pop(); }
			return exec_trap::none;
		}
		case 027: r.put(sf::CC, (stk(0) & 0xff) ? CCG : CCE);
			if (b3) pop();
			return exec_trap::none;                            // TSTB
		case 032: {                                            // XCH
			const u32 a = stk(0), b = stk(1);
			set_stk(0, b); set_stk(1, a);
			cca(stk(0));
			return exec_trap::none;
		}
		case 033: {                                            // DIVS
			// Quotient replaces (S-1), remainder replaces (S); with bit 3
			// set the remainder is forced positive (ERS sheet 78).
			const s32 divisor = s32(stk(0)), dividend = s32(stk(1));
			if (divisor == 0)
				return exec_trap::divide_by_zero;
			// The one quotient that will not fit, and the one division
			// that is undefined behaviour in C++ as well.
			if (dividend == INT32_MIN && divisor == -1)
			{
				r.put(sf::O, 1);
				set_stk(1, u32(dividend));
				set_stk(0, 0);
				cca(stk(1));
				return exec_trap::none;
			}
			s32 quot = dividend / divisor, rem = dividend % divisor;
			// As in DIV: a remainder forced positive moves the quotient
			// too, so that dividend = quotient*divisor + remainder.
			if (b3 && rem < 0)
			{
				if (divisor > 0) { rem += divisor; quot -= 1; }
				else             { rem -= divisor; quot += 1; }
			}
			set_stk(1, u32(quot));
			set_stk(0, u32(rem));
			cca(u32(quot));
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
			cca(stk(0));
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
		case 045: push(stk(0)); cca(stk(0));
			return exec_trap::none;                            // DUP
		case 046: { const u32 a = stk(0), b = stk(1); push(b); push(a);
			dcca(stk(1), stk(0));
			return exec_trap::none; }                          // DDUP
		case 056: {                                            // CAB
			const u32 a = stk(0), b = stk(1), c = stk(2);
			set_stk(0, c); set_stk(1, a); set_stk(2, b);
			cca(stk(0));
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
			push(fix(d, b3)); cca(stk(0));
			return exec_trap::none; }
		case 042: { const s32 v = s32(pop()); push_double(double(v));
			set_fp_cc(double(v));
			return exec_trap::none; }                          // FLTD
		case 050: {                                            // FCMP
			const double y = pop_double();
			const double x = to_double((u64(stk(1)) << 32) | stk(0));
			r.put(sf::CC, (x < y) ? CCL : (x == y) ? CCE : CCG);
			if (b3) r.s -= 8;
			return exec_trap::none;
		}
		case 051: { cycles = CYCLES_FADD;
			const double y = pop_double(), x = pop_double();
			push_double(x + y); set_fp_cc(x + y);
			return exec_trap::none; }                          // FADD
		case 052: { cycles = CYCLES_FADD;
			const double y = pop_double(), x = pop_double();
			push_double(x - y); set_fp_cc(x - y);
			return exec_trap::none; }                          // FSUB
		case 053: { cycles = CYCLES_FMPY;
			const double y = pop_double(), x = pop_double();
			push_double(x * y); set_fp_cc(x * y);
			return exec_trap::none; }                          // FMPY
		case 054: { cycles = CYCLES_FMPY;   // undocumented; nearest measured
			const double y = pop_double(), x = pop_double();
			push_double(x / y); set_fp_cc(x / y);
			return exec_trap::none; }                          // FDIV
		case 055: { const double x = pop_double(); push_double(-x);
			set_fp_cc(-x); return exec_trap::none; }           // FNEG
		case 070: { const s32 v = s32(pop()); push(from_float(float(v)));
			set_fp_cc(double(float(v)));
			return exec_trap::none; }                          // FLTS
		case 071: { push(fix(double(to_float(pop())), b3));    // FIXS
			cca(stk(0)); return exec_trap::none; }
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
		// The logical group (ERS sheet 76, items 61.1-61.3) works on 32-bit
		// POSITIVE integers: the limbs of multiple precision arithmetic.
		// "(S-1):=(S-1)+(S)+If CIR(19:1)=1 then Carry else 0" and
		// "(S-1):=(S-1)-(S)-If CIR(19:1)=1 then 1-Carry else 0" -- the b
		// bit chains the carry in, and the borrow travels inverted, Carry
		// set meaning no borrow, exactly as DLSB above has it. Neither
		// form took a carry in at all. HP-UX's decimal-to-binary
		// conversion -- the C compiler's for a floating constant, and
		// atof's -- builds its mantissa with LMPY / LADD / LADD,B, so
		// every carry between limbs was lost: `double x = 1.0;` printed
		// as 9.999993116074e-01, atof("0.1") came back exactly 2^22 units
		// low, and 20! as 2.432900333385e+18.
		//
		// The indicators are not CCA either -- an unsigned result has no
		// sign: "If result#0 then CCG; if result=0 and CIR(19:1)=0 then
		// CCE; if result=0 and CIR(19:1)=1 then [unchanged]", the same
		// chaining rule as the decimal pair. LMPY, which set none, reports
		// "If result#0 then CCG else CCE" on its doubleword product.
		case 060: {                                            // LADD
			const u32 a = pop(), b = pop();
			const u64 sum = u64(b) + u64(a) + (b3 && r.test(sf::C) ? 1 : 0);
			r.put(sf::C, (sum >> 32) & 1);
			push(u32(sum));
			if (u32(sum) != 0) r.put(sf::CC, CCG);
			else if (!b3)      r.put(sf::CC, CCE);
			return exec_trap::none;
		}
		case 061: {                                            // LSUB
			const u32 a = pop(), b = pop();
			const u64 diff = u64(b) - u64(a) - (b3 && !r.test(sf::C) ? 1 : 0);
			r.put(sf::C, ((diff >> 32) & 1) ^ 1);              // set = no borrow
			push(u32(diff));
			if (u32(diff) != 0) r.put(sf::CC, CCG);
			else if (!b3)       r.put(sf::CC, CCE);
			return exec_trap::none;
		}
		case 062: { cycles = CYCLES_MPY;                       // LMPY
			const u64 prod = u64(pop()) * u64(pop());
			push(u32(prod >> 32)); push(u32(prod));
			r.put(sf::CC, prod != 0 ? CCG : CCE);
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
	//----------------------------------------------------------------
	//  the decimal conversion group (ERS sheets 84-87)
	//----------------------------------------------------------------
	//
	// BASIC's numbers are three-word decimal floating point: a ten-bit
	// exponent in (S-2)(0:10) and seventeen decimal digits, which the
	// sheets describe as "a 17-digit integer", filling the rest of that
	// word and the two below it. BTD packs a binary double into that form,
	// DTB unpacks it, and POT supplies the powers of ten a caller needs to
	// scale between the two.
	//
	// The digits are BCD, and BASIC settles that: it runs DLAD and DLSB
	// straight down this mantissa, and a binary integer there raises an
	// Illegal Decimal trap on the first nibble above nine.
	//
	// The scanned field list "(S-2)(15:17)" is damaged. It is
	// (S-2)(16:16) -- sixteen bits, four BCD digits -- giving a twenty
	// digit mantissa: four in (S-2) and eight in each of (S-1) and (S),
	// most significant first, least significant in the low nibble of (S),
	// the order bcd_add and bcd_sub already use. One digit sits before
	// the point and nineteen after.
	//
	// BASIC settles that too, and to the digit: packing seventeen digits
	// instead of twenty printed 0.004 for 8/2 and 0.002 for SQR(4) --
	// right digits, point three places out, which is the three digits the
	// shorter field left off.

	// Split a seventeen digit value into the twenty BCD digits of a
	// packed mantissa: the value scaled by a thousand, as four top digits
	// and sixteen below. Done without ever forming the twenty digit
	// integer, which would overflow sixty-four bits.
	static u64 bcd20_pack(u64 m17, u32 &top4)
	{
		u64 t = m17 / 10000000000000ull;              // the top four digits
		u32 hi = 0;
		for (unsigned d = 0; d < 4; d++) { hi |= u32(t % 10) << (4 * d); t /= 10; }
		top4 = hi;

		u64 v = (m17 % 10000000000000ull) * 1000ull;  // the sixteen below
		u64 lo = 0;
		for (unsigned d = 0; d < 16; d++) { lo |= (v % 10) << (4 * d); v /= 10; }
		return lo;
	}

	// The same twenty digits back as a value. Accumulated in a double
	// because twenty digits do not fit an integer register.
	static double bcd20_unpack(u32 top4, u64 lo)
	{
		double v = 0;
		for (int d = 3; d >= 0; d--) v = v * 10 + double((top4 >> (4 * d)) & 0xf);
		for (int d = 15; d >= 0; d--) v = v * 10 + double((lo >> (4 * d)) & 0xf);
		return v;
	}

	// Sign-extend the ten-bit exponent field, whose range the sheets give
	// as -512..511.
	static s32 exp10_field(u32 w)
	{
		const s32 v = s32(ers32::get(w, 0, 10));
		return (v & 0x200) ? v - 0x400 : v;
	}

	// A and B with A/B = 10^k, pushed as two doubles. The pair exists so a
	// caller can scale without an intermediate overflow, which is why the
	// sheets hand over a ratio rather than 10^k whole. Splitting at 22 --
	// the largest power of ten a double holds exactly -- keeps both halves
	// exact for |k| <= 44 and both well inside range for the whole legal
	// -308 < k < 308.
	void push_power_pair(s32 k)
	{
		const s32 a = (k >= 0) ? std::min(k, 22) : std::max(k, -22);
		const s32 b = a - k;
		push_double(std::pow(10.0, double(a)));
		push_double(std::pow(10.0, double(b)));
	}

	exec_trap exec_fmt4(u8 op3, bool b3)
	{
		// The privileged absolute-address group: the operand is an absolute
		// address rather than a segment-relative one, optionally indexed
		// (ERS sheets 60, 65).
		const u32 xx = b3 ? r.x : 0;

		switch (op3)
		{
		case 000: {                                             // DTB
			if (b3)
			{
				// Decimal to binary, b form (sheet 84): the exponent word
				// of a decimal float becomes the scaling pair for the
				// conversion, offset by the digits the mantissa carries
				// behind its point.
				//
				// The sheet says sixteen. It is nineteen, for the same
				// reason the mantissa is twenty digits and not seventeen
				// -- the two have to agree or the scaling misses by the
				// difference, and BASIC shows it: with sixteen here, the
				// literal in DISP 4.5+1 comes back a thousand times too
				// large and the POT two instructions later rejects its
				// exponent.
				const s32 e = exp10_field(stk(0));
				if (!(e > -308 && e < 308))
					return take_trap(trap_cause::exponent_size, m_cir);
				pop();
				push_power_pair(e - 19);
				r.put(sf::CC, CCG);
				return exec_trap::none;
			}
			// Plain DTB: the three-word decimal float in (S-2,S-1,S) has
			// its exponent discarded and its digits, "treated as a
			// 17-digit integer", converted to a double left in (S-1,S).
			double v = bcd20_unpack(ers32::get(stk(2), 16, 16),
					(u64(stk(1)) << 32) | stk(0));
			if (ers32::get(stk(2), 10, 1)) v = -v;
			r.s -= 12;
			push_double(v);
			set_fp_cc(v);
			return exec_trap::none;
		}
		case 001: {                                             // BTD
			if (b3)
			{
				// Binary to decimal, b form (sheet 86): estimate I so that
				// N/10^I lands in [1,10). The sheet specifies a fast
				// approximation that is wrong by +1 for about 4% of
				// values, and says the plain form corrects it. An exact
				// floor(log10) is computed instead -- the caller's loop
				// adjusts in both directions, so being right first time
				// only makes it converge on the first pass.
				// An infinity or a NaN has no decimal exponent, and
				// converting its log to an integer is undefined in C++;
				// it is the FP Operand Trap's case on the hardware (cause
				// 34), which is not modelled, so it gets 0 -- the plain
				// form then reports the mantissa out of range and the
				// caller's rescaling ends in POT's Exponent Size trap.
				const double n = pop_double();
				const s32 i = (n == 0.0 || !std::isfinite(n)) ? 0
						: s32(std::floor(std::log10(std::fabs(n))));
				push(u32(i));
				cca(u32(i));
				return exec_trap::none;
			}
			// Plain BTD: pack N into the decimal float, using I as its
			// exponent. Out-of-range magnitudes are not an error -- the
			// stack is trimmed to the corrected exponent and the caller
			// rescales and comes back.
			const s32 i = s32(stk(2));
			const double n = to_double((u64(stk(1)) << 32) | stk(0));
			if (n == 0.0)
			{
				set_stk(2, 0); set_stk(1, 0); set_stk(0, 0);
				r.put(sf::CC, CCE);
				return exec_trap::none;
			}
			const double a = std::fabs(n);
			const double ip = std::floor(a);
			if (ip < 1.0)
			{
				r.s -= 8; set_stk(0, u32(i) - 1u);
				r.put(sf::CC, CCL);
				return exec_trap::none;
			}
			if (ip > 9.0)
			{
				r.s -= 8; set_stk(0, u32(i) + 1u);
				r.put(sf::CC, CCG);
				return exec_trap::none;
			}
			if (!(i > -512 && i < 511))
				return take_trap(trap_cause::exponent_size, m_cir);
			// Seventeen digits: one before the point and sixteen after,
			// which is what the exponent's -16 offset in DTB pays for.
			u32 top4 = 0;
			const u64 lo = bcd20_pack(u64(std::llround(a * 1e16)), top4);
			u32 w = ers32::set(0, 0, 10, ers32::get(u32(i), 22, 10));
			if (n < 0) w = ers32::set(w, 10, 1, 1);
			w = ers32::set(w, 16, 16, top4);
			set_stk(2, w);
			set_stk(1, u32(lo >> 32));
			set_stk(0, u32(lo));
			r.put(sf::CC, CCE);
			return exec_trap::none;
		}
		case 002: {                                             // LBIT
			// The top of stack is a bit offset: bits 0-28 give the byte
			// index, which goes to X, and bits 29-31 the bit within that
			// byte, counted from the most significant (ERS sheet 61).
			//
			// The operands stay on the stack until the pointer has been
			// evaluated. Every trap the evaluation can raise -- absent
			// page, absent segment, absent page table -- is a note-2 trap
			// that returns to THIS instruction, and its marker is pushed
			// at S: popped first, the two operand words lay under the
			// marker, and the re-executed LBIT read its bit offset and
			// pointer out of whatever was beneath them and left its
			// caller's stack two words short. The class of defect HPUX.md
			// 10.3 records for MVB.
			const u32 spec = stk(0);
			const u32 bitno = spec & 7;
			r.x = u32(s32(spec) >> 3);
			const u32 ptr = stk(1);
			const ea_result e = m_au.resolve_data_pointer(ptr, true, false);
			m_addr_trap = e.t;
			if (!e.ok()) return exec_trap::address_error;
			r.s -= 8;
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
			// Popped only once the pointer is good; see LBIT.
			const ea_result e = m_au.resolve_data_pointer(ptr, true, true);
			m_addr_trap = e.t;
			if (!e.ok()) return exec_trap::address_error;
			r.s -= 12;
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
			// The interesting moment is often thousands of events in
			// (the console open is past 400), so FOCUS_SEMALLMAX raises
			// the cap and FOCUS_SEMALLSKIP drops the boot's own traffic
			// off the front.
			static knob::channel sem_all("FOCUS_SEMALL", 400,
					"FOCUS_SEMALLMAX", "FOCUS_SEMALLSKIP");
			if (sem_all)
				sem_all("[%u %s%s ptr=%08x -> %08x count=%d p=%08x]\n",
						sem_all.seq(),
						op3 == 021 ? "DOWN" : "UP", b3 ? ",B" : "",
						ptr, a, s32(m_mem.read32(a + 4)), r.p);
			const u32 head = m_mem.read32(a);
			if (head == 0xffffffffu)
			{
				// Another processor holds the word; spin by retrying
				// the instruction. Single-CPU, this only happens if
				// the OS itself left it locked.
				branch_to(r.p);
				return exec_trap::none;
			}
			// Both forms consume their operand. The ",B" flag chooses how
			// the operand is read, not whether it is popped: no site in
			// HP-UX 5.30 deletes anything after a plain UP or DOWN, and
			// leaving the pointer behind was a stack leak of one word per
			// semaphore operation -- invisible in a frame that soon
			// EXITs, but the IOS task that reads the inode table at mount
			// does a DOWN/UP pair per block in one frame, and after 754
			// blocks its stack had grown by 6.9 KB and the next procedure
			// entry's stack probe overflowed the task's stack by a word:
			// "System halted due to system error. TRAP:00000024".
			pop();
			const u32 ca = a + 4;
			const s32 count = s32(m_mem.read32(ca));
			const u32 ctcbp = m_mem.read32(ded_base() + 4);
			if (op3 == 021)
			{
				m_mem.write32(ca, u32(count - 1));
				if (count - 1 >= 0)
					return exec_trap::none;

				// Block. HP-UX's own software semaphore routines (CORE
				// SYSTEM +0x5060.., the "signal one, wait on another" family
				// at file 0x13c18/0x13d88/0x13ea4/0x13f20) work on the SAME
				// semaphores these instructions do, so the microcode has to
				// build exactly the list they build and take apart:
				//
				//   - the wait list is CIRCULAR and doubly linked through
				//     the node at CTCBP-8 ({forward, backward}); the first
				//     waiter is linked to itself (LINS into an empty list);
				//   - it is PRIORITY ORDERED on the word at CTCBP-12 (node-4),
				//     smallest first and first-come-first-served among
				//     equals: `LLSH 3` from the head, limited to the number
				//     of waiters already there, advances while target <=
				//     own, and `LINS` puts the task in front of where it
				//     stopped;
				//   - the head moves to the new node only when the search
				//     did not advance at all;
				//   - the state word at CTCBP-16 takes 2 in its low halfword
				//     and CTCBP+104 (node+112) records the semaphore operand.
				//
				// The first model here pushed a nil-terminated node on the
				// head, LIFO, with the head's backward link aimed at the
				// count word. Alone that is self-consistent; mixed with the
				// kernel's routines it is not. An UP over a list the kernel
				// built found the sole waiter's forward link pointing at
				// itself and left the head on a task that was no longer
				// waiting; the next DOWN chained in behind that stale node,
				// the UP after it readied a task that was already running,
				// and the real waiter was never woken. That is the
				// multiuser stall: the file system's lock at 0x1fe5f4 ends
				// with count 0, a head naming a running task, and the task
				// that asked for it parked for ever.
				const u32 link = ctcbp - 8;
				const s32 ahead = -(count - 1) - 1;     // waiters already queued
				const u32 sw = ctcbp - 16;
				m_mem.write32(sw,
						(m_mem.read32(sw) & 0xffff0000u) | 2);
				const s32 prio = s32(m_mem.read32(ctcbp - 12));
				u32 pos = head;
				s32 left = ahead;
				while (left > 0 && pos != 0
						&& s32(m_mem.read32(pos - 4)) <= prio)
				{
					pos = m_mem.read32(pos);
					left--;
				}
				if (pos == 0)
				{
					// LINS into an empty list: circular on itself.
					m_mem.write32(link, link);
					m_mem.write32(link + 4, link + 4);
				}
				else
				{
					const u32 bkw = m_mem.read32(pos + 4);
					m_mem.write32(link, pos);
					m_mem.write32(pos + 4, link + 4);
					if (bkw != 0)
						m_mem.write32(bkw - 4, link);
					m_mem.write32(link + 4, bkw);
				}
				m_mem.write32(a, (left == ahead) ? link : head);
				m_mem.write32(ctcbp + 104, ptr);
				if (sem_hook) sem_hook(true, a, ctcbp, r.p);
				return do_disp();
			}
			else
			{
				m_mem.write32(ca, u32(count + 1));
				if (count + 1 <= 0)
				{
					// Wake the first waiter, the way the kernel's routine
					// does: when the count reaches zero it was the only one
					// and the head simply becomes nil (its links are left
					// as they were); otherwise LDEL takes it out of the
					// ring and its forward neighbour becomes the head. Its
					// state word goes back to 1 and the doorbell bit the
					// dispatcher's idle mask always leaves enabled is rung,
					// so a STOPped processor comes back to find it.
					const u32 l = head;
					if (l != 0)
					{
						u32 new_head = 0;
						if (count + 1 < 0)
						{
							const u32 fwd = m_mem.read32(l);
							const u32 bkw = m_mem.read32(l + 4);
							if (bkw != 0)
								m_mem.write32(bkw - 4, fwd);
							if (fwd != 0)
								m_mem.write32(fwd + 4, bkw);
							new_head = fwd;
						}
						m_mem.write32(a, new_head);
						const u32 sw = l - 8;
						m_mem.write32(sw,
								(m_mem.read32(sw) & 0xffff0000u) | 1);
						r.msg |= 0x00200000u;
						m_halted = false;
						if (sem_hook) sem_hook(false, a, l - 0x10, r.p);
						// The kernel's own routines go one step further
						// and end by comparing the priority of the task
						// they readied with the caller's (`PLDA CTCBP-12;
						// LDS woken; CMP,B; BCCS.EL; DISP`), entering the
						// dispatcher when the waiter outranks it. The
						// instruction does NOT: the code around HP-UX's
						// UP sites is written for a caller that keeps the
						// processor (`LDS Q-16; UP; EXIT`, three UPs in a
						// row at CORE SYSTEM +0x4698), and with the
						// dispatch added 3 of 10 multiuser boots stopped
						// across FOCUS_IOPPOLL 300..700 -- one traced to a
						// shell parked in a console write that never
						// completed -- against 0 of 10 without it.
						// FOCUS_SEMPREEMPT turns the dispatch on, as the
						// A/B control for that measurement.
						static const knob::flag preempt("FOCUS_SEMPREEMPT");
						if (preempt
								&& s32(m_mem.read32(ctcbp - 12))
									> s32(m_mem.read32(l - 4)))
						{
							// Indicators unaffected: a deferred DISP
							// reports through CC, and that is DISP's
							// business, not this instruction's.
							const u32 cc = r.get(sf::CC);
							const u32 qi = m_mem.read32(ded_base() + 8);
							const bool deferred = m_ics_flag
									|| m_mem.read32(qi - 3 - 16) != 0;
							const exec_trap t = do_disp();
							if (deferred)
								r.put(sf::CC, cc);
							return t;
						}
					}
				}
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
			// "(S)(0:16) := (S)(16:1) ... right justified and sign
			// extended" (ERS 12.2), unlike PLBA's zero fill.
			const u32 v = u32(s32(s16(m_mem.read16(stk(0) + xx))));
			set_stk(0, v); cca(v);
			return exec_trap::none;
		}
		case 052: {                                             // PSHA
			if (!r.privileged()) return exec_trap::unimplemented;
			const u32 v = pop(), a = pop();
			m_mem.write16(a + xx, u16(v));
			return exec_trap::none;
		}
		case 053: {                                             // POT
			// Power of Ten (sheet 87): an integer becomes the pair A and
			// B with A/B = 10^I, which is how the decimal conversions get
			// their scaling without risking an overflow on the way.
			const s32 i = s32(stk(0));
			if (!(i > -308 && i < 308))
				return take_trap(trap_cause::exponent_size, m_cir);
			pop();
			push_power_pair(i);
			r.put(sf::CC, CCG);
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
			// No condition code: ERS sheet 118 says Indicators Unaffected.
			set_stk(0, e.addr);
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
			// Branch Stack Indirect (ERS item 108.1): a faster BR S-0,I
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
		case 011:                                               // INT
			// "This is a privileged instruction" (sheet 119), and has to
			// be: with it any user program could hold off every
			// interrupt for as long as it liked. Cause 026, parameter 1:
			// a privileged instruction in unprivileged mode.
			if (!r.privileged())
				return take_trap(trap_cause::privileged_mode, 1);
			r.put(sf::I, b3 ? 1 : 0);
			return exec_trap::none;

		case 040: {                                             // TRY
			// Creates or deletes a pseudo stack marker for the high level
			// language TRY feature (ERS sheet 126). The frame has the same
			// four-word shape as a real stack marker, but its first slot
			// chains to the previous frame instead of holding X. The chain
			// head lives at (SB+2) as an SB-relative byte offset.
			static knob::channel try_log("FOCUS_TRYLOG", 20000);
			if (try_log)
				try_log("[TRY%s p=%08x sb=%08x s=%08x q=%08x head=%08x]\n",
						b3 ? ",B" : "", r.p, r.sb, r.s, r.q,
						m_mem.read32(r.sb + 8));
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
			// words below the frame's top. (The ERS's own bound is
			// Q+4 < TEMP < S -- strictly below S -- which would refuse
			// even the boot frame the statement tail eats; see the
			// statement-lifecycle notes before tightening this.)
			// FOCUS_TRYNOP is an experiment, not an emulation claim: an
			// empty chain makes the delete a no-op instead of a fault,
			// to learn whether BASIC's statement loop is sustainable
			// with the tail delete simply absorbed.
			static const knob::flag try_nop("FOCUS_TRYNOP");
			if (try_nop && sb_word(2) == 0)
			{
				set_sb_word(4, 0);
				return exec_trap::none;
			}
			const u32 temp = r.sb + sb_word(2);
			if (!(r.q + 16 <= temp && temp <= r.sl))
				return exec_trap::bounds_violation;
			set_sb_word(2, word_below(temp, 3));
			r.s = temp - 16;
			set_sb_word(4, 0);
			return exec_trap::none;
		}

		case 041: {                                             // ESCP
			// The same switch as TRY, on its own budget: ESCP is the
			// other end of the frame chain and the two interleave.
			// Sharing the name with the channel in TRY is deliberate --
			// one switch turns both ends on -- and so are the two
			// separate budgets, which keep a flood of one end from
			// spending the other's. Not a duplicate to be merged.
			static knob::channel escp_log("FOCUS_TRYLOG", 4000);
			if (escp_log)
				escp_log("[ESCP%s p=%08x sb=%08x head=%08x val=%08x]\n",
						b3 ? ",B" : "", r.p, r.sb, m_mem.read32(r.sb + 8),
						b3 ? sb_word(3) : stk(0));
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
				u32 out;
				const exec_trap t = index_external_pointer(ptr, r.x, out);
				if (t != exec_trap::none)
					return t;
				set_stk(0, out);
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

		case 071: {                                             // VPTR
			// Validate Pointer (ERS 155.1): a data segment pointer on
			// the TOS, the mode bits wanted of it (WRITE in bit 30, PRIV
			// in bit 31) beneath, and an SB-relative byte offset beneath
			// that. The answer is the condition code -- L invalid, E
			// absent, G good -- and the pointer alone is popped. HP-UX's
			// process loader validates every segment it is handed with
			// this; executing it as an unimplemented instruction made
			// the load of /etc/sslibs/sslib fail, and init with it.
			const u32 ptr = stk(0), mode = stk(1), soff = stk(2);
			const bool want_priv = ers32::get(mode, 31, 1) != 0;
			const bool want_write = ers32::get(mode, 30, 1) != 0;
			u32 cc = CCG;
			if (want_priv && !r.privileged())
				cc = CCL;
			else if (ers32::get(ptr, 0, 2) == 2)
			{
				s32 off = s32(ers32::get(ptr, 2, 30));
				if (b3) off += s32(r.x);
				if (off < 0 || u32(off) > r.dl - r.db)
					cc = CCL;
			}
			else if (ers32::get(ptr, 0, 2) == 3)
			{
				s32 off = s32(ers32::get(ptr, 2, 30));
				if (b3) off += s32(r.x);
				if (off < 0 || u32(off) > soff)
					cc = CCL;
			}
			else if (ers32::get(ptr, 0, 1) == 0)
			{
				const bool system = ers32::get(ptr, 1, 1) == 0;
				const u32 seg = ers32::get(ptr, 2, 11);
				u32 off = ers32::get(ptr, 13, 19);
				if (b3) off += r.x;
				const int v = m_au.validate(system, seg, off, want_write, want_priv);
				cc = v < 0 ? CCL : v == 0 ? CCE : CCG;
			}
			r.put(sf::CC, cc);
			pop();
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
				u32 out;
				const exec_trap t = index_external_pointer(ptr, delta, out);
				if (t != exec_trap::none)
					return t;
				r.s -= 4;
				set_stk(0, out);
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
			// "Absent Data Segment. The absence bit in the DST entry for
			// the referenced segment is 1. The DST entry number is passed
			// as a parameter. If this trap occurs on the ICS, the
			// parameter is negated" (ERS 5.2.2, trap 042) -- and XGDS
			// references its segment like anything else. HP-UX depends on
			// it: a process inside a system call runs on the kernel's
			// global segment, its own has a use count of zero for as long
			// as that lasts, and the memory manager is free to swap it
			// out. The system-call exit stub (HP-UX ENTRYPTS +0xce) then
			// executes XGDS on a DST entry marked absent, with a location
			// word of zero, and counts on the trap to bring the segment
			// back and re-execute the instruction. Loading DB from the
			// entry regardless sent the process back to user mode with
			// DB = 0: two logins a second apart were enough memory
			// pressure for both shells to die of "Unexpected memory
			// fault" (trap 030 on the first DB-relative pointer), and
			// the dispatcher then restored one of them from a stack
			// segment that was absent too and halted the system on a
			// stack underflow. Nothing has been changed when the trap is
			// taken, so the handler's return runs the XGDS again.
			if (ers32::get(m_mem.read32(ea), 31, 1))
			{
				u32 param = ers32::get(ptr, 1, 12);
				if (m_ics_flag)
					param = u32(-s32(param));
				return take_trap(trap_cause::absent_data_segment, param);
			}
			r.db = m_mem.read32(ea + 8);
			r.dl = r.db + m_mem.read32(ea + 4);
			const u32 ctcbp = m_mem.read32(ded_base() + 4);
			const u32 w = m_mem.read32(ctcbp + 8);
			const u32 old = ers32::get(w, 20, 12);
			m_mem.write32(ctcbp + 8,
					ers32::set(w, 20, 12, ers32::get(ptr, 1, 12)));
			set_stk(0, ers32::set(ptr, 1, 12, old));
			// "Increment Use Count for New Segment ... Decrement Use Count
			// for Old Segment" (sheet 68). The count is the DST entry's
			// bits 1-12, the number of tasks using the segment, and the
			// memory manager's relocator refuses to move any segment
			// whose count is not zero. HP-UX's dispatcher pairs its own
			// MUC increment on the task it switches to with the
			// decrement XGDS owes the segment it switches from; without
			// this half a process's global data segment gained a count
			// on every dispatch and never lost one, so once init had
			// been scheduled a few dozen times its data segment was
			// pinned in the middle of the heap, the compactor could not
			// gather the free space below it, the paging pool could not
			// take its first 32 KB, and every page-in of /etc/sslibs/
			// sslib failed with 270 -- "Cannot load /etc/sslibs/sslib;
			// errno=14". The old segment's number is the TCB field as
			// it stood before the exchange, bit 20 selecting the table.
			dst_use_count(ea, +1);
			// DB and DL now address the new segment, and what the task
			// stores DB-relative from here on never touches its entry
			// again: this is where it becomes Referenced and Dirty (see
			// dst_mark_loaded). HP-UX's system-call exit is an XGDS back
			// onto the user's global segment.
			dst_mark_loaded(ea);
			{
				const u32 old_table = ers32::get(w, 20, 1)
						? r.dst : dst_walker::SYSTEM_DST_BASE;
				const u32 old_seg = ers32::get(w, 21, 11);
				if (old_seg != 0)
					dst_use_count(old_table + 16 * old_seg, -1);
			}
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
			// Pseudo Interrupt Enable (ERS sheets 99-100):
			//
			//   If (QI-4) = 0 then System Error Trap
			//   (QI-4) := (QI-4) - 1
			//   If (QI-4) = 0 then
			//     If (QI)(0:1) = 1 then
			//       If DISP FLAG = 1 then (QI) := 0; abort dispatcher
			//                             & restart
			//       else Execute DISP Instruction
			//
			// "If it now equals zero, all pseudo-disables have been
			// removed, and it is now permissible to dispatch a new
			// process. If (QI)(0:1)=1 (it may have been set by executing
			// a DISP instruction while on the ICS or while
			// pseudo-disabled) then the dispatcher is aborted and
			// restarted if DISP FLAG=1 or the DISP instruction [is
			// executed]."
			//
			// Only the decrement used to be modelled. HP-UX's dispatcher
			// depends on the rest: when the task it has picked turns out
			// to have its stack or global segment swapped out (`MUC,B`
			// answers CCE), it queues the segment for the memory manager
			// and executes `DISP; PSEB` (TRAP HANDLERS +0x1d6) -- a DISP
			// that can only be deferred, followed by the PSEB that is to
			// restart the dispatcher. Falling through instead ran the
			// rest of the dispatch on a frame the absent path had already
			// zeroed, reached the dispatcher's own closing PSEB with the
			// count at zero, and halted: "System halted due to system
			// error. TRAP:00000012". It takes memory pressure to get
			// there -- two logins a second apart were enough.
			if (!r.privileged()) return exec_trap::unimplemented;
			const u32 qi = m_mem.read32(ded_base() + 8);
			const u32 a = qi - 3 - 16;
			const u32 n = m_mem.read32(a);
			// "If (QI-4) = 0 then System Error Trap" -- cause 014 with
			// parameter 0, "Too many PSEB instructions executed" (ERS
			// 5.2.2), not Inconsistent Registers, which is IXIT's and
			// SETR's.
			if (n == 0)
				return take_trap(trap_cause::system_error, 0);
			m_mem.write32(a, n - 1);
			// CCL says "no dispatch happened"; a deferred DISP that is
			// carried out below reports through the dispatcher instead.
			r.put(sf::CC, CCL);
			if (n - 1 == 0 && ers32::get(m_mem.read32(qi - 3), 0, 1))
			{
				// DISP FLAG is the test, exactly as the pseudocode has it:
				// set, the dispatcher itself is running, and it is
				// abandoned and started again from its frame at QI --
				// wherever its own Q has got to, which is why Q, SB and
				// SL are all put back to the ICS's. Clear, this is a
				// task or a service routine, and DISP does what DISP
				// does: enter the dispatcher from a task, or (on the ICS)
				// leave the request in (QI) for the IXIT to honour.
				if (m_disp_flag)
				{
					r.q = qi;
					r.sb = (qi & ~3u) - 256;
					r.sl = m_mem.read32(ded_base() + 12);
					return launch_ics_frame();       // clears (QI)
				}
				return do_disp();
			}
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
			//
			// "This is a privileged instruction" (sheet 94). Unchecked,
			// the two bytes 10 7F in any user program loaded MASK from
			// whatever was on its stack and stopped the machine for
			// everybody.
			if (!r.privileged())
				return take_trap(trap_cause::privileged_mode, 1);
			if (b3)
			{
				r.mask = pop();
				// "Wait for any unmasked Message bit" tests the register,
				// not an edge: with an unmasked bit already standing the
				// machine does not sleep at all. raise_message() only
				// un-halts a processor that is halted, so a bit that
				// arrived between the dispatcher's last look at MSG and
				// its STOP -- HP-UX's idle sequence is SMPB 2 (consume
				// the doorbell), PMPB 1, LDI, NOT, AND, STOP,B, and an
				// interrupt routine's UP can ring bit 10 anywhere in
				// those five instructions -- left the CPU asleep with a
				// ready task until some unrelated message came along.
				//
				// Not modelled: restoring MASK and STATUS when the wait
				// ends ("MASK:=TEMP; STATUS:=OLDSTAT"). Both systems'
				// idle loops are validated with the mask left as loaded.
				if ((r.msg & ~r.mask) != 0)
					return exec_trap::none;
			}
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

		// LDSI/SDSI: a doubleword whose first word is the last of its
		// page has its second in another page, resolved for itself before
		// anything is popped or written (see ea_double).
		u32 second = e.addr + 4;
		if (width == 8 && e.run_fwd < 8)
		{
			const ea_result e2 = m_au.resolve_data_pointer(ptr, indexed,
					store, 4);
			m_addr_trap = e2.t;
			if (!e2.ok())
				return exec_trap::address_error;
			second = e2.addr;
		}

		if (!store)
		{
			pop();
			switch (width)
			{
			case 1: push(m_mem.read8(e.addr)); break;
			// LHSI: "(S+1)(0:16) := (S+1)(16:1)" -- right justified AND
			// sign extended (ERS 4.5, sheet 59), as LDH is; it used to
			// zero extend and set CCG for a negative halfword. HP's C
			// compiler knows the difference: it loads an unsigned short
			// as `LHSI; EXF 16,16` (HP-UX user segments 86, 89, 91) and
			// a signed one as a bare LHSI.
			case 2: push(u32(s32(s16(m_mem.read16(e.addr))))); break;
			case 4: push(m_mem.read32(e.addr)); break;
			case 8: push(m_mem.read32(e.addr));
				push(m_mem.read32(second)); break;
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
			m_mem.write32(e.addr, high); m_mem.write32(second, low); } break;
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

