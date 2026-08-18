// license:BSD-3-Clause
// Tests for the FOCUS I/O Processor (src/iop/focusiop.h), including the CPU
// driving it through IOW / IOR / IOC.

#include "../iop/focusiop.h"
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


struct test_host : public iop_core::host
{
	sparse_memory mem;
	bool irq = false;
	int irq_changes = 0;

	memory_interface &iop_memory() override { return mem; }
	void iop_interrupt(bool state) override
	{
		if (state != irq) irq_changes++;
		irq = state;
	}
};


// A card that hands out a fixed pattern and records what it is given.
class test_card : public iop_interface
{
public:
	bool wants = false;
	u8 next_in = 0;
	std::vector<u8> written;
	u32 st = 0xabcd, fl = 0x1234;

	u32 status() override { return st; }
	u32 flag() override { return fl; }
	bool service_request() override { return wants; }
	u8 dma_read() override { return next_in++; }
	void dma_write(u8 v) override { written.push_back(v); }
};


static io_interface::target tgt(u8 opcode, u8 channel, u8 pa, u8 ic = 0)
{
	io_interface::target t;
	t.opcode = opcode; t.channel = channel; t.pa = pa; t.ic = ic;
	return t;
}


static void test_registers()
{
	std::printf("IOP register commands\n");
	test_host h;
	iop_core iop(h, 3);
	iop.reset();

	u32 v = 0;

	// Commands for another channel number are not ours to answer.
	CHECK(!iop.io_write(tgt(CMD_WDC, 5, 0), 100));
	CHECK(iop.io_write(tgt(CMD_WDC, 3, 0), 100));

	// DMA registers, per peripheral address.
	CHECK(iop.io_write(tgt(CMD_WDA, 3, 2), 0x40000));
	CHECK(iop.io_write(tgt(CMD_WDC, 3, 2), 0x20));
	CHECK(iop.io_read(tgt(CMD_RDCS, 3, 2), v));
	CHECK_EQ(v & 0xffff, 0x20);

	// Each peripheral address keeps its own set.
	CHECK(iop.io_read(tgt(CMD_RDCS, 3, 0), v));
	CHECK_EQ(v & 0xffff, 100);

	// Interrupt mask and request.
	CHECK(iop.io_write(tgt(CMD_WIMK, 3, 0), 0xf0));
	CHECK(iop.io_read(tgt(CMD_RIMK, 3, 0), v));
	CHECK_EQ(v, 0xf0);

	CHECK(iop.io_write(tgt(CMD_WIMG, 3, 0), 0x5a5a));
	CHECK(iop.io_read(tgt(CMD_RIMG, 3, 0), v));
	CHECK_EQ(v, 0x5a5a);

	// Revision is readable.
	CHECK(iop.io_read(tgt(CMD_RIRV, 3, 0), v));
	CHECK_EQ(v, 1);

	// LED control.
	CHECK(!iop.led());
	CHECK(iop.io_control(tgt(CMD_LON, 3, 0)));
	CHECK(iop.led());
	CHECK(iop.io_control(tgt(CMD_LOFF, 3, 0)));
	CHECK(!iop.led());

	// Writing the channel number moves the IOP.
	CHECK(iop.io_write(tgt(CMD_WMCN, 3, 0), 6));
	CHECK_EQ(iop.channel_number(), 6);
	CHECK(!iop.io_control(tgt(CMD_LON, 3, 0)));
	CHECK(iop.io_control(tgt(CMD_LON, 6, 0)));

	// An unknown opcode is refused rather than silently accepted.
	CHECK(!iop.io_write(tgt(0177, 6, 0), 0));
	CHECK(!iop.io_read(tgt(0177, 6, 0), v));
	CHECK(!iop.io_control(tgt(0177, 6, 0)));
}


static void test_interrupts()
{
	std::printf("IOP interrupt collection\n");
	test_host h;
	iop_core iop(h, 1);
	iop.reset();

	CHECK(!h.irq);

	// A masked request must not pull the line.
	CHECK(iop.io_write(tgt(CMD_WIMK, 1, 0), 1u << 2));
	iop.raise(2);
	CHECK(!h.irq);

	// An unmasked one must.
	iop.raise(3);
	CHECK(h.irq);

	// Clearing the request drops the line again.
	CHECK(iop.io_control(tgt(CMD_CIRQ, 1, 3)));
	CHECK(!h.irq);

	// The masked request is still recorded, and unmasking reveals it.
	u32 v = 0;
	CHECK(iop.io_read(tgt(CMD_RIRQ, 1, 0), v));
	CHECK_EQ(v, 1u << 2);
	CHECK(iop.io_write(tgt(CMD_WIMK, 1, 0), 0));
	CHECK(h.irq);
}


static void test_dma()
{
	std::printf("IOP DMA\n");
	test_host h;
	iop_core iop(h, 1);
	iop.reset();

	test_card card;
	iop.attach(2, &card);

	// Input: card to memory. Status bit 0 clear selects that direction.
	CHECK(iop.io_write(tgt(CMD_WDA, 1, 2), 0x1000));
	CHECK(iop.io_write(tgt(CMD_WDC, 1, 2), 4));
	CHECK(iop.io_write(tgt(CMD_WDS, 1, 2), 0));
	CHECK(iop.io_control(tgt(CMD_EDMA, 1, 2)));
	CHECK(iop.io_control(tgt(CMD_SD, 1, 2)));

	card.next_in = 0x11;
	card.wants = true;
	for (int i = 0; i < 6; i++)
		iop.poll_and_service();

	CHECK_EQ(h.mem.read8(0x1000), 0x11);
	CHECK_EQ(h.mem.read8(0x1001), 0x12);
	CHECK_EQ(h.mem.read8(0x1002), 0x13);
	CHECK_EQ(h.mem.read8(0x1003), 0x14);
	// The count reaching zero ends the transfer and raises an interrupt.
	CHECK_EQ(h.mem.read8(0x1004), 0);
	CHECK(h.irq);

	// Output: memory to card, direction bit set.
	iop.reset();
	iop.attach(2, &card);
	card.written.clear();
	h.mem.write8(0x2000, 0xde);
	h.mem.write8(0x2001, 0xad);
	CHECK(iop.io_write(tgt(CMD_WDA, 1, 2), 0x2000));
	CHECK(iop.io_write(tgt(CMD_WDC, 1, 2), 2));
	CHECK(iop.io_write(tgt(CMD_WDS, 1, 2), ers32::mask(0, 1)));
	CHECK(iop.io_control(tgt(CMD_EDMA, 1, 2)));
	CHECK(iop.io_control(tgt(CMD_SD, 1, 2)));
	for (int i = 0; i < 4; i++)
		iop.poll_and_service();

	CHECK_EQ(card.written.size(), 2u);
	if (card.written.size() == 2)
	{
		CHECK_EQ(card.written[0], 0xde);
		CHECK_EQ(card.written[1], 0xad);
	}

	// A disabled channel transfers nothing even when the card asks.
	iop.reset();
	iop.attach(2, &card);
	card.written.clear();
	CHECK(iop.io_control(tgt(CMD_SD, 1, 2)));   // started but not enabled
	for (int i = 0; i < 4; i++)
		iop.poll_and_service();
	CHECK_EQ(card.written.size(), 0u);
}


static void test_poll_and_timer()
{
	std::printf("IOP interface poll and interval timer\n");
	test_host h;
	iop_core iop(h, 1);
	iop.reset();

	test_card a, b;
	iop.attach(1, &a);
	iop.attach(4, &b);
	a.wants = true;

	u32 v = 0;
	CHECK(iop.io_read(tgt(CMD_IFPL, 1, 0), v));
	CHECK_EQ(v, 1u << 1);
	b.wants = true;
	CHECK(iop.io_read(tgt(CMD_IFPL, 1, 0), v));
	CHECK_EQ(v, (1u << 1) | (1u << 4));

	// Card status and flag reach the CPU.
	CHECK(iop.io_read(tgt(CMD_RIST, 1, 1), v));
	CHECK_EQ(v, 0xabcd);
	CHECK(iop.io_read(tgt(CMD_RIFG, 1, 1), v));
	CHECK_EQ(v, 0x1234);

	// The timer reloads from its start value and interrupts only when
	// enabled.
	CHECK(iop.io_write(tgt(CMD_WTMS, 1, 0), 3));
	for (int i = 0; i < 3; i++)
		iop.timer_tick();
	CHECK(!h.irq);                       // not enabled yet
	CHECK(iop.io_read(tgt(CMD_RTM, 1, 0), v));
	CHECK_EQ(v, 3);                      // reloaded

	CHECK(iop.io_control(tgt(CMD_ETMI, 1, 0)));
	for (int i = 0; i < 3; i++)
		iop.timer_tick();
	CHECK(h.irq);
}


// The CPU driving the IOP through its own instructions.
static void test_cpu_to_iop()
{
	std::printf("CPU to IOP over the MPB\n");

	test_host h;
	iop_core iop(h, 1);
	iop.reset();

	cpu_core cpu(h.mem);
	cpu.set_io(&iop);

	cpu.r.pb = 0x1000; cpu.r.p = 0x1000; cpu.r.pl = 0x1ffc;
	cpu.r.sb = 0x4000; cpu.r.s = 0x4003; cpu.r.q = 0x4003;
	cpu.r.sl = 0x4ffc | 3;

	auto put = [&](u32 addr, u16 insn)
	{
		const u32 cell = addr & ~3u;
		u32 w = h.mem.read32(cell);
		if (addr & 2) w = (w & 0xffff0000u) | insn;
		else          w = (w & 0x0000ffffu) | (u32(insn) << 16);
		h.mem.write32(cell, w);
	};
	auto f5 = [](u8 op2, u8 operand) -> u16
	{ return u16(0x2000 | (op2 << 8) | operand); };

	// Interface address naming IOP channel 1, peripheral address 2.
	const u32 iface = ers32::set(ers32::set(0, 26, 3, 1), 29, 3, 2);

	// IOW CMD_WDC: set the DMA count for PA 2 to 0x55.
	cpu.push(iface);
	cpu.push(0x55);
	cpu.r.p = 0x1000;
	put(0x1000, f5(017, CMD_WDC));
	CHECK(cpu.step() == exec_trap::none);

	u32 v = 0;
	CHECK(iop.io_read(tgt(CMD_RDCS, 1, 2), v));
	CHECK_EQ(v & 0xffff, 0x55);

	// IOR CMD_RIRV: read the revision back onto the stack.
	cpu.push(iface);
	cpu.r.p = 0x1000;
	put(0x1000, f5(020, CMD_RIRV));
	CHECK(cpu.step() == exec_trap::none);
	CHECK_EQ(cpu.stk(0), 1);

	// IOC CMD_LON: light the LED.
	cpu.pop();
	cpu.push(iface);
	cpu.r.p = 0x1000;
	put(0x1000, f5(021, CMD_LON));
	CHECK(cpu.step() == exec_trap::none);
	CHECK(iop.led());

	// A command the IOP does not implement raises the message trap and
	// sets Message register bit 7.
	cpu.r.msg = 0;
	cpu.push(iface);
	cpu.r.p = 0x1000;
	put(0x1000, f5(021, 0177));
	CHECK(cpu.step() == exec_trap::trapped);
	CHECK_EQ(ers32::get(cpu.r.msg, 7, 1), 1u);
}


int main()
{
	std::printf("FOCUS IOP tests\n\n");
	test_registers();
	test_interrupts();
	test_dma();
	test_poll_and_timer();
	test_cpu_to_iop();

	std::printf("\n%d checks, %d failed\n", g_run, g_fail);
	std::printf(g_fail ? "FAIL\n" : "PASS\n");
	return g_fail ? 1 : 0;
}
