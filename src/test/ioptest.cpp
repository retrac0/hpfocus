// license:BSD-3-Clause
// Tests for the FOCUS I/O Processor (src/iop/focusiop.h), including the CPU
// driving it through IOW / IOR / IOC.

#include "../iop/focusiop.h"
#include "../iop/focusscm.h"
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

	// An unknown opcode is refused rather than silently accepted.
	CHECK(!iop.io_write(tgt(0177, 3, 0), 0));
	CHECK(!iop.io_read(tgt(0177, 3, 0), v));
	CHECK(!iop.io_control(tgt(0177, 3, 0)));

	// Reset I/O Bus clears DMA and pending interrupts, which is what a
	// power-up sequence issues first.
	iop.raise(1);
	CHECK(iop.io_control(tgt(CMD_EDMA, 3, 1)));
	CHECK(iop.io_control(tgt(CMD_RIOB, 3, 0)));
	CHECK_EQ(iop.dma_mask(), 0u);
	CHECK_EQ(iop.interrupt_request(), 0u);

	// Interrupt enable is per device (I/O ERS section 4.1.4): DINT on
	// one PA does not gate another, a disabled device's raise is ignored
	// outright, and every accepted raise disables its own device until
	// the CPU re-arms it with EINT.
	CHECK(iop.io_write(tgt(CMD_WIMK, 3, 0), 0));
	iop.raise(4);
	CHECK(!h.irq);                       // powers up disabled
	CHECK(iop.io_control(tgt(CMD_EINT, 3, 5)));
	iop.raise(5);
	CHECK(h.irq);                        // armed, accepted
	CHECK(iop.io_control(tgt(CMD_CIRQ, 3, 5)));
	iop.raise(5);
	CHECK_EQ(iop.interrupt_request(), 0u);   // the raise disarmed it
	CHECK(iop.io_control(tgt(CMD_EINT, 3, 5)));
	iop.raise(5);                        // re-armed, accepted again
	CHECK(h.irq);
	CHECK(iop.io_control(tgt(CMD_CIRQ, 3, 5)));
	CHECK(iop.io_control(tgt(CMD_EINT, 3, 5)));
	CHECK(iop.io_control(tgt(CMD_DINT, 3, 5)));
	iop.raise(5);
	CHECK(!h.irq);                       // DINT disarms again
}


static void test_interrupts()
{
	std::printf("IOP interrupt collection\n");
	test_host h;
	iop_core iop(h, 1);
	iop.reset();

	CHECK(!h.irq);

	// Devices power up disabled; arm the two this test raises.
	CHECK(iop.io_control(tgt(CMD_EINT, 1, 2)));
	CHECK(iop.io_control(tgt(CMD_EINT, 1, 3)));

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
	CHECK(iop.io_control(tgt(CMD_EINT, 1, 2)));   // arm the completion
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

	// The timer reloads from its start value; an underflow becomes a
	// pending message-13 event only once enabled (I/O ERS 7.1: underflow
	// sets "message bit 13", a CPU message, not a device interrupt).
	// Timer registers carry the count in bits 0:24 (value << 8).
	CHECK(iop.io_write(tgt(CMD_WTMS, 1, 0), 3 << 8));
	for (int i = 0; i < 3; i++)
		iop.timer_tick();
	CHECK(!iop.take_timer_event());      // not enabled yet
	CHECK(!h.irq);                       // and never a device interrupt
	CHECK(iop.io_read(tgt(CMD_RTM, 1, 0), v));
	CHECK_EQ(v, 3u << 8);                // reloaded

	CHECK(iop.io_control(tgt(CMD_ETMI, 1, 0)));
	for (int i = 0; i < 3; i++)
		iop.timer_tick();
	CHECK(iop.take_timer_event());
	CHECK(!iop.take_timer_event());      // one event per underflow
	CHECK(!h.irq);
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


static void test_scm()
{
	std::printf("System Control Module\n");
	test_host h;
	iop_core iop(h, 1);
	iop.reset();

	scm_device scm;
	iop.attach(scm_device::SELECT_CODE, &scm);

	// R3 bit 14 announces the loader, which is how the bootstrap finds it.
	CHECK_EQ(scm.read_register(3) & (1u << 14), 1u << 14);
	CHECK_EQ(find_loader(iop, 1), int(scm_device::SELECT_CODE));

	// With nothing on that select code the scan comes up empty.
	{
		test_host h2;
		iop_core bare(h2, 1);
		bare.reset();
		CHECK_EQ(find_loader(bare, 1), -1);
	}

	// Status bits: power fail is bit 1, self-test bit 3, battery fail bit 5.
	CHECK_EQ(scm.read_register(2), 0);
	scm.set_power_fail(true);
	CHECK_EQ(scm.read_register(2) & (1u << 1), 1u << 1);
	scm.set_battery_fail(true);
	CHECK_EQ(scm.read_register(2) & (1u << 5), 1u << 5);
	scm.set_power_fail(false);
	scm.set_battery_fail(false);

	// A loader image, deliberately not all the same byte so the descending
	// address counter is visible.
	u8 rom[scm_device::ROM_SIZE];
	for (unsigned i = 0; i < scm_device::ROM_SIZE; i++)
		rom[i] = u8(i);
	scm.set_rom(rom, sizeof(rom));
	CHECK(scm.rom_present());

	// Before arming, the ROM does not read out.
	CHECK(!scm.service_request());
	CHECK_EQ(scm.read_register(4), 0xff);

	// Writing 84 hex to R1 resets the counter to FFFF, enables DMA and sets
	// the self-test bit.
	scm.write_register(1, scm_device::ARM_ROM);
	CHECK(scm.service_request());
	CHECK_EQ(scm.read_register(2) & (1u << 3), 1u << 3);

	// The counter starts at FFFF and decrements, so with an 8 KB ROM the
	// first byte read is the last byte of the image.
	CHECK_EQ(scm.read_register(4), rom[0x1fff]);
	CHECK_EQ(scm.read_register(4), rom[0x1ffe]);
	CHECK_EQ(scm.read_register(4), rom[0x1ffd]);

	// The burst is 2048 bytes, after which DMA switches itself off.
	while (scm.bytes_read() < scm_device::INITIAL_BURST)
		scm.dma_read();
	CHECK_EQ(scm.bytes_read(), scm_device::INITIAL_BURST);
	CHECK(!scm.service_request());

	// Arming again restarts the sequence.
	scm.write_register(1, scm_device::ARM_ROM);
	CHECK_EQ(scm.bytes_read(), 0u);
	CHECK(scm.service_request());
	CHECK_EQ(scm.read_register(4), rom[0x1fff]);

	// A command other than 84 hex does not arm it.
	scm.reset();
	scm.set_rom(rom, sizeof(rom));
	scm.write_register(1, 0x12);
	CHECK(!scm.service_request());

	// R12 latches its address in complemented form.
	scm.write_register(12, 0x1234);
	CHECK_EQ(scm.read_register(12), (~0x1234u) & 0xffff);

	// R10 carries service-processor traffic both ways.
	scm.write_register(10, 0xbeef);
	CHECK_EQ(scm.read_register(10), 0xbeef);

	// With no image the region reads as erased, which is how a machine with
	// no loader dump behaves.
	scm_device blank;
	blank.set_rom(nullptr, 0);
	CHECK(!blank.rom_present());
	blank.write_register(1, scm_device::ARM_ROM);
	CHECK_EQ(blank.read_register(4), 0xff);
}

int main()
{
	std::printf("FOCUS IOP tests\n\n");
	test_registers();
	test_interrupts();
	test_dma();
	test_poll_and_timer();
	test_cpu_to_iop();
	test_scm();

	std::printf("\n%d checks, %d failed\n", g_run, g_fail);
	std::printf(g_fail ? "FAIL\n" : "PASS\n");
	return g_fail ? 1 : 0;
}
