// license:BSD-3-Clause
// Tests for the HP 27140A six-channel multiplexer model (src/iop/focusmux.h),
// driven through the IOP the way HP-UX's HP27140 driver drives it: orders on
// FR8, the Connect Subchannel command on FR9, DMA legs for the blocks, and
// event codes read back from FR9.

#include "../iop/focusiop.h"
#include "../iop/focusmux.h"

#include <cstdio>
#include <cstdint>
#include <map>
#include <string>
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


struct test_host : public iop_core::host
{
	sparse_memory mem;
	bool irq = false;
	memory_interface &iop_memory() override { return mem; }
	void iop_interrupt(bool state) override { irq = state; }
};


static io_interface::target tgt(u8 opcode, u8 channel, u8 pa, u8 ic = 0)
{
	io_interface::target t;
	t.opcode = opcode; t.channel = channel; t.pa = pa; t.ic = ic;
	return t;
}

static constexpr u8 PA = 2;
static constexpr u32 BUF = 0x1000;
static constexpr u8 SENTINEL = 0xee;

// The card under test, with the IOP in front of it.
struct rig
{
	test_host h;
	iop_core iop;
	mux_card card;
	std::vector<u8> events;     // every code read off FR9
	std::vector<u8> sent[6];    // what each port's sink was given

	rig() : iop(h, 1)
	{
		iop.reset();
		iop.attach(PA, &card);
		card.iop = &iop;
		card.pa = PA;
		card.on_tx = [this](unsigned p, u8 c) { sent[p].push_back(c); return true; };
		// Presence and the subchannel connection, as the kernel's scan
		// does them.
		wif(0, 1 << PA);
		u32 v = 0;
		rif(1, v);
		wif(9, (2 << 4) | PA);
	}

	void wif(unsigned n, u32 v) { iop.io_write(tgt(u8(8 * n + 2), 1, PA), v); }
	bool rif(unsigned n, u32 &v) { return iop.io_read(tgt(u8(8 * n + 1), 1, PA), v); }

	// Run the card's clock, collecting whatever it announces.
	void ticks(unsigned n)
	{
		for (unsigned i = 0; i < n; i++)
		{
			card.tick();
			u32 v = 0;
			rif(1, v);
			if (v & 1)
			{
				rif(9, v);
				events.push_back(u8(v));
			}
		}
	}

	bool saw(u8 code) const
	{
		for (u8 e : events) if (e == code) return true;
		return false;
	}
	unsigned count(u8 code) const
	{
		unsigned n = 0;
		for (u8 e : events) if (e == code) n++;
		return n;
	}

	// An input leg of `count` bytes: what the card put in the buffer,
	// up to the first untouched sentinel.
	std::vector<u8> read_leg(unsigned count)
	{
		for (unsigned i = 0; i < count + 2; i++)
			h.mem.write8(BUF + i, SENTINEL);
		iop.io_write(tgt(CMD_WDA, 1, PA), BUF);
		iop.io_write(tgt(CMD_WDC, 1, PA), count);
		iop.io_write(tgt(CMD_WDS, 1, PA), ers32::mask(29, 1));
		iop.io_control(tgt(CMD_EDMA, 1, PA));
		iop.io_control(tgt(CMD_SD, 1, PA));
		for (unsigned i = 0; i < count + 8; i++)
			iop.poll_and_service();
		std::vector<u8> got;
		for (unsigned i = 0; i < count; i++)
		{
			const u8 b = h.mem.read8(BUF + i);
			if (b == SENTINEL)
				break;
			got.push_back(b);
		}
		return got;
	}

	// An output leg carrying `data`.
	void write_leg(const std::vector<u8> &data)
	{
		for (size_t i = 0; i < data.size(); i++)
			h.mem.write8(BUF + u32(i), data[i]);
		iop.io_write(tgt(CMD_WDA, 1, PA), BUF);
		iop.io_write(tgt(CMD_WDC, 1, PA), u32(data.size()));
		iop.io_write(tgt(CMD_WDS, 1, PA), ers32::mask(27, 1) | ers32::mask(29, 1));
		iop.io_control(tgt(CMD_EDMA, 1, PA));
		iop.io_control(tgt(CMD_SD, 1, PA));
		for (size_t i = 0; i < data.size() + 8; i++)
			iop.poll_and_service();
	}

	// Orders, byte mode.
	enum : u8 { IDY = 0x08, DLD_DATA = 0x18, DLD_CTL = 0x19, DIS = 0x38,
			RD = 0x68, WD = 0x78, RDS = 0xa9, RTS = 0x88 };

	std::vector<u8> read(u8 order, unsigned count)
	{
		wif(8, order);
		return read_leg(count);
	}
	void write(u8 order, const std::vector<u8> &data)
	{
		wif(8, order);
		write_leg(data);
	}
	void download()
	{
		write(DLD_CTL, {1, 0});
		write(DLD_CTL, {2, 8, 0, 0, 0x80, 0x00, 0, 0, 0, 4});
		write(DLD_DATA, {0xde, 0xad, 0xbe, 0xef});
		write(DLD_CTL, {3, 0});
		read(RDS, 4);
		write(DLD_CTL, {4, 4, 0, 0, 0x80, 0x00});
	}
};


static void test_presence_and_connect()
{
	std::printf("MUX presence, readiness, connect\n");
	rig r;
	u32 v = 0;
	r.rif(3, v);
	CHECK_EQ(v, 0);
	r.rif(1, v);
	CHECK_EQ(v & 0xc0, 0xc0);
	// The CSC command is answered with an event after the card's delay,
	// and reading it consumes it.
	CHECK_EQ(v & 1, 0);
	r.ticks(3);
	CHECK_EQ(r.count(0x30), 1u);
	r.rif(9, v);
	CHECK_EQ(v, 0);
}


static void test_identify()
{
	std::printf("MUX identify\n");
	rig r;
	// The slot scan reads two bytes: card ID 7, firmware ID 2.
	std::vector<u8> id = r.read(rig::IDY, 2);
	CHECK_EQ(id.size(), 2u);
	if (id.size() == 2)
	{
		CHECK_EQ(id[0], 7);
		CHECK_EQ(id[1], 2);
	}
	CHECK(!r.card.service_request());      // nothing left over
	r.ticks(8);
	CHECK(r.saw(0x10));
	// A longer read gets the six-byte block and Device End.
	std::vector<u8> full = r.read(rig::IDY, 16);
	CHECK_EQ(full.size(), 6u);
	if (full.size() == 6)
	{
		CHECK_EQ(full[4], 0x32);    // MPX 3, MOD 2
		CHECK_EQ(full[5], 0);       // one active port
	}
}


static void test_download()
{
	std::printf("MUX download\n");
	rig r;
	std::vector<u8> s = r.read(rig::RDS, 4);
	CHECK_EQ(s.size(), 1u);
	if (!s.empty())
		CHECK_EQ(s[0], 0xff);       // no verification yet
	r.write(rig::DLD_CTL, {1, 0});
	r.write(rig::DLD_CTL, {2, 8, 0, 0, 0x80, 0x00, 0, 0, 0, 4});
	r.write(rig::DLD_DATA, {0xde, 0xad, 0xbe, 0xef});
	CHECK_EQ(r.card.ram()[0x8000], 0xde);
	CHECK_EQ(r.card.ram()[0x8003], 0xef);
	r.write(rig::DLD_CTL, {3, 0});
	s = r.read(rig::RDS, 4);
	CHECK_EQ(s.size(), 3u);
	if (s.size() == 3)
	{
		CHECK_EQ(s[0], 3);
		CHECK_EQ(s[1], 1);
		CHECK_EQ(s[2], 0);
	}
	CHECK(!r.card.downloaded());
	r.write(rig::DLD_CTL, {4, 4, 0, 0, 0x80, 0x00});
	CHECK(r.card.downloaded());
	std::vector<u8> id = r.read(rig::IDY, 8);
	CHECK_EQ(id.size(), 6u);
	if (id.size() == 6)
	{
		CHECK_EQ(id[1], 3);
		CHECK_EQ(id[5], 5);
	}
	// Six receivers, all disabled: a character typed at port 3 is dropped.
	r.card.receive(3, 'x');
	std::vector<u8> blk = r.read(rig::RD, 64);
	CHECK_EQ(blk.size(), 6 * 4 + 1);
	r.ticks(3);
	CHECK(!r.saw(0x70 | mux_card::PER_LENGTH));
}


static void test_read_block_idle()
{
	std::printf("MUX idle read block\n");
	rig r;
	r.download();
	std::vector<u8> blk = r.read(rig::RD, 64);
	// Status 128 for each of six ports, then the terminator; never empty.
	CHECK_EQ(blk.size(), 25u);
	if (blk.size() == 25)
	{
		for (unsigned p = 0; p < 6; p++)
		{
			CHECK_EQ(blk[4 * p + 0], 0x80 | p);
			CHECK_EQ(blk[4 * p + 1], 128);
			CHECK_EQ(blk[4 * p + 2], 1);
			CHECK_EQ(blk[4 * p + 3], 0);
		}
		CHECK_EQ(blk[24], 0xff);
	}
	// Subfunction 155: status 128 only on change.
	r.write(rig::WD, {0x80, 155, 0, 0xff});
	blk = r.read(rig::RD, 64);
	CHECK_EQ(blk.size(), 1u);
}


static void test_write_data()
{
	std::printf("MUX write data\n");
	rig r;
	r.download();
	r.write(rig::WD, {0x02, 0x00, 3, 'a', 'b', 'c', 0x82, 148, 0, 0xff});
	r.ticks(2);
	CHECK_EQ(r.sent[2].size(), 3u);
	if (r.sent[2].size() == 3)
		CHECK_EQ(r.sent[2][2], 'c');
	CHECK(r.card.port(2).rx_enabled);
	// A stopped transmitter holds its bytes; status 128 counts them.
	r.write(rig::WD, {0x81, 139, 0, 0x01, 0x00, 2, 'x', 'y', 0xff});
	r.ticks(2);
	CHECK_EQ(r.sent[1].size(), 0u);
	std::vector<u8> blk = r.read(rig::RD, 64);
	CHECK_EQ(blk.size(), 25u);
	if (blk.size() == 25)
		CHECK_EQ(blk[4 * 1 + 3], 2);
	r.write(rig::WD, {0x81, 140, 0, 0xff});
	r.ticks(2);
	CHECK_EQ(r.sent[1].size(), 2u);
}


static void test_receive()
{
	std::printf("MUX receive quads\n");
	rig r;
	r.download();
	r.write(rig::WD, {0x80, 148, 0, 0x84, 148, 0, 0xff});
	r.card.receive(0, 'h');
	r.card.receive(4, 'i', mux_card::ATTR_PARITY);
	r.card.receive(0, '\r');
	std::vector<u8> blk = r.read(rig::RD, 128);
	CHECK_EQ(blk.size(), 3 * 4 + 6 * 4 + 1);
	if (blk.size() >= 12)
	{
		CHECK_EQ(blk[0], 0);  CHECK_EQ(blk[1], 0);  CHECK_EQ(blk[2], 1);  CHECK_EQ(blk[3], 'h');
		CHECK_EQ(blk[4], 4);  CHECK_EQ(blk[5], mux_card::ATTR_PARITY);  CHECK_EQ(blk[7], 'i');
		CHECK_EQ(blk[11], '\r');
	}
	// The silo is emptied by the read.
	blk = r.read(rig::RD, 128);
	CHECK_EQ(blk.size(), 25u);
	// Overflow marks the last quad that fitted.
	for (unsigned i = 0; i < mux_card::SILO + 5; i++)
		r.card.receive(0, u8('a' + (i % 26)));
	blk = r.read(rig::RD, 8192);
	CHECK_EQ(blk.size(), 4 * mux_card::SILO + 25);
	if (blk.size() == 4 * mux_card::SILO + 25)
		CHECK_EQ(blk[4 * (mux_card::SILO - 1) + 1], mux_card::ATTR_OVERFLOW);
}


static void test_protocol_errors()
{
	std::printf("MUX protocol errors\n");
	rig r;
	r.download();
	r.write(rig::WD, {0x06, 0, 1, 'x', 0xff});
	r.ticks(12);
	CHECK(r.saw(0x70 | mux_card::PER_PORT));
	r.events.clear();
	r.write(rig::WD, {0x80, 200, 0, 0xff});
	r.ticks(12);
	CHECK(r.saw(0x70 | mux_card::PER_SUBFUNCTION));
	r.events.clear();
	r.write(rig::WD, {0x00, 0, 0, 0xff});
	r.ticks(12);
	CHECK(r.saw(0x70 | mux_card::PER_LENGTH));
	r.events.clear();
	// 200 + 100 bytes to a 255-byte buffer: the second block is refused,
	// the first stands -- processing stops at the first error.
	std::vector<u8> big{0x00, 0, 200};
	for (unsigned i = 0; i < 200; i++) big.push_back('a');
	big.insert(big.end(), {0x00, 0, 100});
	for (unsigned i = 0; i < 100; i++) big.push_back('b');
	big.push_back(0xff);
	r.card.on_tx = nullptr;          // keep the buffer full
	r.write(rig::WD, big);
	r.ticks(12);
	CHECK(r.saw(0x70 | mux_card::PER_TXOVERFLOW));
	r.events.clear();
	// An order this protocol does not use is refused too.
	r.wif(8, rig::RTS);
	r.ticks(12);
	CHECK(r.saw(0x70 | mux_card::PER_ORDER));
	// Bad parameter: a baud code off the table.
	r.events.clear();
	r.write(rig::WD, {0x80, 2, 1, 0x1f, 0xff});
	r.ticks(12);
	CHECK(r.saw(0x70 | mux_card::PER_PARAMETER));
}


static void test_async()
{
	std::printf("MUX asynchronous events\n");
	rig r;
	r.download();
	r.write(rig::WD, {0x80, 148, 0, 0xff});
	r.ticks(10);
	r.events.clear();
	// No mask, no event.
	r.card.receive(0, 'a');
	r.ticks(20);
	CHECK(!r.saw(0x11));
	// Masked in: one data-available event, and only one until the host
	// reads.
	r.write(rig::WD, {0x80, 146, 1, 0x01, 0xff});
	r.events.clear();
	r.ticks(20);
	CHECK_EQ(r.count(0x11), 1u);
	r.card.receive(0, 'b');
	r.ticks(20);
	CHECK_EQ(r.count(0x11), 1u);
	r.read(rig::RD, 128);
	r.events.clear();
	r.card.receive(0, 'c');
	r.ticks(20);
	CHECK_EQ(r.count(0x11), 1u);
	// Silenced.
	r.read(rig::RD, 128);
	r.events.clear();
	r.card.no_async = true;
	r.card.receive(0, 'd');
	r.ticks(20);
	CHECK(!r.saw(0x11));
}


static void test_configuration()
{
	std::printf("MUX configuration subfunctions\n");
	rig r;
	r.download();
	unsigned configured = 0, lines = 0;
	r.card.on_config = [&](unsigned) { configured++; };
	r.card.on_lines = [&](unsigned) { lines++; };
	r.write(rig::WD, {0x83, 2, 1, 0x10,      // 19200
			0x83, 3, 1, 0x26,               // 7 bits, 1 stop, even
			0x83, 135, 0,                   // TR on
			0x83, 141, 0,                   // ask for the lines
			0x83, 142, 0,                   // and the link
			0xff});
	CHECK_EQ(configured, 2u);
	CHECK_EQ(lines, 1u);
	CHECK_EQ(r.card.port(3).baud, 0x10);
	CHECK_EQ(r.card.port(3).frame, 0x26);
	CHECK(r.card.port(3).tr);
	std::vector<u8> blk = r.read(rig::RD, 128);
	// Port by port: ports 0-2 give their status 128, then port 3 its
	// 141, 142 and 128, then the rest.
	CHECK_EQ(blk.size(), 4 + 4 + 25);
	if (blk.size() == 33)
	{
		CHECK_EQ(blk[12], 0x83);
		CHECK_EQ(blk[13], 141);
		CHECK_EQ(blk[15], mux_card::LINE_TR | mux_card::LINE_DM
				| mux_card::LINE_RR | mux_card::LINE_CS);
		CHECK_EQ(blk[17], 142);
		CHECK_EQ(blk[19], 1);           // hardwired: always connected
		CHECK_EQ(blk[21], 128);
	}
}


int main()
{
	test_presence_and_connect();
	test_identify();
	test_download();
	test_read_block_idle();
	test_write_data();
	test_receive();
	test_protocol_errors();
	test_async();
	test_configuration();
	std::printf("\n%d checks, %d failures\n", g_run, g_fail);
	return g_fail ? 1 : 0;
}
