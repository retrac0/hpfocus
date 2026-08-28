// license:BSD-3-Clause
// Tests for the FOCUS I/O Processor (src/iop/focusiop.h), including the CPU
// driving it through IOW / IOR / IOC.

#include "../iop/focusiop.h"
#include "../iop/focuscards.h"
#include "../iop/focusscm.h"
#include "../iop/focusgfx.h"
#include "../cpu/focus/focusexec.h"

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <ctime>
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
	u32 fr[16] = {};

	u32 read_fr(unsigned n) override { return fr[n & 15]; }
	void write_fr(unsigned n, u32 v) override { fr[n & 15] = v & 0xffff; }
	u32 status() override { return 1; }
	u32 status_reg() override { return st; }
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

	// Revision is readable: the microcode's release date as eight hex
	// digits, which HP-UX compares against 19810630 to decide whether
	// the IOP is new enough (FOCUS_IOPREV overrides it).
	CHECK(iop.io_read(tgt(CMD_RIRV, 3, 0), v));
	CHECK_EQ(v, 0x19820920u);

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

	// Interrupt enable is per device (I/O ERS section 4.1.4): a disabled
	// device's raise latches until EINT delivers it, and every accepted
	// raise disables its own device until the CPU re-arms it with EINT.
	CHECK(iop.io_write(tgt(CMD_WIMK, 3, 0), 0));
	iop.raise(4);
	CHECK(!h.irq);                       // powers up disabled
	CHECK(iop.io_control(tgt(CMD_EINT, 3, 5)));
	iop.raise(5);
	CHECK(h.irq);                        // armed, accepted
	CHECK(iop.io_control(tgt(CMD_CIRQ, 3, 5)));
	iop.raise(5);
	// The raise disarmed the device, so nothing is POSTED -- but the
	// Interrupt Request register records it anyway: "DMA termination or
	// a non-DMA service request will cause a bit in the Interrupt
	// Request register corresponding to the proper PA to be set" (I/O
	// ERS 4.1.1), with no mention of the mask. Bit 4 from the raise
	// above is still standing too, never having been cleared. HP-UX's
	// panic printer depends on exactly this: it masks every interrupt
	// off, starts its transfer with SDC, and polls RIRQ for the
	// completion.
	CHECK(!h.irq);
	CHECK_EQ(iop.interrupt_request(), (1u << 4) | (1u << 5));
	CHECK(iop.io_control(tgt(CMD_EINT, 3, 5)));
	CHECK(h.irq);                        // ...but EINT delivers the latch
	CHECK(iop.io_control(tgt(CMD_CIRQ, 3, 5)));
	CHECK(iop.io_control(tgt(CMD_EINT, 3, 5)));
	iop.raise(5);                        // re-armed, accepted again
	CHECK(h.irq);
	CHECK(iop.io_control(tgt(CMD_CIRQ, 3, 5)));
	CHECK(iop.io_control(tgt(CMD_DINT, 3, 5)));
	iop.raise(5);
	CHECK(!h.irq);                       // DINT holds the latch back
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

	// Input: card to memory. F1 (status bit 27) clear selects that
	// direction; bit 29 set selects byte transfers (Table 5.1).
	CHECK(iop.io_write(tgt(CMD_WDA, 1, 2), 0x1000));
	CHECK(iop.io_write(tgt(CMD_WDC, 1, 2), 4));
	CHECK(iop.io_write(tgt(CMD_WDS, 1, 2), ers32::mask(29, 1)));
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

	// Output: memory to card, F1 set; byte width again.
	iop.reset();
	iop.attach(2, &card);
	card.written.clear();
	h.mem.write8(0x2000, 0xde);
	h.mem.write8(0x2001, 0xad);
	CHECK(iop.io_write(tgt(CMD_WDA, 1, 2), 0x2000));
	CHECK(iop.io_write(tgt(CMD_WDC, 1, 2), 2));
	CHECK(iop.io_write(tgt(CMD_WDS, 1, 2),
			ers32::mask(27, 1) | ers32::mask(29, 1)));
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

	// The interface status register and the flag reach the CPU. RIST
	// reads the register a card composes, which is not the one-bit
	// STATUS line the poll reports.
	CHECK(iop.io_read(tgt(CMD_RIST, 1, 1), v));
	CHECK_EQ(v, 0xabcd);
	CHECK(iop.io_read(tgt(CMD_RIFG, 1, 1), v));
	CHECK_EQ(v, 0x1234);

	// Opcode 011 reads the card's FR1, the register the powerup self
	// test writes a pattern against.
	a.fr[1] = 0x5aa5;
	CHECK(iop.io_read(tgt(CMD_RIF1, 1, 1), v));
	CHECK_EQ(v, 0x5aa5);

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
	CHECK_EQ(cpu.stk(0), 0x19820920);

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

	// R12 latches what was written, and the two entry points into it --
	// the interface-command path here and the FR path every guest uses --
	// agree. The manual's "complemented form" is what the OS puts on the
	// bus, and the decode is where the inversion is applied.
	scm.write_register(12, 0x1234);
	CHECK_EQ(scm.read_register(12), 0x1234u);
	scm.write_fr(12, 0x1234);
	CHECK_EQ(scm.read_fr(12), 0x1234u);

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

// The display graphics interface, select code 1. Every case below is one
// step of HP's own power-up self test for the display -- PWP SELF TEST,
// segment 13 of the diagnostics boot image, local entries 21 to 32 -- so
// that the escape code the machine would report has a check here that
// runs in microseconds instead of a forty-second boot.
static void test_gfx()
{
	std::printf("Display graphics interface\n");
	test_host h;
	iop_core iop(h, 1);
	iop.reset();

	gfx_card gfx;
	gfx.pa = 1;
	gfx.iop = &iop;
	iop.attach(1, &gfx);
	iop.enable_interrupt(1);

	// Entry 21, the reset, in the order it asks. RIST odd or escape 1;
	// FR1 = 0 then the flag EVEN or escape 2; the interface poll clear
	// or escape 3.
	CHECK_EQ(gfx.status_reg() & 1, 1);
	gfx.write_fr(1, 0);
	CHECK_EQ(gfx.flag() & 1, 0);
	CHECK(!gfx.poll_request());

	// Then a command, and the flag comes back -- escapes 4 and 5 are the
	// waits after 0x100 and 0x180 timing out.
	gfx.write_fr(1, 0x100);
	CHECK_EQ(gfx.flag() & 1, 1);
	gfx.write_fr(1, 0x180);
	CHECK_EQ(gfx.flag() & 1, 1);

	// The kernel's escape path writes FR1 = 6 and FR1 = 1. Neither is a
	// command: they must not complete anything and must not disturb the
	// memory the test is about to fill.
	gfx.write_fr(1, 0x1a0);
	gfx.write_fr(0, 0x1234);
	gfx.write_fr(1, 6);
	gfx.write_fr(1, 1);
	gfx.write_fr(1, 0x1a1);
	CHECK_EQ(gfx.read_fr(0), 0x1234);

	// Local 28's pattern test: 98,304 bytes out and the same back. The
	// length is the whole memory and the transfer starts at the beginning
	// of it, which is what says the read/write command rewinds the
	// pointer.
	const unsigned words = gfx.plane_words();
	CHECK_EQ(words, 3 * 16384);
	gfx.write_fr(1, 0x1a0);
	for (unsigned i = 0; i < words; i++)
		gfx.dma_write16(u16(0x5555 + i));
	gfx.write_fr(1, 0x1a1);
	unsigned bad = 0;
	for (unsigned i = 0; i < words; i++)
		if (gfx.dma_read16() != u16(0x5555 + i))
			bad++;
	CHECK_EQ(bad, 0);

	// The same through byte transfers, which the channel picks instead
	// when its status word says bytes: high half of each word first.
	gfx.write_fr(1, 0x1a0);
	gfx.dma_write(0xab);
	gfx.dma_write(0xcd);
	gfx.write_fr(1, 0x1a1);
	CHECK_EQ(gfx.dma_read(), 0xab);
	CHECK_EQ(gfx.dma_read(), 0xcd);

	// Entry 32, the check that escapes 8. Zero the whole memory, set the
	// three planes to modes 17, 49 and 113, unmask, then read three words
	// and expect a generated pattern from each plane in turn.
	gfx.write_fr(1, 0x1a0);
	for (unsigned i = 0; i < words; i++)
		gfx.dma_write16(0);
	const unsigned PLANE_WORD[3] = { 57, 58, 60 };
	const unsigned MODE_N[3] = { 0, 1, 3 };
	for (unsigned k = 0; k < 3; k++)
	{
		gfx.write_fr(1, 0x18a);
		gfx.write_fr(0, PLANE_WORD[k]);
		gfx.write_fr(1, 0x1ab);
		gfx.write_fr(0, (MODE_N[k] << 5) + 17);
		gfx.write_fr(1, 0x1ad);
		gfx.write_fr(0, 1023);
		gfx.write_fr(0, 1023);
		gfx.write_fr(0, 2047);
		gfx.write_fr(0, 2031);
	}
	gfx.write_fr(1, 0x1a8);
	gfx.write_fr(0, 0xffff);
	gfx.write_fr(1, 0x1a9);
	gfx.write_fr(0, 0xffff);
	gfx.write_fr(1, 0x1a1);
	CHECK_EQ(gfx.read_fr(0), 0xaaaa);
	CHECK_EQ(gfx.read_fr(0), 0xcccc);
	CHECK_EQ(gfx.read_fr(0), 0xf0f0);

	// Local 29, the mono display's version, holds the plane at 0 and
	// varies the rest -- which is what separates "the mode selects the
	// pattern" from "the plane does". An x of 1 in the region command
	// rotates it one place.
	gfx_card mono;
	mono.pa = 1;
	mono.planes = 1;
	mono.stride = gfx_card::MONO_STRIDE_WORDS;
	const struct { unsigned x, n; u16 want; } CASES[] = {
		{ 1, 0, 0x5555 }, { 0, 0, 0xaaaa }, { 0, 1, 0xcccc }, { 0, 3, 0xf0f0 },
	};
	for (const auto &c : CASES)
	{
		mono.write_fr(1, 0x18a);
		mono.write_fr(0, 57);
		mono.write_fr(1, 0x186);
		mono.write_fr(0, (c.n << 5) + 17);
		mono.write_fr(1, 0x181);
		mono.write_fr(0, 0);
		mono.write_fr(0, c.x);
		mono.write_fr(0, 2048);
		mono.write_fr(0, 2064);
		mono.write_fr(1, 0x187);
		mono.write_fr(0, 0);
		mono.write_fr(0, 0);
		mono.write_fr(1, 0x189);
		CHECK_EQ(mono.read_fr(0), c.want);
	}
	// The self test's own length for the mono display, 31,850 bytes, is
	// 560 x 455 to the bit -- which is where the raster came from.
	CHECK_EQ(gfx_card::WIDTH * gfx_card::HEIGHT / 8, 31850);
	// and the colour display's 98,304 is three planes of 32,768.
	CHECK_EQ(3 * gfx_card::PLANE_WORDS * 2, 98304);

	// A transfer longer than the memory runs off the end quietly rather
	// than writing past it or wrapping onto what it already stored. The
	// reset first, because the patterns above are still armed and FR1 = 0
	// is what disarms them.
	gfx.write_fr(1, 0);
	gfx.write_fr(1, 0x1a0);
	for (unsigned i = 0; i < words + 64; i++)
		gfx.dma_write16(u16(0x1000 + i));
	gfx.write_fr(1, 0x1a1);
	CHECK_EQ(gfx.dma_read16(), 0x1000);

	// The self test's transfer as the IOP actually runs it, because the
	// card behaving under direct calls says nothing about the channel:
	// the write leg is F1=1 F2=0 (output) and the read leg F1=1 F2=1,
	// which Table 5.1 calls "Input, Verify" -- the IOP reads the card and
	// compares against the same buffer instead of storing.
	{
		test_host dh;
		iop_core diop(dh, 1);
		diop.reset();
		gfx_card d;
		d.pa = 1;
		d.iop = &diop;
		diop.attach(1, &d);

		const u32 BUF = 0x1000;
		const unsigned LEG = 64;            // halfwords per leg
		for (unsigned i = 0; i < LEG; i++)
			dh.mem.write16(BUF + 2 * i, u16((i & 1) ? 0xaaaa : 0x5555));

		d.write_fr(1, 0x1a0);
		for (unsigned leg = 0; leg < 3; leg++)
		{
			CHECK(diop.io_write(tgt(CMD_WDA, 1, 1), BUF));
			CHECK(diop.io_write(tgt(CMD_WDC, 1, 1), LEG));
			CHECK(diop.io_write(tgt(CMD_WDS, 1, 1), ers32::mask(27, 1)));
			CHECK(diop.io_control(tgt(CMD_EDMA, 1, 1)));
			CHECK(diop.io_control(tgt(CMD_SD, 1, 1)));
			diop.poll_and_service();
		}

		d.write_fr(1, 0x1a1);
		CHECK(diop.io_write(tgt(CMD_WDA, 1, 1), BUF));
		CHECK(diop.io_write(tgt(CMD_WDC, 1, 1), LEG));
		CHECK(diop.io_write(tgt(CMD_WDS, 1, 1),
				ers32::mask(27, 1) | ers32::mask(28, 1)));
		CHECK(diop.io_control(tgt(CMD_EDMA, 1, 1)));
		CHECK(diop.io_control(tgt(CMD_SD, 1, 1)));
		diop.poll_and_service();
		// The verify runs to the end of the leg: every word the card
		// gave back matched, so the pointer reached the leg length
		// instead of the compare stopping it short.
		CHECK_EQ(d.pointer(), LEG);
	}

	// A plane bit reaches the raster where the stride says it should.
	gfx_card r;
	r.write_fr(1, 0x1a0);
	for (unsigned i = 0; i < r.plane_words(); i++)
		r.dma_write16(0);
	r.vram[0][gfx_card::STRIDE_WORDS] = 0x8000;   // x = 0 of line 1
	CHECK_EQ(r.pixel(0, 1), 1);
	CHECK_EQ(r.pixel(1, 1), 0);
	CHECK_EQ(r.pixel(0, 0), 0);
}

// The clock and the configuration memory, on both boards that carry them.
// They differ in exactly two ways -- the SCM complements the RTC address and
// masks it to four bits, the keyboard board takes it plain over six -- so
// every decode case runs against both from one table, which is the point of
// their sharing a model.
static void test_rtc_nvm()
{
	std::printf("Real-time clock and non-volatile memory\n");

	struct board { unsigned sc; rtc_decode d; };
	const board boards[2] = {
		{ 7, rtc_decode::scm() },
		{ 6, rtc_decode::keyboard() },
	};

	for (const board &b : boards)
	{
		rtc_nvm nv(b.d);

		// Every location round-trips through R12 and R9, and the data is
		// complemented on the bus in both directions.
		for (unsigned off = 0; off < nvloc::RTC_LOCATIONS; off++)
		{
			nv.write_latch(off);
			nv.write_rtc(0xa0 | off);
			nv.write_latch(off);
			CHECK_EQ(nv.read_rtc(), u8(0xa0 | off));
			CHECK_EQ(nv.rtc[nv.index_for(off)], u8(~(0xa0 | off)));
		}

		// The decode, stated rather than implied by a cell number.
		CHECK_EQ(nv.index_for(nvloc::SECOND), b.d.rtc_addr_complement ? 15u : 0u);
		CHECK_EQ(nv.index_for(nvloc::MINUTE), b.d.rtc_addr_complement ? 13u : 2u);
		CHECK_EQ(nv.index_for(nvloc::YEAR),   b.d.rtc_addr_complement ? 6u : 9u);

		// "Succeeding reads or writes cause automatic increments of the
		// address": four bytes written from one latch come back in the same
		// order from one latch. A step inverted on either board reverses them.
		nv.write_latch(0);
		for (unsigned i = 0; i < 4; i++)
			nv.write_rtc(0x10 + i);
		nv.write_latch(0);
		for (unsigned i = 0; i < 4; i++)
			CHECK_EQ(nv.read_rtc(), u8(0x10 + i));

		// The whole 2 KB, addressed complemented, data plain, and the latch
		// does not step for NVM.
		for (unsigned a = 0; a < rtc_nvm::NVM_SIZE; a++)
		{
			nv.write_latch((~a) & 0xffff);
			nv.write_nvm(a & 0xff);
		}
		bool all = true;
		for (unsigned a = 0; a < rtc_nvm::NVM_SIZE; a++)
			all = all && nv.nvm[a] == u8(a);
		CHECK(all);
		nv.write_latch((~7u) & 0xffff);
		CHECK_EQ(nv.read_nvm(), 7);
		CHECK_EQ(nv.read_nvm(), 7);

		// A reset is not a power cycle: the contents are the battery's.
		nv.power_on_reset();
		CHECK_EQ(nv.nvm[7], 7);
		CHECK_EQ(nv.latch, 0u);
	}

	// The two boards' location masks differ deliberately: six address bits
	// on the keyboard board, four on the SCM.
	{
		rtc_nvm scm(rtc_decode::scm()), kbd(rtc_decode::keyboard());
		CHECK_EQ(kbd.index_for(20), 20u);
		CHECK_EQ(scm.index_for(20), (~20u) & 15u);
	}

	// The calendar. Both boards used to wrap the day at 31 or 32 and the
	// month at 12 or 13, so February had 31 days on one and the month could
	// reach 0 on the other.
	CHECK_EQ(calendar::month_length(2000, 2), 29);   // divisible by 400
	CHECK_EQ(calendar::month_length(1900, 2), 28);   // by 100, not 400
	CHECK_EQ(calendar::month_length(2024, 2), 29);
	CHECK_EQ(calendar::month_length(2023, 2), 28);
	CHECK_EQ(calendar::month_length(1985, 4), 30);
	{
		calendar c; c.year = 1970; c.month = 1; c.day = 1;
		CHECK_EQ(c.day_of_week(), 4);                // a Thursday
		c.year = 2000; CHECK_EQ(c.day_of_week(), 6); // a Saturday
	}
	{
		calendar c{ 1999, 12, 31, 23, 59, 59, 0 };
		c.advance(1);
		CHECK(c.year == 2000 && c.month == 1 && c.day == 1);
		CHECK(c.hour == 0 && c.minute == 0 && c.second == 0);
	}
	{
		calendar c{ 1900, 2, 28, 12, 0, 0, 0 };
		c.advance(86400);
		CHECK(c.month == 3 && c.day == 1);           // 1900 is not a leap year
	}
	// The epoch conversions round-trip, which is what seeding from the host
	// clock and running the battery over an off-time both rest on.
	{
		bool ok = true;
		for (long long t = 0; t < 2000000000LL; t += 7919117)
			ok = ok && calendar::from_epoch(t).to_epoch() == t;
		CHECK(ok);
	}

	// The clock through the map, on both boards, across a year boundary.
	for (const board &b : boards)
	{
		rtc_nvm nv(b.d);
		CHECK(!nv.clock_set());                      // all-zero is "never set"
		nv.set_time(calendar{ 1985, 12, 31, 23, 59, 59, 0 });
		CHECK(nv.clock_set());
		CHECK_EQ(nv.at(nvloc::CENTURY), 19);         // the map splits the year
		CHECK_EQ(nv.at(nvloc::YEAR), 85);
		nv.tick_second();
		const calendar r = nv.time();
		CHECK(r.year == 1986 && r.month == 1 && r.day == 1);
		CHECK(r.hour == 0 && r.minute == 0 && r.second == 0);

		// An unset clock still has to count, or the boot's clock-sync --
		// which polls the seconds location between 100 ms sleeps -- parks
		// there forever.
		rtc_nvm z(b.d);
		for (int i = 0; i < 5; i++)
			z.tick_second();
		CHECK_EQ(z.at(nvloc::SECOND), 5);
		CHECK(!z.clock_set());
	}

	// ---- the battery file ------------------------------------------
	const char *const tmpdir = std::getenv("TMPDIR");
	std::string base = std::string(tmpdir ? tmpdir : "/tmp") + "/focus-nv-test";
	const std::string path = base + ".bin";

	{
		rtc_nvm a(rtc_decode::scm());
		for (unsigned i = 0; i < rtc_nvm::RTC_CELLS; i++) a.rtc[i] = u8(i * 3);
		for (unsigned i = 0; i < rtc_nvm::NVM_SIZE; i++) a.nvm[i] = u8(i ^ 0x5a);
		a.dirty = true;
		CHECK(a.save(path.c_str(), 7));
		CHECK(!a.dirty);                             // saving clears it

		rtc_nvm b2(rtc_decode::scm());
		const nv_load_result r = b2.load(path.c_str(), 7);
		CHECK(r.ok);
		CHECK(!r.legacy);
		CHECK_EQ(r.board, 7u);
		CHECK(r.saved_at > 0);
		bool same = true;
		for (unsigned i = 0; i < rtc_nvm::RTC_CELLS; i++)
			same = same && b2.rtc[i] == a.rtc[i];
		for (unsigned i = 0; i < rtc_nvm::NVM_SIZE; i++)
			same = same && b2.nvm[i] == a.nvm[i];
		CHECK(same);

		// The temporary the atomic save goes through is not left behind.
		const std::string tmp = path + ".new";
		CHECK(std::fopen(tmp.c_str(), "rb") == nullptr);
	}

	// A truncated file is refused AND leaves the live part alone. That is
	// the bug this format exists to close: the raw version read the clock
	// in, failed on the second read, and returned a false the caller
	// ignored -- a half-loaded battery, in silence.
	{
		rtc_nvm live(rtc_decode::scm());
		live.set_time(calendar{ 1985, 3, 1, 12, 0, 0, 0 });
		u8 before[rtc_nvm::RTC_CELLS];
		std::memcpy(before, live.rtc, sizeof before);

		const std::string cut = base + "-cut.bin";
		std::FILE *f = std::fopen(cut.c_str(), "wb");
		CHECK(f != nullptr);
		if (f) { u8 z[100] = {}; std::fwrite(z, 1, sizeof z, f); std::fclose(f); }
		const nv_load_result r = live.load(cut.c_str(), 7);
		CHECK(!r.ok);
		CHECK(r.error == nv_error::bad_size);
		CHECK(std::memcmp(before, live.rtc, sizeof before) == 0);
		std::remove(cut.c_str());
	}

	// A corrupt payload is refused the same way.
	{
		const std::string bad = base + "-bad.bin";
		std::FILE *in = std::fopen(path.c_str(), "rb");
		std::vector<u8> img;
		if (in)
		{
			u8 b[4096];
			size_t n;
			while ((n = std::fread(b, 1, sizeof b, in)) > 0)
				img.insert(img.end(), b, b + n);
			std::fclose(in);
		}
		CHECK_EQ(img.size(), size_t(nvfile::SIZE));
		if (img.size() == nvfile::SIZE)
		{
			img[nvfile::HEADER + 10] ^= 0xff;
			std::FILE *out = std::fopen(bad.c_str(), "wb");
			if (out) { std::fwrite(img.data(), 1, img.size(), out); std::fclose(out); }
			rtc_nvm live(rtc_decode::scm());
			live.rtc[0] = 0xa5;
			const nv_load_result r = live.load(bad.c_str(), 7);
			CHECK(!r.ok);
			CHECK(r.error == nv_error::bad_crc);
			CHECK_EQ(live.rtc[0], 0xa5);
		}
		std::remove(bad.c_str());
	}

	// The headerless files written before the format are still read: the
	// repo's own software/cct/scm.nv is one.
	{
		const std::string old = base + "-legacy.bin";
		std::FILE *f = std::fopen(old.c_str(), "wb");
		CHECK(f != nullptr);
		if (f)
		{
			u8 payload[nvfile::PAYLOAD];
			for (unsigned i = 0; i < sizeof payload; i++) payload[i] = u8(i);
			std::fwrite(payload, 1, sizeof payload, f);
			std::fclose(f);
		}
		rtc_nvm live(rtc_decode::scm());
		const nv_load_result r = live.load(old.c_str(), 7);
		CHECK(r.ok);
		CHECK(r.legacy);
		CHECK_EQ(r.saved_at, 0);                     // no off-time is knowable
		CHECK_EQ(live.rtc[5], 5);
		CHECK_EQ(live.nvm[0], u8(rtc_nvm::RTC_CELLS));
		std::remove(old.c_str());
	}

	// A missing file is its own answer, and no error text is a null.
	{
		rtc_nvm live(rtc_decode::scm());
		const nv_load_result r = live.load((base + "-absent.bin").c_str(), 7);
		CHECK(!r.ok);
		CHECK(r.error == nv_error::no_file);
		CHECK(nv_error_text(r.error) != nullptr);
	}

	// The battery running over an off-time, and giving up past its rating.
	{
		rtc_nvm live(rtc_decode::scm());
		live.set_time(calendar{ 1985, 3, 1, 12, 0, 0, 0 });
		nv_load_result r;
		r.ok = true;
		r.saved_at = (long long)std::time(nullptr) - 3600;
		live.run_battery(r, rtc_nvm::BATTERY_DAYS);
		CHECK(!r.battery_expired);
		CHECK_EQ(live.time().hour, 13);              // an hour off, an hour on

		rtc_nvm flat(rtc_decode::scm());
		flat.set_time(calendar{ 1985, 3, 1, 12, 0, 0, 0 });
		nv_load_result q;
		q.ok = true;
		q.saved_at = (long long)std::time(nullptr) - 40LL * 86400;
		flat.run_battery(q, rtc_nvm::BATTERY_DAYS);
		CHECK(q.battery_expired);
		CHECK(flat.battery_fail);
		CHECK_EQ(flat.time().hour, 12);              // and the clock stands
	}

	// The bit as the boards present it: R2 bit 5 on the SCM, R1 bit 3 on
	// the keyboard board, and the manuals' four-second window when asked for.
	{
		scm_device scm;
		scm.set_battery_fail(true);
		CHECK_EQ(scm.read_register(2) & (1u << 5), 1u << 5);

		console_card kbd;
		kbd.nv.battery_fail = true;
		CHECK_EQ(kbd.read_fr(1) & (1u << 3), 1u << 3);

		kbd.nv.battery_window = true;
		CHECK_EQ(kbd.read_fr(1) & (1u << 3), 1u << 3);
		for (int i = 0; i < 4; i++)
			kbd.rtc_second_tick();
		CHECK_EQ(kbd.read_fr(1) & (1u << 3), 0u);
	}

	std::remove(path.c_str());
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
	test_rtc_nvm();
	test_gfx();

	std::printf("\n%d checks, %d failed\n", g_run, g_fail);
	std::printf(g_fail ? "FAIL\n" : "PASS\n");
	return g_fail ? 1 : 0;
}
