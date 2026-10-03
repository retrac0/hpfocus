// license:BSD-3-Clause
// Tests for the HP 27128A Asynchronous Serial Interface model
// (src/iop/focusasi.h): its line discipline, driven through the IOP the way
// HP-UX's HP27128/30 driver drives it -- orders on FR8, DMA legs for the
// request blocks and the data, and Read Transparent Status for what the
// card has to say. The configuration blocks are the two HP-UX 5.30 was
// measured writing on the console: one for a tty in canonical mode, one
// for every other mode.

#include "../iop/focusiop.h"
#include "../iop/focusasi.h"

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

static constexpr u8 PA = 0;
static constexpr u32 BUF = 0x1000;
static constexpr u8 SENTINEL = 0xee;

// What HP-UX writes with write card configuration, subfunction 0.
// CANONICAL is a tty in canonical mode: the card echoes, edits, ends a
// record on NL, CR, EOT or NUL, takes ^C as signal character 1 and expands
// LF to CR-LF on output. RAW is every other mode: no processing, alert 1.
static const std::vector<u8> CANONICAL = {
	0xb2, 0x00, 0x00, 0x00, 0x00, 0x02, 0x08, 0x15, 0x01, 0x76, 0x0f, 0x03,
	0x00, 0x30, 0x19, 0x00, 0x00, 0x28, 0x00, 0x05, 0xf0, 0x00, 0x37, 0x00,
	0x11, 0x13, 0x11, 0x13, 0x05, 0x06, 0x50, 0x00, 0x00, 0x0d, 0x02, 0x0d,
	0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1f, 0x00, 0x5c, 0x0a, 0x03, 0x1c,
	0x04, 0x04, 0x0a, 0x0d, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00 };
static const std::vector<u8> RAW = {
	0x00, 0x00, 0x00, 0x01, 0x00, 0x02, 0x23, 0x40, 0x02, 0x30, 0x0f, 0x03,
	0x00, 0x10, 0x19, 0x00, 0x00, 0x28, 0x00, 0x05, 0xf0, 0x00, 0x37, 0x00,
	0x11, 0x13, 0x11, 0x13, 0x05, 0x06, 0x50, 0x00, 0x00, 0x00, 0x02, 0x0d,
	0x0a, 0x00, 0x00, 0x00, 0x00, 0x00, 0x28, 0x00, 0x5c, 0x0a, 0x7f, 0x1c,
	0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

// The card under test, with the IOP in front of it.
struct rig
{
	test_host h;
	iop_core iop;
	asi_card card;
	std::string screen;         // what the terminal was sent
	bool last_leg_ended = false;

	rig() : iop(h, 1)
	{
		iop.reset();
		iop.attach(PA, &card);
		card.iop = &iop;
		card.pa = PA;
		card.on_char = [this](u8 c) { screen.push_back(char(c)); };
		wif(0, 1 << PA);
		u32 v = 0;
		rif(1, v);
		wif(9, (2 << 4) | PA);
	}

	void wif(unsigned n, u32 v) { iop.io_write(tgt(u8(8 * n + 2), 1, PA), v); }
	bool rif(unsigned n, u32 &v) { return iop.io_read(tgt(u8(8 * n + 1), 1, PA), v); }

	// The card's clock. The event codes it announces on FR9 are read and
	// dropped, as the kernel's attention handler does.
	void ticks(unsigned n)
	{
		for (unsigned i = 0; i < n; i++)
		{
			card.tick();
			u32 v = 0;
			rif(1, v);
			if (v & 1)
				rif(9, v);
		}
	}

	bool leg_over()
	{
		u32 v = 0;
		iop.io_read(tgt(CMD_RDCS, 1, PA), v);
		return (v & 0x2001) != 0;   // on count, or on Device End
	}

	// An input leg of `count` units; `bytes` false is halfword mode, as
	// the driver's read device data order (0x60) uses.
	std::vector<u8> read_leg(unsigned count, bool bytes = true)
	{
		const unsigned size = bytes ? count : 2 * count;
		for (unsigned i = 0; i < size + 2; i++)
			h.mem.write8(BUF + i, SENTINEL);
		iop.io_write(tgt(CMD_WDA, 1, PA), BUF);
		iop.io_write(tgt(CMD_WDC, 1, PA), count);
		iop.io_write(tgt(CMD_WDS, 1, PA), bytes ? ers32::mask(29, 1) : 0);
		iop.io_control(tgt(CMD_EDMA, 1, PA));
		iop.io_control(tgt(CMD_SD, 1, PA));
		for (unsigned i = 0; i < 8; i++)
			iop.poll_and_service();
		last_leg_ended = leg_over();
		iop.io_control(tgt(CMD_DDMA, 1, PA));
		std::vector<u8> got;
		for (unsigned i = 0; i < size; i++)
		{
			const u8 b = h.mem.read8(BUF + i);
			if (b == SENTINEL)
				break;
			got.push_back(b);
		}
		return got;
	}

	void write_leg(const std::vector<u8> &data)
	{
		for (size_t i = 0; i < data.size(); i++)
			h.mem.write8(BUF + u32(i), data[i]);
		iop.io_write(tgt(CMD_WDA, 1, PA), BUF);
		iop.io_write(tgt(CMD_WDC, 1, PA), u32(data.size()));
		iop.io_write(tgt(CMD_WDS, 1, PA), ers32::mask(27, 1) | ers32::mask(29, 1));
		iop.io_control(tgt(CMD_EDMA, 1, PA));
		iop.io_control(tgt(CMD_SD, 1, PA));
		for (unsigned i = 0; i < 8; i++)
			iop.poll_and_service();
		last_leg_ended = leg_over();
		iop.io_control(tgt(CMD_DDMA, 1, PA));
	}

	enum : u8 { PAU = 0x20, RS = 0x49, RD = 0x68, RD_WORDS = 0x60, WD = 0x78,
			RTS = 0x88, WTC = 0x98, CLC = 0xb8 };
	enum : u8 { READ = 1, WRITE = 2, CONTROL_DEVICE = 3, CARD_INFO = 4,
			CONFIGURE = 5, CONTROL_CARD = 6 };

	// Read Transparent Status: the report, as many bytes as the card gave.
	std::vector<u8> rts()
	{
		wif(8, RTS);
		return read_leg(17);
	}
	void aek()
	{
		wif(8, WTC);
		write_leg({4, 0});
	}
	void clc(u8 request, u8 sf, u16 length, u16 lcn = 2, u8 flags = 0x40)
	{
		wif(8, CLC);
		write_leg({u8(lcn >> 8), u8(lcn), u8(flags | request), sf, 0,
				u8(length >> 8), u8(length)});
	}
	std::vector<u8> status()
	{
		wif(8, RS);
		return read_leg(23);
	}

	// A whole write-family transaction, the way the driver runs one.
	std::vector<u8> send(u8 request, u8 sf, const std::vector<u8> &data)
	{
		clc(request, sf, u16(data.size()));
		rts();                      // SWI
		if (!data.empty())
		{
			wif(8, WD);
			write_leg(data);
		}
		const std::vector<u8> st = status();
		return st;
	}
	void configure(const std::vector<u8> &block) { send(CONFIGURE, 0, block); }

	// A read device data transaction: the record, then its status block.
	std::vector<u8> read(std::vector<u8> &st, u16 length = 200,
			bool words = false)
	{
		clc(READ, 0, length, 1, 0xc0);
		rts();                      // SWI
		wif(8, words ? RD_WORDS : RD);
		std::vector<u8> data = read_leg(words ? length / 2 : length, !words);
		st = status();
		return data;
	}

	void type(const char *s)
	{
		for (; *s; s++)
			card.type(u8(*s));
	}
};


// --------------------------------------------------------------------
//  what the configuration block asks the card to do
// --------------------------------------------------------------------

static void test_unconfigured_card()
{
	std::printf("ASI: before any configuration a record ends on CR, unechoed\n");
	// The card's power-up defaults, which are what it always did before
	// it took the host's configuration: CR ends a record and stays in it,
	// and nothing is echoed.
	rig r;
	r.card.type('l');
	r.card.type('s');
	CHECK(r.screen.empty());
	r.card.type('\r');
	CHECK(r.screen.empty());
	std::vector<u8> st;
	const std::vector<u8> data = r.read(st);
	CHECK(std::string(data.begin(), data.end()) == "ls\r");
	CHECK(st.size() == 7);
	if (st.size() == 7)
	{
		CHECK_EQ(st[5], 1);
		CHECK_EQ(st[6], '\r');
	}
}


static void test_canonical_echo_and_edit()
{
	std::printf("ASI: canonical mode echoes, edits and ends on a terminator\n");
	rig r;
	r.configure(CANONICAL);
	r.screen.clear();
	r.type("ecko\x08\x08ho\r");
	// Echo as typed; backspace as BS-space-BS (option 1, overwrite); the
	// CR as CR-LF.
	CHECK(r.screen == "ecko\x08 \x08\x08 \x08ho\r\n");
	r.ticks(2);
	const std::vector<u8> report = r.rts();
	CHECK(report.size() >= 7);
	if (report.size() >= 7)
	{
		CHECK_EQ(report[0], 4);
		CHECK_EQ(report[1], 1);
		CHECK_EQ((report[3] << 8) | report[4], 5);  // "echo\r"
		CHECK_EQ(report[5], 1);
		CHECK_EQ(report[6], '\r');
	}
	std::vector<u8> st;
	const std::vector<u8> data = r.read(st);
	CHECK(std::string(data.begin(), data.end()) == "echo\r");
	CHECK(st.size() == 7);
	if (st.size() == 7)
	{
		CHECK_EQ(st[5], 1);
		CHECK_EQ(st[6], '\r');
	}

	// Line delete (^U here) empties the line, echoed as backslash and a
	// new line; EOT ends a record too; a backspace with nothing left to
	// take does nothing.
	r.screen.clear();
	r.type("junk\x15" "\x08" "ok\x04");
	CHECK(r.screen == "junk\\\r\nok\x04");
	r.ticks(2);
	r.rts();
	const std::vector<u8> second = r.read(st);
	CHECK(std::string(second.begin(), second.end()) == "ok\x04");
	if (st.size() == 7)
		CHECK_EQ(st[6], 0x04);
}


static void test_canonical_is_line_at_a_time()
{
	std::printf("ASI: canonical mode says nothing until the line is complete\n");
	rig r;
	r.configure(CANONICAL);
	r.type("abc");
	r.ticks(5);
	CHECK_EQ(r.rts()[0], 0);
	r.type("\n");
	r.ticks(2);
	CHECK_EQ(r.rts()[0], 4);
	std::vector<u8> st;
	const std::vector<u8> data = r.read(st);
	CHECK(std::string(data.begin(), data.end()) == "abc\n");
	if (st.size() == 7)
		CHECK_EQ(st[6], '\n');
}


static void test_typeahead_keeps_records_apart()
{
	std::printf("ASI: lines typed ahead are read one record at a time\n");
	rig r;
	r.configure(CANONICAL);
	r.type("one\rtwo\rthr");
	r.ticks(2);
	r.rts();
	std::vector<u8> st;
	std::vector<u8> data = r.read(st);
	CHECK(std::string(data.begin(), data.end()) == "one\r");
	r.ticks(2);
	r.rts();
	data = r.read(st);
	CHECK(std::string(data.begin(), data.end()) == "two\r");
	// The line still being typed is not offered...
	r.ticks(3);
	CHECK_EQ(r.rts()[0], 0);
	// ...and is still editable.
	r.type("\x08" "ree\r");
	r.ticks(2);
	r.rts();
	data = r.read(st);
	CHECK(std::string(data.begin(), data.end()) == "three\r");
}


static void test_alert_mode()
{
	std::printf("ASI: alert 1 delivers each keystroke, unechoed\n");
	rig r;
	r.configure(RAW);
	r.screen.clear();
	r.type("x");
	CHECK(r.screen.empty());
	r.ticks(2);
	const std::vector<u8> report = r.rts();
	CHECK(report.size() >= 6);
	if (report.size() >= 6)
	{
		CHECK_EQ(report[0], 4);
		CHECK_EQ(report[1], 1);
		CHECK_EQ((report[3] << 8) | report[4], 1);
		CHECK_EQ(report[5], 8);         // alert
	}
	// "The card will return all characters received from the time that
	// the alert event status was posted up to the execution of the read."
	// Backspace and CR are data here: edit mode and terminators are off.
	r.type("y\x08z\r");
	std::vector<u8> st;
	const std::vector<u8> data = r.read(st);
	CHECK(std::string(data.begin(), data.end()) == "xy\x08z\r");
	if (st.size() == 7)
		CHECK_EQ(st[5], 8);
	// ^C is data here too: signal detection is off in this block.
	r.type("\x03");
	r.ticks(2);
	CHECK_EQ(r.rts()[1], 1);
}


static void test_halfword_read_length()
{
	std::printf("ASI: a halfword read reports the record's length, not the pad\n");
	// The driver reads device data with halfword legs (order 0x60). Under
	// alert 1 every keystroke is a one-character record; a length that
	// counted the pad handed HP-UX a NUL after each one.
	rig r;
	r.configure(RAW);
	r.type("r");
	r.ticks(2);
	r.rts();
	std::vector<u8> st;
	std::vector<u8> data = r.read(st, 200, true);
	CHECK(!data.empty() && data[0] == 'r');
	CHECK(st.size() == 7);
	if (st.size() == 7)
	{
		CHECK_EQ((st[1] << 8) | st[2], 1);
		CHECK_EQ(st[5], 8);
	}
	r.configure(CANONICAL);
	r.type("ls\r");
	r.ticks(2);
	r.rts();
	data = r.read(st, 200, true);
	CHECK(data.size() >= 3);
	if (st.size() == 7)
	{
		CHECK_EQ((st[1] << 8) | st[2], 3);
		CHECK_EQ(st[5], 1);
		CHECK_EQ(st[6], '\r');
	}
}


static void test_signal_character()
{
	std::printf("ASI: the signal character is an event, not data\n");
	rig r;
	r.configure(CANONICAL);
	r.screen.clear();
	r.type("sleep\x03");
	CHECK(r.screen == "sleep");
	r.ticks(2);
	const std::vector<u8> report = r.rts();
	CHECK_EQ(report[0], 4);
	CHECK_EQ(report[1], 5);             // signal character 1
	// ^\ is signal character 2 in this block.
	r.type("\x1c");
	r.ticks(2);
	CHECK_EQ(r.rts()[1], 6);
	// The line it interrupted is still the open record, without either.
	r.type("\r");
	r.ticks(2);
	r.rts();
	std::vector<u8> st;
	const std::vector<u8> data = r.read(st);
	CHECK(std::string(data.begin(), data.end()) == "sleep\r");
}


static void test_handshake_characters()
{
	std::printf("ASI: the device's X-ON/X-OFF and the host ENQ's ACK are not data\n");
	rig r;
	r.configure(CANONICAL);
	r.type("a\x13" "b\x11" "c\x06\r");
	r.ticks(2);
	r.rts();
	std::vector<u8> st;
	const std::vector<u8> data = r.read(st);
	CHECK(std::string(data.begin(), data.end()) == "abc\r");
}


static void test_output_separators()
{
	std::printf("ASI: LF goes out as CR-LF when the block asks for it\n");
	rig r;
	r.configure(CANONICAL);
	r.screen.clear();
	std::vector<u8> st = r.send(rig::WRITE, 0, {'a', '\n', 'b'});
	CHECK(r.screen == "a\r\nb");
	// The length is what the host wrote, not what the terminal got.
	CHECK(st.size() == 3);
	if (st.size() == 3)
		CHECK_EQ((st[1] << 8) | st[2], 3);
	// Subfunction 1 appends the separators to the message (4-15).
	r.screen.clear();
	r.send(rig::WRITE, 1, {'o', 'k'});
	CHECK(r.screen == "ok\r\n");
	// Without the option the bytes pass untouched.
	r.configure(RAW);
	r.screen.clear();
	r.send(rig::WRITE, 0, {'a', '\n', 'b'});
	CHECK(r.screen == "a\nb");
}


static void test_end_on_buffer_limit()
{
	std::printf("ASI: a line longer than the card's buffer is cut into records\n");
	rig r;
	r.configure(CANONICAL);
	std::string longline(250, 'x');
	r.type(longline.c_str());
	r.ticks(2);
	CHECK_EQ(r.rts()[0], 4);
	std::vector<u8> st;
	std::vector<u8> data = r.read(st, 256);
	CHECK_EQ(data.size(), 200u);
	if (st.size() == 7)
		CHECK_EQ(st[5], 9);             // terminated by the card, more coming
}


static void test_flush()
{
	std::printf("ASI: control card 2 flushes what was typed, and its announcement\n");
	rig r;
	r.configure(CANONICAL);
	r.type("gone\rhalf");
	r.ticks(2);
	r.send(rig::CONTROL_CARD, 2, {});
	CHECK_EQ(r.rts()[0], 0);
	r.type("kept\r");
	r.ticks(2);
	CHECK_EQ(r.rts()[0], 4);
	std::vector<u8> st;
	const std::vector<u8> data = r.read(st);
	CHECK(std::string(data.begin(), data.end()) == "kept\r");
}


int main()
{
	test_unconfigured_card();
	test_canonical_echo_and_edit();
	test_canonical_is_line_at_a_time();
	test_typeahead_keeps_records_apart();
	test_alert_mode();
	test_halfword_read_length();
	test_signal_character();
	test_handshake_characters();
	test_output_separators();
	test_end_on_buffer_limit();
	test_flush();
	std::printf("\n%d checks, %d failed\n", g_run, g_fail);
	return g_fail ? 1 : 0;
}
