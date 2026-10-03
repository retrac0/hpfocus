// license:BSD-3-Clause
// Tests for the FOCUS Data Segment Table walk (src/cpu/focus/focusdst.h).
//
// Covers the unpaged path, both chaining modes, the paged path, and each
// protection trap. The entry layouts are from ERS sheets 32-34 and the walk
// itself from sheets 55-56.

#include "../cpu/focus/focusdst.h"

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

#define CHECK_EQ(got, want) do { \
		g_run++; \
		const std::uint64_t g_ = (std::uint64_t)(got); \
		const std::uint64_t w_ = (std::uint64_t)(want); \
		if (g_ != w_) { \
			g_fail++; \
			std::printf("  FAIL %s:%d  %s\n        got  0x%llx\n" \
					"        want 0x%llx\n", __FILE__, __LINE__, #got, \
					(unsigned long long)g_, (unsigned long long)w_); \
		} \
	} while (0)


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


// Build a DST word 0 from its fields.
static u32 make_w0(u32 use_count, u32 pagesize, bool p, bool w, bool d,
		bool r, bool m, bool a)
{
	u32 v = 0;
	v = ers32::set(v, 1, 12, use_count);
	v = ers32::set(v, 22, 4, pagesize);
	v = ers32::set(v, 26, 1, p);
	v = ers32::set(v, 27, 1, w);
	v = ers32::set(v, 28, 1, d);
	v = ers32::set(v, 29, 1, r);
	v = ers32::set(v, 30, 1, m);
	v = ers32::set(v, 31, 1, a);
	return v;
}

static u32 make_w3(bool consecutive, u32 link)
{
	return ers32::set(ers32::set(0, 0, 1, consecutive), 1, 12, link);
}

struct fixture
{
	registers r;
	sparse_memory mem;
	dst_walker walker{r, mem};

	static constexpr u32 TABLE = 0x10000;

	fixture()
	{
		r.dst = TABLE;
		r.put(sf::M, 1);              // privileged unless a test says otherwise
		set_entry_count(64);
	}

	void set_entry_count(u32 n) { mem.write32(TABLE, 16 * n); }

	void put_entry(u32 seg, u32 w0, u32 len, u32 loc, u32 w3 = 0)
	{
		const u32 a = TABLE + 16 * seg;
		mem.write32(a, w0);
		mem.write32(a + 4, len - 1);        // the field stores LENGTH - 1
		mem.write32(a + 8, loc);
		mem.write32(a + 12, w3);
	}

	u32 entry_w0(u32 seg) { return mem.read32(TABLE + 16 * seg); }
};


static void test_entry_decoding()
{
	std::printf("DST entry field decoding\n");

	dst_entry e;
	e.w0 = make_w0(1234, 9, true, true, false, false, true, false);
	CHECK_EQ(e.use_count(), 1234u);
	CHECK_EQ(e.pagesize(), 9u);
	CHECK(e.paged());
	CHECK(e.writable());
	CHECK(!e.dirty());
	CHECK(!e.referenced());
	CHECK(e.priv_only());
	CHECK(!e.absent());

	// Flags occupy bits 26-31 in the order P W D R M A, so A is the LSB.
	CHECK_EQ(make_w0(0, 0, false, false, false, false, false, true), 1u);
	CHECK_EQ(make_w0(0, 0, true, false, false, false, false, false), 0x20u);

	// The length field holds LENGTH-1, whose two low bits must both be 1.
	e.w1 = 0x3ff;
	CHECK_EQ(e.length(), 0x400u);
	CHECK_EQ(e.length_minus_1(), 0x3ffu);

	e.w3 = make_w3(true, 77);
	CHECK(e.consecutive());
	CHECK_EQ(e.link(), 77u);
}


static void test_unpaged()
{
	std::printf("unpaged segment resolution\n");

	fixture f;
	u32 addr = 0;
	trap t = trap::none;

	// Segment 5: 0x400 bytes at 0x40000, writable.
	f.put_entry(5, make_w0(1, 0, false, true, false, false, false, false),
			0x400, 0x40000);

	CHECK(f.walker.resolve(false, 5, 0, false, addr, t));
	CHECK_EQ(addr, 0x40000u);

	CHECK(f.walker.resolve(false, 5, 0x100, false, addr, t));
	CHECK_EQ(addr, 0x40100u);

	// The last byte is in bounds: valid offsets run 0 .. length-1 (sheet 18).
	t = trap::none;
	CHECK(f.walker.resolve(false, 5, 0x3ff, false, addr, t));
	CHECK_EQ(addr, 0x403ffu);

	// One past the end, with no link, is a bounds violation.
	t = trap::none;
	CHECK(!f.walker.resolve(false, 5, 0x400, false, addr, t));
	CHECK(t == trap::eds_bounds_violation);

	// "R := 1; If mod mem inst then D := 1" (ERS sheet 55): every
	// successful evaluation marks the entry referenced, a modifying one
	// marks it dirty as well, and nothing else in word 0 moves. HP-UX's
	// segment aging sweep depends on R (see focusdst.h). The CST layout's
	// absence bit is 31, not this one.
	dst_entry e;
	e.w0 = f.entry_w0(5);
	CHECK(e.referenced());
	CHECK(!e.dirty());
	CHECK_EQ(e.w0, make_w0(1, 0, false, true, false, true, false, false));

	t = trap::none;
	CHECK(f.walker.resolve(false, 5, 0x10, true, addr, t));
	e.w0 = f.entry_w0(5);
	CHECK(e.referenced());
	CHECK(e.dirty());
	CHECK_EQ(e.w0, make_w0(1, 0, false, true, true, true, false, false));

	// A failed evaluation marks nothing: segment 6 is absent, and one
	// that is out of bounds is not touched either.
	f.put_entry(6, make_w0(0, 0, false, true, false, false, false, true),
			0x400, 0x50000);
	t = trap::none;
	CHECK(!f.walker.resolve(false, 6, 0x10, true, addr, t));
	CHECK(t == trap::absent_segment);
	CHECK_EQ(f.entry_w0(6), make_w0(0, 0, false, true, false, false, false, true));
	f.put_entry(7, make_w0(0, 0, false, true, false, false, false, false),
			0x400, 0x60000);
	t = trap::none;
	CHECK(!f.walker.resolve(false, 7, 0x400, true, addr, t));
	CHECK(t == trap::eds_bounds_violation);
	CHECK_EQ(f.entry_w0(7), make_w0(0, 0, false, true, false, false, false, false));
}


static void test_protection()
{
	std::printf("protection traps\n");

	fixture f;
	u32 addr = 0;
	trap t = trap::none;

	// Read-only segment.
	f.put_entry(6, make_w0(1, 0, false, false, false, false, false, false),
			0x100, 0x50000);
	CHECK(f.walker.resolve(false, 6, 0, false, addr, t));
	t = trap::none;
	CHECK(!f.walker.resolve(false, 6, 0, true, addr, t));
	CHECK(t == trap::data_seg_write_violation);

	// Privileged-only segment, accessed unprivileged.
	f.put_entry(7, make_w0(1, 0, false, true, false, false, true, false),
			0x100, 0x51000);
	t = trap::none;
	CHECK(f.walker.resolve(false, 7, 0, false, addr, t));   // still privileged
	f.r.put(sf::M, 0);
	t = trap::none;
	CHECK(!f.walker.resolve(false, 7, 0, false, addr, t));
	CHECK(t == trap::priv_mode_data_violation);
	f.r.put(sf::M, 1);

	// Absent segment.
	f.put_entry(8, make_w0(1, 0, false, true, false, false, false, true),
			0x100, 0x52000);
	t = trap::none;
	CHECK(!f.walker.resolve(false, 8, 0, false, addr, t));
	CHECK(t == trap::absent_segment);

	// Segment number beyond the table.
	t = trap::none;
	CHECK(!f.walker.resolve(false, 1000, 0, false, addr, t));
	CHECK(t == trap::dst_violation);

	// Segment zero.
	t = trap::none;
	CHECK(!f.walker.resolve(false, 0, 0, false, addr, t));
	CHECK(t == trap::eds_pointer_violation);
}


static void test_chaining()
{
	std::printf("segment chaining (section 3.2.2)\n");

	// Non-consecutive chain: C = 0, LINK names the next segment, and the
	// offset has the current segment's length subtracted before the hop.
	{
		fixture f;
		u32 addr = 0;
		trap t = trap::none;

		f.put_entry(10, make_w0(1, 0, false, true, false, false, false, false),
				0x100, 0x60000, make_w3(false, 20));
		f.put_entry(20, make_w0(1, 0, false, true, false, false, false, false),
				0x100, 0x70000, make_w3(false, 30));
		f.put_entry(30, make_w0(1, 0, false, true, false, false, false, false),
				0x100, 0x80000, make_w3(false, 0));

		CHECK(f.walker.resolve(false, 10, 0x40, false, addr, t));
		CHECK_EQ(addr, 0x60040u);

		t = trap::none;
		CHECK(f.walker.resolve(false, 10, 0x140, false, addr, t));
		CHECK_EQ(addr, 0x70040u);          // second link

		t = trap::none;
		CHECK(f.walker.resolve(false, 10, 0x240, false, addr, t));
		CHECK_EQ(addr, 0x80040u);          // third link

		// Past the end of the chain: the last entry's link is zero.
		t = trap::none;
		CHECK(!f.walker.resolve(false, 10, 0x340, false, addr, t));
		CHECK(t == trap::eds_bounds_violation);
	}

	// Consecutive chain: C = 1, the segments occupy consecutive DST entries
	// so the walk indexes straight to the right one, and LINK is the highest
	// entry number in the run.
	{
		fixture f;
		u32 addr = 0;
		trap t = trap::none;

		for (u32 s = 40; s <= 43; s++)
			f.put_entry(s, make_w0(1, 0, false, true, false, false, false, false),
					0x100, 0x90000 + (s - 40) * 0x1000, make_w3(true, 43));

		CHECK(f.walker.resolve(false, 40, 0x40, false, addr, t));
		CHECK_EQ(addr, 0x90040u);

		t = trap::none;
		CHECK(f.walker.resolve(false, 40, 0x240, false, addr, t));
		CHECK_EQ(addr, 0x92040u);          // two entries along

		// Beyond the run's last entry.
		t = trap::none;
		CHECK(!f.walker.resolve(false, 40, 0x440, false, addr, t));
		CHECK(t == trap::eds_bounds_violation);
	}
}


static void test_paged()
{
	std::printf("paged segment resolution\n");

	fixture f;
	u32 addr = 0;
	trap t = trap::none;

	// Segment 50 is paged with 256-byte pages (pagesize 8). Its LOCATION
	// points at a page table whose length covers four 2-word entries.
	const u32 PT = 0xa0000;
	f.put_entry(50, make_w0(1, 8, true, false, false, false, false, false),
			0x40, PT);

	// Page table entries: word 0 flags, word 1 page-aligned location.
	auto put_page = [&](u32 n, u32 loc, bool w, bool m, bool a)
	{
		u32 w0 = 0;
		w0 = ers32::set(w0, 27, 1, w);
		w0 = ers32::set(w0, 30, 1, m);
		w0 = ers32::set(w0, 31, 1, a);
		f.mem.write32(PT + 8 * n, w0);
		f.mem.write32(PT + 8 * n + 4, loc);
	};

	put_page(0, 0xb0000, true, false, false);
	put_page(1, 0xc0000, true, false, false);
	put_page(2, 0xd0000, false, false, false);   // read only
	put_page(3, 0xe0000, true, false, true);     // absent

	// Offset 0x0080 -> page 0, offset 0x80.
	CHECK(f.walker.resolve(false, 50, 0x0080, false, addr, t));
	CHECK_EQ(addr, 0xb0080u);

	// Offset 0x0140 -> page 1, offset 0x40.
	t = trap::none;
	CHECK(f.walker.resolve(false, 50, 0x0140, false, addr, t));
	CHECK_EQ(addr, 0xc0040u);

	// Page 2 is read only.
	t = trap::none;
	CHECK(f.walker.resolve(false, 50, 0x0200, false, addr, t));
	t = trap::none;
	CHECK(!f.walker.resolve(false, 50, 0x0200, true, addr, t));
	CHECK(t == trap::data_seg_write_violation);

	// Page 3 is absent.
	t = trap::none;
	CHECK(!f.walker.resolve(false, 50, 0x0300, false, addr, t));
	CHECK(t == trap::absent_page);

	// Beyond the page table.
	t = trap::none;
	CHECK(!f.walker.resolve(false, 50, 0x4000, false, addr, t));
	CHECK(t == trap::page_table_violation);

	// The reference bit lands in the page table entry, not the DST entry.
	page_entry pe;
	pe.w0 = f.mem.read32(PT + 0);
	CHECK(pe.referenced());

	// An absent page table traps before any page is consulted.
	f.put_entry(51, make_w0(1, 8, true, false, false, false, false, true),
			0x40, PT);
	t = trap::none;
	CHECK(!f.walker.resolve(false, 51, 0, false, addr, t));
	CHECK(t == trap::absent_page_table);

	// A paged segment reached by following a chain is an error (sheet 56).
	f.put_entry(60, make_w0(1, 0, false, true, false, false, false, false),
			0x100, 0x60000, make_w3(false, 50));
	t = trap::none;
	CHECK(!f.walker.resolve(false, 60, 0x180, false, addr, t));
	CHECK(t == trap::unexpected_eds_type);
}


static void test_system_table()
{
	std::printf("system versus user table\n");

	fixture f;
	u32 addr = 0;
	trap t = trap::none;

	// The system table's entry zero sits at 2200 octal (ERS sheet 32).
	CHECK_EQ(dst_walker::SYSTEM_DST_BASE, 02200u);
	f.mem.write32(dst_walker::SYSTEM_DST_BASE, 16 * 64);

	const u32 sys_entry = dst_walker::SYSTEM_DST_BASE + 16 * 3;
	f.mem.write32(sys_entry, make_w0(1, 0, false, true, false, false, false, false));
	f.mem.write32(sys_entry + 4, 0xff);
	f.mem.write32(sys_entry + 8, 0x77000);
	f.mem.write32(sys_entry + 12, 0);

	CHECK(f.walker.resolve(true, 3, 0x20, false, addr, t));
	CHECK_EQ(addr, 0x77020u);

	// The same segment number in the user table is a different segment.
	f.put_entry(3, make_w0(1, 0, false, true, false, false, false, false),
			0x100, 0x88000);
	t = trap::none;
	CHECK(f.walker.resolve(false, 3, 0x20, false, addr, t));
	CHECK_EQ(addr, 0x88020u);
}


static void test_integration_with_addressing()
{
	std::printf("addressing unit driving a real DST\n");

	registers r;
	sparse_memory mem;
	dst_walker walker(r, mem);
	addressing_unit au(r, mem, walker);

	r.sb = 0x3000; r.q = 0x3200; r.s = 0x33ff;
	r.db = 0x5000; r.dl = 0x5fff;
	r.dst = 0x10000;
	r.x = 0;
	r.put(sf::M, 1);

	mem.write32(r.dst, 16 * 64);
	const u32 a = r.dst + 16 * 33;
	mem.write32(a, make_w0(1, 0, false, true, false, false, false, false));
	mem.write32(a + 4, 0x3ff);
	mem.write32(a + 8, 0x8000);
	mem.write32(a + 12, 0);

	// An external data segment pointer in the stack, as Figure 2.1 has it.
	// PTR(1:1) selects the table: 0 is the system DST, 1 the user DST
	// (ERS sheet 55). This one targets the user table set up above.
	u32 ptr = ers32::set(ers32::set(0, 2, 11, 33), 13, 19, 120);
	ptr = ers32::set(ptr, 1, 1, 1);
	mem.write32(r.q - 20, ptr);
	mem.write32(0x8000 + 120, 0xfeedface);

	ea_result e = au.effective_address(BASE_Q_MINUS, 20, true, false);
	CHECK(e.ok());
	CHECK_EQ(e.addr, 0x8000u + 120u);
	CHECK_EQ(mem.read32(e.addr), 0xfeedfaceu);

	// Indexing adds to the offset within the segment.
	r.x = 20;
	mem.write32(0x8000 + 140, 0xcafebabe);
	e = au.effective_address(BASE_Q_MINUS, 20, true, true);
	CHECK(e.ok());
	CHECK_EQ(mem.read32(e.addr), 0xcafebabeu);
}


int main()
{
	std::printf("FOCUS Data Segment Table tests\n\n");
	test_entry_decoding();
	test_unpaged();
	test_protection();
	test_chaining();
	test_paged();
	test_system_table();
	test_integration_with_addressing();

	std::printf("\n%d checks, %d failed\n", g_run, g_fail);
	std::printf(g_fail ? "FAIL\n" : "PASS\n");
	return g_fail ? 1 : 0;
}
