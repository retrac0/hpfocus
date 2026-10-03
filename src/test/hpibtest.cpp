// license:BSD-3-Clause
// Tests for the HP 27110A HP-IB card and the CS/80 drive behind it
// (src/iop/focushpib.h), driven the way HP-UX 5.30's HP27110 module drives
// them: WC and a control block, then RD or WD with one DMA leg per 1 KB
// buffer, then RS and the status block.

#include <cstdint>
// The card models take u64 from MAME's own headers; this build has none.
using u64 = std::uint64_t;

#include "../iop/focushpib.h"

#include <cstdio>
#include <initializer_list>
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
		const long long g_ = (long long)(got); \
		const long long w_ = (long long)(want); \
		if (g_ != w_) { \
			g_fail++; \
			std::printf("  FAIL %s:%d  %s\n        got  %lld\n" \
					"        want %lld\n", __FILE__, __LINE__, #got, g_, w_); \
		} \
	} while (0)

// The orders, as FR8 carries them (opcode in bits 7:4, 27110A 4-2).
enum : u8 { WC = 0x58, RD = 0x60, WD = 0x70, RS = 0x48 };

// The card's header for a CS/80 transaction -- function code, flags, a
// four-byte length, the device address, the secondary -- and from byte 8
// the CS/80 command message (27110A 4-30). Function code 0x88 is a CS/80
// transaction with a data block going out (a write), 0x8c one coming in.
static std::vector<u8> control_block(u8 fc, u32 length, std::vector<u8> cs80)
{
	std::vector<u8> b{ fc, 0x41, u8(length >> 24), u8(length >> 16),
			u8(length >> 8), u8(length), 0x00, 0xe5 };
	b.insert(b.end(), cs80.begin(), cs80.end());
	return b;
}

static std::vector<u8> set_address(u32 block)
{
	return { 0x10, 0, 0, u8(block >> 24), u8(block >> 16), u8(block >> 8),
			u8(block) };
}

static std::vector<u8> set_length(u32 bytes)
{
	return { 0x18, u8(bytes >> 24), u8(bytes >> 16), u8(bytes >> 8),
			u8(bytes) };
}

static std::vector<u8> cat(std::initializer_list<std::vector<u8>> parts)
{
	std::vector<u8> v;
	for (const auto &p : parts)
		v.insert(v.end(), p.begin(), p.end());
	return v;
}

// One outbound DMA leg: the IOP hands the card the bytes and the count
// runs out.
static void leg_out(hpib_card &card, const std::vector<u8> &bytes)
{
	for (u8 b : bytes)
		card.dma_write(b);
	card.dma_complete(true);
}

// One inbound DMA leg of up to `n` bytes.
static std::vector<u8> leg_in(hpib_card &card, size_t n)
{
	std::vector<u8> v;
	while (v.size() < n && card.service_request())
		v.push_back(card.dma_read());
	card.dma_complete(false);
	return v;
}

// The status leg: TSTAT, QSTAT, and when QSTAT is not zero the Request
// Status report the card fetched by itself and its closing QSTAT.
static std::vector<u8> status(hpib_card &card)
{
	card.write_fr(8, RS);
	return leg_in(card, 23);
}

static u8 qstat(const std::vector<u8> &st) { return st.size() > 1 ? st[1] : 0xff; }

// Status bit n of the report the card appended (CS/80 2-38: two
// identification bytes, then the eight error-field bytes).
static bool status_bit(const std::vector<u8> &st, unsigned n)
{
	const size_t at = 2 + 2 + n / 8;
	return st.size() > at && (st[at] & (0x80 >> (n & 7)));
}

static void collect_poweron(hpib_card &card)
{
	// The first transaction answers QSTAT 2 and the card fetches the
	// Request Status that clears it.
	card.write_fr(8, WC);
	leg_out(card, control_block(0x80, 0, { 0x20, 0x40, 0x34 }));
	status(card);
}

// One write transaction: the command, then each leg of data in turn.
static std::vector<u8> write(hpib_card &card, u32 block, u32 length,
		std::initializer_list<std::vector<u8>> legs,
		std::vector<u8> prefix = { 0x20, 0x40 })
{
	card.write_fr(8, WC);
	leg_out(card, control_block(0x88, length,
			cat({ prefix, set_address(block), set_length(length), { 0x02 } })));
	card.write_fr(8, WD);
	for (const auto &l : legs)
		leg_out(card, l);
	return status(card);
}


// A write longer than one DMA leg. HP-UX sends a 3 KB write as one WD order
// and three 1 KB legs, each out of a different buffer; all three have to
// reach the medium. The card used to store the first and discard the rest.
static void test_write_in_several_legs()
{
	std::printf("CS/80 Locate and Write fed in several DMA legs\n");
	hpib_card card;
	card.disc.image.assign(64 * 1024, 0xee);
	collect_poweron(card);

	const std::vector<u8> st = write(card, 16, 3072, {
			std::vector<u8>(1024, 0x11), std::vector<u8>(1024, 0x22),
			std::vector<u8>(1024, 0x33) });
	CHECK_EQ(st.size(), 2);
	CHECK_EQ(qstat(st), 0);
	CHECK(!card.disc.writing());        // the length was met

	const std::vector<u8> &img = card.disc.image;
	CHECK_EQ(img[4096 - 1], 0xee);      // nothing before
	CHECK_EQ(img[4096], 0x11);
	CHECK_EQ(img[4096 + 1023], 0x11);
	CHECK_EQ(img[4096 + 1024], 0x22);   // the second leg
	CHECK_EQ(img[4096 + 2047], 0x22);
	CHECK_EQ(img[4096 + 2048], 0x33);   // and the third
	CHECK_EQ(img[4096 + 3071], 0x33);
	CHECK_EQ(img[4096 + 3072], 0xee);   // nothing after

	// The target address is left after everything that was written: a
	// Locate and Read with a Set Length alone continues from there.
	card.disc.image[4096 + 3072] = 0x5a;
	card.write_fr(8, WC);
	leg_out(card, control_block(0x8c, 256, cat({ set_length(256), { 0x00 } })));
	card.write_fr(8, RD);
	const std::vector<u8> next = leg_in(card, 256);
	CHECK_EQ(next.size(), 256);
	CHECK_EQ(next.empty() ? 0 : next[0], 0x5a);
	status(card);
}


// Legs past the Set Length are not the write's: a host that sends more
// than it said stores only what it said.
static void test_write_stops_at_its_length()
{
	std::printf("A write takes no more than its Set Length\n");
	hpib_card card;
	card.disc.image.assign(16 * 1024, 0xee);
	collect_poweron(card);

	write(card, 4, 1024, { std::vector<u8>(1024, 0x11),
			std::vector<u8>(1024, 0x22) });
	CHECK_EQ(card.disc.image[1024], 0x11);
	CHECK_EQ(card.disc.image[2047], 0x11);
	CHECK_EQ(card.disc.image[2048], 0xee);
}


// The data of one write must not leak into the next transaction: a write
// whose data never came ends with its transaction, and a later data leg
// under a transaction that is not a write stores nothing.
static void test_write_ends_with_its_transaction()
{
	std::printf("An abandoned write ends with its transaction\n");
	hpib_card card;
	card.disc.image.assign(16 * 1024, 0xee);
	collect_poweron(card);

	card.write_fr(8, WC);
	leg_out(card, control_block(0x88, 1024,
			cat({ set_address(4), set_length(1024), { 0x02 } })));
	status(card);                       // the host gave up before the data

	// A describe, then a stray data leg.
	card.write_fr(8, WC);
	leg_out(card, control_block(0x8c, 37, { 0x20, 0x40, 0x35 }));
	CHECK(!card.disc.writing());
	card.write_fr(8, WD);
	leg_out(card, std::vector<u8>(1024, 0x77));
	status(card);
	CHECK_EQ(card.disc.image[1024], 0xee);

	// A write left half done is closed by the next one, which lands at
	// its own address and not after the first one's half.
	write(card, 8, 2048, { std::vector<u8>(1024, 0x44) });
	const std::vector<u8> st = write(card, 20, 1024,
			{ std::vector<u8>(1024, 0x55) });
	CHECK_EQ(qstat(st), 0);
	CHECK_EQ(card.disc.image[8 * 256], 0x44);
	CHECK_EQ(card.disc.image[8 * 256 + 1024], 0xee);
	CHECK_EQ(card.disc.image[20 * 256], 0x55);
}


// "If a partial block is written, the remainder of the block is filled by
// duplicating the last data byte of the transfer" (CS/80 2-10).
static void test_partial_block()
{
	std::printf("A partial block is filled with the last byte\n");
	hpib_card card;
	card.disc.image.assign(16 * 1024, 0xee);
	collect_poweron(card);

	std::vector<u8> data(300, 0x11);
	data.back() = 0x99;
	write(card, 2, 300, { data });
	CHECK_EQ(card.disc.image[512 + 298], 0x11);
	CHECK_EQ(card.disc.image[512 + 299], 0x99);
	CHECK_EQ(card.disc.image[512 + 300], 0x99);
	CHECK_EQ(card.disc.image[512 + 511], 0x99);
	CHECK_EQ(card.disc.image[512 + 512], 0xee);     // the next block
}


// A write that runs off the end of the volume is an address bounds error
// in its own status, and its data goes nowhere. It used to be clipped at
// the end of the image with a QSTAT of 0.
static void test_write_past_the_end()
{
	std::printf("A write past the end is an address bounds error\n");
	hpib_card card;
	card.disc.image.assign(16 * 1024, 0xee);    // 64 blocks
	collect_poweron(card);

	const std::vector<u8> st = write(card, 62, 1024,
			{ std::vector<u8>(1024, 0x11) });
	CHECK_EQ(qstat(st), 1);
	CHECK(status_bit(st, 7));
	CHECK_EQ(card.disc.image[62 * 256], 0xee);
	CHECK(!card.disc.writing());

	// The last whole kilobyte is fine.
	const std::vector<u8> ok = write(card, 60, 1024,
			{ std::vector<u8>(1024, 0x22) });
	CHECK_EQ(qstat(ok), 0);
	CHECK_EQ(card.disc.image[16 * 1024 - 1], 0x22);
}


// "An illegal unit number ... produces a Module Addressing status error"
// (CS/80 2-12), and so does an illegal volume (2-13); a reject error stops
// the message there. HP-UX's boot probe asks every unit for a Describe and
// takes QSTAT 1 for "no such unit". The drive used to flag the error and
// then run the rest of the message against unit 0's medium anyway.
static void test_no_such_unit()
{
	std::printf("Set Unit / Set Volume to one that is not there\n");
	hpib_card card;
	card.disc.image.assign(16 * 1024, 0xee);
	collect_poweron(card);

	card.write_fr(8, WC);
	leg_out(card, control_block(0x8c, 37, { 0x21, 0x40, 0x35 }));
	card.write_fr(8, RD);
	const std::vector<u8> d = leg_in(card, 37);
	const std::vector<u8> st = status(card);
	CHECK_EQ(d.size(), 0);              // no Describe of a unit not there
	CHECK_EQ(qstat(st), 1);
	CHECK(status_bit(st, 6));           // module addressing

	// A write addressed to unit 1 does not reach unit 0's medium.
	const std::vector<u8> w = write(card, 4, 1024,
			{ std::vector<u8>(1024, 0x11) }, { 0x21, 0x40 });
	CHECK_EQ(qstat(w), 1);
	CHECK_EQ(card.disc.image[1024], 0xee);

	// Nor one addressed to volume 1 of unit 0.
	const std::vector<u8> v = write(card, 4, 1024,
			{ std::vector<u8>(1024, 0x22) }, { 0x20, 0x41 });
	CHECK_EQ(qstat(v), 1);
	CHECK(status_bit(v, 6));
	CHECK_EQ(card.disc.image[1024], 0xee);

	// Unit 0, volume 0 and the controller are still there.
	const std::vector<u8> ok = write(card, 4, 1024,
			{ std::vector<u8>(1024, 0x33) });
	CHECK_EQ(qstat(ok), 0);
	CHECK_EQ(card.disc.image[1024], 0x33);
	card.write_fr(8, WC);
	leg_out(card, control_block(0x8c, 5, { 0x2f, 0x35 }));
	card.write_fr(8, RD);
	CHECK_EQ(leg_in(card, 37).size(), 5);
	CHECK_EQ(qstat(status(card)), 0);
}


// Describe's volume field: 7 heads of 64 sectors, as many cylinders as
// the image holds WHOLE, and a maximum block address inside the image.
static void test_describe_size()
{
	std::printf("Describe stays inside the image\n");
	for (u32 blocks : { 523264u, 448u * 10 + 100, 300u })
	{
		hpib_card card;
		card.disc.image.assign(size_t(blocks) * 256, 0);
		collect_poweron(card);
		card.write_fr(8, WC);
		leg_out(card, control_block(0x8c, 37, { 0x20, 0x40, 0x35 }));
		card.write_fr(8, RD);
		const std::vector<u8> d = leg_in(card, 37);
		status(card);
		CHECK_EQ(d.size(), 37);
		if (d.size() != 37)
			continue;
		const u32 maxcyl = (u32(d[24]) << 16) | (u32(d[25]) << 8) | d[26];
		const u32 maxblk = (u32(d[33]) << 16) | (u32(d[34]) << 8) | d[35];
		CHECK_EQ(d[27], 6);                         // max head
		CHECK_EQ((d[28] << 8) | d[29], 63);         // max sector
		CHECK(maxblk < blocks);
		if (blocks >= 448)
		{
			CHECK_EQ(maxcyl, blocks / 448 - 1);
			CHECK_EQ(maxblk + 1, (maxcyl + 1) * 448);
		}
	}
}


int main()
{
	test_write_in_several_legs();
	test_write_stops_at_its_length();
	test_write_ends_with_its_transaction();
	test_partial_block();
	test_write_past_the_end();
	test_no_such_unit();
	test_describe_size();
	std::printf("\n%d checks, %d failed\n", g_run, g_fail);
	return g_fail ? 1 : 0;
}
