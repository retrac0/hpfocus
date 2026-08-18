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

static void test_memory_arithmetic()
{
	std::printf("memory increment and decrement\n");
	harness h;

	// INCM/DECM step by one; INCF/DECF step by *four* (ERS sheets 81.1-81.3
	// name them "Increment/Decrement Memory by Four"). The powerup module's
	// checksum loop counts a byte total down with DECF while advancing X by
	// four, which is what caught DECF stepping by one.
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

	// The decimal group and the remaining string instructions are not
	// modelled yet; they must not be silently treated as no-ops.
	CHECK(h.one(u16(1 << 6 | 000)) == exec_trap::unimplemented);   // DTB
	CHECK(h.one(u16(025 << 6)) == exec_trap::unimplemented);       // SLD
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

static void test_traps()
{
	std::printf("internal traps (sheets 44-45)\n");
	harness h;

	// Without a handler segment, faults are reported to the caller.
	CHECK(!h.cpu.traps_enabled());
	CHECK(h.one(u16(1 << 6 | 000)) == exec_trap::unimplemented);

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

	CHECK(h.one(u16(1 << 6 | 000)) == exec_trap::none);   // DTB, unimplemented
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

		// Level 1 follows one static link: evaluate the frame pointer, then
		// take the word four bytes below the address it yields. The level
		// field is CIR(8:4), bits 20-23, which u32_insn cannot reach.
		h.cpu.r.s = h.cpu.r.sb + 0x200 + 3;
		u32 parent = ers32::set(0, 0, 2, 3);
		parent = ers32::set(parent, 2, 30, 0x20);
		h.mem.write32(h.cpu.r.q - 4, parent);
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
		CHECK(h.one(u32_insn(002, 0, 0) | (10u << 12) | 20u) == exec_trap::none);
		CHECK(h.cpu.last_trap() == trap_cause::check_trap);
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

int main()
{
	std::printf("FOCUS execution core tests\n\n");
	test_arithmetic();
	test_stack_ops();
	test_branches();
	test_load_store();
	test_memory_arithmetic();
	test_modal_pops();
	test_unimplemented_is_reported();
	test_floating_point();
	test_new_groups();
	test_try_escp();
	test_mvb();
	test_io();
	test_tcc();
	test_traps();
	test_system_group();
	test_string_group();
	test_factorial();

	std::printf("\n%d checks, %d failed\n", g_run, g_fail);
	std::printf(g_fail ? "FAIL\n" : "PASS\n");
	return g_fail ? 1 : 0;
}
