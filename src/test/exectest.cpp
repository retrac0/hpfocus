// license:BSD-3-Clause
// Tests for the FOCUS execution core (src/cpu/focus/focusexec.h).
//
// The end-to-end case is the recursive factorial program from section 7 of
// the Instruction Set ERS (sheet 129) -- HP's own worked example, assembled
// here instruction for instruction from the published listing.

#include "../cpu/focus/focusexec.h"

#include <cstdio>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <map>
#include <vector>

using namespace focus;

static int g_fail = 0;
static int g_run = 0;

#define CHECK(cond) do { \
		g_run++; \
		if (!(cond)) { \
			g_fail++; \
			std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
		} \
	} while (0)

#define CHECK_EQ(got, want) do { \
		g_run++; \
		const std::int64_t g_ = (std::int64_t)(std::int32_t)(got); \
		const std::int64_t w_ = (std::int64_t)(std::int32_t)(want); \
		if (g_ != w_) { \
			g_fail++; \
			std::printf("  FAIL %s:%d  %s\n        got  %lld\n" \
					"        want %lld\n", __FILE__, __LINE__, #got, \
					(long long)g_, (long long)w_); \
		} \
	} while (0)


class sparse_memory : public memory_interface
{
public:
	u32 read32(u32 addr) override
	{
		auto it = m_cells.find(addr & ~3u);
		return (it == m_cells.end()) ? 0 : it->second;
	}
	void write32(u32 addr, u32 data) override { m_cells[addr & ~3u] = data; }

private:
	std::map<u32, u32> m_cells;
};


//**************************************************************************
//  A TINY ASSEMBLER
//**************************************************************************

// Encoders for the formats, ERS numbering (bit 0 = MSB).
static u16 f1(u8 op1, bool i, bool x, u8 base, u8 off)
{
	return u16((op1 << 13) | (i << 12) | (x << 11) | (base << 8) | off);
}
static u16 f3(u8 op3, bool b3 = false)
{
	return u16((b3 ? 0x1000 : 0) | op3);
}
static u16 f2(u8 op2, u8 operand, bool b3 = false)
{
	return u16((b3 ? 0x1000 : 0) | (op2 << 6) | operand);
}
static u16 f4(u8 op3, bool b3 = false)
{
	return u16((b3 ? 0x1000 : 0) | (1 << 6) | op3);
}
static u16 f5(u8 op2, u8 operand)
{
	return u16(0x2000 | (op2 << 8) | operand);
}
static u16 bccs(u8 ccf, bool back, u8 halfwords)
{
	return u16((6 << 13) | (ccf << 9) | (back ? 0x100 : 0) | halfwords);
}

// Format 6, 32 bits: 111 OP2(3:5) I(8) X(9) BASE(10:3) OFFSET(13:19)
static u32 u32_insn(u8 op2, u8 base, u32 off)
{
	return (7u << 29) | (u32(op2) << 24) | (u32(base) << 19) | off;
}

// Writes 16-bit instructions into 32-bit memory cells, big endian.
class assembler
{
public:
	assembler(sparse_memory &mem, u32 origin) : m_mem(mem), m_pc(origin) { }

	void emit(u16 insn)
	{
		const u32 cell = m_pc & ~3u;
		u32 v = m_mem.read32(cell);
		if (m_pc & 2)
			v = (v & 0xffff0000u) | insn;
		else
			v = (v & 0x0000ffffu) | (u32(insn) << 16);
		m_mem.write32(cell, v);
		m_pc += 2;
	}

	u32 pc() const { return m_pc; }

private:
	sparse_memory &m_mem;
	u32 m_pc;
};


//**************************************************************************
//  ERS SECTION 7 -- RECURSIVE FACTORIAL
//**************************************************************************

// From the listing on ERS sheet 129. Addresses are octal, as printed.
//
//   calling sequence, at 10000:
//     10000  ZERO          allocate space for the result
//     10002  LDS DB+10     load the actual parameter
//     10004  PCLS 2        call the factorial procedure
//     10006  STS DB+12     store the result
//
//   procedure, at 20000:
//     20000  LDS Q-4       load the passed parameter
//     20002  CMPI 0        test it for zero, popping the stack
//     20004  BCCS GL P+3   if not zero, branch to 20012
//     20006  LDIS 1        if zero, return 1
//     20010  BCCS GEL P+10 branch to 20030
//     20012  ZERO          allocate space for the intermediate result
//     20014  LDS Q-4       load the passed parameter
//     20016  SUBI 1        decrement it
//     20020  PCLS 2        recursive call
//     20022  LDS Q-4       load the passed parameter
//     20024  MPY           multiply by the previous top-of-stack
//     20026  DELB          delete the upper half of the product
//     20030  STS Q-5       store the intermediate product
//     20032  EXIT 1        exit this recursion
//
// The two BCCS offsets are the check that the branch encoding is right: the
// listing's comments name the targets, 20012 and 20030, and both are reached
// by an offset counted in halfwords from the branch instruction itself.
static void test_factorial()
{
	std::printf("ERS section 7 recursive factorial (sheet 129)\n");

	sparse_memory mem;
	cpu_core cpu(mem);

	constexpr u32 CALLER = 010000;
	constexpr u32 PROC   = 020000;
	constexpr u32 CODE_BASE = 04000;
	constexpr u32 CODE_END  = 030000 - 1;

	// One code segment holds both routines, so the STT entry can carry a
	// local program pointer and no Code Segment Table walk is needed.
	cpu.r.pb = CODE_BASE;
	cpu.r.pl = CODE_END & ~3u;
	cpu.r.p  = CALLER;

	// Stack. S and Q must end in 11.
	cpu.r.sb = 0100000;
	cpu.r.s  = 0100000 + 3;
	cpu.r.q  = cpu.r.s;
	cpu.r.sl = 0140000 - 1;

	// Global data segment.
	cpu.r.db = 0200000;
	cpu.r.dl = 0210000 - 1;

	// STT entry 2 sits at PL - 4*2 and holds a PB-relative pointer to the
	// procedure (ERS sheet 95: PCLS reads PL - 4*n). The word at PL itself
	// carries the table's length in (13:19), and PCL/PCLS refuse an offset
	// beyond it ("If Offset>(PL)(13:19) then STT Violation Trap", trap 21
	// condition 1). The harness never wrote that word; once the core made
	// the check (b8f48e9) every factorial trapped on its first PCLS.
	mem.write32(cpu.r.pl, 4 * 2);
	mem.write32(cpu.r.pl - 8, PROC - CODE_BASE);

	{
		assembler a(mem, CALLER);
		a.emit(f3(006));                        // ZERO
		a.emit(f1(2, false, false, 2, 8));      // LDS DB+8  (word offset)
		a.emit(f5(022, 2));                     // PCLS 2
		a.emit(f1(5, false, false, 2, 10));     // STS DB+10 (word offset)
		a.emit(f3(000));                        // NOP -- "rest of program"
	}
	{
		assembler a(mem, PROC);
		a.emit(f1(2, false, false, 5, 4));      // LDS Q-4  (four words)
		a.emit(f5(003, 0));                     // CMPI 0
		a.emit(bccs(5, false, 3));              // BCCS GL P+3   -> 20012
		a.emit(f5(014, 1));                     // LDIS 1
		a.emit(bccs(7, false, 010));            // BCCS GEL P+10 -> 20030
		a.emit(f3(006));                        // ZERO
		a.emit(f1(2, false, false, 5, 4));      // LDS Q-4  (four words)
		a.emit(f5(005, 1));                     // SUBI 1
		a.emit(f5(022, 2));                     // PCLS 2
		a.emit(f1(2, false, false, 5, 4));      // LDS Q-4  (four words)
		a.emit(f3(022, true));                  // MPY, doubleword result
		a.emit(f3(001));                        // DELB
		a.emit(f1(5, false, false, 5, 5));      // STS Q-5
		a.emit(f5(023, 1));                     // EXIT 1
	}

	// The listing's stated branch targets must fall out of the encoding.
	CHECK_EQ(020004 + 3 * 2, 020012);
	CHECK_EQ(020010 + 010 * 2, 020030);

	struct { s32 n; s32 want; } cases[] = {
		{0, 1}, {1, 1}, {2, 2}, {3, 6}, {5, 120}, {8, 40320}, {12, 479001600}
	};

	for (const auto &c : cases)
	{
		cpu.r.p = CALLER;
		cpu.r.s = cpu.r.sb + 3;
		cpu.r.q = cpu.r.s;
		cpu.r.status = 0;
		mem.write32(cpu.r.db + 32, u32(c.n));   // parameter,  word offset 8
		mem.write32(cpu.r.db + 40, 0);   // result slot, word offset 10

		unsigned steps = 0;
		exec_trap t = exec_trap::none;
		// Run until the NOP that stands in for "rest of program".
		for (steps = 0; steps < 20000; steps++)
		{
			if (cpu.r.p == CALLER + 8)
				break;
			t = cpu.step();
			if (t != exec_trap::none)
				break;
		}

		g_run++;
		if (t != exec_trap::none)
		{
			g_fail++;
			std::printf("  FAIL %d! trapped (%d) after %u steps at P=%06o\n",
					c.n, int(t), steps, cpu.r.p);
			continue;
		}
		const s32 got = s32(mem.read32(cpu.r.db + 40));
		if (got != c.want)
		{
			g_fail++;
			std::printf("  FAIL %d! got %d, want %d  (%u steps)\n",
					c.n, got, c.want, steps);
		}
	}

	// The stack must be exactly balanced: ZERO pushed a result slot, the
	// call and EXIT 1 consumed the marker and the parameter, and STS popped
	// the result.
	CHECK_EQ(cpu.r.s, cpu.r.sb + 3);

	// And the rule that made the length word necessary: with a one-entry
	// table, PCLS 2 is an STT Violation before anything is called.
	mem.write32(cpu.r.pl, 4 * 1);
	cpu.r.p = CALLER;
	cpu.r.s = cpu.r.sb + 3;
	cpu.r.q = cpu.r.s;
	cpu.r.status = 0;
	exec_trap t = exec_trap::none;
	for (unsigned steps = 0; steps < 3 && t == exec_trap::none; steps++)
		t = cpu.step();
	CHECK(t != exec_trap::none);
	CHECK_EQ(cpu.r.p, CALLER + 4);                // stopped at the PCLS
	mem.write32(cpu.r.pl, 4 * 2);
}


//**************************************************************************
//  INSTRUCTION GROUPS
//**************************************************************************

struct harness
{
	sparse_memory mem;
	cpu_core cpu{mem};

	harness()
	{
		cpu.r.pb = 0x1000; cpu.r.p = 0x1000; cpu.r.pl = 0x1ffc;
		cpu.r.sb = 0x4000; cpu.r.s = 0x4003; cpu.r.q = 0x4003;
		cpu.r.sl = 0x4ffc | 3;
		cpu.r.db = 0x8000; cpu.r.dl = 0x8ffc | 3;
	}

	void put(u32 addr, u16 insn)
	{
		const u32 cell = addr & ~3u;
		u32 v = mem.read32(cell);
		if (addr & 2) v = (v & 0xffff0000u) | insn;
		else          v = (v & 0x0000ffffu) | (u32(insn) << 16);
		mem.write32(cell, v);
	}

	exec_trap one(u16 insn)
	{
		cpu.r.p = 0x1000;
		put(0x1000, insn);
		return cpu.step();
	}

	exec_trap one(u32 insn)
	{
		cpu.r.p = 0x1000;
		mem.write32(0x1000, insn);
		return cpu.step();
	}
};

// The paged user data segment the restart tests share: user DST entry 1,
// writable, 1 KB pages, page 0 at 0x30000 and page 1 at 0x31000 -- NOT
// physically adjacent to page 0's frame, which is the point of several of
// them. Page 1 starts absent when asked.
static const u32 PG_DST = 0x20000, PG_PTAB = 0x21000;
static const u32 PG_FRAME0 = 0x30000, PG_FRAME1 = 0x31000;
static void paged_segment(harness &h, bool page1_present)
{
	h.cpu.r.dst = PG_DST;
	h.mem.write32(PG_DST, 16 * 4);
	u32 w0 = 0;
	w0 = ers32::set(w0, 22, 4, 10);
	w0 = ers32::set(w0, 26, 1, 1);
	w0 = ers32::set(w0, 27, 1, 1);
	h.mem.write32(PG_DST + 16, w0);
	h.mem.write32(PG_DST + 20, 0xfff);
	h.mem.write32(PG_DST + 24, PG_PTAB);
	h.mem.write32(PG_DST + 28, 0);
	const u32 present = ers32::set(0, 27, 1, 1);
	h.mem.write32(PG_PTAB + 0, present);
	h.mem.write32(PG_PTAB + 4, PG_FRAME0);
	h.mem.write32(PG_PTAB + 8, page1_present ? present
			: ers32::set(present, 31, 1, 1));
	h.mem.write32(PG_PTAB + 12, PG_FRAME1);
}
static void page1_present(harness &h, bool in)
{
	const u32 present = ers32::set(0, 27, 1, 1);
	h.mem.write32(PG_PTAB + 8, in ? present : ers32::set(present, 31, 1, 1));
}
// An EDS pointer into that segment, and the physical address of an offset.
static u32 pg_ptr(u32 off)
{
	u32 p = ers32::set(0, 1, 1, 1);
	p = ers32::set(p, 2, 11, 1);
	return ers32::set(p, 13, 19, off);
}
static u32 pg_phys(u32 off)
{
	return (off < 0x400 ? PG_FRAME0 : PG_FRAME1 - 0x400) + off;
}
// A handler segment (CST entry 1) whose absent-page vector is a bare EXIT
// 0, and the program running in segment 5 at 0x1000: a fault, the "pager"
// making the page present, and the handler's return, all through the real
// trap sequence -- marker, STATUS and X travel the way they do in HP-UX.
static const u32 RT_HB = 0x2000, RT_HLEN = 0x400;
static void restart_handler(harness &h)
{
	const u32 CST = 0x200;
	h.mem.write32(0, CST);
	h.mem.write32(CST, 16 * 8);
	h.mem.write32(CST + 16 + 4, RT_HLEN - 1);
	h.mem.write32(CST + 16 + 8, RT_HB);
	h.mem.write32(CST + 80 + 4, 0xfff);
	h.mem.write32(CST + 80 + 8, 0x1000);
	const u32 HPL = RT_HB + (RT_HLEN - 1) - 3;
	h.mem.write32(HPL - 4 * 040, 0x100);       // absent page -> HB+0x100
	h.put(RT_HB + 0x100, f5(023, 0));          // EXIT 0
	h.cpu.enable_traps(true);
	h.cpu.r.status = 0;
	h.cpu.r.put(sf::CSN, 5);
}

static void test_arithmetic()
{
	std::printf("arithmetic and logic\n");
	harness h;

	h.cpu.push(7); h.cpu.push(5);
	CHECK(h.one(f3(020)) == exec_trap::none);       // ADD
	CHECK_EQ(h.cpu.stk(0), 12);
	CHECK_EQ(h.cpu.r.condition_code(), CCG);

	h.cpu.push(3);
	CHECK(h.one(f3(021)) == exec_trap::none);       // SUB: 12 - 3
	CHECK_EQ(h.cpu.stk(0), 9);

	h.cpu.push(u32(-9));
	CHECK(h.one(f3(020)) == exec_trap::none);       // ADD -> 0
	CHECK_EQ(h.cpu.stk(0), 0);
	CHECK_EQ(h.cpu.r.condition_code(), CCE);

	h.cpu.pop();
	h.cpu.push(6); h.cpu.push(7);
	CHECK(h.one(f3(022)) == exec_trap::none);       // MPY single word
	CHECK_EQ(h.cpu.stk(0), 42);

	h.cpu.push(u32(-3));
	CHECK(h.one(f3(022)) == exec_trap::none);       // signed
	CHECK_EQ(h.cpu.stk(0), -126);
	CHECK_EQ(h.cpu.r.condition_code(), CCL);

	// MPY with bit 3 set keeps the doubleword: high at B, low at A.
	h.cpu.pop();
	h.cpu.push(0x10000); h.cpu.push(0x10000);
	CHECK(h.one(f3(022, true)) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(0), 0);                      // low half
	CHECK_EQ(h.cpu.stk(1), 1);                      // high half
	CHECK(h.one(f3(001)) == exec_trap::none);       // DELB drops the high half
	CHECK_EQ(h.cpu.stk(0), 0);

	// DIV takes a DOUBLEWORD dividend in (S-2,S-1) and a singleword
	// divisor in (S); the doubleword quotient replaces the dividend and
	// the remainder replaces the divisor (ERS sheet 59) -- the stack
	// shape is unchanged.
	h.cpu.pop();
	h.cpu.push(0); h.cpu.push(100); h.cpu.push(7);
	CHECK(h.one(f3(023)) == exec_trap::none);       // DIV
	CHECK_EQ(h.cpu.stk(0), 2);                      // remainder
	CHECK_EQ(h.cpu.stk(1), 14);                     // quotient low
	CHECK_EQ(h.cpu.stk(2), 0);                      // quotient high

	h.cpu.pop(); h.cpu.push(0);
	CHECK(h.one(f3(023)) == exec_trap::divide_by_zero);
	h.cpu.pop(); h.cpu.pop();

	h.cpu.pop(); h.cpu.pop();
	h.cpu.push(0xf0f0); h.cpu.push(0x0ff0);
	CHECK(h.one(f3(067)) == exec_trap::none);       // AND
	CHECK_EQ(h.cpu.stk(0), 0x00f0);
	h.cpu.push(0xf00f);
	CHECK(h.one(f3(065)) == exec_trap::none);       // OR
	CHECK_EQ(h.cpu.stk(0), 0xf0ff);
	CHECK(h.one(f3(064)) == exec_trap::none);       // NOT
	CHECK_EQ(h.cpu.stk(0), s32(~0xf0ffu));
}

// The divisions a guest can hand the core that the host cannot do: -2^63 by
// -1 in DIV and -2^31 by -1 in DIVS overflow the quotient, and in C++ both
// are undefined -- x86-64 raises a divide error, which ended the emulator
// with SIGFPE. Each must set Overflow and carry on.
static void test_divide_extremes()
{
	std::printf("division overflow\n");
	harness h;
	const u32 base = h.cpu.r.s;

	h.cpu.push(0x80000000u); h.cpu.push(0); h.cpu.push(u32(-1));
	CHECK(h.one(f3(023)) == exec_trap::none);       // DIV
	CHECK_EQ(h.cpu.r.s, base + 12);                 // shape unchanged
	CHECK(h.cpu.r.test(sf::O));
	CHECK_EQ(h.cpu.stk(2), 0x80000000u);            // quotient = dividend
	CHECK_EQ(h.cpu.stk(1), 0);
	CHECK_EQ(h.cpu.stk(0), 0);                      // remainder
	CHECK_EQ(h.cpu.r.condition_code(), CCL);

	h.cpu.r.s = base; h.cpu.r.put(sf::O, 0);
	h.cpu.push(0x80000000u); h.cpu.push(0); h.cpu.push(u32(-1));
	CHECK(h.one(f3(023, true)) == exec_trap::none); // DIV,B
	CHECK(h.cpu.r.test(sf::O));

	h.cpu.r.s = base; h.cpu.r.put(sf::O, 0);
	h.cpu.push(0x80000000u); h.cpu.push(u32(-1));
	CHECK(h.one(f3(033)) == exec_trap::none);       // DIVS
	CHECK(h.cpu.r.test(sf::O));
	CHECK_EQ(h.cpu.stk(1), 0x80000000u);
	CHECK_EQ(h.cpu.stk(0), 0);

	// One step inside the range still divides.
	h.cpu.r.s = base; h.cpu.r.put(sf::O, 0);
	h.cpu.push(0x80000000u); h.cpu.push(1); h.cpu.push(u32(-1));
	CHECK(h.one(f3(023)) == exec_trap::none);
	CHECK(!h.cpu.r.test(sf::O));
	CHECK_EQ(h.cpu.stk(2), 0x7fffffffu);
	CHECK_EQ(h.cpu.stk(1), 0xffffffffu);
	CHECK_EQ(h.cpu.stk(0), 0);
}

// Integer divide by zero is User Trap 31 (ERS sheets 47-48): with STATUS.T
// clear it only sets Overflow and execution goes on; with T set the trap is
// taken, Overflow cleared, and the parameter word says "invalid argument"
// (bits 24-27 = 2) for a 32-bit (1) or 64-bit (4) result -- 0x21 or 0x24.
// HP-UX turns 0x21 into SIGFPE; the bare 2 it used to get read as an
// integer overflow, which it ignores.
static void test_divide_by_zero_trap()
{
	std::printf("integer divide by zero\n");
	harness h;
	const u32 CST = 0x200, HB = 0x2000, HLEN = 0x400;
	h.mem.write32(0, CST);
	h.mem.write32(CST + 16 + 0, 0);
	h.mem.write32(CST + 16 + 4, HLEN - 1);
	h.mem.write32(CST + 16 + 8, HB);
	const u32 HPL = HB + (HLEN - 1) - 3;
	h.mem.write32(HPL - 4 * 031, 0x100);             // User Trap handler
	h.cpu.enable_traps(true);
	const u32 s0 = h.cpu.r.s, q0 = h.cpu.r.q;
	auto reset = [&](bool t) {
		h.cpu.r.pb = 0x1000; h.cpu.r.pl = 0x1ffc;
		h.cpu.r.s = s0; h.cpu.r.q = q0;
		h.cpu.r.status = 0;
		h.cpu.r.put(sf::T, t ? 1 : 0);
	};

	// T clear: Overflow, no trap, on to the next instruction.
	reset(false);
	h.cpu.push(7); h.cpu.push(0);
	CHECK(h.one(f3(033)) == exec_trap::none);        // DIVS
	CHECK(h.cpu.r.test(sf::O));
	CHECK_EQ(h.cpu.r.p, 0x1002);
	CHECK_EQ(h.cpu.r.pb, 0x1000);
	CHECK_EQ(h.cpu.r.s, s0 + 8);

	// T set: the trap, with its two-field parameter at Q+12.
	reset(true);
	h.cpu.r.put(sf::O, 1);
	h.cpu.push(7); h.cpu.push(0);
	CHECK(h.one(f3(033)) == exec_trap::none);
	CHECK(h.cpu.last_trap() == trap_cause::user_trap);
	CHECK_EQ(h.cpu.r.p, HB + 0x100);
	CHECK_EQ(h.mem.read32(h.cpu.r.q + 12), 0x21);
	CHECK(!h.cpu.r.test(sf::O));

	reset(true);
	h.cpu.push(0); h.cpu.push(7); h.cpu.push(0);
	CHECK(h.one(f3(023, true)) == exec_trap::none);  // DIV,B: 64-bit quotient
	CHECK(h.cpu.last_trap() == trap_cause::user_trap);
	CHECK_EQ(h.mem.read32(h.cpu.r.q + 12), 0x24);

	reset(true);
	h.cpu.push(7);
	CHECK(h.one(f5(007, 0)) == exec_trap::none);     // DIVI 0
	CHECK(h.cpu.last_trap() == trap_cause::user_trap);
	CHECK_EQ(h.mem.read32(h.cpu.r.q + 12), 0x21);
}

// Integer overflow is the same User Trap, condition 0: it stands "whenever
// both the overflow and trap bits are set", clears Overflow when taken, and
// returns past the instruction (note 1). BASIC's DOUBLE arithmetic depends
// on it to report ERROR 23 instead of printing a wrapped result.
static void test_overflow_trap()
{
	std::printf("integer overflow trap\n");
	harness h;
	const u32 CST = 0x200, HB = 0x2000, HLEN = 0x400;
	h.mem.write32(0, CST);
	h.mem.write32(CST + 16 + 0, 0);
	h.mem.write32(CST + 16 + 4, HLEN - 1);
	h.mem.write32(CST + 16 + 8, HB);
	const u32 HPL = HB + (HLEN - 1) - 3;
	h.mem.write32(HPL - 4 * 031, 0x100);
	h.cpu.enable_traps(true);
	const u32 s0 = h.cpu.r.s, q0 = h.cpu.r.q;
	auto reset = [&](bool t) {
		h.cpu.r.pb = 0x1000; h.cpu.r.pl = 0x1ffc;
		h.cpu.r.s = s0; h.cpu.r.q = q0;
		h.cpu.r.status = 0;
		h.cpu.r.put(sf::T, t ? 1 : 0);
		h.cpu.r.put(sf::M, 1);
	};
	auto param = [&]() { return h.mem.read32(h.cpu.r.q + 12); };
	auto ret = [&]() { return h.cpu.word_below_pub(h.cpu.r.q, 2); };

	// T clear: Overflow only.
	reset(false);
	h.cpu.push(0x7fffffffu); h.cpu.push(1);
	CHECK(h.one(f3(020)) == exec_trap::none);        // ADD
	CHECK(h.cpu.r.test(sf::O));
	CHECK_EQ(h.cpu.r.p, 0x1002);
	CHECK_EQ(h.cpu.r.pb, 0x1000);

	// T set: the trap, Overflow cleared, back to the next instruction.
	reset(true);
	h.cpu.push(0x7fffffffu); h.cpu.push(1);
	CHECK(h.one(f3(020)) == exec_trap::none);
	CHECK(h.cpu.last_trap() == trap_cause::user_trap);
	CHECK_EQ(h.cpu.r.p, HB + 0x100);
	CHECK_EQ(param(), 0x01);
	CHECK_EQ(ret(), 0x1002 - 0x1000);
	CHECK(!h.cpu.r.test(sf::O));

	// No overflow, no trap.
	reset(true);
	h.cpu.push(2); h.cpu.push(3);
	CHECK(h.one(f3(022)) == exec_trap::none);        // MPY
	CHECK_EQ(h.cpu.r.p, 0x1002);
	CHECK_EQ(h.cpu.stk(0), 6);

	reset(true);
	h.cpu.push(123456789); h.cpu.push(987);
	CHECK(h.one(f3(022)) == exec_trap::none);        // MPY overflows
	CHECK_EQ(h.cpu.r.p, HB + 0x100);
	CHECK_EQ(param(), 0x01);

	reset(true);
	h.cpu.push(0x12345);
	CHECK(h.one(f3(034)) == exec_trap::none);        // ISC: a halfword check
	CHECK_EQ(h.cpu.r.p, HB + 0x100);
	CHECK_EQ(param(), 0x00);

	reset(true);
	h.cpu.push(0x80000000u); h.cpu.push(0); h.cpu.push(u32(-1));
	CHECK(h.one(f3(023)) == exec_trap::none);        // DIV: doubleword
	CHECK_EQ(h.cpu.r.p, HB + 0x100);
	CHECK_EQ(param(), 0x04);
}

// The logical group as multiple precision limbs (ERS sheet 76, 61.1-61.3):
// the b bit chains Carry in, LSUB's borrow travels inverted (Carry set =
// no borrow), and the code is CCG/CCE with a zero limb leaving it alone
// when chained. HP-UX's atof and the C compiler's floating constants are
// built from exactly these, and every lost carry put them 2^22 ulp low.
static void test_logical_carry_chain()
{
	std::printf("logical add/subtract carry chain\n");
	harness h;
	const u32 base = h.cpu.r.s;
	auto two = [&](u16 insn, u32 b, u32 a, int carry_in, u32 cc_in) {
		h.cpu.r.s = base;
		h.cpu.r.put(sf::C, carry_in);
		h.cpu.r.put(sf::CC, cc_in);
		h.cpu.push(b); h.cpu.push(a);
		return h.one(insn);
	};

	CHECK(two(f3(060), 0xffffffffu, 1, 0, CCL) == exec_trap::none);    // LADD
	CHECK_EQ(h.cpu.stk(0), 0);
	CHECK(h.cpu.r.test(sf::C));
	CHECK_EQ(h.cpu.r.condition_code(), CCE);
	CHECK_EQ(h.cpu.r.s, base + 4);

	two(f3(060), 5, 6, 1, CCL);                                       // carry ignored
	CHECK_EQ(h.cpu.stk(0), 11);
	CHECK(!h.cpu.r.test(sf::C));
	CHECK_EQ(h.cpu.r.condition_code(), CCG);
	two(f3(060, true), 5, 6, 1, CCL);                                 // LADD,B
	CHECK_EQ(h.cpu.stk(0), 12);
	CHECK_EQ(h.cpu.r.condition_code(), CCG);
	two(f3(060, true), 0x40000000u, 0x40000000u, 0, CCE);
	CHECK_EQ(h.cpu.r.condition_code(), CCG);                          // unsigned: no CCL
	two(f3(060, true), 0xffffffffu, 0, 1, CCG);                       // zero limb
	CHECK_EQ(h.cpu.stk(0), 0);
	CHECK(h.cpu.r.test(sf::C));
	CHECK_EQ(h.cpu.r.condition_code(), CCG);                          // left alone

	two(f3(061), 7, 5, 0, CCL);                                       // LSUB
	CHECK_EQ(h.cpu.stk(0), 2);
	CHECK(h.cpu.r.test(sf::C));                                       // no borrow
	CHECK_EQ(h.cpu.r.condition_code(), CCG);
	two(f3(061), 5, 7, 1, CCL);
	CHECK_EQ(h.cpu.stk(0), 0xfffffffeu);
	CHECK(!h.cpu.r.test(sf::C));                                      // borrow
	CHECK_EQ(h.cpu.r.condition_code(), CCG);
	two(f3(061), 5, 5, 0, CCL);
	CHECK(h.cpu.r.test(sf::C));
	CHECK_EQ(h.cpu.r.condition_code(), CCE);
	two(f3(061, true), 7, 5, 0, CCL);                                 // LSUB,B: 1-C
	CHECK_EQ(h.cpu.stk(0), 1);
	CHECK(h.cpu.r.test(sf::C));
	two(f3(061, true), 7, 5, 1, CCL);
	CHECK_EQ(h.cpu.stk(0), 2);
	two(f3(061, true), 5, 5, 0, CCE);
	CHECK_EQ(h.cpu.stk(0), 0xffffffffu);
	CHECK(!h.cpu.r.test(sf::C));
	two(f3(061, true), 5, 5, 1, CCG);                                 // zero limb
	CHECK_EQ(h.cpu.stk(0), 0);
	CHECK(h.cpu.r.test(sf::C));
	CHECK_EQ(h.cpu.r.condition_code(), CCG);

	// A 96-bit sum and difference, limb by limb from the bottom:
	// 0x00000001_ffffffff_ffffffff + 1 = 0x00000002_00000000_00000000.
	const u32 x[3] = { 0xffffffffu, 0xffffffffu, 1 }, y[3] = { 1, 0, 0 };
	u32 s[3];
	h.cpu.r.s = base;
	for (int i = 0; i < 3; i++)
	{
		h.cpu.push(x[i]); h.cpu.push(y[i]);
		h.one(f3(060, i != 0));
		s[i] = h.cpu.pop();
	}
	CHECK_EQ(s[0], 0); CHECK_EQ(s[1], 0); CHECK_EQ(s[2], 2);
	CHECK_EQ(h.cpu.r.condition_code(), CCG);
	// ...and back: 0x00000002_00000000_00000000 - 1.
	for (int i = 0; i < 3; i++)
	{
		h.cpu.push(s[i]); h.cpu.push(y[i]);
		h.one(f3(061, i != 0));
		s[i] = h.cpu.pop();
	}
	CHECK_EQ(s[0], 0xffffffffu); CHECK_EQ(s[1], 0xffffffffu); CHECK_EQ(s[2], 1);
	CHECK(h.cpu.r.test(sf::C));

	// ADD,B and SUB,B are the signed top limb over LADD/LSUB (sheets
	// 74-75): SUB,B takes away 1-Carry, and with b the code is CCA only on
	// a nonzero result. 0x00000001_00000000 - 1 = 0x00000000_ffffffff:
	h.cpu.r.s = base;
	h.cpu.push(0); h.cpu.push(1);
	h.one(f3(061));                                                   // low
	const u32 lo = h.cpu.pop();
	h.cpu.push(1); h.cpu.push(0);
	CHECK(h.one(f3(021, true)) == exec_trap::none);                   // SUB,B
	CHECK_EQ(h.cpu.stk(0), 0);                                        // high
	CHECK_EQ(lo, 0xffffffffu);
	CHECK_EQ(h.cpu.r.condition_code(), CCG);                          // from LSUB
	CHECK(!h.cpu.r.test(sf::O));
	// -1 - 1 as a doubleword: 0xffffffff_ffffffff - 1, no borrow out of the
	// low limb, so the high limb stays -1 and reports CCL.
	h.cpu.r.s = base;
	h.cpu.push(0xffffffffu); h.cpu.push(1);
	h.one(f3(061));
	CHECK_EQ(h.cpu.pop(), 0xfffffffeu);
	h.cpu.push(0xffffffffu); h.cpu.push(0);
	h.one(f3(021, true));
	CHECK_EQ(h.cpu.stk(0), 0xffffffffu);
	CHECK_EQ(h.cpu.r.condition_code(), CCL);
	// SUB alone: Carry out set when there was no borrow.
	two(f3(021), 7, 5, 0, CCE);
	CHECK_EQ(h.cpu.stk(0), 2);
	CHECK(h.cpu.r.test(sf::C));
	two(f3(021), 5, 7, 1, CCE);
	CHECK_EQ(h.cpu.stk(0), -2);
	CHECK(!h.cpu.r.test(sf::C));
	CHECK_EQ(h.cpu.r.condition_code(), CCL);
	// ADD,B over LADD: 0x00000000_ffffffff + 1 = 0x00000001_00000000.
	h.cpu.r.s = base;
	h.cpu.push(0xffffffffu); h.cpu.push(1);
	h.one(f3(060));
	CHECK_EQ(h.cpu.pop(), 0);
	h.cpu.push(0); h.cpu.push(0);
	CHECK(h.one(f3(020, true)) == exec_trap::none);                   // ADD,B
	CHECK_EQ(h.cpu.stk(0), 1);
	CHECK_EQ(h.cpu.r.condition_code(), CCG);
	// A zero top limb leaves the low limb's report standing.
	two(f3(020, true), 0, 0, 0, CCG);
	CHECK_EQ(h.cpu.stk(0), 0);
	CHECK_EQ(h.cpu.r.condition_code(), CCG);
	two(f3(021, true), 3, 3, 1, CCL);
	CHECK_EQ(h.cpu.stk(0), 0);
	CHECK_EQ(h.cpu.r.condition_code(), CCL);
	two(f3(020), 0, 0, 0, CCG);                                       // plain: CCA
	CHECK_EQ(h.cpu.r.condition_code(), CCE);

	// LMPY: the doubleword product, CCG unless it is zero.
	h.cpu.r.s = base; h.cpu.r.put(sf::CC, CCL);
	h.cpu.push(0xffffffffu); h.cpu.push(0xffffffffu);
	CHECK(h.one(f3(062)) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(1), 0xfffffffeu);
	CHECK_EQ(h.cpu.stk(0), 1);
	CHECK_EQ(h.cpu.r.condition_code(), CCG);
	h.cpu.r.s = base;
	h.cpu.push(0); h.cpu.push(0x12345678u);
	h.one(f3(062));
	CHECK_EQ(h.cpu.r.condition_code(), CCE);
}

// NEG works in unsigned arithmetic -- -s32 of -2^31 is undefined in C++ --
// and that one value, which negates to itself, sets Overflow (ERS sheet 76:
// "Indicators = CCA, Overflow"). Overflow is only ever set by it, never
// cleared: BOV, BNO and SETR clear it (sheet 12).
static void test_negate()
{
	std::printf("negate\n");
	harness h;
	h.cpu.push(0x80000000u);
	CHECK(h.one(f3(024)) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(0), 0x80000000u);
	CHECK(h.cpu.r.test(sf::O));
	CHECK_EQ(h.cpu.r.condition_code(), CCL);
	h.cpu.pop(); h.cpu.push(5);
	CHECK(h.one(f3(024)) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(0), -5);
	CHECK(h.cpu.r.test(sf::O));                     // still set
	CHECK_EQ(h.cpu.r.condition_code(), CCL);
	h.cpu.r.put(sf::O, 0);
	CHECK(h.one(f3(024)) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(0), 5);
	CHECK(!h.cpu.r.test(sf::O));
	CHECK_EQ(h.cpu.r.condition_code(), CCG);
}

// Every instruction whose ERS entry names an indicator has to set one. The
// condition code is the easiest effect in this architecture to leave out:
// nothing in an instruction's own test looks at it, and an omission only
// surfaces when some later branch reads a stale code. Sixteen instructions
// had drifted that way (docs/VALIDATION.md, findings S1 and S2); these
// pin the ones that were fixed, so they cannot drift back.
//
// `tools/ers.py --indicators` is the wider net -- it compares every
// instruction's `Indicators =` line against whether the code sets one.
static void test_condition_codes()
{
	std::printf("condition codes the ERS requires\n");
	harness h;

	// The stack group: "CCA on the new TOS" (ERS sheets 66-68).
	h.cpu.r.status = 0;
	h.cpu.push(5);
	CHECK(h.one(f3(045)) == exec_trap::none);       // DUP of a positive
	CHECK_EQ(h.cpu.r.condition_code(), CCG);

	h.cpu.push(u32(-7));
	CHECK(h.one(f3(045)) == exec_trap::none);       // DUP of a negative
	CHECK_EQ(h.cpu.r.condition_code(), CCL);

	CHECK(h.one(f3(006)) == exec_trap::none);       // ZERO
	CHECK_EQ(h.cpu.r.condition_code(), CCE);

	// XCH brings the word beneath to the top, and the code follows it.
	h.cpu.push(0); h.cpu.push(9);
	CHECK(h.one(f3(032)) == exec_trap::none);       // XCH
	CHECK_EQ(h.cpu.stk(0), 0u);
	CHECK_EQ(h.cpu.r.condition_code(), CCE);

	// CAB rotates three; the code reports whatever lands on top.
	h.cpu.r.s = 0x4003;
	h.cpu.push(1); h.cpu.push(2); h.cpu.push(u32(-3));
	CHECK(h.one(f3(056)) == exec_trap::none);       // CAB
	CHECK_EQ(h.cpu.stk(0), 1u);
	CHECK_EQ(h.cpu.r.condition_code(), CCG);

	// XAX swaps A and X, and reports the new A.
	h.cpu.r.x = u32(-1);
	CHECK(h.one(f3(035)) == exec_trap::none);       // XAX
	CHECK_EQ(h.cpu.r.condition_code(), CCL);

	// The doubleword forms report the 64-bit value, not either half. A
	// negative doubleword has its sign in the word beneath the top, so a
	// naive CCA on the top word alone would call this one CCG.
	h.cpu.r.s = 0x4003;
	h.cpu.push(0xffffffffu);        // high half: negative
	h.cpu.push(0x00000001u);        // low half: positive on its own
	CHECK(h.one(f3(046)) == exec_trap::none);       // DDUP
	CHECK_EQ(h.cpu.r.condition_code(), CCL);

	// ...and a doubleword is zero only when both halves are.
	h.cpu.r.s = 0x4003;
	h.cpu.push(0); h.cpu.push(0);
	CHECK(h.one(f3(016)) == exec_trap::none);       // DXCH
	CHECK_EQ(h.cpu.r.condition_code(), CCE);

	// DIVS reports the quotient, not the remainder left on top.
	h.cpu.r.s = 0x4003;
	h.cpu.push(u32(-20)); h.cpu.push(3);
	CHECK(h.one(f3(033)) == exec_trap::none);       // DIVS
	CHECK_EQ(h.cpu.r.condition_code(), CCL);        // quotient -6

	// The quotient that does not fit: INT_MIN / -1 sets Overflow rather
	// than executing a division C++ leaves undefined.
	h.cpu.r.s = 0x4003;
	h.cpu.r.put(sf::O, 0);
	h.cpu.push(0x80000000u); h.cpu.push(u32(-1));
	CHECK(h.one(f3(033)) == exec_trap::none);       // DIVS
	CHECK_EQ(h.cpu.r.get(sf::O), 1u);

	// BCY and BNC clear Carry, as BOV and BNO clear Overflow
	// (ERS sheet 92, items 110-111).
	h.cpu.r.s = 0x4003;
	h.cpu.r.put(sf::C, 1);
	CHECK(h.one(f2(060, 0)) == exec_trap::none);    // BCY, carry set
	CHECK_EQ(h.cpu.r.get(sf::C), 0u);

	h.cpu.r.put(sf::C, 1);
	CHECK(h.one(f2(061, 0)) == exec_trap::none);    // BNC, carry set
	CHECK_EQ(h.cpu.r.get(sf::C), 0u);

	// PSHN reports only when it pushed something: "If original X>0 then
	// CCA else unaffected".
	h.cpu.r.s = 0x4003;
	h.cpu.r.put(sf::CC, CCG);
	h.cpu.r.x = 0;
	CHECK(h.one(f3(005)) == exec_trap::none);       // PSHN with X = 0
	CHECK_EQ(h.cpu.r.condition_code(), CCG);        // left alone
	h.cpu.r.x = 2;
	CHECK(h.one(f3(005)) == exec_trap::none);       // PSHN with X = 2
	CHECK_EQ(h.cpu.r.condition_code(), CCE);        // pushed zeros
}

static void test_stack_ops()
{
	std::printf("stack manipulation\n");
	harness h;

	h.cpu.push(1); h.cpu.push(2); h.cpu.push(3);
	CHECK_EQ(h.cpu.stk(0), 3);
	CHECK_EQ(h.cpu.stk(2), 1);

	CHECK(h.one(f3(045)) == exec_trap::none);       // DUP
	CHECK_EQ(h.cpu.stk(0), 3);
	CHECK_EQ(h.cpu.stk(1), 3);

	CHECK(h.one(f3(040)) == exec_trap::none);       // DEL
	CHECK_EQ(h.cpu.stk(0), 3);

	CHECK(h.one(f3(032)) == exec_trap::none);       // XCH
	CHECK_EQ(h.cpu.stk(0), 2);
	CHECK_EQ(h.cpu.stk(1), 3);

	CHECK(h.one(f3(056)) == exec_trap::none);       // CAB: C to the top
	CHECK_EQ(h.cpu.stk(0), 1);
	CHECK_EQ(h.cpu.stk(1), 2);
	CHECK_EQ(h.cpu.stk(2), 3);

	CHECK(h.one(f3(006)) == exec_trap::none);       // ZERO
	CHECK_EQ(h.cpu.stk(0), 0);

	// X register moves.
	h.cpu.r.x = 0;
	h.cpu.push(99);
	CHECK(h.one(f3(043)) == exec_trap::none);       // STAX
	CHECK_EQ(h.cpu.r.x, 99);
	CHECK(h.one(f3(044)) == exec_trap::none);       // LDXA
	CHECK_EQ(h.cpu.stk(0), 99);
}

static void test_branches()
{
	std::printf("branches and condition codes\n");
	harness h;

	// BCCS taken on CCE only.
	h.cpu.r.put(sf::CC, CCE);
	h.cpu.r.p = 0x1000;
	h.put(0x1000, bccs(2, false, 4));
	CHECK(h.cpu.step() == exec_trap::none);
	CHECK_EQ(h.cpu.r.p, 0x1000 + 8);

	h.cpu.r.put(sf::CC, CCG);
	h.cpu.r.p = 0x1000;
	CHECK(h.cpu.step() == exec_trap::none);
	CHECK_EQ(h.cpu.r.p, 0x1002);                    // not taken, falls through

	// Backward branch.
	h.cpu.r.put(sf::CC, CCE);
	h.cpu.r.p = 0x1010;
	h.put(0x1010, bccs(2, true, 4));
	CHECK(h.cpu.step() == exec_trap::none);
	CHECK_EQ(h.cpu.r.p, 0x1010 - 8);

	// CMPI sets the condition code from the comparison.
	h.cpu.push(5);
	CHECK(h.one(f5(003, 5)) == exec_trap::none);
	CHECK_EQ(h.cpu.r.condition_code(), CCE);
	h.cpu.push(3);
	CHECK(h.one(f5(003, 5)) == exec_trap::none);
	CHECK_EQ(h.cpu.r.condition_code(), CCL);
	h.cpu.push(9);
	CHECK(h.one(f5(003, 5)) == exec_trap::none);
	CHECK_EQ(h.cpu.r.condition_code(), CCG);

	// BRZ pops and branches when the value is zero.
	h.cpu.push(0);
	h.cpu.r.p = 0x1020;
	h.put(0x1020, u16((070 << 6) | 6));
	CHECK(h.cpu.step() == exec_trap::none);
	CHECK_EQ(h.cpu.r.p, 0x1020 + 12);

	// Branches PRE-index, alone among the instructions (ERS sheet 17:
	// "For all instructions except branch instructions postindexing is
	// used... Branch instructions use preindexing so that high-level
	// language CASE statements can be executed efficiently. In this
	// case, the base, offset and index register select one of a set of
	// indirect pointers which is then evaluated (WITHOUT using the index
	// register) to determine the branch target"; sheet 108 repeats it).
	//
	// A jump table of self-relative pointers at P+8, and X selecting the
	// second arm. Post-indexing would read arm 0's pointer and add 4 to
	// where it lands -- four bytes into the wrong arm, which is exactly
	// how a driver ends up calling the wrong routine.
	h.cpu.r.p = 0x1100;
	h.cpu.r.x = 4;
	h.mem.write32(0x1100, u32_insn(030, 0, 8) | (1u << 23) | (1u << 22));
	h.mem.write32(0x1108, 0x40);                    // arm 0 -> 0x1148
	h.mem.write32(0x110c, 0x80);                    // arm 1 -> 0x118c
	CHECK(h.cpu.step() == exec_trap::none);
	CHECK_EQ(h.cpu.r.p, 0x118c);

	// Arm 0 with the index clear, to show the table itself is right.
	h.cpu.r.p = 0x1100;
	h.cpu.r.x = 0;
	CHECK(h.cpu.step() == exec_trap::none);
	CHECK_EQ(h.cpu.r.p, 0x1148);

	// The index is dropped once the pointer has been selected: a table
	// whose arms both point at the same place lands there whatever X is.
	h.cpu.r.p = 0x1100;
	h.cpu.r.x = 4;
	h.mem.write32(0x110c, 0x3c);                    // arm 1 -> 0x1148 too
	CHECK(h.cpu.step() == exec_trap::none);
	CHECK_EQ(h.cpu.r.p, 0x1148);
}

static void test_load_store()
{
	std::printf("load and store\n");
	harness h;

	h.mem.write32(h.cpu.r.db + 16, 0xdeadbeef);

	// LDS uses a word offset: 16 bytes is offset 4.
	CHECK(h.one(f1(2, false, false, 2, 4)) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(0), s32(0xdeadbeefu));

	CHECK(h.one(f1(5, false, false, 2, 8)) == exec_trap::none);   // STS DB+32
	CHECK_EQ(h.mem.read32(h.cpu.r.db + 32), s32(0xdeadbeefu));

	// A store outside the segment must trap rather than corrupt memory.
	// The largest format 1 offset is 255 words, so the segment is shortened
	// to bring that within reach.
	h.cpu.r.dl = h.cpu.r.db + 0x3f;
	h.cpu.push(1);
	CHECK(h.one(f1(5, false, false, 2, 255)) == exec_trap::address_error);
	CHECK(h.cpu.last_addr_trap() == trap::bounds_violation);
}

static void test_memory_arithmetic()
{
	std::printf("memory increment and decrement\n");
	harness h;

	// INCM/DECM step by one; INCF/DECF step by *four* (ERS sheet 80, items
	// 81.1-81.3, name them "Increment/Decrement Memory by Four"). The
	// powerup module's checksum loop counts a byte total down with DECF
	// while advancing X by four, which is what caught DECF stepping by one.
	h.mem.write32(h.cpu.r.db + 16, 100);
	CHECK(h.one(u32_insn(024, 2, 16)) == exec_trap::none);     // INCM
	CHECK_EQ(h.mem.read32(h.cpu.r.db + 16), 101);
	CHECK(h.one(u32_insn(023, 2, 16)) == exec_trap::none);     // DECM
	CHECK_EQ(h.mem.read32(h.cpu.r.db + 16), 100);
	CHECK(h.one(u32_insn(027, 2, 16)) == exec_trap::none);     // INCF
	CHECK_EQ(h.mem.read32(h.cpu.r.db + 16), 104);
	CHECK(h.one(u32_insn(032, 2, 16)) == exec_trap::none);     // DECF
	CHECK_EQ(h.mem.read32(h.cpu.r.db + 16), 100);

	// DECF sets the condition code on the result: counting 4 down to 0 is
	// CCE, one more takes it negative to CCL -- the checksum loop's exit.
	h.mem.write32(h.cpu.r.db + 20, 4);
	CHECK(h.one(u32_insn(032, 2, 20)) == exec_trap::none);
	CHECK_EQ(h.cpu.r.condition_code(), CCE);
	CHECK(h.one(u32_insn(032, 2, 20)) == exec_trap::none);
	CHECK_EQ(h.cpu.r.condition_code(), CCL);
}

static void test_modal_pops()
{
	std::printf("operand deletion follows the b bit\n");

	// STAX, ADAX, CMP, LCMP and the test group delete their operands only
	// when CIR(19:1) is set (ERS sheets 33-34.5, 61, 74). The powerup
	// module interleaves both forms tightly: its chain walk keeps a value
	// below a plain CMP, and STAX,B's pop pairs against LDXA in the
	// checksum subroutine's prologue.
	harness h;
	const u32 s0 = h.cpu.r.s;

	h.cpu.push(7);
	CHECK(h.one(f3(043)) == exec_trap::none);          // STAX keeps TOS
	CHECK_EQ(h.cpu.r.x, 7);
	CHECK_EQ(h.cpu.r.s, s0 + 4);
	CHECK(h.one(f3(043, true)) == exec_trap::none);    // STAX,B pops
	CHECK_EQ(h.cpu.r.s, s0);

	h.cpu.r.x = 3;
	h.cpu.push(5);
	CHECK(h.one(f3(036)) == exec_trap::none);          // ADAX keeps TOS
	CHECK_EQ(h.cpu.r.x, 8);
	CHECK_EQ(h.cpu.r.s, s0 + 4);
	CHECK(h.one(f3(036, true)) == exec_trap::none);    // ADAX,B pops
	CHECK_EQ(h.cpu.r.x, 13);
	CHECK_EQ(h.cpu.r.s, s0);

	h.cpu.push(9); h.cpu.push(4);
	CHECK(h.one(f3(017)) == exec_trap::none);          // CMP drops TOS only
	CHECK_EQ(h.cpu.r.condition_code(), CCG);
	CHECK_EQ(h.cpu.r.s, s0 + 4);
	CHECK_EQ(h.cpu.stk(0), 9);
	h.cpu.push(12);
	CHECK(h.one(f3(017, true)) == exec_trap::none);    // CMP,B drops both
	CHECK_EQ(h.cpu.r.condition_code(), CCL);
	CHECK_EQ(h.cpu.r.s, s0);

	h.cpu.push(1); h.cpu.push(2);
	CHECK(h.one(f3(057)) == exec_trap::none);          // LCMP drops TOS only
	CHECK_EQ(h.cpu.r.s, s0 + 4);
	h.cpu.push(0);
	CHECK(h.one(f3(057, true)) == exec_trap::none);    // LCMP,B drops both
	CHECK_EQ(h.cpu.r.s, s0);

	h.cpu.push(u32(-5));
	CHECK(h.one(f3(025)) == exec_trap::none);          // TEST keeps TOS
	CHECK_EQ(h.cpu.r.condition_code(), CCL);
	CHECK_EQ(h.cpu.r.s, s0 + 4);
	CHECK(h.one(f3(025, true)) == exec_trap::none);    // TEST,B pops
	CHECK_EQ(h.cpu.r.s, s0);
}

static void test_unimplemented_is_reported()
{
	std::printf("unimplemented opcodes are reported\n");
	harness h;

	// What is left of format 4 must not be silently treated as a set of
	// no-ops. DTB, BTD, POT and VPTR used to stand in here and are
	// implemented now, so the stand-ins are the three that remain.
	CHECK(h.one(u16(1 << 6 | 024)) == exec_trap::unimplemented);   // CTCB
	CHECK(h.one(u16(1 << 6 | 054)) == exec_trap::unimplemented);   // CNTO
	CHECK(h.one(u16(1 << 6 | 056)) == exec_trap::unimplemented);   // RFC
}


// FIXD and FIXS: "Indicators = CCA, Overflow", and the ",B" bit rounds
// where the plain form truncates (ERS sheets 87-88).
//
// The Overflow half is what HP-UX's MATH LIBRARY truncate-to-integral
// routine is built on -- FIXD, then `BOV` to a slow path that masks the
// fraction off by exponent when the value is too big for 32 bits. With
// Overflow never set, the kernel's 10 Hz callout daemon computed its next
// deadline from a saturated integer, found it already past, and never
// slept: one task spinning at priority, the rest of the boot behind it.
static void test_fix_overflow_and_rounding(harness &h)
{
	// Push a double the way the tests above do: a zero low word with the
	// high word written under it.
	const auto push_double = [&h](u32 hi, u32 lo) {
		h.cpu.push(0); h.cpu.push(lo);
		h.mem.write32(h.cpu.r.s - 3 - 4, hi);
	};

	// In range: Overflow is cleared, not left standing.
	h.cpu.r.put(sf::O, 1);
	push_double(0x40240000, 0x00000000);               // 10.0
	CHECK(h.one(f3(041)) == exec_trap::none);          // FIXD
	CHECK_EQ(h.cpu.stk(0), 10);
	CHECK_EQ(h.cpu.r.test(sf::O) ? 1 : 0, 0);
	h.cpu.pop();

	// Truncation versus rounding, both signs.
	push_double(0x40059999, 0x9999999a);               // 2.7
	CHECK(h.one(f3(041)) == exec_trap::none);          // truncates
	CHECK_EQ(h.cpu.stk(0), 2);
	h.cpu.pop();
	push_double(0x40059999, 0x9999999a);               // 2.7
	CHECK(h.one(f3(041, true)) == exec_trap::none);    // FIXD,B rounds
	CHECK_EQ(h.cpu.stk(0), 3);
	h.cpu.pop();
	push_double(0xc0059999, 0x9999999a);               // -2.7
	CHECK(h.one(f3(041)) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(0), -2);
	h.cpu.pop();
	push_double(0xc0059999, 0x9999999a);               // -2.7, rounded by
	CHECK(h.one(f3(041, true)) == exec_trap::none);    // magnitude
	CHECK_EQ(h.cpu.stk(0), -3);
	h.cpu.pop();

	// Out of range: Overflow set. This is the microsecond clock divided
	// by 100000 that HP-UX's callout daemon hands the MATH LIBRARY.
	push_double(0x421492a3, 0xc55e3d1d);               // 22089888087.6
	CHECK(h.one(f3(041)) == exec_trap::none);
	CHECK_EQ(h.cpu.r.test(sf::O) ? 1 : 0, 1);
	h.cpu.pop();

	// The boundaries themselves are in range.
	push_double(0x41dfffff, 0xffc00000);               // 2147483647.0
	CHECK(h.one(f3(041)) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(0), 2147483647);
	CHECK_EQ(h.cpu.r.test(sf::O) ? 1 : 0, 0);
	h.cpu.pop();
	push_double(0xc1e00000, 0x00000000);               // -2147483648.0
	CHECK(h.one(f3(041)) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(0), s32(0x80000000u));
	CHECK_EQ(h.cpu.r.test(sf::O) ? 1 : 0, 0);
	h.cpu.pop();

	// FIXS carries both rules too.
	h.cpu.push(0x406ccccd);                            // 3.7f
	CHECK(h.one(f3(071)) == exec_trap::none);          // truncates
	CHECK_EQ(h.cpu.stk(0), 3);
	CHECK_EQ(h.cpu.r.test(sf::O) ? 1 : 0, 0);
	h.cpu.pop();
	h.cpu.push(0x406ccccd);
	CHECK(h.one(f3(071, true)) == exec_trap::none);    // FIXS,B rounds
	CHECK_EQ(h.cpu.stk(0), 4);
	h.cpu.pop();
	h.cpu.push(0x50000040);                            // 8.59e9f
	CHECK(h.one(f3(071)) == exec_trap::none);
	CHECK_EQ(h.cpu.r.test(sf::O) ? 1 : 0, 1);
	h.cpu.pop();
}

static void test_floating_point()
{
	std::printf("floating point\n");
	harness h;

	// FLTD converts the integer top of stack to a 64-bit double occupying
	// two words, high word below low.
	h.cpu.push(u32(7));
	CHECK(h.one(f3(042)) == exec_trap::none);          // FLTD
	CHECK_EQ(h.cpu.stk(1), 0x401c0000);                // 7.0 high word
	CHECK_EQ(h.cpu.stk(0), 0);

	h.cpu.push(0); h.cpu.push(0);
	h.mem.write32(h.cpu.r.s - 3 - 4, 0x40080000);      // 3.0
	CHECK(h.one(f3(051)) == exec_trap::none);          // FADD -> 10.0
	CHECK_EQ(h.cpu.stk(1), 0x40240000);
	CHECK_EQ(h.cpu.r.condition_code(), CCG);

	CHECK(h.one(f3(041)) == exec_trap::none);          // FIXD -> 10
	CHECK_EQ(h.cpu.stk(0), 10);

	// A round trip through double and back must be exact for integers.
	h.cpu.pop();
	h.cpu.push(u32(-12345));
	CHECK(h.one(f3(042)) == exec_trap::none);
	CHECK(h.one(f3(041)) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(0), -12345);

	// Single precision.
	h.cpu.pop();
	h.cpu.push(5);
	CHECK(h.one(f3(070)) == exec_trap::none);          // FLTS
	CHECK_EQ(h.cpu.stk(0), s32(0x40a00000u));          // 5.0f
	h.cpu.push(0x40000000);                            // 2.0f
	CHECK(h.one(f3(074)) == exec_trap::none);          // FMPS -> 10.0f
	CHECK_EQ(h.cpu.stk(0), s32(0x41200000u));
	CHECK(h.one(f3(071)) == exec_trap::none);          // FIXS
	CHECK_EQ(h.cpu.stk(0), 10);

	// Division and negation.
	h.cpu.pop();
	h.cpu.push(0); h.cpu.push(0);
	h.mem.write32(h.cpu.r.s - 3 - 4, 0x40240000);      // 10.0
	h.cpu.push(0); h.cpu.push(0);
	h.mem.write32(h.cpu.r.s - 3 - 4, 0x40100000);      // 4.0
	CHECK(h.one(f3(054)) == exec_trap::none);          // FDIV -> 2.5
	CHECK_EQ(h.cpu.stk(1), 0x40040000);
	CHECK(h.one(f3(055)) == exec_trap::none);          // FNEG
	CHECK_EQ(h.cpu.stk(1), s32(0xc0040000u));
	CHECK_EQ(h.cpu.r.condition_code(), CCL);

	// CDSF narrows the double in the top two words to one single word;
	// CSDF widens it back. -2.5 is exact in both formats (ERS sheet 88).
	CHECK(h.one(f3(030)) == exec_trap::none);          // CDSF
	CHECK_EQ(h.cpu.stk(0), s32(0xc0200000u));          // -2.5f
	CHECK_EQ(h.cpu.r.condition_code(), CCL);

	CHECK(h.one(f3(031)) == exec_trap::none);          // CSDF
	CHECK_EQ(h.cpu.stk(1), s32(0xc0040000u));          // -2.5 high word
	CHECK_EQ(h.cpu.stk(0), 0);
	CHECK_EQ(h.cpu.r.condition_code(), CCL);

	// The zero CSDF pushes is the low word, so a positive value still
	// reports CCG and narrows back to exactly what it started as.
	h.cpu.pop(); h.cpu.pop();
	h.cpu.push(0x40a00000);                            // 5.0f
	CHECK(h.one(f3(031)) == exec_trap::none);          // CSDF
	CHECK_EQ(h.cpu.stk(1), 0x40140000);                // 5.0 high word
	CHECK_EQ(h.cpu.stk(0), 0);
	CHECK_EQ(h.cpu.r.condition_code(), CCG);
	CHECK(h.one(f3(030)) == exec_trap::none);          // CDSF
	CHECK_EQ(h.cpu.stk(0), s32(0x40a00000u));

	test_fix_overflow_and_rounding(h);
}

static void test_new_groups()
{
	std::printf("shifts, bit fields, privileged access\n");
	harness h;

	// Circular shifts.
	h.cpu.push(0x80000001);
	CHECK(h.one(u16((004 << 6) | 1)) == exec_trap::none);     // CSL 1
	CHECK_EQ(h.cpu.stk(0), 3);
	h.cpu.pop();
	h.cpu.push(0x80000001);
	CHECK(h.one(u16((005 << 6) | 1)) == exec_trap::none);     // CSR 1
	CHECK_EQ(h.cpu.stk(0), s32(0xc0000000u));

	// PSHN pushes X copies of zero.
	h.cpu.pop();
	const u32 base = h.cpu.r.s;
	h.cpu.r.x = 3;
	CHECK(h.one(f3(005)) == exec_trap::none);
	CHECK_EQ(h.cpu.r.s, base + 12);
	CHECK_EQ(h.cpu.r.x, 0);

	// DIVS yields quotient then remainder.
	h.cpu.r.s = base;
	h.cpu.push(u32(-17)); h.cpu.push(5);
	CHECK(h.one(f3(033)) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(1), -3);                                // quotient
	CHECK_EQ(h.cpu.stk(0), -2);                                // remainder
	h.cpu.r.s = base;
	h.cpu.push(u32(-17)); h.cpu.push(5);
	CHECK(h.one(f3(033, true)) == exec_trap::none);            // positive rem
	// "dividend = quotient*divisor + remainder" holds for the b form too
	// (ERS sheet 76): -17 = -4*5 + 3, so the quotient moves with it. The
	// old expectation, remainder 2 beside quotient -3, broke the identity.
	CHECK_EQ(h.cpu.stk(1), -4);
	CHECK_EQ(h.cpu.stk(0), 3);
	h.cpu.r.s = base;
	h.cpu.push(u32(-7)); h.cpu.push(u32(-3));
	CHECK(h.one(f3(033, true)) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(1), 3);                                 // -7 = 3*-3 + 2
	CHECK_EQ(h.cpu.stk(0), 2);
	h.cpu.r.s = base;
	h.cpu.push(7); h.cpu.push(u32(-3));
	CHECK(h.one(f3(033, true)) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(1), -2);                                // already >= 0
	CHECK_EQ(h.cpu.stk(0), 1);
	h.cpu.r.s = base;
	h.cpu.push(u32(-7)); h.cpu.push(u32(0x80000000u));
	CHECK(h.one(f3(033, true)) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(1), 1);                                 // -7 = 1*-2^31 + (2^31-7)
	CHECK_EQ(h.cpu.stk(0), 0x7ffffff9);
	// DIV,B on a doubleword dividend: -7 by 3 is -3 remainder 2.
	h.cpu.r.s = base;
	h.cpu.push(0xffffffffu); h.cpu.push(u32(-7)); h.cpu.push(3);
	CHECK(h.one(f3(023, true)) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(2), -1);
	CHECK_EQ(h.cpu.stk(1), -3);
	CHECK_EQ(h.cpu.stk(0), 2);
	CHECK_EQ(h.cpu.r.condition_code(), CCL);
	h.cpu.r.s = base;
	h.cpu.push(0xffffffffu); h.cpu.push(u32(-7)); h.cpu.push(3);
	CHECK(h.one(f3(023)) == exec_trap::none);                  // plain DIV
	CHECK_EQ(h.cpu.stk(1), -2);
	CHECK_EQ(h.cpu.stk(0), -1);

	// Privileged absolute load and store.
	h.cpu.r.s = base;
	h.cpu.r.put(sf::M, 1);
	h.mem.write32(0x20000, 0xa5a5a5a5);
	h.cpu.push(0x20000);
	CHECK(h.one(u16((1 << 6) | 064)) == exec_trap::none);      // PLDA
	CHECK_EQ(h.cpu.stk(0), s32(0xa5a5a5a5u));
	h.cpu.push(0x20004); h.cpu.push(0x1234);
	CHECK(h.one(u16((1 << 6) | 065)) == exec_trap::none);      // PSTA
	CHECK_EQ(h.mem.read32(0x20004), 0x1234);

	// Unprivileged use must not be allowed.
	h.cpu.r.put(sf::M, 0);
	h.cpu.push(0x20000);
	CHECK(h.one(u16((1 << 6) | 064)) == exec_trap::unimplemented);
	h.cpu.r.put(sf::M, 1);

	// Byte and halfword access.
	h.cpu.r.s = base;
	h.mem.write32(h.cpu.r.db + 64, 0x11223344);
	CHECK(h.one(u32_insn(014, 2, 64)) == exec_trap::none);     // LDB DB+64
	CHECK_EQ(h.cpu.stk(0), 0x11);
	CHECK(h.one(u32_insn(022, 2, 64)) == exec_trap::none);     // LDH DB+64
	CHECK_EQ(h.cpu.stk(0), 0x1122);

	// SOL is a no-op while the start-of-line trap is disabled.
	h.cpu.r.put(sf::SL, 0);
	CHECK(h.one(f5(000, 0)) == exec_trap::none);
	h.cpu.r.put(sf::SL, 1);
	CHECK(h.one(f5(000, 0)) != exec_trap::none);
	h.cpu.r.put(sf::SL, 1 - 1);
}

static void test_try_escp()
{
	std::printf("TRY / ESCP exception frames\n");
	harness h;

	// (SB+2) is the TRY chain head, (SB+3) the escape value and (SB+4) the
	// escape flag. ESCP additionally requires a frame above SB+12 words, so
	// the stack proper has to start clear of all of that.
	h.cpu.r.s = h.cpu.r.sb + 4 * 16 + 3;
	h.cpu.r.q = h.cpu.r.s;
	h.mem.write32(h.cpu.r.sb + 8, 0);
	h.mem.write32(h.cpu.r.sb + 16, 0);

	const u32 s0 = h.cpu.r.s;
	const u32 q0 = h.cpu.r.q;
	// A recognisable sentinel whose CSN field (bits 20-31) is zero: EXIT
	// re-derives PB and PL through the CST when the restored code segment
	// number differs from the current one, and no CST exists here.
	h.cpu.r.status = 0x66500000;

	// TRY with bit 3 set takes a PB-relative recovery address from the top
	// of stack and builds a four-word frame.
	h.cpu.push(0x40);                                   // recovery address
	CHECK(h.one(u16((1 << 6) | 040 | 0x1000)) == exec_trap::none);
	CHECK_EQ(h.cpu.r.s, s0 + 16);                       // one word plus three
	CHECK_EQ(h.mem.read32(h.cpu.r.sb + 8), h.cpu.r.s - h.cpu.r.sb);
	CHECK_EQ(h.cpu.word_below_pub(h.cpu.r.s, 0), 16);   // delta Q
	CHECK_EQ(h.cpu.word_below_pub(h.cpu.r.s, 1), 0x66500000); // saved STATUS
	CHECK_EQ(h.cpu.word_below_pub(h.cpu.r.s, 2), 0x40); // recovery address
	CHECK_EQ(h.cpu.word_below_pub(h.cpu.r.s, 3), 0);    // previous chain head

	// A nested TRY chains to the first. Between the two there is body code,
	// so the stack has grown past the frame -- which matters, because both
	// TRY-delete and ESCP require the frame to lie strictly below S.
	const u32 first = h.cpu.r.s;
	h.cpu.push(0xbeef);                                 // a local
	h.cpu.push(0x80);                                   // recovery address
	CHECK(h.one(u16((1 << 6) | 040 | 0x1000)) == exec_trap::none);
	CHECK_EQ(h.cpu.word_below_pub(h.cpu.r.s, 3), first - h.cpu.r.sb);

	// TRY with bit 3 clear deletes the innermost frame, restores the chain
	// head, and unwinds S to below the frame -- discarding the local too.
	h.cpu.push(0xcafe);                                 // body local
	CHECK(h.one(u16((1 << 6) | 040)) == exec_trap::none);
	CHECK_EQ(h.mem.read32(h.cpu.r.sb + 8), first - h.cpu.r.sb);

	// ESCP unwinds to the remaining frame: Q moves to it, STATUS is
	// restored and control resumes at PB + the recovery address.
	h.cpu.r.s = first + 8;                              // body locals
	h.cpu.r.status = 0;
	h.cpu.push(0xdead);                                 // escape value
	const exec_trap t = h.one(u16((1 << 6) | 041));
	CHECK(t == exec_trap::none);
	CHECK_EQ(h.mem.read32(h.cpu.r.sb + 12), 0xdead);    // stashed at (SB+3)
	CHECK_EQ(h.cpu.r.status, 0x66500000);               // restored
	CHECK_EQ(h.cpu.r.p, h.cpu.r.pb + 0x40);             // recovery address
	CHECK_EQ(h.mem.read32(h.cpu.r.sb + 8), 0);          // chain head popped
	CHECK_EQ(h.cpu.r.q, q0);                            // delta Q unwound

	// INT sets and clears the interrupt enable bit. It is privileged
	// (ERS sheet 119); test_stop_and_int_are_privileged has the trap.
	h.cpu.r.put(sf::I, 0);
	h.cpu.r.put(sf::M, 1);
	CHECK(h.one(u16((1 << 6) | 011 | 0x1000)) == exec_trap::none);
	CHECK(h.cpu.r.test(sf::I));
	CHECK(h.one(u16((1 << 6) | 011)) == exec_trap::none);
	CHECK(!h.cpu.r.test(sf::I));
	h.cpu.r.put(sf::M, 0);

	// CPTR adds a byte offset to a global data segment pointer.
	h.cpu.r.s = s0;
	h.cpu.push(0x80000000u | 100);                      // DB-relative, offset 100
	h.cpu.push(20);
	CHECK(h.one(u16((1 << 6) | 070)) == exec_trap::none);
	CHECK_EQ(ers32::get(h.cpu.stk(0), 2, 30), 120);
	CHECK_EQ(ers32::get(h.cpu.stk(0), 0, 2), 2);        // tag preserved

	// Offset(0:2) is the top two bits, so the violation is a 30-bit
	// overflow rather than a misalignment.
	h.cpu.push(0x40000000);
	CHECK(h.one(u16((1 << 6) | 070)) == exec_trap::pointer_conversion);
}

static void test_traps()
{
	std::printf("internal traps (sheets 44-45)\n");
	harness h;

	// Without a handler segment, faults are reported to the caller.
	CHECK(!h.cpu.traps_enabled());
	CHECK(h.one(u16(1 << 6 | 024)) == exec_trap::unimplemented);

	// Install a Code Segment Table whose entry 1 describes the handler
	// segment. The system CST pointer lives at absolute location 0.
	const u32 CST = 0x200;
	const u32 HB = 0x2000, HLEN = 0x400;
	h.mem.write32(0, CST);
	h.mem.write32(CST + 16 + 0, 0);                  // entry 1 flags
	h.mem.write32(CST + 16 + 4, HLEN - 1);           // LENGTH - 1
	h.mem.write32(CST + 16 + 8, HB);                 // LOCATION
	const u32 HPL = HB + (HLEN - 1) - 3;

	// STT entry for the unimplemented-instruction trap, octal 20.
	h.mem.write32(HPL - 4 * 020, 0x100);             // handler at HB + 0x100
	// ...and for a bounds violation, octal 1.
	h.mem.write32(HPL - 4 * 001, 0x200);

	h.cpu.enable_traps(true);
	h.cpu.r.status = 0;
	h.cpu.r.put(sf::M, 1);
	h.cpu.r.put(sf::CSN, 5);
	h.cpu.r.put(sf::C, 1);                           // must be cleared
	const u32 q0 = h.cpu.r.q;

	CHECK(h.one(u16(1 << 6 | 024)) == exec_trap::none);   // CTCB, unimplemented
	CHECK(h.cpu.last_trap() == trap_cause::unimplemented_instruction);

	// PB and PL come from CST entry 1, and P from the STT entry.
	CHECK_EQ(h.cpu.r.pb, HB);
	CHECK_EQ(h.cpu.r.pl, HPL);
	CHECK_EQ(h.cpu.r.p, HB + 0x100);

	// A stack marker was pushed and Q now points at it.
	CHECK_EQ(h.cpu.r.q, q0 + 16);
	CHECK_EQ(h.cpu.word_below_pub(h.cpu.r.q, 0), 16);     // delta Q

	// Only the mode bit survives in STATUS; the code segment number
	// becomes 1, the handler segment now executing -- the old CSN is in
	// the marker, and EXIT compares the marker against the running CSN
	// to decide whether to switch segments back.
	CHECK(h.cpu.r.test(sf::M));
	CHECK_EQ(h.cpu.r.get(sf::CSN), 1);
	CHECK(!h.cpu.r.test(sf::C));

	// A bounds violation vectors through its own STT entry.
	h.cpu.r.pb = 0x1000; h.cpu.r.pl = 0x1ffc;
	h.cpu.r.dl = h.cpu.r.db + 0x3f;
	h.cpu.push(1);
	CHECK(h.one(f1(5, false, false, 2, 255)) == exec_trap::none);
	CHECK(h.cpu.last_trap() == trap_cause::bounds_violation);
	CHECK_EQ(h.cpu.r.p, HB + 0x200);

	// The debug aids raise their own causes once enabled in STATUS.
	h.cpu.r.pb = 0x1000; h.cpu.r.pl = 0x1ffc;
	h.mem.write32(HPL - 4 * 044, 0x300);
	h.cpu.r.put(sf::SL, 1);
	CHECK(h.one(f5(000, 0)) == exec_trap::none);           // SOL
	CHECK(h.cpu.last_trap() == trap_cause::start_of_line);
	CHECK_EQ(h.cpu.r.p, HB + 0x300);

	// The cause values are the STT entry numbers, in octal.
	CHECK_EQ(unsigned(trap_cause::bounds_violation), 001u);
	CHECK_EQ(unsigned(trap_cause::unimplemented_instruction), 020u);
	CHECK_EQ(unsigned(trap_cause::data_segment_write), 057u);
	CHECK_EQ(unsigned(trap_cause::absent_page), 040u);
}

static void test_mvb()
{
	std::printf("MVB move bytes\n");
	harness h;

	// Source in the global data segment, target further along it. Both
	// pointers are global data segment pointers, tagged 10.
	for (int i = 0; i < 8; i++)
		h.mem.write8(h.cpu.r.db + 0x40 + i, u8(0x10 + i));

	h.cpu.push(0x80000000u | 0x40);      // source pointer
	h.cpu.push(0x80000000u | 0x80);      // target pointer
	h.cpu.push(8);                       // byte count
	// operand: SDEC = 3 words popped, source is a data segment pointer.
	CHECK(h.one(u16((051 << 6) | (1 << 2) | 3)) == exec_trap::none);

	for (int i = 0; i < 8; i++)
		CHECK_EQ(h.mem.read8(h.cpu.r.db + 0x80 + i), 0x10 + i);
	CHECK_EQ(h.cpu.r.x, 8);

	// Descending: bit 3 of the instruction reverses the direction, so the
	// pointers name the highest byte of each area.
	h.cpu.push(0x80000000u | 0x47);
	h.cpu.push(0x80000000u | 0xc7);
	h.cpu.push(8);
	CHECK(h.one(u16(0x1000 | (051 << 6) | (1 << 2) | 3)) == exec_trap::none);
	for (int i = 0; i < 8; i++)
		CHECK_EQ(h.mem.read8(h.cpu.r.db + 0xc0 + i), 0x10 + i);
}

// An MVB into a paged data segment that runs into an absent page must be
// resumable, and end exactly as an uninterrupted one does. HP-UX's exec
// loader copies each 1 KB file block into a process's paged data segment
// with one MVB; the kernel maps the page the copy trips over and returns to
// the instruction. The machine's mechanism is the Instruction Pending bit
// (ERS sheet 11): the interrupted instruction leaves X = the bytes done and
// STATUS.P set, the trap's marker carries both, the EXIT restores them, and
// the re-executed MVB goes on from X with the stack as the program wrote
// it. (With the count alone updated, the remainder was copied to the
// original target again and init lost the C library's character-class
// table; with the stack words rewritten instead, X came back short and the
// pointers an SDEC < 3 keeps came back changed.)
static void test_mvb_resume()
{
	std::printf("MVB resume across an absent page\n");
	harness h;
	paged_segment(h, false);

	// 1 KB of source in the global segment; the target starts 256 bytes
	// before the page boundary.
	for (u32 i = 0; i < 1024; i++)
		h.mem.write8(h.cpu.r.db + 0x100 + i, u8(i * 7 + 3));
	const u32 tptr = pg_ptr(0x300);
	const u32 sptr = 0x80000000u | 0x100;

	const u32 s0 = h.cpu.r.s;
	h.cpu.push(sptr);
	h.cpu.push(tptr);
	h.cpu.push(1024);
	const u16 insn = u16((051 << 6) | (1 << 2) | 3);
	const u32 top = h.cpu.r.s;       // S with the three operands on it
	h.cpu.r.x = 0x5555;              // HP's code does not clear X first
	CHECK(h.one(insn) == exec_trap::address_error);
	CHECK(h.cpu.last_addr_trap() == trap::absent_page);
	// The state the instruction left for its continuation: X the bytes
	// done, the pending bit, the stack untouched (word k below the top is
	// at S-3-4k).
	CHECK_EQ(h.cpu.r.x, 256);
	CHECK(h.cpu.r.test(sf::P));
	CHECK_EQ(h.cpu.r.s, top);
	CHECK_EQ(h.mem.read32(top - 3), 1024);
	CHECK_EQ(h.mem.read32(top - 7), tptr);
	CHECK_EQ(h.mem.read32(top - 11), sptr);
	for (u32 i = 0; i < 256; i++)
		CHECK_EQ(h.mem.read8(0x30300 + i), u8(i * 7 + 3));

	// The kernel maps the page and returns to the instruction; the
	// registers here are what its EXIT restores from the marker.
	page1_present(h, true);
	CHECK(h.one(insn) == exec_trap::none);
	CHECK_EQ(h.cpu.r.x, 1024);       // the whole count, as if never interrupted
	CHECK(!h.cpu.r.test(sf::P));     // nothing pending any more
	for (u32 i = 0; i < 256; i++)
		CHECK_EQ(h.mem.read8(0x30300 + i), u8(i * 7 + 3));
	for (u32 i = 256; i < 1024; i++)
		CHECK_EQ(h.mem.read8(0x31000 + i - 256), u8(i * 7 + 3));
	CHECK_EQ(h.cpu.r.s, s0);         // SDEC 3 popped the operands

	// A first execution is not a continuation: without the pending bit
	// the move starts at zero, whatever X held.
	h.cpu.r.x = 700;
	h.cpu.push(sptr); h.cpu.push(tptr); h.cpu.push(8);
	CHECK(h.one(insn) == exec_trap::none);
	CHECK_EQ(h.cpu.r.x, 8);
	CHECK_EQ(h.cpu.r.s, s0);

	// Nor is an MVB that merely follows an instruction which was handed a
	// pending STATUS: that instruction completing ends the pending state,
	// so a stale X is not taken for progress.
	// (An X below the count, which the plausibility test cannot catch: a
	// resumed move would skip the first three bytes.)
	h.cpu.r.x = 3;
	h.cpu.r.put(sf::P, 1);
	CHECK(h.one(f3(0)) == exec_trap::none);       // NOP
	CHECK(!h.cpu.r.test(sf::P));
	CHECK_EQ(h.cpu.r.x, 3);                       // X itself untouched
	h.cpu.push(sptr); h.cpu.push(pg_ptr(0x200)); h.cpu.push(8);
	CHECK(h.one(insn) == exec_trap::none);
	CHECK_EQ(h.cpu.r.x, 8);
	for (u32 i = 0; i < 8; i++)
		CHECK_EQ(h.mem.read8(pg_phys(0x200 + i)), u8(i * 7 + 3));
	CHECK_EQ(h.cpu.r.s, s0);
}

// The same, through the real trap sequence and with SDEC 0, so every word
// the program left on the stack is visible afterwards: the marker carries
// STATUS.P and X to the handler's EXIT, and the finished move leaves the
// count word reduced by X, both pointers as pushed, and X the whole count --
// identical to a run in which the page was present all along.
static void test_mvb_restart_round_trip()
{
	std::printf("MVB restart through trap and EXIT (SDEC 0)\n");
	auto run = [](bool fault, u32 &x, u32 w[3], unsigned &traps) {
		harness h;
		paged_segment(h, !fault);
		restart_handler(h);
		traps = 0;
		h.cpu.on_trap = [&](trap_cause, u32) { traps++; };
		for (u32 i = 0; i < 1024; i++)
			h.mem.write8(h.cpu.r.db + 0x100 + i, u8(i * 7 + 3));
		h.cpu.r.x = 0x1234;
		h.cpu.push(0x80000000u | 0x100);
		h.cpu.push(pg_ptr(0x300));
		h.cpu.push(1024);
		const u32 top = h.cpu.r.s;
		h.put(0x1000, u16((051 << 6) | (1 << 2) | 0));
		h.put(0x1002, f3(0));                  // NOP: where a finished MVB goes
		h.cpu.r.p = 0x1000;
		for (int n = 0; n < 4 && h.cpu.r.p != 0x1002; n++)
		{
			if (h.cpu.r.p == RT_HB + 0x100)
				page1_present(h, true);         // the "pager"
			CHECK(h.cpu.step() == exec_trap::none);
		}
		CHECK_EQ(h.cpu.r.p, 0x1002);
		CHECK_EQ(h.cpu.r.s, top);               // SDEC 0: nothing popped
		CHECK(!h.cpu.r.test(sf::P));
		x = h.cpu.r.x;
		w[0] = h.cpu.stk(0); w[1] = h.cpu.stk(1); w[2] = h.cpu.stk(2);
		for (u32 i = 0; i < 1024; i += 37)
			CHECK_EQ(h.mem.read8(pg_phys(0x300 + i)), u8(i * 7 + 3));
	};
	u32 xa, xb, wa[3], wb[3];
	unsigned ta, tb;
	run(false, xa, wa, ta);
	run(true, xb, wb, tb);
	CHECK_EQ(ta, 0);
	CHECK_EQ(tb, 1);                 // it did fault, once
	CHECK_EQ(xa, 1024);
	CHECK_EQ(xb, xa);                // X = elements processed (ERS sheet 101)
	CHECK_EQ(wb[0], wa[0]);          // count word: (S) := (S) - D*X
	CHECK_EQ(wa[0], 0);
	CHECK_EQ(wb[1], wa[1]);          // target pointer as pushed
	CHECK_EQ(wa[1], pg_ptr(0x300));
	CHECK_EQ(wb[2], wa[2]);          // source pointer as pushed
}

// Runs the 16-bit instruction at 0x1000 through as many absent-page traps
// as it takes, `pager` making pages present each time the handler is
// entered, until it completes (P reaches 0x1002). Returns the traps taken.
static unsigned run_through_faults(harness &h, u16 insn,
		const std::function<void()> &pager)
{
	unsigned traps = 0;
	h.cpu.on_trap = [&](trap_cause, u32) { traps++; };
	h.put(0x1000, insn);
	h.put(0x1002, f3(0));
	h.cpu.r.p = 0x1000;
	for (int n = 0; n < 16 && h.cpu.r.p != 0x1002; n++)
	{
		if (h.cpu.r.p == RT_HB + 0x100)
			pager();
		CHECK(h.cpu.step() == exec_trap::none);
	}
	CHECK_EQ(h.cpu.r.p, 0x1002);
	CHECK(!h.cpu.r.test(sf::P));
	h.cpu.on_trap = nullptr;
	return traps;
}

// SLD, SAS and SCON end by executing an MVB on parameters they stacked in
// place of their descriptors. A fault in that move used to send them back
// to their own beginning, where they took the MVB parameters for a
// descriptor (a String Trap, or a garbage temp). Now the instruction is
// pending from the move on and finishes it (see string_move). TRAN
// translates in place and must not put any byte through the table twice.
static void test_string_restart()
{
	std::printf("SLD, SCON, SAS and TRAN restart after an absent page\n");
	auto pager = [](harness &h) { return [&h]() { page1_present(h, true); }; };

	{   // SLD: a 300-byte string whose data runs into the absent page 1.
		harness h;
		paged_segment(h, false);
		restart_handler(h);
		h.mem.write32(pg_phys(0x300), 300);
		for (u32 i = 0; i < 300; i++)
			h.mem.write8(pg_phys(0x304 + i), u8(i + 1));
		h.cpu.push(0x11111111);
		h.cpu.push(400); h.cpu.push(pg_ptr(0x300)); h.cpu.push(1); h.cpu.push(300);
		const u32 tempadr = h.cpu.r.s - 15;
		CHECK_EQ(run_through_faults(h, f2(025, 4), pager(h)), 1);
		CHECK_EQ(h.mem.read32(tempadr), 300);
		CHECK_EQ(h.mem.read8(tempadr + 4), 1);
		CHECK_EQ(h.mem.read8(tempadr + 4 + 299), u8(300));
		CHECK_EQ(h.cpu.stk(0), -1);
		CHECK_EQ(h.cpu.stk(1), 1);
		CHECK_EQ(h.cpu.stk(3), 300);
		CHECK_EQ(h.cpu.stk(4), 76);                 // NWORDS = (300+3)/4 + 1
		CHECK_EQ(h.cpu.r.s, tempadr + 76 * 4 + 19);
		CHECK_EQ(h.mem.read32(tempadr - 4), 0x11111111);
		CHECK_EQ(h.cpu.r.x, 300);
	}
	{   // SCON: "AB" in a temp, the appended 300 bytes cross the boundary.
		harness h;
		paged_segment(h, true);
		restart_handler(h);
		h.mem.write32(pg_phys(0x300), 300);
		for (u32 i = 0; i < 300; i++)
			h.mem.write8(pg_phys(0x304 + i), u8(i + 1));
		const u32 gadr = h.cpu.r.db + 0x100;
		h.mem.write32(gadr, 2); h.mem.write8(gadr + 4, 'A'); h.mem.write8(gadr + 5, 'B');
		h.cpu.push(0x11111111);
		h.cpu.push(10); h.cpu.push((2u << 30) | 0x100); h.cpu.push(1); h.cpu.push(2);
		const u32 tempadr = h.cpu.r.s - 15;
		CHECK(h.one(f2(025, 4)) == exec_trap::none);        // SLD -> temp
		h.cpu.push(400); h.cpu.push(pg_ptr(0x300)); h.cpu.push(1); h.cpu.push(300);
		page1_present(h, false);
		CHECK_EQ(run_through_faults(h, f2(027, 4 | 2), pager(h)), 1);
		CHECK_EQ(h.mem.read32(tempadr), 302);
		CHECK_EQ(h.mem.read8(tempadr + 4), 'A');
		CHECK_EQ(h.mem.read8(tempadr + 5), 'B');
		CHECK_EQ(h.mem.read8(tempadr + 6), 1);
		CHECK_EQ(h.mem.read8(tempadr + 6 + 299), u8(300));
		CHECK_EQ(h.cpu.stk(0), -1);
		CHECK_EQ(h.cpu.stk(3), 302);
		CHECK_EQ(h.mem.read32(tempadr - 4), 0x11111111);
	}
	{   // SAS forwards: the target variable runs into the absent page.
		harness h;
		paged_segment(h, false);
		restart_handler(h);
		const u32 sadr = h.cpu.r.db + 0x100;
		h.mem.write32(sadr, 200);
		for (u32 i = 0; i < 200; i++)
			h.mem.write8(sadr + 4 + i, u8(i + 1));
		h.mem.write32(pg_phys(0x380), 0);
		h.cpu.push(0x11111111);
		h.cpu.push(400); h.cpu.push(pg_ptr(0x380)); h.cpu.push(1); h.cpu.push(u32(-1));
		h.cpu.push(200); h.cpu.push((2u << 30) | 0x100); h.cpu.push(1); h.cpu.push(200);
		CHECK_EQ(run_through_faults(h, f2(030, 4 | 2), pager(h)), 1);
		CHECK_EQ(h.mem.read32(pg_phys(0x380)), 200);
		CHECK_EQ(h.mem.read8(pg_phys(0x384)), 1);
		CHECK_EQ(h.mem.read8(pg_phys(0x384 + 199)), u8(200));
		CHECK_EQ(h.cpu.stk(0), 0x11111111);
		CHECK_EQ(h.cpu.r.condition_code(), CCG);
	}
	{   // SAS backwards -- a string assigned to a later part of itself --
		// faulting mid-move: the continuation must keep running DOWN,
		// which it learns from the condition code SAS set. Three pages:
		// the string's length word in page 0, its data across pages 1
		// and 2; page 1 absent, so the descending move faults part way.
		harness h;
		paged_segment(h, false);
		restart_handler(h);
		const u32 FRAME2 = 0x33000;
		h.mem.write32(PG_PTAB + 16, ers32::set(0, 27, 1, 1));
		h.mem.write32(PG_PTAB + 20, FRAME2);
		auto phys3 = [&](u32 off) {
			return off < 0x400 ? PG_FRAME0 + off
					: off < 0x800 ? PG_FRAME1 + off - 0x400 : FRAME2 + off - 0x800;
		};
		const u32 S0 = 0x3f0, LEN = 0x900;
		h.mem.write32(phys3(S0), LEN);
		for (u32 i = 0; i < LEN; i++)
			h.mem.write8(phys3(S0 + 4 + i), u8(i * 3 + 1));
		// target: bytes 0x101.. (0x600 of them); source: bytes 1..0x600.
		h.cpu.push(0xa00); h.cpu.push(pg_ptr(S0)); h.cpu.push(0x101); h.cpu.push(0x600);
		h.cpu.push(0xa00); h.cpu.push(pg_ptr(S0)); h.cpu.push(1); h.cpu.push(0x600);
		const u32 s_desc = h.cpu.r.s;
		CHECK_EQ(run_through_faults(h, f2(030, 4), pager(h)), 1);
		for (u32 i = 0; i < 0x600; i += 17)
			CHECK_EQ(h.mem.read8(phys3(S0 + 4 + 0x100 + i)), u8(i * 3 + 1));
		CHECK_EQ(h.mem.read8(phys3(S0 + 4 + 0x6ff)), u8(0x5ff * 3 + 1));
		CHECK_EQ(h.mem.read8(phys3(S0 + 4 + 0xff)), u8(0xff * 3 + 1));  // untouched
		CHECK_EQ(h.cpu.r.condition_code(), CCL);
		CHECK_EQ(h.cpu.r.s, s_desc - 32);           // both descriptors gone
	}
	{   // TRAN in place across the absent page: each byte translated once.
		harness h;
		paged_segment(h, false);
		restart_handler(h);
		for (u32 i = 0; i < 256; i++)
			h.mem.write8(h.cpu.r.db + 0x100 + i, u8(i + 1));    // table[i] = i+1
		for (u32 i = 0; i < 64; i++)
			h.mem.write8(pg_phys(0x3e0 + i), u8(i));
		h.cpu.r.x = 0x7777;
		h.cpu.push((2u << 30) | 0x100);
		h.cpu.push(pg_ptr(0x3e0));
		h.cpu.push(64);
		const u32 top = h.cpu.r.s;
		CHECK_EQ(run_through_faults(h, f2(056, 4), pager(h)), 1);   // SDEC 0
		for (u32 i = 0; i < 64; i++)
			CHECK_EQ(h.mem.read8(pg_phys(0x3e0 + i)), u8(i + 1));
		CHECK_EQ(h.cpu.r.x, 64);
		CHECK_EQ(h.cpu.r.s, top);
		CHECK_EQ(h.cpu.stk(0), 0);
		CHECK_EQ(h.cpu.stk(1), pg_ptr(0x3e0));      // as pushed
	}
	{   // TRAN with its table straddling a page boundary, page 1 absent at
		// first: entries past the boundary come from page 1's own frame.
		harness h;
		paged_segment(h, false);
		restart_handler(h);
		for (u32 i = 0; i < 256; i++)
			h.mem.write8(pg_phys(0x380 + i), u8(255 - i));       // table[i] = 255-i
		h.mem.write32(PG_FRAME0 + 0x400, 0xbadbad00);           // the frame after page 0
		const u32 tgt = h.cpu.r.db + 0x40;
		h.mem.write8(tgt, 0x10); h.mem.write8(tgt + 1, 0xf0); h.mem.write8(tgt + 2, 0x7f);
		h.cpu.push(pg_ptr(0x380));
		h.cpu.push((2u << 30) | 0x40);
		h.cpu.push(3);
		CHECK_EQ(run_through_faults(h, f2(056, 4 | 3), pager(h)), 1);
		CHECK_EQ(h.mem.read8(tgt), 255 - 0x10);
		CHECK_EQ(h.mem.read8(tgt + 1), 255 - 0xf0);              // past the boundary
		CHECK_EQ(h.mem.read8(tgt + 2), 255 - 0x7f);
		CHECK_EQ(h.cpu.r.x, 3);
	}
}

// Records what the CPU sends over the MPB.
class mock_iop : public io_interface
{
public:
	target last{};
	u32 last_data = 0;
	u32 next_read = 0;
	bool answer = true;
	int writes = 0, reads = 0, controls = 0;

	bool io_write(const target &t, u32 data) override
	{ last = t; last_data = data; writes++; return answer; }
	bool io_read(const target &t, u32 &data) override
	{ last = t; data = next_read; reads++; return answer; }
	bool io_control(const target &t) override
	{ last = t; controls++; return answer; }
};

static void test_io()
{
	std::printf("I/O instructions\n");
	harness h;
	mock_iop iop;
	h.cpu.set_io(&iop);

	// Interface address: IC in bits 22-25, channel in 26-28, PA in 29-31.
	const u32 iface = ers32::set(ers32::set(ers32::set(0, 22, 4, 0xb),
			26, 3, 3), 29, 3, 5);

	// IOW pops the data word, then the interface address beneath it.
	h.cpu.push(iface);
	h.cpu.push(0xdeadbeef);
	CHECK(h.one(f5(017, 0x42)) == exec_trap::none);
	CHECK_EQ(iop.writes, 1);
	CHECK_EQ(iop.last.opcode, 0x42);
	CHECK_EQ(iop.last.ic, 0xb);
	CHECK_EQ(iop.last.channel, 3);
	CHECK_EQ(iop.last.pa, 5);
	CHECK_EQ(iop.last_data, s32(0xdeadbeefu));

	// IOR takes the interface address from the top and pushes the reply.
	iop.next_read = 0x1234;
	h.cpu.push(iface);
	CHECK(h.one(f5(020, 0x43)) == exec_trap::none);
	CHECK_EQ(iop.reads, 1);
	CHECK_EQ(h.cpu.stk(0), 0x1234);
	CHECK_EQ(h.cpu.r.condition_code(), CCG);

	// IOC sends no data.
	h.cpu.pop();
	h.cpu.push(iface);
	CHECK(h.one(f5(021, 0x44)) == exec_trap::none);
	CHECK_EQ(iop.controls, 1);
	CHECK_EQ(iop.last.opcode, 0x44);

	// An unanswered channel sets Message register bit 7, "illegal I/O
	// opcode", and raises a message trap.
	iop.answer = false;
	h.cpu.r.msg = 0;
	h.cpu.push(iface);
	CHECK(h.one(f5(021, 0x45)) == exec_trap::trapped);
	CHECK(h.cpu.last_trap() == trap_cause::message_trap);
	CHECK_EQ(ers32::get(h.cpu.r.msg, 7, 1), 1u);

	// With no IOP at all the same thing happens, so a machine without one
	// still behaves predictably.
	h.cpu.set_io(nullptr);
	h.cpu.r.msg = 0;
	h.cpu.push(iface);
	CHECK(h.one(f5(020, 0x46)) == exec_trap::trapped);
	CHECK_EQ(ers32::get(h.cpu.r.msg, 7, 1), 1u);
}

static void test_tcc()
{
	std::printf("TCC\n");
	harness h;

	h.cpu.r.put(sf::CC, CCE);
	CHECK(h.one(u16((037 << 6) | 2)) == exec_trap::none);   // mask = CCE
	CHECK_EQ(h.cpu.stk(0), 1);
	CHECK(h.one(u16((037 << 6) | 4)) == exec_trap::none);   // mask = CCG
	CHECK_EQ(h.cpu.stk(0), 0);
	CHECK(h.one(u16((037 << 6) | 0)) == exec_trap::none);   // mask = never
	CHECK_EQ(h.cpu.stk(0), 0);
	CHECK(h.one(u16((037 << 6) | 7)) == exec_trap::none);   // mask = always
	CHECK_EQ(h.cpu.stk(0), 1);
}


// LPP/LPPS, LIA/LIAS, PSLA, CPAA, CRB and MUC -- the system group the HP-UX
// 5.30 kernel needs. Frequencies quoted are static counts over that kernel.
static void test_system_group()
{
	std::printf("program pointers, semaphores and range checks\n");

	// ---- LPP / LPPS (ERS sheet 62) -----------------------------------
	{
		harness h;
		// The word at PL carries the STT length in its low 19 bits.
		h.mem.write32(h.cpu.r.pl, 64);

		// An entry whose bit 0 is set is already an external program
		// pointer and is pushed unchanged.
		h.mem.write32(h.cpu.r.pl - 8, 0x80000000u | 0x1234);
		CHECK(h.one(u32_insn(004, 0, 8)) == exec_trap::none);
		CHECK_EQ(h.cpu.stk(0), int32_t(0x80000000u | 0x1234));

		// A local pointer -- bit 0 clear -- is converted to an external one
		// naming this code segment and this STT offset.
		h.cpu.r.put(sf::CSN, 5);
		h.mem.write32(h.cpu.r.pl - 12, 0x000000ff);
		CHECK(h.one(u32_insn(004, 0, 12)) == exec_trap::none);
		const u32 v = h.cpu.stk(0);
		CHECK_EQ(ers32::get(v, 0, 1), 1);        // now external
		CHECK_EQ(ers32::get(v, 1, 19), 12);      // the STT offset
		CHECK_EQ(ers32::get(v, 20, 12), 5);      // the code segment

		// Past the end of the STT is a violation, not a wild read.
		CHECK(h.one(u32_insn(004, 0, 65)) == exec_trap::trapped);
		CHECK(h.cpu.last_trap() == trap_cause::stt_violation);

		// LPPS scales its 8-bit field by four, so operand 2 is offset 8.
		h.cpu.r.s = h.cpu.r.sb + 3;
		CHECK(h.one(f5(034, 2)) == exec_trap::none);
		CHECK_EQ(h.cpu.stk(0), int32_t(0x80000000u | 0x1234));
	}

	// ---- LIA / LIAS (ERS sheet 122) ----------------------------------
	{
		harness h;
		h.cpu.r.q = h.cpu.r.sb + 0x40 + 3;
		h.cpu.r.s = h.cpu.r.sb + 0x200 + 3;    // room for the chain walk

		// Level 0 names the current frame: a stack pointer to Q, plus the
		// byte offset.
		CHECK(h.one(u32_insn(005, 0, 16)) == exec_trap::none);
		u32 p = h.cpu.stk(0);
		CHECK_EQ(ers32::get(p, 0, 2), 3);                       // stack tag
		CHECK_EQ(ers32::get(p, 2, 30), (h.cpu.r.q - h.cpu.r.sb) + 16);

		// The sign of a 32-bit LIA offset is CIR(12:1), which is the low
		// bit of the BASE field.
		h.cpu.r.s = h.cpu.r.sb + 0x200 + 3;
		CHECK(h.one(u32_insn(005, 1, 4)) == exec_trap::none);
		p = h.cpu.stk(0);
		CHECK_EQ(ers32::get(p, 2, 30), (h.cpu.r.q - h.cpu.r.sb) - 4);

		// Level 1 follows one static link: evaluate the frame pointer,
		// then take "the word preceding the stack marker" (ERS sheet
		// 157) -- one word below the four-word marker, sixteen bytes
		// under the marker-top address. The level field is CIR(8:4),
		// bits 20-23, which u32_insn cannot reach.
		h.cpu.r.s = h.cpu.r.sb + 0x200 + 3;
		u32 parent = ers32::set(0, 0, 2, 3);
		parent = ers32::set(parent, 2, 30, 0x20);
		h.mem.write32(h.cpu.r.q - 16, parent);
		CHECK(h.one(u32_insn(005, 0, 8) | (1u << 20)) == exec_trap::none);
		CHECK_EQ(ers32::get(h.cpu.stk(0), 2, 30), 0x20 + 8);

		// LIAS packs level into CIR(24:2) and offset into CIR(26:6).
		h.cpu.r.s = h.cpu.r.sb + 0x200 + 3;
		CHECK(h.one(f5(025, (0 << 6) | 12)) == exec_trap::none);
		CHECK_EQ(ers32::get(h.cpu.stk(0), 2, 30),
				(h.cpu.r.q - h.cpu.r.sb) + 12);
	}

	// ---- PSLA (ERS sheet 118) ----------------------------------------
	{
		harness h;
		h.mem.write32(0x9000, 0x5555);
		h.cpu.push(0x9000);
		// Unprivileged it is not available at all.
		CHECK(h.one(f4(063)) == exec_trap::unimplemented);

		h.cpu.r.put(sf::M, 1);
		h.cpu.r.s = h.cpu.r.sb + 3;
		h.cpu.push(0x9000);
		CHECK(h.one(f4(063)) == exec_trap::none);
		CHECK_EQ(h.cpu.stk(0), 0x5555);              // old value returned
		CHECK_EQ(h.mem.read32(0x9000), -1);          // and the lock taken

		// A second attempt sees the lock held, which is the whole point.
		h.cpu.r.s = h.cpu.r.sb + 3;
		h.cpu.push(0x9000);
		CHECK(h.one(f4(063)) == exec_trap::none);
		CHECK_EQ(h.cpu.stk(0), -1);

		// Bit 3 indexes by X.
		h.mem.write32(0x9010, 0x77);
		h.cpu.r.x = 0x10;
		h.cpu.r.s = h.cpu.r.sb + 3;
		h.cpu.push(0x9000);
		CHECK(h.one(f4(063, true)) == exec_trap::none);
		CHECK_EQ(h.cpu.stk(0), 0x77);
		CHECK_EQ(h.mem.read32(0x9010), -1);
	}

	// ---- CPAA (ERS sheet 118) ----------------------------------------
	{
		harness h;
		h.cpu.r.s = h.cpu.r.sb + 0x100 + 3;    // the target must be in bounds
		// A stack segment pointer evaluates to SB + its offset.
		u32 ptr = ers32::set(0, 0, 2, 3);
		ptr = ers32::set(ptr, 2, 30, 0x24);
		h.cpu.push(ptr);
		CHECK(h.one(f4(066)) == exec_trap::none);
		CHECK_EQ(h.cpu.stk(0), int32_t(h.cpu.r.sb + 0x24));

		// A global one evaluates against DB.
		h.cpu.r.s = h.cpu.r.sb + 0x100 + 3;
		u32 g = ers32::set(0, 0, 2, 2);
		g = ers32::set(g, 2, 30, 0x30);
		h.cpu.push(g);
		CHECK(h.one(f4(066)) == exec_trap::none);
		CHECK_EQ(h.cpu.stk(0), int32_t(h.cpu.r.db + 0x30));
	}

	// ---- CRB (ERS sheet 93) ------------------------------------------
	{
		harness h;
		// value, lower bound, upper bound -- the value ends up on top.
		h.cpu.push(5); h.cpu.push(1); h.cpu.push(10);
		CHECK(h.one(f2(073, 4)) == exec_trap::none);
		CHECK_EQ(h.cpu.r.condition_code(), CCE);
		CHECK_EQ(h.cpu.stk(0), 5);                    // bounds popped
		CHECK_EQ(h.cpu.r.p, 0x1002);                  // no branch

		// The endpoints are inside the range.
		h.cpu.r.s = h.cpu.r.sb + 3;
		h.cpu.push(10); h.cpu.push(1); h.cpu.push(10);
		CHECK(h.one(f2(073, 4)) == exec_trap::none);
		CHECK_EQ(h.cpu.r.condition_code(), CCE);

		// Out of range with a branch field: CCG and a forward branch of
		// `operand` halfwords from P.
		h.cpu.r.s = h.cpu.r.sb + 3;
		h.cpu.push(99); h.cpu.push(1); h.cpu.push(10);
		CHECK(h.one(f2(073, 4)) == exec_trap::none);
		CHECK_EQ(h.cpu.r.get(sf::CC), CCG);
		CHECK_EQ(h.cpu.stk(0), 99);
		CHECK_EQ(h.cpu.r.p, 0x1000 + 2 * 4);

		// Opcode 77 is the backward form.
		h.cpu.r.s = h.cpu.r.sb + 3;
		h.cpu.push(99); h.cpu.push(1); h.cpu.push(10);
		CHECK(h.one(f2(077, 4)) == exec_trap::none);
		CHECK_EQ(h.cpu.r.p, 0x1000 - 2 * 4);

		// "The bounds parameters are popped from the stack if the
		// integer is within the range or if the branch is taken. They
		// are not popped for the trap case" (ERS sheet 93). With traps
		// disabled take_trap returns without touching the stack, so
		// what CRB itself did to it is what is left.
		h.cpu.r.s = h.cpu.r.sb + 3;
		h.cpu.push(99); h.cpu.push(1); h.cpu.push(10);
		{
			const u32 s_before = h.cpu.r.s;
			CHECK(h.one(f2(073, 0)) == exec_trap::trapped);
			CHECK(h.cpu.last_trap() == trap_cause::check_trap);
			CHECK_EQ(h.cpu.r.s, s_before);        // nothing popped
			CHECK_EQ(h.cpu.stk(0), 10);
			CHECK_EQ(h.cpu.stk(1), 1);
			CHECK_EQ(h.cpu.stk(2), 99);
			CHECK_EQ(h.cpu.r.get(sf::CC), CCG);
		}

		// A zero branch field means Check Trap, STT entry 2, with CIR as
		// the parameter (ERS sheet 46).
		h.mem.write32(0, 0x2000);                 // system CST
		h.mem.write32(0x2000 + 16 + 4, 0x0fff);   // entry 1 length-1
		h.mem.write32(0x2000 + 16 + 8, 0x1000);   // entry 1 location
		h.mem.write32(0x1ffc - 4 * 002, 0x40);    // its STT entry
		h.cpu.enable_traps(true);
		h.cpu.r.s = h.cpu.r.sb + 3;
		h.cpu.push(99); h.cpu.push(1); h.cpu.push(10);
		CHECK(h.one(f2(073, 0)) == exec_trap::none);
		CHECK(h.cpu.last_trap() == trap_cause::check_trap);
		CHECK_EQ(unsigned(trap_cause::check_trap), 002u);

		// CHEK raises the same cause, not a bounds violation.
		h.cpu.r.pb = 0x1000; h.cpu.r.pl = 0x1ffc;
		h.cpu.r.s = h.cpu.r.sb + 3;
		h.cpu.push(500);
		CHECK(h.one(u32_insn(002, 0, 0) | (10u << 19) | 20u) == exec_trap::none);
		CHECK(h.cpu.last_trap() == trap_cause::check_trap);

		// The bounds are CIR(8:5) and CIR(13:19), inclusive (sheet 128,
		// FOCUS.md 8.3) -- not a 10-bit and a 12-bit field, which only
		// coincide with them for [0, n < 4096] and [1, 0].
		auto chek = [&](s32 v, u32 lo, u32 hi) {
			h.cpu.r.pb = 0x1000; h.cpu.r.pl = 0x1ffc;
			h.cpu.r.s = h.cpu.r.sb + 3;
			h.cpu.push(u32(v));
			h.one(u32_insn(002, 0, 0) | (lo << 19) | hi);
			return h.cpu.r.pb == 0x1000 && h.cpu.r.p == 0x1004;   // passed
		};
		CHECK(chek(5, 1, 10));
		CHECK(chek(1, 1, 10));
		CHECK(chek(10, 1, 10));
		CHECK(!chek(11, 1, 10));
		CHECK(!chek(0, 1, 10));
		CHECK(chek(5000, 0, 32767));
		CHECK(chek(0x7ffff, 31, 0x7ffff));
		CHECK(!chek(30, 31, 0x7ffff));
		CHECK(!chek(-1, 0, 25));
		CHECK(chek(25, 0, 25));
		CHECK(!chek(0, 1, 0));                    // the always-trap idiom
		CHECK(!chek(1, 1, 0));
	}

	// ---- MUC (ERS sheet 127) -----------------------------------------
	{
		harness h;
		// A minimal user DST: entry 0 holds 16 * entry count, entry 1 is an
		// unpaged, present, writable segment.
		h.cpu.r.dst = 0x20000;
		h.mem.write32(0x20000, 16 * 4);
		const u32 e1 = 0x20000 + 16;
		u32 w0 = 0;
		w0 = ers32::set(w0, 1, 12, 7);     // use count
		w0 = ers32::set(w0, 27, 1, 1);     // writable
		h.mem.write32(e1, w0);
		h.mem.write32(e1 + 4, 0xff);       // length - 1
		h.mem.write32(e1 + 8, 0x30000);    // location

		// An external data segment pointer: bit 0 clear, bit 1 set to pick
		// the user DST, segment number in (2:11), offset in (13:19).
		u32 ptr = ers32::set(0, 1, 1, 1);
		ptr = ers32::set(ptr, 2, 11, 1);

		h.cpu.push(ptr);
		CHECK(h.one(f4(013, true)) == exec_trap::none);      // increment
		CHECK_EQ(h.cpu.r.get(sf::CC), CCG);
		CHECK_EQ(ers32::get(h.mem.read32(e1), 1, 12), 8);

		h.cpu.r.s = h.cpu.r.sb + 3;
		h.cpu.push(ptr);
		CHECK(h.one(f4(013)) == exec_trap::none);            // decrement
		CHECK_EQ(ers32::get(h.mem.read32(e1), 1, 12), 7);

		// An absent segment reports CCE rather than trapping, so the memory
		// manager can decide what to do about it.
		h.mem.write32(e1, ers32::set(w0, 31, 1, 1));
		h.cpu.r.s = h.cpu.r.sb + 3;
		h.cpu.push(ptr);
		CHECK(h.one(f4(013, true)) == exec_trap::none);
		CHECK_EQ(h.cpu.r.condition_code(), CCE);

		// Any other evaluation failure reports CCL. Segment 3 is past the
		// end of this table.
		h.mem.write32(e1, w0);
		h.cpu.r.s = h.cpu.r.sb + 3;
		u32 far_ptr = ers32::set(0, 1, 1, 1);
		far_ptr = ers32::set(far_ptr, 2, 11, 9);   // past the end of the table
		h.cpu.push(far_ptr);
		CHECK(h.one(f4(013, true)) == exec_trap::none);
		CHECK_EQ(h.cpu.r.condition_code(), CCL);
	}
}


// SCW, SCU, CMPB and TRAN -- the byte scan and compare group (ERS 103-106).
static void test_string_group()
{
	std::printf("byte scan, compare and translate\n");

	// Lay a string into the global data segment and point at it with a
	// global data segment pointer, which evaluates to DB + offset.
	auto gptr = [](u32 off) {
		u32 p = ers32::set(0, 0, 2, 2);
		return ers32::set(p, 2, 30, off);
	};

	// ---- SCW: scan while the byte equals the test character ----------
	{
		harness h;
		const char *str = "aaabZ";
		for (unsigned i = 0; i < 5; i++)
			h.mem.write8(h.cpu.r.db + 0x40 + i, u8(str[i]));

		// terminal 'Z' in (16:8), test 'a' in (24:8).
		u32 ctrl = ers32::set(0, 16, 8, 'Z');
		ctrl = ers32::set(ctrl, 24, 8, 'a');

		h.cpu.push(gptr(0x40));       // source
		h.cpu.push(ctrl);             // control word
		h.cpu.push(5);                // limit count
		CHECK(h.one(f2(052, 004)) == exec_trap::none);   // DS source, sdec 0

		// Three 'a's matched, then 'b' stopped it.
		CHECK_EQ(h.cpu.r.x, 3);
		CHECK_EQ(h.cpu.stk(0), 2);                       // count left
		const u32 out = h.cpu.stk(1);
		CHECK_EQ(ers32::get(out, 0, 8), 'b');            // last char seen
		CHECK_EQ(ers32::get(out, 8, 8), u32('b' ^ 'Z')); // XOR terminal
	}

	// ---- SCU: scan until either the test or terminal character -------
	{
		harness h;
		const char *str = "xxxZy";
		for (unsigned i = 0; i < 5; i++)
			h.mem.write8(h.cpu.r.db + 0x40 + i, u8(str[i]));

		u32 ctrl = ers32::set(0, 16, 8, 'Z');    // terminal
		ctrl = ers32::set(ctrl, 24, 8, 'q');     // test, absent from the string

		h.cpu.push(gptr(0x40));
		h.cpu.push(ctrl);
		h.cpu.push(5);
		CHECK(h.one(f2(053, 004)) == exec_trap::none);

		// Stops on the terminal character at index 3.
		CHECK_EQ(h.cpu.r.x, 3);
		CHECK_EQ(h.cpu.stk(0), 2);
		const u32 out = h.cpu.stk(1);
		CHECK_EQ(ers32::get(out, 0, 8), 'Z');
		CHECK_EQ(ers32::get(out, 8, 8), 0);      // Z XOR Z, so the caller
	}                                            // knows it was the terminal

	// ---- SCU running the count out -----------------------------------
	{
		harness h;
		for (unsigned i = 0; i < 4; i++)
			h.mem.write8(h.cpu.r.db + 0x40 + i, 'k');
		u32 ctrl = ers32::set(0, 16, 8, 'Z');
		ctrl = ers32::set(ctrl, 24, 8, 'q');
		h.cpu.push(gptr(0x40));
		h.cpu.push(ctrl);
		h.cpu.push(4);
		CHECK(h.one(f2(053, 004)) == exec_trap::none);
		CHECK_EQ(h.cpu.r.x, 4);
		CHECK_EQ(h.cpu.stk(0), 0);               // exhausted, nothing found
	}

	// ---- CMPB ---------------------------------------------------------
	{
		harness h;
		for (unsigned i = 0; i < 4; i++)
		{
			h.mem.write8(h.cpu.r.db + 0x40 + i, u8("abcd"[i]));
			h.mem.write8(h.cpu.r.db + 0x60 + i, u8("abcd"[i]));
		}
		h.cpu.push(gptr(0x40));      // source
		h.cpu.push(gptr(0x60));      // target
		h.cpu.push(4);
		CHECK(h.one(f2(054, 004)) == exec_trap::none);
		CHECK_EQ(h.cpu.r.x, 4);
		CHECK_EQ(h.cpu.stk(0), 0);
		CHECK_EQ(h.cpu.r.condition_code(), CCE);      // equal all the way

		// Source greater at the third byte.
		h.mem.write8(h.cpu.r.db + 0x40 + 2, 'z');
		h.cpu.r.s = h.cpu.r.sb + 3;
		h.cpu.push(gptr(0x40));
		h.cpu.push(gptr(0x60));
		h.cpu.push(4);
		CHECK(h.one(f2(054, 004)) == exec_trap::none);
		CHECK_EQ(h.cpu.r.x, 2);                  // two matched
		CHECK_EQ(h.cpu.stk(0), 2);
		CHECK_EQ(h.cpu.r.get(sf::CC), CCG);      // 'z' > 'c'

		// And the other way round.
		h.mem.write8(h.cpu.r.db + 0x40 + 2, 'a');
		h.cpu.r.s = h.cpu.r.sb + 3;
		h.cpu.push(gptr(0x40));
		h.cpu.push(gptr(0x60));
		h.cpu.push(4);
		CHECK(h.one(f2(054, 004)) == exec_trap::none);
		CHECK_EQ(h.cpu.r.condition_code(), CCL);      // 'a' < 'c'

		// A non-positive count compares equal and touches nothing.
		h.cpu.r.s = h.cpu.r.sb + 3;
		h.cpu.push(gptr(0x40));
		h.cpu.push(gptr(0x60));
		h.cpu.push(0);
		CHECK(h.one(f2(054, 004)) == exec_trap::none);
		CHECK_EQ(h.cpu.r.condition_code(), CCE);
	}

	// ---- TRAN ---------------------------------------------------------
	{
		harness h;
		// A table that upper-cases the lower-case letters.
		for (unsigned i = 0; i < 256; i++)
			h.mem.write8(h.cpu.r.db + 0x100 + i,
					u8((i >= 'a' && i <= 'z') ? i - 32 : i));
		const char *str = "hp-ux";
		for (unsigned i = 0; i < 5; i++)
			h.mem.write8(h.cpu.r.db + 0x40 + i, u8(str[i]));

		h.cpu.push(gptr(0x100));     // translation table
		h.cpu.push(gptr(0x40));      // the string, translated in place
		h.cpu.push(5);
		CHECK(h.one(f2(056, 004)) == exec_trap::none);
		CHECK_EQ(h.cpu.r.x, 5);
		CHECK_EQ(h.cpu.stk(0), 0);
		for (unsigned i = 0; i < 5; i++)
			CHECK_EQ(h.mem.read8(h.cpu.r.db + 0x40 + i), u8("HP-UX"[i]));
	}

	// ---- SDEC pops the parameters, and bit 3 reverses direction -------
	{
		harness h;
		for (unsigned i = 0; i < 4; i++)
			h.mem.write8(h.cpu.r.db + 0x40 + i, 'm');
		u32 ctrl = ers32::set(0, 24, 8, 'm');
		const u32 s0 = h.cpu.r.s;
		h.cpu.push(gptr(0x40));
		h.cpu.push(ctrl);
		h.cpu.push(4);
		// sdec 3 pops all three parameter words.
		CHECK(h.one(f2(052, 004 | 3)) == exec_trap::none);
		CHECK_EQ(h.cpu.r.s, s0);

		// Backward: start at the top of the run and walk down.
		h.cpu.push(gptr(0x43));
		h.cpu.push(ctrl);
		h.cpu.push(4);
		CHECK(h.one(f2(052, 004, true)) == exec_trap::none);
		CHECK_EQ(s32(h.cpu.r.x), -4);
		CHECK_EQ(h.cpu.stk(0), 0);
	}
}

// SVAL and the descriptor checks every string instruction is specified in
// terms of (ERS sheet 107). A descriptor is four stack words -- maximum
// length, pointer, index of the first byte of interest, count of bytes of
// interest -- and the string keeps its current length in the word the
// pointer addresses. Here the pointer is a local program pointer, which is
// the CIR(29:1)=0 case, so no data segment table is needed.
static void test_string_descriptors()
{
	std::printf("string descriptors (SVAL)\n");
	harness h;

	constexpr u32 SOFF = 0x800;                        // PB-relative
	const u32 sadr = h.cpu.r.pb + SOFF;
	const u32 base = h.cpu.r.s;

	auto descriptor = [&](s32 maxlen, u32 ptr, s32 first, s32 len) {
		h.cpu.r.s = base;
		h.cpu.push(u32(maxlen));
		h.cpu.push(ptr);
		h.cpu.push(u32(first));
		h.cpu.push(u32(len));
	};

	h.mem.write32(sadr, 10);                           // current length

	// Operand bit 4 is CIR(27:1), which asks for the actual length to be
	// written back over the count of bytes of interest.
	constexpr u8 RET = 0x10;

	// A wholly valid descriptor passes and reports the length unchanged.
	descriptor(20, SOFF, 1, 5);
	CHECK(h.one(f2(026, RET)) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(0), 5);

	// The two low bits of the pointer are forced to zero in place.
	descriptor(20, SOFF | 3, 1, 5);
	CHECK(h.one(f2(026, RET)) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(2), s32(SOFF));

	// A negative count means "as far as the string goes", but only when
	// bit 3 of the instruction permits it: 10 - 1 + 1 = 10 bytes remain.
	descriptor(20, SOFF, 1, -1);
	CHECK(h.one(f2(026, RET, true)) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(0), 10);

	// The count is clamped to what is left rather than what was asked
	// for: from byte 8 of a ten-byte string only three bytes remain.
	descriptor(20, SOFF, 8, 10);
	CHECK(h.one(f2(026, RET)) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(0), 3);

	// Without the option bit the descriptor is checked but left alone.
	descriptor(20, SOFF, 8, 10);
	CHECK(h.one(f2(026, 0)) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(0), 10);

	// Each check in turn, by the error numbers of section 5.2.2.
	struct { s32 maxlen, first, len, curlen; const char *why; } bad[] = {
		{ -1, 1,  5, 10, "1: negative maximum length" },
		{ 20, 1, -1, 10, "2: unexpectedly negative count" },
		{ 20, 0,  5, 10, "3: first byte index not positive" },
		{ 20, 1, 25, 10, "4: substring runs past the maximum length" },
		{ 20, 1,  5, -1, "5: negative current length" },
		{ 20, 1,  5, 25, "6: current length exceeds the maximum" },
		{ 20, 8,  5,  3, "7: first byte beyond the current string" },
	};
	for (const auto &c : bad)
	{
		h.mem.write32(sadr, u32(c.curlen));
		descriptor(c.maxlen, SOFF, c.first, c.len);
		CHECK(h.one(f2(026, RET)) == exec_trap::trapped);
		CHECK(h.cpu.last_trap() == trap_cause::string_trap);
	}
	h.mem.write32(sadr, 10);

	// With a handler installed the trap carries a parameter word: the
	// error number in bits 12-15, the source/target flag in bit 11 and
	// the instruction itself in bits 16-31 (ERS sheet 47).
	const u32 CST = 0x200, HB = 0x2000, HLEN = 0x400;
	h.mem.write32(0, CST);
	h.mem.write32(CST + 16 + 0, 0);
	h.mem.write32(CST + 16 + 4, HLEN - 1);
	h.mem.write32(CST + 16 + 8, HB);
	h.mem.write32(HB + (HLEN - 1) - 3 - 4 * 005, 0x100);   // string trap
	h.cpu.enable_traps(true);

	const u16 insn = f2(026, RET);
	descriptor(-1, SOFF, 1, 5);                        // error 1, source
	CHECK(h.one(insn) == exec_trap::none);
	CHECK(h.cpu.last_trap() == trap_cause::string_trap);
	CHECK_EQ(h.cpu.stk(0), s32((1u << 16) | insn));
}

// SLD (ERS sheet 108): the descriptor on top of the stack is replaced by a
// string temp -- the substring's bytes, a size word, and a descriptor naming
// them -- built where the old descriptor stood.
static void test_string_load()
{
	std::printf("string load (SLD)\n");
	harness h;

	constexpr u32 SOFF = 0x800;
	const u32 sadr = h.cpu.r.pb + SOFF;

	h.mem.write32(sadr, 10);                           // current length
	for (int i = 0; i < 10; i++)
		h.mem.write8(sadr + 4 + u32(i), u8('A' + i));  // "ABCDEFGHIJ"

	const u32 base = h.cpu.r.s;
	h.cpu.push(20);                                    // maximum length
	h.cpu.push(SOFF);                                  // pointer
	h.cpu.push(3);                                     // first byte of interest
	h.cpu.push(4);                                     // bytes of interest

	const u32 tempadr = h.cpu.r.s - 15;
	CHECK(h.one(f2(025, 0)) == exec_trap::none);

	// One word of length and one holding the four bytes.
	CHECK_EQ(h.cpu.r.s, s32(tempadr + 2 * 4 + 19));

	CHECK_EQ(h.mem.read32(tempadr), 4);                // the temp's length
	CHECK_EQ(h.mem.read8(tempadr + 4), 'C');
	CHECK_EQ(h.mem.read8(tempadr + 5), 'D');
	CHECK_EQ(h.mem.read8(tempadr + 6), 'E');
	CHECK_EQ(h.mem.read8(tempadr + 7), 'F');

	// Above the data sit the size word and a descriptor over the whole of
	// it: maximum length 4, an SB-relative pointer, first byte 1, and a
	// negative count meaning "as far as it goes".
	CHECK_EQ(h.cpu.stk(4), 2);
	CHECK_EQ(h.cpu.stk(3), 4);
	CHECK_EQ(h.cpu.stk(2), s32((3u << 30) | (tempadr - h.cpu.r.sb)));
	CHECK_EQ(h.cpu.stk(1), 1);
	CHECK_EQ(h.cpu.stk(0), -1);

	// The source string is not modified.
	CHECK_EQ(h.mem.read32(sadr), 10);
	CHECK_EQ(h.mem.read8(sadr + 4), 'A');

	// A negative count loads to the end: from byte 8 of ten, three bytes.
	h.cpu.r.s = base;
	h.cpu.push(20); h.cpu.push(SOFF); h.cpu.push(8); h.cpu.push(u32(-1));
	const u32 t2 = h.cpu.r.s - 15;
	CHECK(h.one(f2(025, 0, true)) == exec_trap::none);
	CHECK_EQ(h.mem.read32(t2), 3);
	CHECK_EQ(h.mem.read8(t2 + 4), 'H');
	CHECK_EQ(h.mem.read8(t2 + 6), 'J');

	// An invalid descriptor is rejected before anything is built.
	h.cpu.r.s = base;
	h.cpu.push(20); h.cpu.push(SOFF); h.cpu.push(0); h.cpu.push(4);
	CHECK(h.one(f2(025, 0)) == exec_trap::trapped);
	CHECK(h.cpu.last_trap() == trap_cause::string_trap);
}

// SAS (ERS sheets 109-111). Both strings live in the global data segment,
// so both descriptors carry data segment pointers.
static void test_string_assign()
{
	std::printf("string assign (SAS)\n");
	harness h;

	constexpr u32 SRC = 0x100, TGT = 0x200;
	const u32 sadr = h.cpu.r.db + SRC, tadr = h.cpu.r.db + TGT;
	auto dsptr = [](u32 off) { return (2u << 30) | off; };

	// The target's own current length matters: a first byte of interest
	// must land within the target string or just past its end.
	auto setup = [&](s32 tcurlen) {
		const char *w = "HELLO";
		h.mem.write32(sadr, 5);
		for (int i = 0; i < 5; i++)
			h.mem.write8(sadr + 4 + u32(i), u8(w[i]));
		h.mem.write32(tadr, u32(tcurlen));
		for (int i = 0; i < 12; i++)
			h.mem.write8(tadr + 4 + u32(i), '.');
	};

	const u32 base = h.cpu.r.s;
	auto descriptors = [&](u32 tptr, s32 tfirst, s32 tlen,
			s32 sfirst, s32 slen) {
		h.cpu.r.s = base;
		h.cpu.push(10); h.cpu.push(tptr);
		h.cpu.push(u32(tfirst)); h.cpu.push(u32(tlen));
		h.cpu.push(10); h.cpu.push(dsptr(SRC));
		h.cpu.push(u32(sfirst)); h.cpu.push(u32(slen));
	};

	// Bit 2 says the source pointer is a data segment pointer, bit 1 that
	// the target's count of bytes of interest may be negative.
	constexpr u8 OP = 0x6;

	// A whole-string assignment into an empty target.
	setup(0);
	descriptors(dsptr(TGT), 1, -1, 1, 5);
	CHECK(h.one(f2(030, OP)) == exec_trap::none);
	CHECK_EQ(h.cpu.r.s, s32(base));                 // both descriptors gone
	CHECK_EQ(h.mem.read32(tadr), 5);                // the new current length
	CHECK_EQ(h.mem.read8(tadr + 4), 'H');
	CHECK_EQ(h.mem.read8(tadr + 8), 'O');
	CHECK_EQ(h.mem.read8(tadr + 9), '.');           // nothing written beyond
	CHECK_EQ(h.cpu.r.condition_code(), CCG);        // moved forwards

	// A substring of the source into a substring of the target: "ELL"
	// starting at the target's third byte.
	setup(5);
	descriptors(dsptr(TGT), 3, -1, 2, 3);
	CHECK(h.one(f2(030, OP)) == exec_trap::none);
	CHECK_EQ(h.mem.read32(tadr), 5);                // 3 + 3 - 1
	CHECK_EQ(h.mem.read8(tadr + 6), 'E');
	CHECK_EQ(h.mem.read8(tadr + 7), 'L');
	CHECK_EQ(h.mem.read8(tadr + 8), 'L');

	// With bit 3 set and a target substring longer than the source, the
	// parameters for a following FILB are left on the stack: the blank
	// itself, where to start, and how many.
	setup(5);
	descriptors(dsptr(TGT), 1, 8, 1, 5);
	CHECK(h.one(f2(030, OP | 8)) == exec_trap::none);
	CHECK_EQ(h.cpu.r.s, s32(base + 12));
	CHECK_EQ(h.cpu.stk(2), 32);
	CHECK_EQ(h.cpu.stk(1), s32(dsptr(TGT + 9)));
	CHECK_EQ(h.cpu.stk(0), 3);
	CHECK_EQ(h.mem.read32(tadr), 8);                // 1 + 8 - 1

	// Assigning a string over a later part of itself has to run backwards,
	// and says so in the condition code. "HEL" onto bytes 3-5 of "HELLO"
	// leaves "HEHEL".
	setup(5);
	descriptors(dsptr(SRC), 3, 3, 1, 3);
	CHECK(h.one(f2(030, OP)) == exec_trap::none);
	CHECK_EQ(h.cpu.r.condition_code(), CCL);
	CHECK_EQ(h.mem.read8(sadr + 4), 'H');
	CHECK_EQ(h.mem.read8(sadr + 5), 'E');
	CHECK_EQ(h.mem.read8(sadr + 6), 'H');
	CHECK_EQ(h.mem.read8(sadr + 7), 'E');
	CHECK_EQ(h.mem.read8(sadr + 8), 'L');

	// A source longer than a target that says how long it is: error 9,
	// raised against the target.
	setup(8);
	descriptors(dsptr(TGT), 9, -1, 1, 5);           // 9 + 5 - 1 > 10
	CHECK(h.one(f2(030, OP)) == exec_trap::trapped);
	CHECK(h.cpu.last_trap() == trap_cause::string_trap);
}

// SCON (ERS sheets 111-112). Built on a real temp: SLD makes one, then SCON
// appends a substring of the same source string to it.
static void test_string_concatenate()
{
	std::printf("string concatenate (SCON)\n");
	harness h;

	constexpr u32 SRC = 0x100;
	const u32 sadr = h.cpu.r.db + SRC;
	auto dsptr = [](u32 off) { return (2u << 30) | off; };

	h.mem.write32(sadr, 20);
	for (int i = 0; i < 20; i++)
		h.mem.write8(sadr + 4 + u32(i), u8('A' + i));   // "ABC...T"

	// A temp holding the whole of it.
	h.cpu.push(20); h.cpu.push(dsptr(SRC)); h.cpu.push(1); h.cpu.push(20);
	const u32 tempadr = h.cpu.r.s - 15;
	CHECK(h.one(f2(025, 4)) == exec_trap::none);        // SLD
	CHECK_EQ(h.mem.read32(tempadr), 20);

	// A source descriptor for "ABC" goes on top; the temp's own descriptor
	// is then the four words below it, which is the CIR(31:1)=0 case.
	h.cpu.push(20); h.cpu.push(dsptr(SRC)); h.cpu.push(1); h.cpu.push(3);
	const u32 s_before = h.cpu.r.s;

	CHECK(h.one(f2(027, 6)) == exec_trap::none);        // SCON

	// The result fitted in the space the operands occupied, so S is
	// unchanged and the temp simply claims all of it.
	CHECK_EQ(h.cpu.r.s, s32(s_before));
	CHECK_EQ(h.mem.read32(tempadr), 23);                // 20 + 3

	// The original twenty bytes, then the three appended ones.
	CHECK_EQ(h.mem.read8(tempadr + 4), 'A');
	CHECK_EQ(h.mem.read8(tempadr + 23), 'T');
	CHECK_EQ(h.mem.read8(tempadr + 24), 'A');
	CHECK_EQ(h.mem.read8(tempadr + 25), 'B');
	CHECK_EQ(h.mem.read8(tempadr + 26), 'C');

	// And a descriptor over the whole of the new temp.
	CHECK_EQ(h.cpu.stk(3), 23);
	CHECK_EQ(h.cpu.stk(2), s32((3u << 30) | (tempadr - h.cpu.r.sb)));
	CHECK_EQ(h.cpu.stk(1), 1);
	CHECK_EQ(h.cpu.stk(0), -1);

	// The source string is not modified.
	CHECK_EQ(h.mem.read32(sadr), 20);
	CHECK_EQ(h.mem.read8(sadr + 4), 'A');

	// Two SHORT temps, the shape BASIC's `"A"&"B"` puts on the stack: each
	// is a length word, one word of data, NWORDS and a descriptor, so the
	// target's length word sits thirteen words under S. The bound is
	// "TSTRINGADR > S-4" (ERS sheet 111) -- under the source descriptor --
	// and a reading of it as S-14 refused exactly this with trap 1.
	harness g;
	const u32 gadr = g.cpu.r.db + SRC;
	g.mem.write32(gadr, 2);
	g.mem.write8(gadr + 4, 'A');
	g.mem.write8(gadr + 5, 'B');
	g.cpu.push(2); g.cpu.push(dsptr(SRC)); g.cpu.push(1); g.cpu.push(1);
	const u32 ta = g.cpu.r.s - 15;
	CHECK(g.one(f2(025, 4)) == exec_trap::none);        // SLD "A"
	g.cpu.push(2); g.cpu.push(dsptr(SRC)); g.cpu.push(2); g.cpu.push(1);
	CHECK(g.one(f2(025, 4)) == exec_trap::none);        // SLD "B"
	const u32 s2 = g.cpu.r.s;
	CHECK_EQ(s2 - 3 - ta, 13 * 4);
	// Source is the temp on top (CIR(31:1)=1), both counts are -1.
	CHECK(g.one(f2(027, 7, true)) == exec_trap::none);  // SCON
	CHECK_EQ(g.mem.read32(ta), 2);
	CHECK_EQ(g.mem.read8(ta + 4), 'A');
	CHECK_EQ(g.mem.read8(ta + 5), 'B');
	CHECK_EQ(g.cpu.r.s, s32(s2));
	CHECK_EQ(g.cpu.stk(3), 2);
	CHECK_EQ(g.cpu.stk(2), s32((3u << 30) | (ta - g.cpu.r.sb)));
	CHECK_EQ(g.cpu.stk(0), -1);
}

// CPEP (ERS sheet 120): stack and global pointers become external data
// segment pointers, using the segment numbers held in the current TCB.
static void test_cpep()
{
	std::printf("convert pointer to external (CPEP)\n");
	harness h;

	constexpr u32 TCB = 0x600, STACK_SEG = 7, GLOBAL_SEG = 9;
	constexpr u32 SEG_BASE = 0x3000;               // where the stack segment starts

	// The dedicated block for this CPU points at the TCB, whose word 2
	// carries the stack segment number at (4:12) and the global at (20:12).
	// Bit 0 of each twelve-bit field selects the user table; left clear
	// here, so both name system DST entries.
	h.mem.write32(16 * ers32::get(h.cpu.r.mask, 3, 3) + 4, TCB);
	h.mem.write32(TCB + 8,
			ers32::set(ers32::set(0, 4, 12, STACK_SEG), 20, 12, GLOBAL_SEG));

	// The stack segment's DST entry, so SB can be rebased onto it.
	h.mem.write32(dst_walker::SYSTEM_DST_BASE + 16 * STACK_SEG + 8, SEG_BASE);

	// A stack pointer at SB+0x40 becomes an external pointer whose offset
	// is measured from the segment base instead: (SB - 0x3000) + 0x40.
	h.cpu.push((3u << 30) | 0x40);
	CHECK(h.one(f4(067)) == exec_trap::none);
	u32 v = h.cpu.stk(0);
	CHECK_EQ(ers32::get(v, 0, 1), 0);              // now external
	CHECK_EQ(ers32::get(v, 1, 12), STACK_SEG);
	CHECK_EQ(ers32::get(v, 13, 19), h.cpu.r.sb - SEG_BASE + 0x40);

	// A global pointer keeps its offset and takes the global segment.
	h.cpu.pop();
	h.cpu.push((2u << 30) | 0x80);
	CHECK(h.one(f4(067)) == exec_trap::none);
	v = h.cpu.stk(0);
	CHECK_EQ(ers32::get(v, 0, 1), 0);
	CHECK_EQ(ers32::get(v, 1, 12), GLOBAL_SEG);
	CHECK_EQ(ers32::get(v, 13, 19), 0x80);

	// With bit 3 set the index register is folded in first.
	h.cpu.pop();
	h.cpu.r.x = 0x10;
	h.cpu.push((2u << 30) | 0x80);
	CHECK(h.one(f4(067, true)) == exec_trap::none);
	CHECK_EQ(ers32::get(h.cpu.stk(0), 13, 19), 0x90);

	// An offset too large for the nineteen-bit field cannot be converted.
	h.cpu.pop();
	h.cpu.r.x = 0;
	h.cpu.push((2u << 30) | (1u << 19));
	CHECK(h.one(f4(067)) == exec_trap::pointer_conversion);

	// An external pointer is left alone unless indexing is asked for.
	h.cpu.pop();
	const u32 ext = ers32::set(0, 13, 19, 0x30);
	h.cpu.push(ext);
	CHECK(h.one(f4(067)) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(0), s32(ext));
	h.cpu.r.x = 4;
	CHECK(h.one(f4(067, true)) == exec_trap::none);
	CHECK_EQ(ers32::get(h.cpu.stk(0), 13, 19), 0x34);
}

// DLAD and DLSB (ERS sheet 84): eight packed BCD digits per word, with the
// carry chained through bit 3 so wider decimal numbers can be built up.
static void test_decimal()
{
	std::printf("packed decimal add and subtract\n");
	harness h;

	auto pair = [&](u32 lo, u32 hi) {          // (S-1) = lo, (S) = hi
		h.cpu.r.s = h.cpu.r.sb + 3;
		h.cpu.push(lo); h.cpu.push(hi);
	};

	// Straight decimal addition, no carry anywhere.
	pair(0x12345678, 0x11111111);
	CHECK(h.one(f3(011)) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(0), s32(0x23456789u));
	CHECK_EQ(h.cpu.r.condition_code(), CCG);
	CHECK_EQ(h.cpu.r.get(sf::C), 0);

	// Every digit carries, and the word rolls over to zero.
	pair(0x99999999, 0x00000001);
	CHECK(h.one(f3(011)) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(0), 0);
	CHECK_EQ(h.cpu.r.get(sf::C), 1);           // carry out of the top digit
	CHECK_EQ(h.cpu.r.condition_code(), CCE);   // zero, and bit 3 was clear

	// A single digit carrying into the next.
	pair(0x00000005, 0x00000005);
	CHECK(h.one(f3(011)) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(0), s32(0x00000010u));

	// With bit 3 the carry comes in, which is the second limb of a wider
	// addition. A zero result then leaves the condition code alone rather
	// than erasing what a previous limb reported.
	h.cpu.r.put(sf::C, 1);
	h.cpu.r.put(sf::CC, CCG);
	pair(0x00000001, 0x00000001);
	CHECK(h.one(f3(011, true)) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(0), s32(0x00000003u));

	h.cpu.r.put(sf::C, 0);
	h.cpu.r.put(sf::CC, CCG);
	pair(0, 0);
	CHECK(h.one(f3(011, true)) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(0), 0);
	CHECK_EQ(h.cpu.r.condition_code(), CCG);   // untouched

	// Subtraction. The ERS carries a borrow inverted, so no borrow leaves
	// Carry set.
	pair(0x23456789, 0x11111111);
	CHECK(h.one(f3(012)) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(0), s32(0x12345678u));
	CHECK_EQ(h.cpu.r.get(sf::C), 1);           // no borrow
	CHECK_EQ(h.cpu.r.condition_code(), CCG);

	// Borrowing all the way out of the top.
	pair(0x00000000, 0x00000001);
	CHECK(h.one(f3(012)) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(0), s32(0x99999999u));
	CHECK_EQ(h.cpu.r.get(sf::C), 0);           // borrow

	// Digit-by-digit borrow within the word.
	pair(0x00000100, 0x00000001);
	CHECK(h.one(f3(012)) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(0), s32(0x00000099u));

	// Chained subtraction takes 1-Carry as its borrow in.
	h.cpu.r.put(sf::C, 0);                     // a borrow is pending
	pair(0x00000005, 0x00000002);
	CHECK(h.one(f3(012, true)) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(0), s32(0x00000002u));
	h.cpu.r.put(sf::C, 1);                     // none pending
	pair(0x00000005, 0x00000002);
	CHECK(h.one(f3(012, true)) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(0), s32(0x00000003u));

	// A digit above nine is an illegal operand, not something to fix up.
	pair(0x1234567a, 0x00000001);
	CHECK(h.one(f3(011)) == exec_trap::trapped);
	CHECK(h.cpu.last_trap() == trap_cause::illegal_decimal);
	pair(0x00000001, 0x0000000f);
	CHECK(h.one(f3(012)) == exec_trap::trapped);
	CHECK(h.cpu.last_trap() == trap_cause::illegal_decimal);
}

// ISC (ERS sheet 87). The diagnostics compile it as FIXD,B / ISC / STH:
// check that a full word integer survives the trip into a halfword, then
// store it as one.
// BTD, DTB and POT: the binary/decimal conversions BASIC's number
// formatter runs on every real value it prints (ERS sheets 84-87).
// BASIC's numbers are three-word decimal floating point -- a ten-bit
// exponent and seventeen digits held as a binary integer -- and these are
// the three instructions that move between that and a double.
// EXF and DPF: extract and deposit a bit field (ERS sheets 73-74). The
// compiler leans on these to assemble instruction words, and the boot
// image drives DPF past the end of the word at six sites.
static void test_field_ops()
{
	std::printf("EXF and DPF (sheets 73-74)\n");
	harness h;

	// CIR(22:10) carries K in its high five bits and J in its low five,
	// where J is the leftmost bit and K is 32 minus the width.
	auto fields = [](u32 j, u32 k) { return (k << 5) | j; };

	// ---- EXF ---------------------------------------------------------
	// Bits J..J+L-1, right justified. J=8 K=24 -> the third byte.
	h.cpu.r.s = h.cpu.r.sb + 3;
	h.cpu.push(0x12345678);
	CHECK(h.one(u32_insn(000, 0, fields(8, 24))) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(0), 0x34u);

	// Running off the end reads zeros: J=28, width 8, so bits 28..31 are
	// the value and the four beyond are 0.
	h.cpu.r.s = h.cpu.r.sb + 3;
	h.cpu.push(0x0000000f);
	CHECK(h.one(u32_insn(000, 0, fields(28, 24))) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(0), 0xf0u);

	// ---- DPF ---------------------------------------------------------
	// The L low bits of the TOS land at J in the word beneath, the rest
	// of that word untouched.
	h.cpu.r.s = h.cpu.r.sb + 3;
	h.cpu.push(0xffffffff); h.cpu.push(0x00000034);
	CHECK(h.one(u32_insn(001, 0, fields(8, 24))) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(0), 0xff34ffffu);
	CHECK_EQ(h.cpu.r.s, h.cpu.r.sb + 3 + 4);        // the TOS is deleted

	// A whole-word deposit, K=0 so L=32.
	h.cpu.r.s = h.cpu.r.sb + 3;
	h.cpu.push(0x11111111); h.cpu.push(0xaabbccdd);
	CHECK(h.one(u32_insn(001, 0, fields(0, 0))) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(0), 0xaabbccddu);

	// "All bits greater than 31 are lost." J + L past the end truncates
	// the field there and keeps the leading source bits; computing the
	// shift as 32 - J - L and using it unguarded is undefined exactly
	// here. J=20 K=16 gives L=16 with only twelve bits of room, so the
	// top twelve of the source land in bits 20..31.
	h.cpu.r.s = h.cpu.r.sb + 3;
	h.cpu.push(0x00000000); h.cpu.push(0x0000abcd);
	CHECK(h.one(u32_insn(001, 0, fields(20, 16))) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(0), 0x00000abcu);

	// The image's other shape: J=2 K=1, so L=31 into thirty bits.
	h.cpu.r.s = h.cpu.r.sb + 3;
	h.cpu.push(0x00000000); h.cpu.push(0x7fffffff);
	CHECK(h.one(u32_insn(001, 0, fields(2, 1))) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(0), 0x3fffffffu);
}


static void test_decimal_conversion()
{
	std::printf("BTD, DTB and POT (sheets 84-87)\n");
	harness h;

	auto pushd = [&](double d) {
		std::uint64_t b; std::memcpy(&b, &d, 8);
		h.cpu.push(u32(b >> 32)); h.cpu.push(u32(b));
	};
	auto topd = [&]() {
		const std::uint64_t b = (std::uint64_t(h.cpu.stk(1)) << 32) | h.cpu.stk(0);
		double d; std::memcpy(&d, &b, 8); return d;
	};
	auto reset = [&]() { h.cpu.r.s = h.cpu.r.sb + 3; };

	// ---- BTD,B, the exponent estimate --------------------------------
	// I is chosen so that N/10^I lands in [1,10).
	struct { double n; int i; } est[] = {
		{ 4.0, 0 }, { 9.999, 0 }, { 10.0, 1 }, { 1234.5, 3 },
		{ 0.5, -1 }, { 0.001, -3 }, { -250.0, 2 },
	};
	for (const auto &e : est)
	{
		reset(); pushd(e.n);
		CHECK(h.one(f4(001, true)) == exec_trap::none);
		CHECK_EQ(s32(h.cpu.stk(0)), e.i);
		CHECK_EQ(h.cpu.r.s, h.cpu.r.sb + 3 + 4);   // two words in, one out
	}
	reset(); pushd(0.0);
	CHECK(h.one(f4(001, true)) == exec_trap::none);
	CHECK_EQ(s32(h.cpu.stk(0)), 0);
	// An infinity or a NaN has no exponent; converting log10 of one to an
	// integer is undefined behaviour in the host, so it is pinned to 0.
	for (double n : { HUGE_VAL, -HUGE_VAL, std::nan("") })
	{
		reset(); pushd(n);
		CHECK(h.one(f4(001, true)) == exec_trap::none);
		CHECK_EQ(s32(h.cpu.stk(0)), 0);
	}

	// ---- POT: A and B with A/B = 10^I --------------------------------
	for (int i : { 0, 1, -1, 22, -22, 307, -307 })
	{
		reset(); h.cpu.push(u32(i));
		CHECK(h.one(f4(053)) == exec_trap::none);
		CHECK_EQ(h.cpu.r.get(sf::CC), CCG);
		CHECK_EQ(h.cpu.r.s, h.cpu.r.sb + 3 + 4 * 4);   // one word in, four out
		const double b = topd();
		const std::uint64_t ab = (std::uint64_t(h.cpu.stk(3)) << 32) | h.cpu.stk(2);
		double a; std::memcpy(&a, &ab, 8);
		CHECK(std::fabs(a / b / std::pow(10.0, double(i)) - 1.0) < 1e-12);
	}
	// Outside -308 < I < 308 it is an Exponent Size Trap, and with traps
	// disabled take_trap returns without touching the stack.
	reset(); h.cpu.push(u32(400));
	CHECK(h.one(f4(053)) == exec_trap::trapped);
	CHECK(h.cpu.last_trap() == trap_cause::exponent_size);

	// ---- BTD then DTB: pack a value and read the digits back ---------
	// DTB hands back the twenty digits as a value, which is the
	// mantissa times ten to the nineteenth.
	for (double n : { 1.0, 4.0, 9.5, 3.25, 1.234567890123456 })
	{
		reset();
		h.cpu.push(0);                                  // I, the exponent
		pushd(n);
		CHECK(h.one(f4(001)) == exec_trap::none);       // BTD packs
		CHECK_EQ(h.cpu.r.get(sf::CC), CCE);
		CHECK_EQ(h.cpu.r.s, h.cpu.r.sb + 3 + 3 * 4);    // still three words
		CHECK(h.one(f4(000)) == exec_trap::none);       // DTB unpacks
		CHECK_EQ(h.cpu.r.s, h.cpu.r.sb + 3 + 2 * 4);    // three in, two out
		CHECK(std::fabs(topd() / (n * 1e19) - 1.0) < 1e-15);
	}

	// The exponent occupies (S-2)(0:10).
	reset(); h.cpu.push(3); pushd(4.0);
	CHECK(h.one(f4(001)) == exec_trap::none);
	CHECK_EQ((h.cpu.stk(2) >> 22) & 0x3ff, 3u);

	// Out of [1,10) nothing is packed: the value is dropped, the exponent
	// is corrected, and the caller rescales and comes back.
	reset(); h.cpu.push(5); pushd(0.5);
	CHECK(h.one(f4(001)) == exec_trap::none);
	CHECK_EQ(s32(h.cpu.stk(0)), 4);
	CHECK_EQ(h.cpu.r.get(sf::CC), CCL);

	reset(); h.cpu.push(5); pushd(50.0);
	CHECK(h.one(f4(001)) == exec_trap::none);
	CHECK_EQ(s32(h.cpu.stk(0)), 6);
	CHECK_EQ(h.cpu.r.get(sf::CC), CCG);

	// Zero packs to a three-word decimal zero and I is ignored.
	reset(); h.cpu.push(3); pushd(0.0);
	CHECK(h.one(f4(001)) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(0), 0u);
	CHECK_EQ(h.cpu.stk(1), 0u);
	CHECK_EQ(h.cpu.stk(2), 0u);
	CHECK_EQ(h.cpu.r.get(sf::CC), CCE);

	// An exponent outside -512 < I < 511 traps.
	reset(); h.cpu.push(u32(-600)); pushd(4.0);
	CHECK(h.one(f4(001)) == exec_trap::trapped);
	CHECK(h.cpu.last_trap() == trap_cause::exponent_size);

	// A negative value keeps its sign through the round trip.
	reset(); h.cpu.push(0); pushd(-4.0);
	CHECK(h.one(f4(001)) == exec_trap::none);
	CHECK(h.one(f4(000)) == exec_trap::none);
	CHECK(topd() < 0);
}


static void test_isc()
{
	std::printf("integer size check (ISC)\n");
	harness h;

	auto isc = [&](s32 v) {
		h.cpu.r.s = h.cpu.r.sb + 3;
		h.cpu.push(u32(v));
		CHECK(h.one(f3(034)) == exec_trap::none);
	};

	// Values a halfword holds: the operand stays on the stack.
	isc(0);
	CHECK_EQ(h.cpu.r.get(sf::O), 0);
	CHECK_EQ(h.cpu.stk(0), 0);
	CHECK_EQ(h.cpu.r.condition_code(), CCE);

	isc(32767);
	CHECK_EQ(h.cpu.r.get(sf::O), 0);
	CHECK_EQ(h.cpu.r.condition_code(), CCG);

	isc(-32768);
	CHECK_EQ(h.cpu.r.get(sf::O), 0);
	CHECK_EQ(h.cpu.r.condition_code(), CCL);

	isc(-1);
	CHECK_EQ(h.cpu.r.get(sf::O), 0);

	// One step beyond in either direction does not.
	isc(32768);
	CHECK_EQ(h.cpu.r.get(sf::O), 1);
	CHECK_EQ(h.cpu.stk(0), 32768);             // still there for the STH
	isc(-32769);
	CHECK_EQ(h.cpu.r.get(sf::O), 1);

	isc(s32(0x7fffffff));
	CHECK_EQ(h.cpu.r.get(sf::O), 1);
	isc(s32(0x80000000u));
	CHECK_EQ(h.cpu.r.get(sf::O), 1);

	// And the flag is assigned, not accumulated: a value that fits clears
	// what a previous one set.
	isc(1);
	CHECK_EQ(h.cpu.r.get(sf::O), 0);
}

// The shift group's index bit (ERS sheet 72, the note under ASL): the count
// is the six-bit operand plus X when bit 3 is set. The powerup device scan
// depends on it -- "LDIS #1; LSL 0,X" is how it builds the bit for select
// code X -- so getting it wrong silently registered every card as card 0.
static void test_shift_index()
{
	std::printf("shift counts take the index bit\n");
	harness h;

	// Without the bit, the operand alone.
	h.cpu.r.s = h.cpu.r.sb + 3;
	h.cpu.r.x = 5;
	h.cpu.push(1);
	CHECK(h.one(f2(002, 3)) == exec_trap::none);          // LSL 3
	CHECK_EQ(h.cpu.stk(0), 8);

	// With it, operand + X: the scan's own "LSL 0,X" with X = 5.
	h.cpu.r.s = h.cpu.r.sb + 3;
	h.cpu.push(1);
	CHECK(h.one(f2(002, 0, true)) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(0), 32);

	// Operand and X add.
	h.cpu.r.s = h.cpu.r.sb + 3;
	h.cpu.r.x = 2;
	h.cpu.push(1);
	CHECK(h.one(f2(002, 3, true)) == exec_trap::none);    // 3 + 2
	CHECK_EQ(h.cpu.stk(0), 32);

	// A count can reach 63, past the width of the word, and saturates.
	h.cpu.r.s = h.cpu.r.sb + 3;
	h.cpu.r.x = 0;
	h.cpu.push(0xffffffff);
	CHECK(h.one(f2(002, 32)) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(0), 0);
	h.cpu.r.s = h.cpu.r.sb + 3;
	h.cpu.push(0xffffffff);
	CHECK(h.one(f2(003, 40)) == exec_trap::none);         // LSR
	CHECK_EQ(h.cpu.stk(0), 0);

	// The circular shifts take the count modulo the word.
	h.cpu.r.s = h.cpu.r.sb + 3;
	h.cpu.r.x = 4;
	h.cpu.push(0x12345678);
	CHECK(h.one(f2(004, 0, true)) == exec_trap::none);    // CSL by 4
	CHECK_EQ(h.cpu.stk(0), s32(0x23456781u));

	// ASR divides toward zero, which is not what an arithmetic shift
	// right does: the ERS negates, shifts the magnitude, and negates back.
	h.cpu.r.s = h.cpu.r.sb + 3;
	h.cpu.r.x = 0;
	h.cpu.push(u32(-7));
	CHECK(h.one(f2(007, 1)) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(0), -3);                           // not -4
	h.cpu.r.s = h.cpu.r.sb + 3;
	h.cpu.push(u32(-8));
	CHECK(h.one(f2(007, 1)) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(0), -4);
	// -2^31 has no positive magnitude in 32 bits; negated unsigned it
	// shifts to -2^30, and unshifted it negates back to itself.
	h.cpu.r.s = h.cpu.r.sb + 3;
	h.cpu.push(0x80000000u);
	CHECK(h.one(f2(007, 1)) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(0), s32(0xc0000000u));
	h.cpu.r.s = h.cpu.r.sb + 3;
	h.cpu.push(0x80000000u);
	CHECK(h.one(f2(007, 0)) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(0), s32(0x80000000u));

	// MPYI forms its product in 64 bits and says whether it fit.
	h.cpu.r.s = h.cpu.r.sb + 3;
	h.cpu.push(0x40000000u);
	CHECK(h.one(f5(006, 4)) == exec_trap::none);          // MPYI 4
	CHECK_EQ(h.cpu.stk(0), 0);
	CHECK_EQ(h.cpu.r.get(sf::O), 1);
	h.cpu.r.s = h.cpu.r.sb + 3;
	h.cpu.push(u32(-3));
	CHECK(h.one(f5(006, 5)) == exec_trap::none);          // MPYI 5
	CHECK_EQ(h.cpu.stk(0), -15);
	CHECK_EQ(h.cpu.r.get(sf::O), 0);

	// ASL reports a sign change as Overflow.
	h.cpu.r.s = h.cpu.r.sb + 3;
	h.cpu.push(0x40000000);
	CHECK(h.one(f2(006, 1)) == exec_trap::none);
	CHECK_EQ(h.cpu.r.get(sf::O), 1);
	h.cpu.r.s = h.cpu.r.sb + 3;
	h.cpu.push(1);
	CHECK(h.one(f2(006, 1)) == exec_trap::none);
	CHECK_EQ(h.cpu.r.get(sf::O), 0);

	// LSXL shifts X by a count that includes X, and reports CCA on it.
	h.cpu.r.x = 3;
	CHECK(h.one(f2(010, 1)) == exec_trap::none);          // X <<= 1
	CHECK_EQ(h.cpu.r.x, 6);
	CHECK_EQ(h.cpu.r.condition_code(), CCG);
	h.cpu.r.x = 1;
	CHECK(h.one(f2(010, 2, true)) == exec_trap::none);    // count 2 + 1
	CHECK_EQ(h.cpu.r.x, 8);
}

// LRA and LRAS (ERS sheet 61), which had no coverage at all until the day a
// P-relative indexed one was found dropping its index.
//
// The sheet gives three direct forms. P-relative pushes `E - PB` untagged,
// where E is the effective address and so already carries X; the two tagged
// forms build their pointer from base + offset and then add X to the pointer,
// which the pseudocode spells out because those never form E. The
// implementation added X in the tagged arms and not in the P-relative one, so
// every `LRAS P+n,X` pushed a pointer to element zero of its table.
//
// The site that made it matter is BASIC's compiler: it selects the code
// template for each statement and each operator with `LRAS P+14,X` over a
// pool in its own code segment, so with X lost it compiled every statement
// from template 0.
static void test_lra_index()
{
	std::printf("LRA/LRAS, and the index the P-relative form takes in E\n");
	harness h;

	const u32 base = h.cpu.r.sb + 3;
	auto lras = [&](bool i, bool x, u8 b, u8 off) {
		h.cpu.r.s = base;
		CHECK(h.one(f1(3, i, x, b, off)) == exec_trap::none);
	};

	// Direct P-relative: a PB-relative byte offset, untagged. P is 0x1000
	// and so is PB, so the offset is the answer.
	h.cpu.r.x = 0;
	lras(false, false, 0, 14);
	CHECK_EQ(h.cpu.stk(0), 14);

	// The index bit, with the compiler's own value. Before the fix this
	// pushed 14.
	h.cpu.r.x = 0x138;
	lras(false, true, 0, 14);
	CHECK_EQ(h.cpu.stk(0), 14 + 0x138);

	// It is a two's complement byte index, and the backward base works the
	// same way.
	h.cpu.r.x = u32(-6);
	lras(false, true, 0, 14);
	CHECK_EQ(h.cpu.stk(0), 8);
	h.cpu.r.x = 0x20;
	lras(false, true, 1, 8);                  // P-8 + 0x20
	CHECK_EQ(h.cpu.stk(0), u32(-8) + 0x20);

	// The 32-bit form, same arm: 111 OP2(3:5) I(8) X(9) BASE(10:3) OFFSET.
	h.cpu.r.s = base;
	h.cpu.r.x = 0x138;
	CHECK(h.one(u32_insn(036, 0, 14) | 0x00400000u) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(0), 14 + 0x138);

	// The tagged arms are unchanged: the index goes into the pointer, and
	// the tag is 10 for the global segment and 11 for the stack.
	h.cpu.r.x = 4;
	lras(false, true, 2, 16);                 // DB+16
	CHECK_EQ(h.cpu.stk(0), 0x80000000u | (16 + 4));
	lras(false, true, 6, 16);                 // SB+16
	CHECK_EQ(h.cpu.stk(0), 0xc0000000u | (16 + 4));
	h.cpu.r.x = 0;
	lras(false, false, 2, 16);
	CHECK_EQ(h.cpu.stk(0), 0x80000000u | 16);

	// Indirect P-relative goes through a self-relative pointer and POST-
	// indexes, per sheet 17's rule that only branches preindex. The word at
	// P+16 is a byte offset from its own location.
	h.mem.write32(0x1000 + 16, 0x40);         // -> 0x1050
	h.cpu.r.x = 0;
	lras(true, false, 0, 16);
	CHECK_EQ(h.cpu.stk(0), 0x50);
	h.cpu.r.x = 8;
	lras(true, true, 0, 16);                  // ... + X, after resolution
	CHECK_EQ(h.cpu.stk(0), 0x58);

	// Indirect through the stack pushes the word as it stands, indexed.
	h.mem.write32(h.cpu.r.sb + 32, 0xc0000010u);
	h.cpu.r.x = 2;
	lras(true, true, 6, 32);
	CHECK_EQ(h.cpu.stk(0), 0xc0000012u);
}

// SBIT (ERS sheet 66). Its operands run the opposite way up to LBIT's: the
// bit value is on top, the offset beneath it, the pointer below that. The
// powerup device scan builds its present bitmap with one, so getting the
// order wrong wrote the bit through whatever happened to be in the second
// word -- which is how a machine with any device attached ended up taking
// an unexpected-pointer-type trap.
static void test_sbit()
{
	std::printf("SBIT operand order\n");
	harness h;

	const u32 ptr = (2u << 30) | 16;          // global segment, DB+16
	const u32 addr = h.cpu.r.db + 16;
	const u32 base = h.cpu.r.sb + 3;

	auto sbit = [&](s32 offset, u32 value) {
		h.cpu.r.s = base;
		h.cpu.push(ptr);
		h.cpu.push(u32(offset));
		h.cpu.push(value);
		CHECK(h.one(f4(003, true)) == exec_trap::none);
	};

	// Bit 0 is the left-most bit of the byte the pointer addresses.
	h.mem.write32(addr, 0);
	sbit(0, 1);
	CHECK_EQ(h.mem.read8(addr), 0x80);
	CHECK_EQ(h.cpu.r.s, s32(base));           // all three words popped
	CHECK_EQ(h.cpu.r.x, 0);

	sbit(3, 1);
	CHECK_EQ(h.mem.read8(addr), 0x90);        // and bit 3
	sbit(7, 1);
	CHECK_EQ(h.mem.read8(addr), 0x91);

	// A zero value clears rather than sets.
	sbit(0, 0);
	CHECK_EQ(h.mem.read8(addr), 0x11);

	// Bits past the first byte carry into X as a byte offset, and the
	// pointer is evaluated indexed by it.
	h.mem.write32(addr + 4, 0);
	sbit(8 + 1, 1);                           // byte 1, bit 1
	CHECK_EQ(h.cpu.r.x, 1);
	CHECK_EQ(h.mem.read8(addr + 1), 0x40);
	sbit(32 + 2, 1);                          // byte 4, bit 2
	CHECK_EQ(h.cpu.r.x, 4);
	CHECK_EQ(h.mem.read8(addr + 4), 0x20);

	// The offset is two's complement, so it can reach backwards.
	h.mem.write32(addr - 4, 0);
	sbit(-8, 1);                              // byte -1, bit 0
	CHECK_EQ(h.cpu.r.x, -1);
	CHECK_EQ(h.mem.read8(addr - 1), 0x80);
}


// The CST reference bit, word 0 bit 29: "set to 1 when the segment is
// accessed" (ERS sheet 27). Entering a segment -- a PCL into it, an EXIT
// back to it -- marks it; leaving does not clear it (the OS does that), and
// nothing else in the word moves except the use count.
static void test_cst_reference()
{
	std::printf("CST reference bit on segment entry\n");
	harness h;
	const u32 CST = 0x200;
	h.mem.write32(0, CST);
	h.mem.write32(CST, 16 * 8);
	// Segment 5: the caller, where the harness runs (PB 0x1000, PL 0x1ffc).
	h.mem.write32(CST + 80 + 0, ers32::set(0, 1, 12, 1));   // count 1, R clear
	h.mem.write32(CST + 80 + 4, 0xfff);
	h.mem.write32(CST + 80 + 8, 0x1000);
	h.mem.write32(0x1ffc, 4);                               // STT: one entry
	u32 xptr = ers32::set(0, 0, 1, 1);
	xptr = ers32::set(xptr, 1, 19, 4);
	xptr = ers32::set(xptr, 20, 12, 6);
	h.mem.write32(0x1ff8, xptr);                            // -> seg 6, entry 1
	// Segment 6: the callee, 256 bytes at 0xa000, entry 1 at +0x10.
	h.mem.write32(CST + 96 + 0, 0x00000100u);               // an OS bit, R clear
	h.mem.write32(CST + 96 + 4, 0xff);
	h.mem.write32(CST + 96 + 8, 0xa000);
	h.mem.write32(0xa0fc, 4);
	h.mem.write32(0xa0f8, 0x10);
	h.put(0xa010, f5(023, 0));                              // EXIT 0

	h.cpu.r.status = 0;
	h.cpu.r.put(sf::M, 1);
	h.cpu.r.put(sf::CSN, 5);
	CHECK(h.one(f5(022, 1)) == exec_trap::none);           // PCLS 1
	CHECK_EQ(h.cpu.r.p, 0xa010);
	CHECK_EQ(h.cpu.r.get(sf::CSN), 6);
	const u32 w6 = h.mem.read32(CST + 96);
	CHECK_EQ(ers32::get(w6, 29, 1), 1);                    // referenced
	CHECK_EQ(ers32::get(w6, 1, 12), 1);
	CHECK_EQ(w6 & ~(ers32::mask(29, 1) | ers32::mask(1, 12)), 0x100u);
	CHECK_EQ(ers32::get(h.mem.read32(CST + 80), 1, 12), 0);
	CHECK_EQ(ers32::get(h.mem.read32(CST + 80), 29, 1), 0);

	CHECK(h.cpu.step() == exec_trap::none);                // EXIT to seg 5
	CHECK_EQ(h.cpu.r.get(sf::CSN), 5);
	CHECK_EQ(h.cpu.r.p, 0x1002);
	CHECK_EQ(ers32::get(h.mem.read32(CST + 80), 29, 1), 1);
	CHECK_EQ(ers32::get(h.mem.read32(CST + 80), 1, 12), 1);
	CHECK_EQ(ers32::get(h.mem.read32(CST + 96), 29, 1), 1);   // still set
	CHECK_EQ(ers32::get(h.mem.read32(CST + 96), 1, 12), 0);
	CHECK_EQ(ers32::get(h.mem.read32(CST + 96), 31, 1), 0);   // never "absent"
}

// The segment-bound causes each have their own STT entry in segment 1: an
// address outside an external data segment is cause 013 and a pointer to
// data segment 0 cause 015, not 001 and 030 (ERS sheets 45-46); running off
// the end of the code segment is cause 055, Instruction Sequencing Bounds
// Violation (sheet 50), not a latched machine.
static void test_segment_causes()
{
	std::printf("EDS bounds 013, EDS pointer 015, sequencing 055\n");
	auto setup = [](harness &h) {
		const u32 CST = 0x200, HB = 0x2000, HLEN = 0x400;
		h.mem.write32(0, CST);
		h.mem.write32(CST, 16 * 8);
		h.mem.write32(CST + 16 + 4, HLEN - 1);
		h.mem.write32(CST + 16 + 8, HB);
		h.mem.write32(CST + 80 + 4, 0xfff);
		h.mem.write32(CST + 80 + 8, 0x1000);
		const u32 HPL = HB + (HLEN - 1) - 3;
		for (u32 c : {001u, 013u, 015u, 030u, 055u})
			h.mem.write32(HPL - 4 * c, 0x100 + 4 * c);  // a vector per cause
		h.cpu.enable_traps(true);
		h.cpu.r.status = 0;
		h.cpu.r.put(sf::CSN, 5);
	};
	const u32 HB = 0x2000;

	// User segment 1: 64 bytes, unpaged, writable, no link.
	harness h;
	setup(h);
	const u32 dst_base = 0x20000;
	h.cpu.r.dst = dst_base;
	h.mem.write32(dst_base, 16 * 4);
	h.mem.write32(dst_base + 16, ers32::set(0, 27, 1, 1));
	h.mem.write32(dst_base + 20, 63);
	h.mem.write32(dst_base + 24, 0x30000);
	u32 ptr = ers32::set(0, 1, 1, 1);
	ptr = ers32::set(ptr, 2, 11, 1);
	ptr = ers32::set(ptr, 13, 19, 64);         // one past the end
	h.cpu.push(ptr);
	CHECK(h.one(f4(005)) == exec_trap::none);  // LSI
	CHECK(unsigned(h.cpu.last_trap()) == 013u);
	CHECK_EQ(h.cpu.r.p, HB + 0x100 + 4 * 013);
	CHECK_EQ(h.cpu.word_below_pub(h.cpu.r.q, 2), 0);   // returns to the LSI

	// A pointer to data segment 0.
	harness z;
	setup(z);
	z.cpu.push(ers32::set(0, 13, 19, 8));
	CHECK(z.one(f4(005)) == exec_trap::none);
	CHECK(unsigned(z.cpu.last_trap()) == 015u);
	CHECK_EQ(z.cpu.r.p, HB + 0x100 + 4 * 015);

	// A global pointer out of DB..DL is still cause 1.
	harness g;
	setup(g);
	g.cpu.push((2u << 30) | 0x100000);
	CHECK(g.one(f4(005)) == exec_trap::none);
	CHECK(g.cpu.last_trap() == trap_cause::bounds_violation);
	CHECK_EQ(g.cpu.r.p, HB + 0x100 + 4 * 001);

	// P past PL: the trap, with the marker pointing at the offending P.
	harness s;
	setup(s);
	s.cpu.r.p = s.cpu.r.pl + 4;
	const u32 q0 = s.cpu.r.q;
	CHECK(s.cpu.step() == exec_trap::none);
	CHECK(s.cpu.last_trap() == trap_cause::instruction_sequencing);
	CHECK_EQ(s.cpu.r.p, HB + 0x100 + 4 * 055);
	CHECK_EQ(s.cpu.r.q, q0 + 16);
	CHECK_EQ(s.cpu.word_below_pub(s.cpu.r.q, 2), 0x1000 + 0xfff - 3 + 4 - 0x1000);
	CHECK(!s.cpu.halted());

	// Without a handler segment it is still reported to the caller.
	harness n;
	n.cpu.r.p = n.cpu.r.pl + 4;
	CHECK(n.cpu.step() == exec_trap::code_bounds);
}

// LHSI and PLHA sign extend (ERS 4.5 and 12.2: "(S)(0:16) := (S)(16:1)"),
// exactly as LDH does, and set CCA on the extended value. Both used to
// zero extend.
static void test_halfword_sign()
{
	std::printf("LHSI and PLHA sign extension\n");
	harness h;
	h.cpu.r.put(sf::M, 1);

	const u32 addr = h.cpu.r.db + 16;
	h.mem.write32(addr, 0xfffe7fffu);          // halfwords -2 and 32767

	h.cpu.push((2u << 30) | 16);               // global pointer, DB+16
	CHECK(h.one(f4(010)) == exec_trap::none);  // LHSI
	CHECK_EQ(h.cpu.stk(0), 0xfffffffeu);
	CHECK_EQ(h.cpu.r.condition_code(), CCL);
	h.cpu.pop();

	h.cpu.push((2u << 30) | 18);
	CHECK(h.one(f4(010)) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(0), 0x7fffu);
	CHECK_EQ(h.cpu.r.condition_code(), CCG);
	h.cpu.pop();

	h.cpu.push(addr);                          // absolute
	CHECK(h.one(f4(051)) == exec_trap::none);  // PLHA
	CHECK_EQ(h.cpu.stk(0), 0xfffffffeu);
	CHECK_EQ(h.cpu.r.condition_code(), CCL);
	h.cpu.pop();

	h.cpu.push(addr + 2);
	CHECK(h.one(f4(051)) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(0), 0x7fffu);
	CHECK_EQ(h.cpu.r.condition_code(), CCG);
	h.cpu.pop();
}

// A doubleword is aligned to a WORD (ERS 2.2.6), so one can start on the
// last word of a page: its second word is in the next page, whose frame is
// anywhere. LDSI/SDSI/LDD/STD/LDDS must reach it there -- not in whatever
// frame follows the first one physically -- and if that page is absent,
// trap before anything is popped or written.
static void test_double_cross_page()
{
	std::printf("doublewords straddling a page boundary\n");
	harness h;
	paged_segment(h, true);
	const u32 next_frame = PG_FRAME0 + 0x400;  // physically after page 0
	h.mem.write32(pg_phys(0x3fc), 0x11112222);
	h.mem.write32(pg_phys(0x400), 0x33334444); // first word of page 1
	h.mem.write32(next_frame, 0xbadbad00);

	h.cpu.push(pg_ptr(0x3fc));
	CHECK(h.one(f4(007)) == exec_trap::none);  // LDSI
	CHECK_EQ(h.cpu.stk(1), 0x11112222);
	CHECK_EQ(h.cpu.stk(0), 0x33334444);

	h.cpu.r.s = 0x4003;
	h.cpu.push(pg_ptr(0x3fc));
	h.cpu.push(0x55556666); h.cpu.push(0x77778888);
	CHECK(h.one(f4(027)) == exec_trap::none);  // SDSI
	CHECK_EQ(h.mem.read32(pg_phys(0x3fc)), 0x55556666);
	CHECK_EQ(h.mem.read32(pg_phys(0x400)), 0x77778888);
	CHECK_EQ(h.mem.read32(next_frame), 0xbadbad00);
	CHECK_EQ(h.cpu.r.s, 0x4003);

	// LDD / STD / LDDS through a pointer in the global segment.
	h.mem.write32(h.cpu.r.db + 0x10, pg_ptr(0x3fc));
	h.cpu.r.s = 0x4003;
	const u32 ldd = (7u << 29) | (020u << 24) | (1u << 23) | (2u << 19) | 0x10;
	CHECK(h.one(ldd) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(1), 0x55556666);
	CHECK_EQ(h.cpu.stk(0), 0x77778888);
	h.cpu.r.s = 0x4003;
	h.cpu.push(0x0a0a0a0a); h.cpu.push(0x0b0b0b0b);
	const u32 std_ = (7u << 29) | (035u << 24) | (1u << 23) | (2u << 19) | 0x10;
	CHECK(h.one(std_) == exec_trap::none);
	CHECK_EQ(h.mem.read32(pg_phys(0x3fc)), 0x0a0a0a0a);
	CHECK_EQ(h.mem.read32(pg_phys(0x400)), 0x0b0b0b0b);
	CHECK_EQ(h.mem.read32(next_frame), 0xbadbad00);
	h.cpu.r.s = 0x4003;
	CHECK(h.one(f1(4, true, false, 2, 4)) == exec_trap::none);   // LDDS DB+4,I
	CHECK_EQ(h.cpu.stk(1), 0x0a0a0a0a);
	CHECK_EQ(h.cpu.stk(0), 0x0b0b0b0b);

	// The second page absent: every one of them traps with nothing done.
	page1_present(h, false);
	h.cpu.r.s = 0x4003;
	h.cpu.push(pg_ptr(0x3fc));
	u32 s0 = h.cpu.r.s;
	CHECK(h.one(f4(007)) == exec_trap::address_error);          // LDSI
	CHECK(h.cpu.last_addr_trap() == trap::absent_page);
	CHECK_EQ(h.cpu.r.s, s0);
	CHECK_EQ(h.cpu.stk(0), pg_ptr(0x3fc));
	h.cpu.push(0x55556666); h.cpu.push(0x77778888);
	s0 = h.cpu.r.s;
	CHECK(h.one(f4(027)) == exec_trap::address_error);          // SDSI
	CHECK(h.cpu.last_addr_trap() == trap::absent_page);
	CHECK_EQ(h.cpu.r.s, s0);
	CHECK_EQ(h.mem.read32(pg_phys(0x3fc)), 0x0a0a0a0a);         // not written
	CHECK_EQ(h.mem.read32(next_frame), 0xbadbad00);
	s0 = h.cpu.r.s;
	CHECK(h.one(ldd) == exec_trap::address_error);              // LDD
	CHECK_EQ(h.cpu.r.s, s0);
	CHECK(h.one(std_) == exec_trap::address_error);             // STD
	CHECK_EQ(h.cpu.r.s, s0);
	CHECK_EQ(h.mem.read32(pg_phys(0x3fc)), 0x0a0a0a0a);
	CHECK(h.one(f1(4, true, false, 2, 4)) == exec_trap::address_error);
	CHECK_EQ(h.cpu.r.s, s0);

	// One wholly inside a page is the plain case, and unaffected.
	page1_present(h, true);
	h.cpu.r.s = 0x4003;
	h.mem.write32(pg_phys(0x3f4), 0x01020304);
	h.mem.write32(pg_phys(0x3f8), 0x05060708);
	h.cpu.push(pg_ptr(0x3f4));
	CHECK(h.one(f4(007)) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(1), 0x01020304);
	CHECK_EQ(h.cpu.stk(0), 0x05060708);
}

// LBIT and SBIT must leave their operands on the stack until the pointer
// has evaluated: an absent page traps back to the instruction (note 2),
// the marker is pushed on top of S, and the retry has to find the same
// two (three) words it started with. This runs the whole round trip --
// trap 040 into code segment 1, the page made present, EXIT, retry.
static void test_bit_restart()
{
	std::printf("LBIT/SBIT restart after an absent page\n");
	harness h;
	paged_segment(h, false);
	restart_handler(h);
	h.mem.write8(pg_phys(0x408), 0x04);        // byte 0x408, bit 5 set

	const u32 ptr = pg_ptr(0x3f8);             // in page 0
	const u32 spec = 8 * 16 + 5;               // 16 bytes on: page 1, bit 5

	h.cpu.push(0x11111111u);
	h.cpu.push(ptr);
	h.cpu.push(spec);
	const u32 s_before = h.cpu.r.s;

	CHECK(h.one(f4(002, true)) == exec_trap::none);        // LBIT: traps
	CHECK(h.cpu.last_trap() == trap_cause::absent_page);
	CHECK_EQ(h.cpu.r.p, RT_HB + 0x100);

	page1_present(h, true);                                // "page it in"
	CHECK(h.cpu.step() == exec_trap::none);                // EXIT
	CHECK_EQ(h.cpu.r.p, 0x1000);
	CHECK_EQ(h.cpu.r.s, s_before);                         // operands intact
	CHECK_EQ(h.cpu.stk(0), spec);
	CHECK_EQ(h.cpu.stk(1), ptr);

	CHECK(h.cpu.step() == exec_trap::none);                // LBIT again
	CHECK_EQ(h.cpu.stk(0), 1);
	CHECK_EQ(h.cpu.stk(1), 0x11111111u);
	CHECK_EQ(h.cpu.r.s, s_before - 4);

	// SBIT the same way: bit 6 of the same byte, page 1 absent again.
	page1_present(h, false);
	h.cpu.pop();
	h.cpu.push(ptr);
	h.cpu.push(8 * 16 + 6);
	h.cpu.push(1);
	const u32 s2 = h.cpu.r.s;
	CHECK(h.one(f4(003, true)) == exec_trap::none);        // SBIT: traps
	CHECK(h.cpu.last_trap() == trap_cause::absent_page);
	page1_present(h, true);
	CHECK(h.cpu.step() == exec_trap::none);                // EXIT
	CHECK_EQ(h.cpu.r.s, s2);
	CHECK(h.cpu.step() == exec_trap::none);                // SBIT again
	CHECK_EQ(h.mem.read8(pg_phys(0x408)), 0x06);
	CHECK_EQ(h.cpu.r.s, s2 - 12);
	CHECK_EQ(h.cpu.stk(0), 0x11111111u);
}

// UP and DOWN (format 4, 020/021): the semaphore pair named only by the
// shipped assembler and executed only by the HP-UX kernels. Layout fixed
// by both kernels' data and the BASIC kernel's software twin: word 0 the
// wait-list head, word 1 the count.
static void test_up_down()
{
	std::printf("UP and DOWN semaphores\n");
	harness h;
	h.cpu.r.put(sf::M, 1);                       // privileged

	// The plain form takes a data segment pointer and ",B" an absolute
	// address (focusexec_alu.ipp; 796 of HP-UX 5.01's 876 plain sites are
	// fed by LRA, LRAS or CPTR,B, none of its 288 ",B" sites are). This
	// test once gave the tagged pointer to ",B", which then read 0x80000040
	// as an address, blocked on empty memory and wrecked the frame for
	// every check after it.
	const u32 sem = h.cpu.r.db + 0x40;           // via a global pointer
	const u32 ptr = (2u << 30) | 0x40;
	h.mem.write32(sem, 0);                       // no waiters
	h.mem.write32(sem + 4, 1);                   // one unit free

	// DOWN acquires without blocking while the count is positive. The
	// plain form takes a data segment pointer (HP-UX's unit semaphores
	// arrive as external pointers to segment 15); ",B" takes an absolute
	// address (its first DOWN,B of the boot names 0x00ffe6f8). This test
	// once handed the tagged pointer to the ",B" form, from before the
	// kernels' operands settled what the bit means.
	h.cpu.push(ptr);
	CHECK(h.one(f4(021)) == exec_trap::none);
	CHECK_EQ(h.mem.read32(sem + 4), 0);
	CHECK_EQ(h.cpu.r.s, s32(h.cpu.r.sb + 3));    // popped

	// UP releases it.
	h.cpu.push(ptr);
	CHECK(h.one(f4(020)) == exec_trap::none);
	CHECK_EQ(h.mem.read32(sem + 4), 1);

	// ",B" takes the raw absolute address.
	h.cpu.push(sem);
	CHECK(h.one(f4(021, true)) == exec_trap::none);
	CHECK_EQ(h.mem.read32(sem + 4), 0);
	CHECK_EQ(h.cpu.r.s, s32(h.cpu.r.sb + 3));    // popped
	h.cpu.push(sem);
	CHECK(h.one(f4(020, true)) == exec_trap::none);
	CHECK_EQ(h.mem.read32(sem + 4), 1);

	// The readings do not overlap. A raw address given to the plain form
	// is an external pointer to segment zero, which is no segment: it
	// faults BEFORE anything is popped or counted, so the instruction can
	// be retried once a handler has dealt with it.
	h.cpu.push(sem);
	CHECK(h.one(f4(021)) == exec_trap::address_error);
	CHECK_EQ(h.mem.read32(sem + 4), 1);          // untouched
	CHECK_EQ(h.cpu.stk(0), sem);                 // operand still there
	h.cpu.pop();

	// Both are privileged.
	h.cpu.r.put(sf::M, 0);
	h.cpu.push(ptr);
	CHECK(h.one(f4(021)) == exec_trap::unimplemented);
	CHECK_EQ(h.mem.read32(sem + 4), 1);
	h.cpu.pop();
	h.cpu.r.put(sf::M, 1);
	h.cpu.push(ptr);
	CHECK(h.one(f4(021)) == exec_trap::none);    // take it again
	CHECK_EQ(h.mem.read32(sem + 4), 0);

	// The pointer is popped with or without the b bit: HP-UX 5.30's
	// dispatcher pairs `DOWN,B`/`UP` on the same semaphore and its
	// stack has no room for the pointer to survive the UP -- the
	// process-context stack overflowed, one word per lock, when it did.
	// The b bit chooses the lock test, not the pop.
	h.cpu.push(ptr);
	CHECK(h.one(f4(020)) == exec_trap::none);
	CHECK_EQ(h.cpu.r.s, s32(h.cpu.r.sb + 3));    // popped
	CHECK_EQ(h.mem.read32(sem + 4), 1);

	// A waiter: park a fake TCB on the list the way a blocking DOWN
	// does -- link node at CTCBP-8, a ring of one (test_up_down_wait_list
	// has the whole format), state word at CTCBP-16 -- and check that UP
	// unlinks it, marks it ready, and rings the doorbell bit.
	const u32 TCB = 0x3000;
	h.mem.write32(16 * ers32::get(h.cpu.r.mask, 3, 3) + 4, TCB + 0x18);
	const u32 link = TCB + 0x10;
	h.mem.write32(link, link);
	h.mem.write32(link + 4, link + 4);
	h.mem.write32(sem, link);
	h.mem.write32(sem + 4, u32(-1));             // one waiter
	h.mem.write32(TCB + 8, 0x00050002);          // blocked

	h.cpu.r.msg = 0;
	h.cpu.push(ptr);
	CHECK(h.one(f4(020)) == exec_trap::none);
	CHECK_EQ(h.mem.read32(sem + 4), 0);
	CHECK_EQ(h.mem.read32(sem), 0);              // unlinked
	CHECK_EQ(h.mem.read32(TCB + 8), 0x00050001); // ready
	CHECK_EQ(h.cpu.r.msg, 0x00200000);           // doorbell rung
}

// The wait list UP and DOWN build has to be the one HP-UX's own software
// semaphore routines build (CORE SYSTEM, file 0x13c18..0x14090), because
// the kernel runs both on the same semaphores: circular, doubly linked
// through CTCBP-8, ordered on the priority word at CTCBP-12 (smallest
// first, first come first served among equals), the head made nil when
// the count returns to zero. The first model was a nil-terminated LIFO,
// and an UP over a kernel-built ring left the head on a task that was no
// longer waiting -- the multiuser boot stalled on the file system's lock.
static void test_up_down_wait_list()
{
	std::printf("UP and DOWN: the wait list is the kernel's\n");
	harness h;
	h.cpu.r.put(sf::M, 1);

	const u32 sem = 0x2800;                      // absolute, the ,B form
	const u32 ded = 16 * ers32::get(h.cpu.r.mask, 3, 3);
	const u32 QI = 0x2f03;
	h.mem.write32(ded + 8, QI);
	h.mem.write32(QI - 3 - 16, 1);               // dispatching pseudo-disabled:
	                                             // a DISP only flags (QI)
	auto tcb = [&](u32 at, s32 prio) {
		h.mem.write32(at - 16, 0x00050001);      // state: ready
		h.mem.write32(at - 12, u32(prio));
		h.mem.write32(at - 8, 0xdeadbeef);
		h.mem.write32(at - 4, 0xdeadbeef);
		return at;
	};
	const u32 A = tcb(0x3100, 5), B = tcb(0x3200, 3), C = tcb(0x3300, 5);
	auto down_as = [&](u32 t) {
		h.mem.write32(ded + 4, t);
		h.cpu.push(sem);
		return h.one(f4(021, true));
	};
	auto up_as = [&](u32 t) {
		h.mem.write32(ded + 4, t);
		h.cpu.push(sem);
		return h.one(f4(020, true));
	};

	// 1. An UP over a ring the kernel's routine built: one waiter, linked
	// to itself. The head must become nil, not stay on the woken task.
	h.mem.write32(A - 8, A - 8);
	h.mem.write32(A - 4, A - 4);
	h.mem.write32(A - 16, 0x00050002);
	h.mem.write32(sem, A - 8);
	h.mem.write32(sem + 4, u32(-1));
	CHECK(up_as(A) == exec_trap::none);
	CHECK_EQ(h.mem.read32(sem), 0);
	CHECK_EQ(h.mem.read32(sem + 4), 0);
	CHECK_EQ(h.mem.read32(A - 16), 0x00050001);

	// 2. The first blocker makes a ring of one and records the semaphore.
	h.mem.write32(QI - 3, 0);
	CHECK(down_as(A) == exec_trap::none);
	CHECK_EQ(h.mem.read32(sem + 4), u32(-1));
	CHECK_EQ(h.mem.read32(sem), A - 8);
	CHECK_EQ(h.mem.read32(A - 8), A - 8);
	CHECK_EQ(h.mem.read32(A - 4), A - 4);
	CHECK_EQ(h.mem.read32(A - 16), 0x00050002);
	CHECK_EQ(h.mem.read32(A + 104), sem);
	CHECK_EQ(h.mem.read32(QI - 3), 0x80000000);  // the dispatch was asked for

	// 3. A better priority goes in front and takes the head; an equal one
	// goes behind those already there.
	CHECK(down_as(B) == exec_trap::none);
	CHECK_EQ(h.mem.read32(sem), B - 8);
	CHECK(down_as(C) == exec_trap::none);
	CHECK_EQ(h.mem.read32(sem + 4), u32(-3));
	CHECK_EQ(h.mem.read32(sem), B - 8);
	CHECK_EQ(h.mem.read32(B - 8), A - 8);        // B -> A -> C -> B
	CHECK_EQ(h.mem.read32(A - 8), C - 8);
	CHECK_EQ(h.mem.read32(C - 8), B - 8);
	CHECK_EQ(h.mem.read32(B - 4), C - 4);        // and backwards
	CHECK_EQ(h.mem.read32(A - 4), B - 4);
	CHECK_EQ(h.mem.read32(C - 4), A - 4);

	// 4. They come off in that order, and the last one leaves nil.
	h.cpu.r.msg = 0;
	CHECK(up_as(C) == exec_trap::none);
	CHECK_EQ(h.mem.read32(B - 16), 0x00050001);
	CHECK_EQ(h.mem.read32(sem), A - 8);
	CHECK_EQ(h.mem.read32(A - 8), C - 8);
	CHECK_EQ(h.mem.read32(C - 8), A - 8);
	CHECK_EQ(h.cpu.r.msg, 0x00200000);
	CHECK(up_as(B) == exec_trap::none);
	CHECK_EQ(h.mem.read32(A - 16), 0x00050001);
	CHECK_EQ(h.mem.read32(sem), C - 8);
	CHECK(up_as(B) == exec_trap::none);
	CHECK_EQ(h.mem.read32(C - 16), 0x00050001);
	CHECK_EQ(h.mem.read32(sem), 0);
	CHECK_EQ(h.mem.read32(sem + 4), 0);

	// 5. The instruction readies and rings; it does not dispatch, whatever
	// the two priorities are. (The kernel's software routines do, and
	// FOCUS_SEMPREEMPT is the experiment that gives UP the same ending.)
	CHECK(down_as(B) == exec_trap::none);        // B (3) waits
	h.mem.write32(QI - 3, 0);
	h.cpu.r.put(sf::CC, 2);
	CHECK(up_as(A) == exec_trap::none);          // A (5) readies B (3)
	CHECK_EQ(h.mem.read32(B - 16), 0x00050001);
	CHECK_EQ(h.mem.read32(QI - 3), 0);
	CHECK_EQ(h.cpu.r.get(sf::CC), 2);
}

// XGDS on a global data segment that is not in memory (ERS 5.2.2, trap
// 042). HP-UX's system-call exit runs XGDS back onto the user's segment,
// which the memory manager may have swapped out while the process was in
// the kernel; the trap is how it comes back. Loading DB from the absent
// entry's zero location word returned the process to user mode with
// DB = 0 -- "Unexpected memory fault" in both shells of two concurrent
// logins, and a system halt after it.
static void test_xgds_absent()
{
	std::printf("XGDS: an absent global data segment traps\n");
	harness h;
	h.cpu.r.put(sf::M, 1);
	const u32 ded = 16 * ers32::get(h.cpu.r.mask, 3, 3);
	const u32 TCB = 0x3000, DST = 0x6000;
	h.mem.write32(ded + 4, TCB);
	h.cpu.r.dst = DST;
	h.mem.write32(TCB + 8, ers32::set(0, 20, 12, 0x802));   // on user segment 2
	h.mem.write32(DST + 16 * 2, ers32::set(0x2058, 1, 12, 1));
	h.mem.write32(DST + 16 * 9, 0x2058);         // present, written, count 0
	h.mem.write32(DST + 16 * 9 + 4, 0xad2f);
	h.mem.write32(DST + 16 * 9 + 8, 0xcc4b4);
	const u32 ptr = ers32::set(0, 1, 12, 0x809);

	h.cpu.push(ptr);
	CHECK(h.one(f4(014)) == exec_trap::none);
	CHECK_EQ(h.cpu.r.db, 0xcc4b4);
	CHECK_EQ(h.cpu.r.dl, 0xcc4b4 + 0xad2f);
	CHECK_EQ(ers32::get(h.mem.read32(TCB + 8), 20, 12), 0x809);
	CHECK_EQ(ers32::get(h.cpu.stk(0), 1, 12), 0x802);
	CHECK_EQ(ers32::get(h.mem.read32(DST + 16 * 9), 1, 12), 1);
	CHECK_EQ(ers32::get(h.mem.read32(DST + 16 * 2), 1, 12), 0);

	// Swapped out: the absence bit, and a location word of zero.
	h.cpu.pop();
	h.mem.write32(TCB + 8, ers32::set(0, 20, 12, 0x802));
	h.mem.write32(DST + 16 * 2, ers32::set(0x2058, 1, 12, 1));
	h.mem.write32(DST + 16 * 9, 0x2051);
	h.mem.write32(DST + 16 * 9 + 8, 0);
	h.cpu.r.db = 0x8000;
	u32 param = 0;
	h.cpu.on_trap = [&](trap_cause, u32 p) { param = p; };
	h.cpu.push(ptr);
	const u32 s_before = h.cpu.r.s;
	CHECK(h.one(f4(014)) == exec_trap::trapped);
	CHECK(h.cpu.last_trap() == trap_cause::absent_data_segment);
	CHECK_EQ(param, 0x809);                      // the DST entry number
	CHECK_EQ(h.cpu.r.db, 0x8000);                // nothing was exchanged,
	CHECK_EQ(h.cpu.r.s, s_before);               // the pointer still stands,
	CHECK_EQ(h.cpu.stk(0), ptr);
	CHECK_EQ(ers32::get(h.mem.read32(TCB + 8), 20, 12), 0x802);
	CHECK_EQ(ers32::get(h.mem.read32(DST + 16 * 9), 1, 12), 0);   // no count taken
	CHECK_EQ(ers32::get(h.mem.read32(DST + 16 * 2), 1, 12), 1);
	h.cpu.on_trap = nullptr;
}

//**************************************************************************
//  THE DISPATCHER, STOP, AND THE MACHINE INSTRUCTION TRAP
//**************************************************************************
//
// A machine with a Code Segment Table, a handler segment, an Interrupt
// Control Stack with a dispatcher frame at QI, one task with a TCB and a
// stack segment, and one device on the DRT. Every case is a sequence HP-UX
// 5.30 executes, reduced to the few instructions that matter, checked
// against the Instruction Set ERS text it depends on.

static const u16 D_NOP   = f3(0);
static const u16 D_PSDB  = f4(015);
static const u16 D_DISP  = f4(016);
static const u16 D_PSEB  = f4(017);
static const u16 D_IXIT  = f4(074);
static const u16 D_CTCB  = f4(024);      // unimplemented: raises cause 020
static const u16 D_STOPB = f4(077, true);
static const u16 D_EXIT0 = f5(023, 0);

static constexpr u32 ST_M  = 0x00020000u;    // bit 14
static constexpr u32 ST_I  = 0x00040000u;    // bit 13
static constexpr u32 ST_MI = 0x00200000u;    // bit 10
static constexpr u32 ST_EM = 0x00400000u;    // bit 9

struct dmachine
{
	static constexpr u32 CST = 0x200;
	static constexpr u32 HB = 0x2000, HLEN = 0x400, HPL = HB + HLEN - 1 - 3;
	static constexpr u32 QI = 0x6103, SLI = 0x6fff;
	static constexpr u32 TCB = 0x7000;
	static constexpr u32 DED = 16;                  // channel 1
	static constexpr u32 DEV = 0x90;                // channel 1, PA 1
	static constexpr u32 ISR = 0x3000;              // absolute, segment 0
	static constexpr u32 STK = 0x3ff0;              // the task's stack segment
	static constexpr u32 PROC = 0x1000;             // the task's code, segment 5

	sparse_memory mem;
	cpu_core cpu{mem};

	// disp_csn: the segment the dispatcher frame names (HP-UX uses 0).
	explicit dmachine(u32 disp_csn = 1)
	{
		cpu.r.mask = ers32::set(0, 3, 3, 1);
		mem.write32(0, CST);
		mem.write32(CST, 16 * 64);
		seg(1, HB, HLEN - 1);
		seg(5, PROC, 0xfff);

		mem.write32(DED + 4, TCB);
		mem.write32(DED + 8, QI);
		mem.write32(DED + 12, SLI);

		// The dispatcher's frame (ERS sheet 41).
		mem.write32(QI - 3 - 12, 0);
		mem.write32(QI - 3 - 8, disp_csn ? 0x40 : HB + 0x40);
		mem.write32(QI - 3 - 4, ST_M | ST_I | disp_csn);
		mem.write32(QI - 3, 0);
		mem.write32(QI - 3 + 4, (2u << 30) | 0x8000);

		// The task: stack segment 5, global segment 6.
		mem.write32(TCB, 0);
		mem.write32(TCB + 4, 0);
		mem.write32(TCB + 8, (5u << 16) | 6);
		const u32 dst = dst_walker::SYSTEM_DST_BASE;
		mem.write32(dst + 16 * 5 + 4, 0x1fff);
		mem.write32(dst + 16 * 5 + 8, STK);
		mem.write32(dst + 16 * 6 + 4, 0xfff);
		mem.write32(dst + 16 * 6 + 8, 0x8000);
		mem.write32(STK, 0x10);                     // SB = STK + 0x10
		mem.write32(STK + 4, 0xfff);                // SL = SB + 0xfff

		// The device: a global pointer for its data, a local program
		// pointer -- absolute, segment 0 -- for its service routine.
		mem.write32(DEV, 0);
		mem.write32(DEV + 4, (2u << 30) | 0x8000);
		mem.write32(DEV + 8, ISR);

		// Trap vectors: each cause lands on its own EXIT.
		for (unsigned c = 1; c < 060; c++)
			mem.write32(HPL - 4 * c, 0x100 + 8 * c);

		task();
		cpu.enable_traps(true);
	}

	void seg(u32 n, u32 base, u32 len_m1)
	{
		mem.write32(CST + 16 * n, 0x2);             // M: privileged
		mem.write32(CST + 16 * n + 4, len_m1);
		mem.write32(CST + 16 * n + 8, base);
	}

	void task()
	{
		cpu.r.pb = PROC; cpu.r.pl = PROC + 0xfff - 3; cpu.r.p = PROC;
		cpu.r.sb = 0x4000; cpu.r.s = 0x4003; cpu.r.q = 0x4003;
		cpu.r.sl = 0x4fff;
		cpu.r.db = 0x8000; cpu.r.dl = 0x8fff;
		cpu.r.status = ST_M | ST_I | ST_EM | 5;
	}

	void put(u32 addr, u16 insn)
	{
		const u32 cell = addr & ~3u;
		u32 v = mem.read32(cell);
		if (addr & 2) v = (v & 0xffff0000u) | insn;
		else          v = (v & 0x0000ffffu) | (u32(insn) << 16);
		mem.write32(cell, v);
	}

	void code(u32 addr, std::initializer_list<u16> insns)
	{
		for (u16 i : insns) { put(addr, i); addr += 2; }
	}

	u32 use_count(u32 n) { return ers32::get(mem.read32(CST + 16 * n), 1, 12); }

	// Queue the device at level 2 and let the microcode handler take it.
	bool interrupt()
	{
		mem.write32(DEV, mem.read32(DEV) & 0xffffu);
		mem.write32(1024 + 8 * 2, DEV);
		mem.write32(1024 + 8 * 2 + 4, DEV);
		cpu.raise_message(18);
		return cpu.service_interrupts();
	}

	u32 vector(unsigned cause) const { return HB + 0x100 + 8 * cause; }
};

// "If (QI-4) = 0 then: if (QI)(0:1) = 1 then: if DISP FLAG = 1 then (QI)
// := 0, abort dispatcher & restart, else execute DISP instruction" (ERS
// sheets 98-99). From a task, the deferred DISP is carried out.
static void test_pseb_dispatches()
{
	std::printf("PSEB honours a deferred DISP\n");
	dmachine m;
	m.code(m.PROC, { D_PSDB, D_DISP, D_PSEB, D_NOP });

	CHECK(m.cpu.step() == exec_trap::none);         // PSDB
	CHECK_EQ(m.mem.read32(m.QI - 3 - 16), 1);
	CHECK(m.cpu.step() == exec_trap::none);         // DISP: deferred
	CHECK_EQ(m.cpu.r.p, m.PROC + 4);
	CHECK_EQ(m.mem.read32(m.QI - 3) >> 31, 1);

	CHECK(m.cpu.step() == exec_trap::none);         // PSEB: now it happens
	CHECK_EQ(m.mem.read32(m.QI - 3 - 16), 0);
	CHECK_EQ(m.cpu.r.p, m.HB + 0x40);               // in the dispatcher
	CHECK_EQ(m.cpu.r.q, m.QI);
	CHECK_EQ(m.mem.read32(m.QI - 3), 0);            // (QI) := 0
	// The task's marker returns past the PSEB, and S is saved at (SB-1).
	CHECK_EQ(m.mem.read32(0x4013 - 3 - 8), 6);
	CHECK_EQ(m.mem.read32(0x4000 - 4), 0x13);

	// "If (QI-4) = 0 then System Error Trap": cause 014, parameter 0.
	u32 param = 0xffffffffu;
	m.cpu.on_trap = [&](trap_cause, u32 p) { param = p; };
	m.code(m.HB + 0x40, { D_PSEB });
	CHECK(m.cpu.step() == exec_trap::none);
	CHECK(m.cpu.last_trap() == trap_cause::system_error);
	CHECK_EQ(param, 0);
	CHECK_EQ(m.cpu.r.p, m.vector(014));
}

// The same pair inside the dispatcher (DISP FLAG = 1) aborts it and starts
// it over from its frame at QI: HP-UX's `DISP; PSEB` at TRAP HANDLERS
// +0x1d6, when the task it picked has a segment swapped out.
static void test_pseb_restarts_dispatcher()
{
	std::printf("PSEB in the dispatcher restarts it\n");
	dmachine m;
	m.code(m.PROC, { D_DISP, D_NOP });
	m.code(m.HB + 0x40, { D_PSDB, D_NOP, D_DISP, D_PSEB, D_NOP });

	CHECK(m.cpu.step() == exec_trap::none);         // DISP from the task
	CHECK_EQ(m.cpu.r.p, m.HB + 0x40);
	CHECK(m.cpu.step() == exec_trap::none);         // PSDB
	CHECK(m.cpu.step() == exec_trap::none);         // NOP
	m.cpu.r.q += 8;                                 // wherever its Q went
	m.cpu.r.s += 8;
	CHECK(m.cpu.step() == exec_trap::none);         // DISP: on the ICS, deferred
	CHECK_EQ(m.mem.read32(m.QI - 3) >> 31, 1);
	CHECK_EQ(m.cpu.r.p, m.HB + 0x46);
	CHECK(m.cpu.step() == exec_trap::none);         // PSEB: abort and restart
	CHECK_EQ(m.cpu.r.p, m.HB + 0x40);
	CHECK_EQ(m.cpu.r.q, m.QI);
	CHECK_EQ(m.cpu.r.s, m.QI + 12);
	CHECK_EQ(m.mem.read32(m.QI - 3), 0);
	CHECK_EQ(m.mem.read32(m.QI - 3 - 16), 0);
}

// "DISP FLAG := 1" (sheet 98); the interrupt handler moves the flag into
// the marker it pushes, and IXIT restarts the dispatcher when (QI)(0:1)
// is set under such a marker (case 8).
static void test_disp_sets_flag()
{
	std::printf("an interrupted dispatcher is restarted by IXIT (case 8)\n");
	dmachine m;
	m.code(m.PROC, { D_DISP, D_NOP });
	m.code(m.HB + 0x40, { D_NOP, D_NOP, D_NOP });
	m.code(m.ISR, { D_DISP, D_IXIT });

	CHECK(m.cpu.step() == exec_trap::none);         // DISP: enters
	CHECK_EQ(m.cpu.r.p, m.HB + 0x40);
	CHECK(m.cpu.step() == exec_trap::none);         // the dispatcher runs
	CHECK_EQ(m.cpu.r.p, m.HB + 0x42);

	CHECK(m.interrupt());
	CHECK_EQ(m.cpu.r.p, m.ISR);
	CHECK_EQ(m.mem.read32(m.cpu.r.q - 3) >> 31, 1); // marker flagged

	CHECK(m.cpu.step() == exec_trap::none);         // the routine's DISP
	CHECK_EQ(m.mem.read32(m.QI - 3) >> 31, 1);
	CHECK(m.cpu.step() == exec_trap::none);         // IXIT
	CHECK_EQ(m.cpu.r.p, m.HB + 0x40);               // restarted, not resumed
	CHECK_EQ(m.cpu.r.q, m.QI);
	CHECK_EQ(m.mem.read32(m.QI - 3), 0);
}

// The same interruption with nothing requested is case 5: back to the
// dispatcher where it was. (QI) is zero and the disable count is zero,
// which is also how the boot handoff's IXIT finds them -- the two must
// not be confused. And the dispatcher's own IXIT is case 1: the task's
// registers come from its TCB, and MASK is left alone.
static void test_interrupted_dispatcher_resumes()
{
	std::printf("an interrupted dispatcher with no request is resumed (case 5)\n");
	dmachine m;
	m.code(m.PROC, { D_DISP, D_NOP });
	m.code(m.HB + 0x40, { D_NOP, D_NOP, D_NOP });
	m.code(m.ISR, { D_IXIT });

	CHECK(m.cpu.step() == exec_trap::none);
	CHECK(m.cpu.step() == exec_trap::none);
	CHECK(m.interrupt());
	CHECK(m.cpu.step() == exec_trap::none);         // IXIT
	CHECK_EQ(m.cpu.r.p, m.HB + 0x42);
	CHECK(m.cpu.last_trap() != trap_cause::inconsistent_registers);

	m.mem.write32(m.QI - 3 + 8, 0x12345678u);       // a stale (QI+2)
	const u32 mask = m.cpu.r.mask;
	m.code(m.HB + 0x42, { D_IXIT });
	CHECK(m.cpu.step() == exec_trap::none);
	CHECK_EQ(m.cpu.r.p, m.PROC + 2);
	CHECK_EQ(m.cpu.r.s, 0x4003);
	CHECK_EQ(m.cpu.r.mask, mask);
}

// "Wait for any unmasked Message bit" (sheet 94).
static void test_stop_is_level_sensitive()
{
	std::printf("STOP,B does not sleep on a bit already standing\n");
	dmachine m;
	m.code(m.PROC, { D_STOPB, D_NOP });

	// HP-UX's idle sequence: SMPB 2 consumes the doorbell, PMPB 1 / AND
	// builds the mask, STOP,B. A doorbell rung between the SMPB and the
	// STOP is in MSG when the STOP executes.
	m.cpu.r.msg = 0x00200000u;
	m.cpu.push(m.cpu.r.mask);
	CHECK(m.cpu.step() == exec_trap::none);
	CHECK(!m.cpu.halted());
	CHECK_EQ(m.cpu.r.p, m.PROC + 2);

	// With nothing standing it sleeps, and a message wakes it.
	m.cpu.r.msg = 0;
	m.cpu.r.p = m.PROC;
	m.cpu.push(m.cpu.r.mask);
	CHECK(m.cpu.step() == exec_trap::halted);
	CHECK(m.cpu.halted());
	m.cpu.raise_message(13);
	CHECK(!m.cpu.halted());

	// A masked bit does not count.
	m.cpu.r.msg = 0x00200000u;
	m.cpu.r.p = m.PROC;
	m.cpu.push(m.cpu.r.mask | 0x00200000u);
	CHECK(m.cpu.step() == exec_trap::halted);
}

// HP-UX's dispatcher delivers a signal to a task that is not in a system
// call by setting MI in the STATUS word of the marker the task resumes
// through (TRAP HANDLERS +0x102..0x122), and IXIT's "STATUS := (Q-1)"
// loads it. The signal hook withdraws the request the same way, clearing
// the bit in the trap's own marker (KERNEL 340 +0x3b04..0x3b14).
static void test_mi_through_marker()
{
	std::printf("MI set in a marker reaches the task; cleared there, it is gone\n");
	dmachine m;
	m.code(m.PROC, { D_NOP, D_NOP, D_NOP });
	m.code(m.ISR, { D_IXIT });
	m.code(m.vector(004), { D_EXIT0 });

	CHECK(m.interrupt());
	// The interrupted task's marker is at 0x4013; its STATUS at (Q-1).
	const u32 st = 0x4013 - 3 - 4;
	CHECK_EQ(m.mem.read32(st) & ST_MI, 0);
	m.mem.write32(st, m.mem.read32(st) | ST_MI);

	CHECK(m.cpu.step() == exec_trap::none);         // IXIT, through the TCB
	CHECK_EQ(m.cpu.r.p, m.PROC);
	CHECK_EQ(m.cpu.r.status & ST_MI, ST_MI);

	CHECK(m.cpu.step() == exec_trap::none);         // the trap, not the NOP
	CHECK(m.cpu.last_trap() == trap_cause::machine_instruction);
	CHECK_EQ(m.cpu.r.p, m.vector(004));

	// The trap's marker: MI still requested, EM cleared (note 4).
	const u32 tst = m.cpu.r.q - 3 - 4;
	CHECK_EQ(m.mem.read32(tst) & (ST_MI | ST_EM), ST_MI);

	// The hook clears the request in the marker and returns.
	m.mem.write32(tst, m.mem.read32(tst) & ~ST_MI);
	CHECK(m.cpu.step() == exec_trap::none);         // EXIT
	CHECK_EQ(m.cpu.r.p, m.PROC);
	CHECK_EQ(m.cpu.r.status & ST_MI, 0);
	CHECK(m.cpu.step() == exec_trap::none);         // the NOP at last
	CHECK_EQ(m.cpu.r.p, m.PROC + 2);
}

// Left in the marker, MI is a single step: the instruction returned to
// runs (EM came back clear), and the trap is taken before the next one.
static void test_mi_single_step()
{
	std::printf("MI left in the trap marker single-steps\n");
	dmachine m;
	m.code(m.PROC, { D_NOP, D_NOP, D_NOP });
	m.code(m.vector(004), { D_EXIT0 });
	m.cpu.r.status |= ST_MI;

	CHECK(m.cpu.step() == exec_trap::none);         // trap before PROC+0
	CHECK_EQ(m.cpu.r.p, m.vector(004));
	CHECK(m.cpu.step() == exec_trap::none);         // EXIT
	CHECK_EQ(m.cpu.r.p, m.PROC);
	CHECK(m.cpu.step() == exec_trap::none);         // PROC+0 executes
	CHECK_EQ(m.cpu.r.p, m.PROC + 2);
	CHECK(m.cpu.step() == exec_trap::none);         // trap before PROC+2
	CHECK_EQ(m.cpu.r.p, m.vector(004));
	CHECK_EQ(m.mem.read32(m.cpu.r.q - 3 - 8), 2);   // marker names PROC+2
}

// I/O interrupts outrank the Machine Instruction Trap at the end of an
// instruction (sheet 45, conditions 4 and 8), and "all tests that are
// skipped ... will be performed again later when the interrupted code
// sequence is resumed". HP-UX's system call stub requests the trap with
// PSHR 4 / OR / SETR 4 (KERNEL 254); an interrupt in the gap must not
// cost the task its signal.
static void test_mi_survives_interrupt()
{
	std::printf("an interrupt between the request and the trap does not lose MI\n");
	dmachine m;
	m.code(m.PROC, { D_NOP, D_NOP });
	m.code(m.ISR, { D_NOP, D_IXIT });
	m.cpu.r.status |= ST_MI;                        // requested, EM set

	CHECK(m.interrupt());
	CHECK(m.cpu.step() == exec_trap::none);         // the routine runs
	CHECK(m.cpu.step() == exec_trap::none);         // IXIT
	CHECK_EQ(m.cpu.r.p, m.PROC);
	CHECK(m.cpu.step() == exec_trap::none);
	CHECK(m.cpu.last_trap() == trap_cause::machine_instruction);
}

// A task interrupted within four words of SL has its marker in the stack
// overflow area (sheet 41). Resuming it is not an error; the overflow, if
// there still is one, is reported as Stack Overflow before its next
// instruction.
static void test_resume_above_sl()
{
	std::printf("a task whose saved S is above SL is resumed, not refused\n");
	dmachine m;
	m.code(m.PROC, { D_NOP, D_NOP });
	m.code(m.ISR, { D_IXIT });
	m.cpu.r.s = m.cpu.r.q = m.cpu.r.sl - 8;

	CHECK(m.interrupt());
	CHECK(m.mem.read32(0x4000 - 4) + 0x4000 > 0x4fff);  // saved S > SL
	CHECK(m.cpu.step() == exec_trap::none);         // IXIT
	CHECK(m.cpu.last_trap() != trap_cause::inconsistent_registers);
	CHECK_EQ(m.cpu.r.p, m.PROC);
	CHECK_EQ(m.cpu.r.s, 0x4fff - 8);
}

// "The interrupt handler decrements the use count for the current code
// segment, and increments the use count for the new segment. For the
// special case of code segment 0 the use count is neither incremented
// nor decremented" (sheet 43): segment 0 has no count, the segment on
// the other side still does. HP-UX's dispatcher runs in segment 0.
static void test_use_counts_through_segment_zero()
{
	std::printf("use counts balance when the dispatcher is in segment 0\n");
	dmachine m(0);
	m.code(m.PROC, { D_DISP, D_NOP });
	m.code(m.HB + 0x40, { D_CTCB, D_IXIT });
	m.code(m.vector(020), { D_EXIT0 });
	m.mem.write32(m.CST + 16 * 5, 0x2 | (1u << 19));    // the task is in 5

	CHECK(m.cpu.step() == exec_trap::none);         // DISP: 5 -> 0
	CHECK_EQ(m.cpu.r.p, m.HB + 0x40);
	CHECK_EQ(m.cpu.r.get(sf::CSN), 0);
	CHECK_EQ(m.use_count(5), 0);

	CHECK(m.cpu.step() == exec_trap::none);         // a trap: 0 -> 1
	CHECK_EQ(m.cpu.r.p, m.vector(020));
	CHECK_EQ(m.use_count(1), 1);
	CHECK(m.cpu.step() == exec_trap::none);         // its EXIT: 1 -> 0
	CHECK_EQ(m.cpu.r.p, m.HB + 0x42);
	CHECK_EQ(m.use_count(1), 0);

	CHECK(m.cpu.step() == exec_trap::none);         // IXIT: 0 -> 5
	CHECK_EQ(m.cpu.r.p, m.PROC + 2);
	CHECK_EQ(m.use_count(5), 1);

	// An interrupt whose routine runs absolute (a local DRT pointer, so
	// segment 0) gives up the task's count and its IXIT takes it back.
	m.code(m.ISR, { D_IXIT });
	CHECK(m.interrupt());
	CHECK_EQ(m.use_count(5), 0);
	CHECK(m.cpu.step() == exec_trap::none);         // IXIT, through the TCB
	CHECK_EQ(m.cpu.r.p, m.PROC + 2);
	CHECK_EQ(m.use_count(5), 1);

	// A message trap taken out of segment 0 and returned to it.
	m.mem.write32(m.CST + 16 * 5, 0x2);
	m.cpu.r.p = m.PROC;
	CHECK(m.cpu.step() == exec_trap::none);         // DISP again
	CHECK(m.cpu.step() == exec_trap::none);         // CTCB trap
	CHECK(m.cpu.step() == exec_trap::none);         // EXIT
	m.code(m.vector(054), { D_IXIT });
	m.cpu.raise_message(13);
	CHECK(m.cpu.service_interrupts());
	CHECK_EQ(m.cpu.r.p, m.vector(054));
	CHECK_EQ(m.use_count(1), 1);
	CHECK(m.cpu.step() == exec_trap::none);         // IXIT back to it
	CHECK_EQ(m.cpu.r.p, m.HB + 0x42);
	CHECK_EQ(m.use_count(1), 0);
}

// The microcode's ICS and dispatcher flags are cleared by reset.
static void test_reset_clears_flags()
{
	std::printf("reset leaves the machine off the ICS\n");
	dmachine m;
	m.code(m.PROC, { D_DISP, D_NOP });
	CHECK(m.cpu.step() == exec_trap::none);         // on the ICS now
	CHECK_EQ(m.cpu.r.q, m.QI);

	m.cpu.reset();
	m.cpu.r.mask = ers32::set(0, 3, 3, 1);
	m.task();
	CHECK(m.interrupt());
	CHECK_EQ(m.cpu.r.q, m.QI);                      // switched stacks
	CHECK_EQ(m.mem.read32(m.QI - 3) >> 31, 0);      // and flagged nothing
}

// STOP and INT are privileged (sheets 94 and 119): cause 026, parameter 1.
// SETR may load only some of STATUS (sheet 71).
static void test_stop_and_int_are_privileged()
{
	std::printf("STOP and INT trap in unprivileged mode; SETR's STATUS fields\n");
	dmachine m;
	m.code(m.PROC, { D_STOPB, f4(011), f4(011, true) });
	m.cpu.r.status &= ~ST_M;
	const u32 mask = m.cpu.r.mask;

	m.cpu.push(0xffffffffu);
	CHECK(m.cpu.step() == exec_trap::none);
	CHECK(m.cpu.last_trap() == trap_cause::privileged_mode);
	CHECK(!m.cpu.halted());
	CHECK_EQ(m.cpu.r.mask, mask);
	CHECK_EQ(m.cpu.r.p, m.vector(026));
	CHECK_EQ(m.cpu.stk(0), 1);                      // parameter 1

	m.task();
	m.cpu.r.status &= ~ST_M;
	m.cpu.r.p = m.PROC + 2;
	CHECK(m.cpu.step() == exec_trap::none);         // INT
	CHECK_EQ(m.cpu.r.p, m.vector(026));
	// The marker's STATUS still has I set: nothing was changed.
	CHECK_EQ(m.mem.read32(m.cpu.r.q - 3 - 4) & ST_I, ST_I);

	// SETR 4 unprivileged: "only STATUS(0:9), STATUS(10:3), and
	// STATUS(16:2) are modified" -- not M, not I, not the CSN.
	const u16 SETR4 = f2(035, 4);
	m.task();
	m.cpu.r.status = ST_EM | 5;                     // user mode, I clear
	m.code(m.PROC, { SETR4, SETR4 });
	// Everything but T (bit 8): with both T and O set the integer-overflow
	// User Trap stands at once (sheets 47-48), which is a different test.
	m.cpu.push(0xffffffffu & ~0x00800000u);
	CHECK(m.cpu.step() == exec_trap::none);
	CHECK_EQ(m.cpu.r.status & (ST_M | ST_I), 0);
	CHECK_EQ(m.cpu.r.get(sf::CSN), 5);
	CHECK(m.cpu.r.test(sf::O));
	CHECK_EQ(m.cpu.r.status & ST_MI, ST_MI);
	// Privileged: everything but EM, P and the CSN (HP-UX's KERNEL 254
	// sets MI this way).
	m.cpu.r.status = ST_M | ST_EM | 5;
	m.cpu.push(ST_M | ST_I | ST_MI | 7);
	CHECK(m.cpu.step() == exec_trap::none);
	CHECK_EQ(m.cpu.r.status & (ST_M | ST_I | ST_MI), ST_M | ST_I | ST_MI);
	CHECK_EQ(m.cpu.r.get(sf::CSN), 5);
}

// The DST entries a task's registers are loaded from become Referenced and
// Dirty at the load: IXIT's TCB path for the stack and global data
// segments, XGDS for a new global data segment (ERS sheet 32: R "set to 1
// when data in the segment ... is accessed", D "when the [unpaged] data
// segment is altered"; what the task does through SB, S, Q and DB never
// evaluates a pointer, so nothing later would set them). HP-UX's swap-out
// discards a segment whose D is clear without writing it -- kernel image
// 0x1dd7a: LSR 3; ANDI #1; TEST; BCCS.E -- and a stack discarded that way
// came back from a swap slot nothing had been written to.
static void test_task_segments_are_marked_dirty()
{
	std::printf("IXIT and XGDS mark the task's stack and global segments R and D\n");
	constexpr u32 R = 0x4, D = 0x8;
	{
		dmachine m;
		const u32 dst = dst_walker::SYSTEM_DST_BASE;
		const u32 stack = dst + 16 * 5, global = dst + 16 * 6;
		// As HP-UX leaves a segment it has just read back in: bits 14 and
		// 18 its own, writable, privileged, R from the dispatcher's MUC --
		// and one use, which must stand.
		m.mem.write32(stack, 0x000a2016u);
		m.mem.write32(global, 0x000a2012u);
		m.code(m.PROC, { D_NOP, D_NOP });
		m.code(m.ISR, { D_IXIT });

		CHECK(m.interrupt());
		CHECK_EQ(m.mem.read32(stack), 0x000a2016u);     // not by the interrupt
		CHECK(m.cpu.step() == exec_trap::none);         // IXIT, through the TCB
		CHECK_EQ(m.cpu.r.p, m.PROC);
		CHECK_EQ(m.mem.read32(stack), 0x000a2016u | R | D);
		CHECK_EQ(m.mem.read32(global), 0x000a2012u | R | D);

		// A second return writes nothing new, and an entry the OS holds
		// locked (all ones) or has marked absent is not touched.
		m.mem.write32(stack, 0xffffffffu);
		m.mem.write32(global, 0x00022013u);
		CHECK(m.interrupt());
		CHECK(m.cpu.step() == exec_trap::none);
		CHECK_EQ(m.cpu.r.p, m.PROC);
		CHECK_EQ(m.mem.read32(stack), 0xffffffffu);
		CHECK_EQ(m.mem.read32(global), 0x00022013u);
	}
	{
		// XGDS onto user segment 9, clean and unreferenced.
		harness h;
		h.cpu.r.put(sf::M, 1);
		const u32 ded = 16 * ers32::get(h.cpu.r.mask, 3, 3);
		const u32 TCB = 0x3000, DST = 0x6000;
		h.mem.write32(ded + 4, TCB);
		h.cpu.r.dst = DST;
		h.mem.write32(TCB + 8, ers32::set(0, 20, 12, 0x802));
		h.mem.write32(DST + 16 * 2, ers32::set(0x2058, 1, 12, 1));
		h.mem.write32(DST + 16 * 9, 0x00022012u);
		h.mem.write32(DST + 16 * 9 + 4, 0xad2f);
		h.mem.write32(DST + 16 * 9 + 8, 0xcc4b4);
		h.cpu.push(ers32::set(0, 1, 12, 0x809));
		CHECK(h.one(f4(014)) == exec_trap::none);
		CHECK_EQ(h.cpu.r.db, 0xcc4b4);
		// one use, and R and D
		CHECK_EQ(h.mem.read32(DST + 16 * 9), 0x000a2012u | R | D);
		// the segment it left keeps its bits and gives up its use
		CHECK_EQ(h.mem.read32(DST + 16 * 2), 0x2058);
	}
}

// CPTR,B and CPEP,B on an external pointer evaluate it (ERS sheet 120), and
// for a paged segment the evaluation adds the index to the whole pointer
// (sheet 56): a sum past 512 KB is a pointer into the next segment, not a
// Pointer Conversion Violation. HP-UX's kernel forms a transfer's user
// address this way -- CPTR,B on the base of the process's data space
// (0x40580000: user segment 11, offset 0) and a byte offset -- so a read,
// a write or a core dump reaching past 512 KB of a process's data, which
// is user segment 12, depends on it.
static void test_cptr_paged_carry()
{
	std::printf("CPTR,B and CPEP,B carry a paged pointer into the next segment\n");
	harness h;
	h.cpu.r.put(sf::M, 1);
	const u32 DST = 0x6000;
	h.cpu.r.dst = DST;
	h.mem.write32(DST, 16 * 20);
	for (u32 seg : { 11u, 12u })
	{
		h.mem.write32(DST + 16 * seg, 0x000202b8);       // paged, 1 KB pages
		h.mem.write32(DST + 16 * seg + 4, 0xfff);
		h.mem.write32(DST + 16 * seg + 8, 0x20000 + 0x1000 * seg);
	}
	h.mem.write32(DST + 16 * 5, 0x00020010);             // unpaged, writable
	h.mem.write32(DST + 16 * 5 + 4, 0x7ffff);
	h.mem.write32(DST + 16 * 5 + 8, 0x100000);

	// The kernel's own operands, from a core dump of `sort`.
	h.cpu.push(0x40580000);
	h.cpu.push(0x000800e0);
	CHECK(h.one(f4(070, true)) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(0), 0x406000e0);
	CHECK_EQ(h.cpu.r.x, 0x000800e0);
	h.cpu.pop();

	// Inside the segment nothing changes, and a negative delta carries back.
	h.cpu.push(0x40580000);
	h.cpu.push(0x0007fffc);
	CHECK(h.one(f4(070, true)) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(0), 0x405ffffc);
	h.cpu.pop();
	h.cpu.push(0x40600000);
	h.cpu.push(u32(-4));
	CHECK(h.one(f4(070, true)) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(0), 0x405ffffc);
	h.cpu.pop();

	// CPEP,B: "the pointer is evaluated as in the CPTR instruction".
	h.cpu.push(0x40580010);
	h.cpu.r.x = 0x00080000;
	CHECK(h.one(f4(067, true)) == exec_trap::none);
	CHECK_EQ(h.cpu.stk(0), 0x40600010);
	h.cpu.pop();

	// Segment 13 is not there: the sum is no pointer at all.
	h.cpu.push(0x40600000);
	h.cpu.push(0x00080000);
	CHECK(h.one(f4(070, true)) == exec_trap::address_error);
	h.cpu.pop(); h.cpu.pop();

	// An unpaged segment's pointer has nowhere to carry to: "If the
	// pointer format cannot accomodate the final offset then a Pointer
	// Conversion Trap occurs."
	h.cpu.push(0x40280000 | 0x7fff0);
	h.cpu.push(0x20);
	CHECK(h.one(f4(070, true)) == exec_trap::pointer_conversion);
}

int main()
{
	std::printf("FOCUS execution core tests\n\n");
	test_arithmetic();
	test_divide_extremes();
	test_logical_carry_chain();
	test_divide_by_zero_trap();
	test_overflow_trap();
	test_negate();
	test_stack_ops();
	test_condition_codes();
	test_branches();
	test_load_store();
	test_memory_arithmetic();
	test_modal_pops();
	test_unimplemented_is_reported();
	test_floating_point();
	test_new_groups();
	test_try_escp();
	test_mvb();
	test_mvb_resume();
	test_mvb_restart_round_trip();
	test_string_restart();
	test_io();
	test_tcc();
	test_traps();
	test_system_group();
	test_string_group();
	test_string_descriptors();
	test_string_load();
	test_string_assign();
	test_string_concatenate();
	test_cpep();
	test_decimal();
	test_field_ops();
	test_decimal_conversion();
	test_isc();
	test_shift_index();
	test_lra_index();
	test_sbit();
	test_bit_restart();
	test_double_cross_page();
	test_halfword_sign();
	test_cst_reference();
	test_segment_causes();
	test_up_down();
	test_up_down_wait_list();
	test_xgds_absent();
	test_pseb_dispatches();
	test_pseb_restarts_dispatcher();
	test_disp_sets_flag();
	test_interrupted_dispatcher_resumes();
	test_stop_is_level_sensitive();
	test_mi_through_marker();
	test_mi_single_step();
	test_mi_survives_interrupt();
	test_resume_above_sl();
	test_use_counts_through_segment_zero();
	test_reset_clears_flags();
	test_stop_and_int_are_privileged();
	test_task_segments_are_marked_dirty();
	test_cptr_paged_carry();
	test_factorial();

	std::printf("\n%d checks, %d failed\n", g_run, g_fail);
	std::printf(g_fail ? "FAIL\n" : "PASS\n");
	return g_fail ? 1 : 0;
}
