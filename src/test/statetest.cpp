// license:GPL-2.0+
// copyright-holders:Joel Heikkila
// Unit tests for the FOCUS register model (src/cpu/focus/focusstate.h).
//
// The interesting risk here is bit numbering: the ERS counts bit 0 as the
// most significant, which is the opposite of every convention the host uses.
// Most of these tests pin down that conversion against values worked out by
// hand from the ERS field diagram on sheet 12.

#include "../cpu/focus/focusstate.h"

#include <cstdio>
#include <cstdint>

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


static void test_ers_bit_numbering()
{
	std::printf("ERS bit numbering\n");

	// Bit 0 is the MSB, so a one-bit field at 0 is 0x80000000.
	CHECK_EQ(ers32::mask(0, 1), 0x80000000u);
	CHECK_EQ(ers32::mask(31, 1), 0x00000001u);

	// CSN is (20:12): the low twelve bits.
	CHECK_EQ(ers32::mask(20, 12), 0x00000fffu);
	CHECK_EQ(ers32::get(0x00000abc, 20, 12), 0xabcu);

	// RM is (11:2) -> bits 11 and 12 counting from the MSB, which is
	// 0x00180000 in host terms.
	CHECK_EQ(ers32::mask(11, 2), 0x00180000u);

	// A full-width field must not shift by 32.
	CHECK_EQ(ers32::mask(0, 32), 0xffffffffu);
	CHECK_EQ(ers32::get(0xdeadbeef, 0, 32), 0xdeadbeefu);

	// set() must not disturb neighbouring bits.
	CHECK_EQ(ers32::set(0xffffffff, 11, 2, 0), 0xffe7ffffu);
	CHECK_EQ(ers32::set(0x00000000, 11, 2, 3), 0x00180000u);

	// The 8-bit FLAGS register uses the same convention in a byte.
	CHECK_EQ(ers8::mask(0, 1), 0x80u);
	CHECK_EQ(ers8::mask(7, 1), 0x01u);
}


static void test_status_layout()
{
	std::printf("STATUS field layout\n");

	// Every field occupies a distinct, non-overlapping slot. This is the
	// property that makes the reading of the sheet 12 diagram credible, so
	// it is worth asserting rather than assuming.
	const status_field all[] = {
		sf::TV, sf::SL, sf::EP, sf::SS, sf::ES, sf::IR, sf::C, sf::O, sf::T,
		sf::EM, sf::MI, sf::RM, sf::I, sf::M, sf::CC, sf::P, sf::CSN
	};
	CHECK_EQ(sizeof(all) / sizeof(all[0]), 17u);

	u32 seen = 0;
	for (const auto &f : all)
	{
		const u32 m = ers32::mask(f.start, f.width);
		CHECK_EQ(seen & m, 0u);   // no overlap
		seen |= m;
	}

	// Bits 15 and 18 are unlabelled in the diagram and undescribed in the
	// text; everything else is accounted for.
	const u32 reserved = ers32::mask(15, 1) | ers32::mask(18, 1);
	CHECK_EQ(seen | reserved, 0xffffffffu);
	CHECK_EQ(seen & reserved, 0u);
}


static void test_status_access()
{
	std::printf("STATUS accessors\n");

	registers r;
	CHECK_EQ(r.status, 0u);
	CHECK(!r.privileged());

	r.put(sf::M, 1);
	CHECK(r.privileged());
	CHECK_EQ(r.status, ers32::mask(14, 1));

	r.put(sf::CSN, 0x123);
	CHECK_EQ(r.get(sf::CSN), 0x123u);
	CHECK(r.privileged());          // CSN write must not clobber M

	r.put(sf::RM, RM_MINUS_INF);
	CHECK_EQ(r.get(sf::RM), 3u);
	CHECK_EQ(r.get(sf::CSN), 0x123u);

	// Condition code, CCA interpretation (ERS sheet 12).
	r.set_cca(-5);
	CHECK_EQ(r.condition_code(), CCL);
	r.set_cca(0);
	CHECK_EQ(r.condition_code(), CCE);
	r.set_cca(7);
	CHECK_EQ(r.condition_code(), CCG);

	// Setting the condition code must leave CSN and RM alone.
	CHECK_EQ(r.get(sf::CSN), 0x123u);
	CHECK_EQ(r.get(sf::RM), 3u);

	// CCE is 10 binary, not 01 -- an easy thing to get backwards.
	CHECK_EQ(CCE, 2u);
	CHECK_EQ(CCL, 1u);
	CHECK_EQ(CCG, 0u);
}


static void test_alignment_invariants()
{
	std::printf("pointer alignment invariants\n");

	registers r;
	// All-zero fails: SL, S, Q and DL must end in 11 (ERS sheets 10-11).
	CHECK(!r.alignment_ok());

	r.sl = r.s = r.q = r.dl = 3;
	CHECK(r.alignment_ok());

	r.p = 1;                       // P's LSB must be 0
	CHECK(!r.alignment_ok());
	r.p = 2;
	CHECK(r.alignment_ok());

	r.pb = 2;                      // PB's two LSBs must be 0
	CHECK(!r.alignment_ok());
	r.pb = 4;
	CHECK(r.alignment_ok());

	r.s = 2;                       // S's two LSBs must be 1
	CHECK(!r.alignment_ok());
}


static void test_tos_cache()
{
	std::printf("top-of-stack register cache\n");

	tos_cache t;
	t.reset();
	CHECK(t.empty());
	CHECK(!t.full());

	t.push(0x11111111);
	CHECK_EQ(t.valid(), 1u);
	CHECK_EQ(t.a(), 0x11111111u);

	t.push(0x22222222);
	t.push(0x33333333);
	t.push(0x44444444);
	CHECK(t.full());

	// A is the most recently pushed (ERS sheet 57).
	CHECK_EQ(t.a(), 0x44444444u);
	CHECK_EQ(t.b(), 0x33333333u);
	CHECK_EQ(t.c(), 0x22222222u);
	CHECK_EQ(t.d(), 0x11111111u);

	// Pushing when full displaces D, which the caller must spill.
	CHECK_EQ(t.push(0x55555555), 0x11111111u);
	CHECK_EQ(t.a(), 0x55555555u);
	CHECK_EQ(t.d(), 0x22222222u);
	CHECK(t.full());

	CHECK_EQ(t.pop(), 0x55555555u);
	CHECK_EQ(t.a(), 0x44444444u);
	CHECK_EQ(t.valid(), 3u);

	// XCH exchanges A and B.
	t.exchange();
	CHECK_EQ(t.a(), 0x33333333u);
	CHECK_EQ(t.b(), 0x44444444u);
	t.exchange();

	// CAB rotates A, B, C so C comes to the top.
	t.reset();
	t.push(3); t.push(2); t.push(1);     // A=1 B=2 C=3
	t.rotate_abc();
	CHECK_EQ(t.a(), 3u);
	CHECK_EQ(t.b(), 1u);
	CHECK_EQ(t.c(), 2u);

	// DXCH exchanges the double words AB and CD.
	t.reset();
	t.push(4); t.push(3); t.push(2); t.push(1);   // A=1 B=2 C=3 D=4
	t.double_exchange();
	CHECK_EQ(t.a(), 3u);
	CHECK_EQ(t.b(), 4u);
	CHECK_EQ(t.c(), 1u);
	CHECK_EQ(t.d(), 2u);

	// The renamer must not physically move data: a full rotation of pushes
	// and pops returns the window to its starting arrangement.
	t.reset();
	t.push(0xaa); t.push(0xbb);
	const u32 a0 = t.a(), b0 = t.b();
	for (int i = 0; i < 4; i++) { t.push(0); t.pop(); }
	CHECK_EQ(t.a(), a0);
	CHECK_EQ(t.b(), b0);
}


int main()
{
	std::printf("FOCUS register model tests\n\n");
	test_ers_bit_numbering();
	test_status_layout();
	test_status_access();
	test_alignment_invariants();
	test_tos_cache();

	std::printf("\n%d checks, %d failed\n", g_run, g_fail);
	if (g_fail == 0)
		std::printf("PASS\n");
	else
		std::printf("FAIL\n");
	return g_fail ? 1 : 0;
}
