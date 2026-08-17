// license:BSD-3-Clause
// Tests for the FOCUS addressing unit (src/cpu/focus/focusaddr.h).
//
// The centrepiece is Figure 2.1 of the Instruction Set ERS (sheet 19), a
// worked address-computation example that HP supplied with the memory
// contents and the expected result of ten LOAD instructions. It exercises
// direct, indirect, indexed and indirect-plus-indexed addressing across all
// four pointer formats, so reproducing it exactly is a strong check on the
// whole unit.

#include "../cpu/focus/focusaddr.h"

#include <cstdio>
#include <cstdint>
#include <map>

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

#define CHECK_EQ_MSG(got, want, msg) do { \
		g_run++; \
		const std::int64_t g_ = (std::int64_t)(std::int32_t)(got); \
		const std::int64_t w_ = (std::int64_t)(std::int32_t)(want); \
		if (g_ != w_) { \
			g_fail++; \
			std::printf("  FAIL %s  got %lld, want %lld\n", msg, \
					(long long)g_, (long long)w_); \
		} \
	} while (0)


//**************************************************************************
//  TEST DOUBLES
//**************************************************************************

class sparse_memory : public memory_interface
{
public:
	u32 read32(u32 addr) override
	{
		auto it = m_cells.find(addr);
		return (it == m_cells.end()) ? 0 : it->second;
	}
	void write32(u32 addr, u32 data) override { m_cells[addr] = data; }

private:
	std::map<u32, u32> m_cells;
};

// Stands in for the Data Segment Table walk, which needs the DST entry
// layout (not yet transcribed). Segments are placed at fixed bases.
class flat_eds : public eds_resolver
{
public:
	void add(u32 segnum, u32 base, u32 length)
	{
		m_seg[segnum] = {base, length};
	}

	bool resolve(bool, u32 segment, u32 offset, bool, u32 &addr,
			trap &t) override
	{
		auto it = m_seg.find(segment);
		if (it == m_seg.end())
		{
			t = trap::dst_violation;
			return false;
		}
		if (offset >= it->second.length)
		{
			t = trap::eds_bounds_violation;
			return false;
		}
		addr = it->second.base + offset;
		return true;
	}

private:
	struct seg { u32 base, length; };
	std::map<u32, seg> m_seg;
};


//**************************************************************************
//  FIGURE 2.1
//**************************************************************************

// Pointer constructors, per the formats on ERS sheets 16-17.
static u32 stack_ptr(u32 off)  { return 0xc0000000u | off; }        // tag 11
static u32 global_ptr(u32 off) { return 0x80000000u | off; }        // tag 10
static u32 eds_ptr(u32 seg, u32 off)                                // bit 0 = 0
{
	return ers32::set(ers32::set(0, 2, 11, seg), 13, 19, off);
}

static void test_figure_2_1()
{
	std::printf("ERS Figure 2.1 (sheet 19) address computation\n");

	registers r;
	sparse_memory mem;
	flat_eds eds;
	addressing_unit au(r, mem, eds);

	// Segment layout. Absolute bases are arbitrary; only the relationships
	// in the figure matter. Limits are set wide enough that the figure's
	// references are all in bounds.
	r.pb = 0x1000;  r.p  = 0x1100;  r.pl = 0x1fff;
	r.sb = 0x3000;  r.q  = 0x3200;  r.s  = 0x33ff;  r.sl = 0x3fff;
	r.db = 0x5000;  r.dl = 0x5fff;
	r.x  = 20;                       // "The Index Register contains 20"

	const u32 EDS33 = 0x8000;
	eds.add(33, EDS33, 0x1000);

	// Program segment.
	mem.write32(r.p - 40, 10);
	mem.write32(r.p - 20, 5);
	mem.write32(r.p + 20, u32(-60));
	mem.write32(r.p + 40, 100);

	// Stack segment.
	mem.write32(r.sb + 200, 300);
	mem.write32(r.q - 20, eds_ptr(33, 120));
	mem.write32(r.q + 12, stack_ptr(200));
	mem.write32(r.q + 32, global_ptr(40));

	// Global data segment.
	mem.write32(r.db + 40, 27);
	mem.write32(r.db + 60, global_ptr(252));
	mem.write32(r.db + 72, eds_ptr(33, 212));
	mem.write32(r.db + 252, 18);

	// External data segment #33.
	mem.write32(EDS33 + 120, u32(-63));
	mem.write32(EDS33 + 140, 31);
	mem.write32(EDS33 + 212, 52);

	struct testcase
	{
		const char *text;
		base_reg base;
		u32 offset;
		bool ind, idx;
		std::int32_t expected;
	};

	// The ten instructions in the figure and the "New Top-of-Stack" HP gives
	// for each.
	static const testcase cases[] = {
		{ "LOAD P+20",        BASE_P_PLUS,  20, false, false, -60 },
		{ "LOAD P+20,I",      BASE_P_PLUS,  20, true,  false,  10 },
		{ "LOAD P+20,X",      BASE_P_PLUS,  20, false, true,  100 },
		{ "LOAD P+20,I,X",    BASE_P_PLUS,  20, true,  true,    5 },
		{ "LOAD Q+12,I",      BASE_Q_PLUS,  12, true,  false, 300 },
		{ "LOAD Q+32,I",      BASE_Q_PLUS,  32, true,  false,  27 },
		{ "LOAD Q-20,I",      BASE_Q_MINUS, 20, true,  false, -63 },
		{ "LOAD Q-20,I,X",    BASE_Q_MINUS, 20, true,  true,   31 },
		{ "LOAD DB+60,I",     BASE_DB_PLUS, 60, true,  false,  18 },
		{ "LOAD DB+72,I",     BASE_DB_PLUS, 72, true,  false,  52 },
	};

	for (const auto &c : cases)
	{
		const ea_result e = au.effective_address(c.base, c.offset, c.ind, c.idx);
		g_run++;
		if (!e.ok())
		{
			g_fail++;
			std::printf("  FAIL %-14s trapped (%d)\n", c.text, int(e.t));
			continue;
		}
		const std::int32_t got = std::int32_t(mem.read32(e.addr));
		if (got != c.expected)
		{
			g_fail++;
			std::printf("  FAIL %-14s got %d, want %d  (ea 0x%08x)\n",
					c.text, got, c.expected, e.addr);
		}
	}
}


static void test_bounds_checking()
{
	std::printf("segment bounds checking (sheet 18)\n");

	registers r;
	sparse_memory mem;
	flat_eds eds;
	addressing_unit au(r, mem, eds);

	r.pb = 0x1000; r.p = 0x1100; r.pl = 0x1fff;
	r.sb = 0x3000; r.q = 0x3200; r.s = 0x33ff; r.sl = 0x3fff;
	r.db = 0x5000; r.dl = 0x5fff;
	r.x = 0;

	// Code: PB..PL.
	CHECK(au.effective_address(BASE_P_PLUS, 0, false, false).ok());
	CHECK(!au.effective_address(BASE_P_PLUS, 0x1000, false, false).ok());
	CHECK(!au.effective_address(BASE_P_MINUS, 0x200, false, false).ok());

	// Stack is bounded by S, not SL -- addresses above the stack pointer are
	// out of bounds even though the segment extends to SL.
	CHECK(au.effective_address(BASE_SB_PLUS, 0x100, false, false).ok());
	CHECK(!au.effective_address(BASE_SB_PLUS, 0x500, false, false).ok());
	CHECK(au.in_stack(r.s));
	CHECK(!au.in_stack(r.s + 4));
	CHECK(!au.in_stack(r.sl));

	// Global data: DB..DL.
	CHECK(au.effective_address(BASE_DB_PLUS, 0x100, false, false).ok());
	CHECK(!au.effective_address(BASE_DB_PLUS, 0x1000, false, false).ok());

	// The trap raised is a bounds violation.
	const ea_result e = au.effective_address(BASE_DB_PLUS, 0x1000, false, false);
	CHECK(e.t == trap::bounds_violation);

	// Indexing happens before the check, so a valid base with a wild index
	// still traps.
	r.x = 0x4000;
	CHECK(!au.effective_address(BASE_DB_PLUS, 0, false, true).ok());
}


static void test_eds_traps()
{
	std::printf("external data segment traps\n");

	registers r;
	sparse_memory mem;
	flat_eds eds;
	addressing_unit au(r, mem, eds);

	r.sb = 0x3000; r.q = 0x3200; r.s = 0x33ff;
	r.db = 0x5000; r.dl = 0x5fff;
	r.x = 0;
	eds.add(33, 0x8000, 0x100);

	// Segment number zero is an EDS pointer violation (sheet 55).
	mem.write32(r.q + 4, eds_ptr(0, 0));
	ea_result e = au.effective_address(BASE_Q_PLUS, 4, true, false);
	CHECK(e.t == trap::eds_pointer_violation);

	// A segment absent from the table is a DST violation.
	mem.write32(r.q + 8, eds_ptr(99, 0));
	e = au.effective_address(BASE_Q_PLUS, 8, true, false);
	CHECK(e.t == trap::dst_violation);

	// An offset past the segment length is an EDS bounds violation.
	mem.write32(r.q + 12, eds_ptr(33, 0x200));
	e = au.effective_address(BASE_Q_PLUS, 12, true, false);
	CHECK(e.t == trap::eds_bounds_violation);

	// Indexing an EDS pointer adds to the offset within the segment, not to
	// the resolved address (sheet 55).
	mem.write32(r.q + 16, eds_ptr(33, 0xf0));
	r.x = 0x20;
	e = au.effective_address(BASE_Q_PLUS, 16, true, true);
	CHECK(e.t == trap::eds_bounds_violation);
	r.x = 0x08;
	e = au.effective_address(BASE_Q_PLUS, 16, true, true);
	CHECK(e.ok());
	CHECK_EQ_MSG(e.addr, 0x8000 + 0xf8, "EDS indexed offset");
}


static void test_pointer_formats()
{
	std::printf("pointer format tags\n");

	// Stack segment pointers are tagged 11, global 10, external 0x
	// (ERS sheets 16-17).
	CHECK_EQ_MSG(ers32::get(stack_ptr(200), 0, 2), 3, "stack tag");
	CHECK_EQ_MSG(ers32::get(global_ptr(40), 0, 2), 2, "global tag");
	CHECK_EQ_MSG(ers32::get(eds_ptr(33, 120), 0, 1), 0, "eds tag");

	// The offset survives the round trip.
	CHECK_EQ_MSG(ers32::get(stack_ptr(200), 2, 30), 200, "stack offset");
	CHECK_EQ_MSG(ers32::get(global_ptr(40), 2, 30), 40, "global offset");
	CHECK_EQ_MSG(ers32::get(eds_ptr(33, 120), 2, 11), 33, "eds segment");
	CHECK_EQ_MSG(ers32::get(eds_ptr(33, 120), 13, 19), 120, "eds offset");
}


int main()
{
	std::printf("FOCUS addressing unit tests\n\n");
	test_pointer_formats();
	test_figure_2_1();
	test_bounds_checking();
	test_eds_traps();

	std::printf("\n%d checks, %d failed\n", g_run, g_fail);
	std::printf(g_fail ? "FAIL\n" : "PASS\n");
	return g_fail ? 1 : 0;
}
