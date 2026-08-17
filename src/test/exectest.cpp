// license:BSD-3-Clause
// Tests for the FOCUS execution core (src/cpu/focus/focusexec.h).
//
// The end-to-end case is the recursive factorial program from section 7 of
// the Instruction Set ERS (sheet 129) -- HP's own worked example, assembled
// here instruction for instruction from the published listing.

#include "../cpu/focus/focusexec.h"

#include <cstdio>
#include <cstdint>
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
	// procedure (ERS sheet 92: PCLS reads PL - 4*n).
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

	h.cpu.pop();
	h.cpu.push(100); h.cpu.push(7);
	CHECK(h.one(f3(023)) == exec_trap::none);       // DIV
	CHECK_EQ(h.cpu.stk(0), 14);

	h.cpu.push(0);
	CHECK(h.one(f3(023)) == exec_trap::divide_by_zero);

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

static void test_unimplemented_is_reported()
{
	std::printf("unimplemented opcodes are reported\n");
	harness h;

	// Format 4 specials are not modelled yet; they must not be silently
	// treated as no-ops.
	CHECK(h.one(u16(1 << 6 | 000)) == exec_trap::unimplemented);   // DTB
	CHECK(h.one(u16(051 << 6)) == exec_trap::unimplemented);       // MVB
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
	CHECK_EQ(h.cpu.stk(0), 2);

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
	h.cpu.r.status = 0x1234;

	// TRY with bit 3 set takes a PB-relative recovery address from the top
	// of stack and builds a four-word frame.
	h.cpu.push(0x40);                                   // recovery address
	CHECK(h.one(u16((1 << 6) | 040 | 0x1000)) == exec_trap::none);
	CHECK_EQ(h.cpu.r.s, s0 + 16);                       // one word plus three
	CHECK_EQ(h.mem.read32(h.cpu.r.sb + 8), h.cpu.r.s - h.cpu.r.sb);
	CHECK_EQ(h.cpu.word_below_pub(h.cpu.r.s, 0), 16);   // delta Q
	CHECK_EQ(h.cpu.word_below_pub(h.cpu.r.s, 1), 0x1234); // saved STATUS
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
	CHECK_EQ(h.cpu.r.status, 0x1234);                   // restored
	CHECK_EQ(h.cpu.r.p, h.cpu.r.pb + 0x40);             // recovery address
	CHECK_EQ(h.mem.read32(h.cpu.r.sb + 8), 0);          // chain head popped
	CHECK_EQ(h.cpu.r.q, q0);                            // delta Q unwound

	// INT sets and clears the interrupt enable bit.
	h.cpu.r.put(sf::I, 0);
	CHECK(h.one(u16((1 << 6) | 011 | 0x1000)) == exec_trap::none);
	CHECK(h.cpu.r.test(sf::I));
	CHECK(h.one(u16((1 << 6) | 011)) == exec_trap::none);
	CHECK(!h.cpu.r.test(sf::I));

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

int main()
{
	std::printf("FOCUS execution core tests\n\n");
	test_arithmetic();
	test_stack_ops();
	test_branches();
	test_load_store();
	test_unimplemented_is_reported();
	test_floating_point();
	test_new_groups();
	test_try_escp();
	test_factorial();

	std::printf("\n%d checks, %d failed\n", g_run, g_fail);
	std::printf(g_fail ? "FAIL\n" : "PASS\n");
	return g_fail ? 1 : 0;
}
