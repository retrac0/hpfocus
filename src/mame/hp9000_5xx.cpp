// license:BSD-3-Clause
// copyright-holders:
/***************************************************************************

    HP 9000 Series 500

    The first commercial 32-bit workstations, built on the HP FOCUS chip set
    (1982). Four models share one processor stack and differ in packaging:

      520 (HP 9020)  integrated workstation, built-in display and keyboard.
                     Boots HP BASIC or HP-UX.
      530 (HP 9030)  19 inch rack mount, HP-UX only.
      540 (HP 9040)  free-standing cabinet, HP-UX only.
      550            industrial system, 1984, replaced the 530 and 540.

    The card stack holds twelve slots on the Memory Processor Bus: slot 1 a
    CPU, slot 2 an I/O Processor, further CPUs and IOPs above, then RAM.

    STATUS: early. The CPU, its address space and RAM are present and the
    machine can be single-stepped in the debugger. The IOP, the System
    Control Module with its loader ROM, mass storage and the 520's display
    and keyboard are all still to come, so nothing boots yet. See
    docs/IMPLEMENTATION.md in the hpfocus project for the plan.

    References:
      Focus Machine Instruction Set ERS, A-1FE1-3020-B, 12/06/82
      HP 9000 Models 530/540 Service Manual, 09040-90038, Mar 1985
      HP Journal, August 1983 (the chip set), March and May 1984

***************************************************************************/

#include "emu.h"
#include "cpu/focus/focus.h"
#include "iop/focusiop.h"
#include "iop/focusscm.h"
#include "iop/focuscards.h"
#include "imagedev/snapquik.h"
#include "machine/terminal.h"
#include "sound/beep.h"  // terminal.cpp needs it linked; the scanner follows driver includes
#include "screen.h"

namespace {

class hp9000_5xx_state : public driver_device, public focus::iop_core::host
{
public:
	hp9000_5xx_state(const machine_config &mconfig, device_type type,
			const char *tag)
		: driver_device(mconfig, type, tag)
		, m_cpu(*this, "cpu")
		, m_ram(*this, "ram")
		, m_terminal(*this, "terminal")
		, m_iop(*this, 2)   // the channel the boot handoff's inventory announces
	{
	}

	void hp9020(machine_config &config) ATTR_COLD;
	void hp9040(machine_config &config) ATTR_COLD;

protected:
	virtual void machine_start() override ATTR_COLD;
	virtual void machine_reset() override ATTR_COLD;

private:
	void mem_map(address_map &map) ATTR_COLD;
	void kbd_put(u8 data);

	// focus::iop_core::host -- the IOP reaches memory and the CPU through
	// these. The CPU device already implements focus::memory_interface.
	virtual focus::memory_interface &iop_memory() override
	{ return m_cpu->memory(); }
	virtual void iop_interrupt(bool state) override
	{
		// Message register bit 16 upward are the I/O device interrupts
		// (Instruction Set ERS sheet 13).
		m_cpu->set_input_line(16, state ? ASSERT_LINE : CLEAR_LINE);
	}

	TIMER_CALLBACK_MEMBER(iop_tick);
	TIMER_CALLBACK_MEMBER(rtc_tick);

	QUICKLOAD_LOAD_MEMBER(quickload_cb);

	required_device<focus_device> m_cpu;
	required_shared_ptr<u32> m_ram;
	optional_device<generic_terminal_device> m_terminal;

	focus::iop_core m_iop;
	focus::scm_device m_scm;
	focus::console_card m_console0;   // select code 0: display/reporter
	focus::console_card m_console6;   // select code 6: keyboard board
	focus::probe_card m_probe1;       // hp9040: a serial-slot probe
	emu_timer *m_iop_timer = nullptr;
	emu_timer *m_rtc_timer = nullptr;
	u32 m_prev_rq = 0;
	bool m_nudged = false;
	u32 m_handoff_q = 0;
};


// The FOCUS address space is 29 bits of byte address. 16 MB matches the
// boot handoff this driver performs: the powerup module sizes memory from
// the top-of-memory word at Q+64, and the loader's stack lives just below
// the top (SB 0xe00000, Q 0xf00203).
static constexpr u32 RAM_SIZE = 16 * 1024 * 1024;

void hp9000_5xx_state::mem_map(address_map &map)
{
	map.unmap_value_high();
	map(0x0000000, 0x0000000 + RAM_SIZE - 1).ram().share("ram");
}


void hp9000_5xx_state::machine_start()
{
	// The System Control Module answers select code 7, which is IOP
	// channel 1, peripheral address 7. The consoles: select code 0 is
	// the display -- the panic reporter writes there, and so does the
	// alphanumeric driver, which binds only when the card answers 1 or
	// 6 and then writes the screen with halfword DMA. 6 is the identity
	// that also drives the frame buffer directly (2400 halfwords of
	// blanks, one per character cell) the way the 520 manual describes,
	// so it is the default. Select code 6 is the keyboard board.
	m_iop.attach(focus::scm_device::SELECT_CODE, &m_scm);
	m_console0.id = 6;
	if (const char *e = getenv("FOCUS_SC0ID"))
		m_console0.id = strtoul(e, nullptr, 0);
	m_console0.log_fr = getenv("FOCUS_SC0LOG") != nullptr;
	if (const char *e = getenv("FOCUS_SC0ST"))
		m_console0.st = strtoul(e, nullptr, 0);
	m_iop.attach(0, &m_console0);
	m_console6.id = 2;   // keyboard ID register: data bus bit 1
	m_console6.log_fr = getenv("FOCUS_KBDLOG") != nullptr;
	if (const char *e = getenv("FOCUS_KBDST"))
		m_console6.st = strtoul(e, nullptr, 0);
	m_console6.iop = &m_iop;
	m_console6.pa = 6;
	m_iop.attach(6, &m_console6);
	// SC6 keyboard interrupts are enabled by the hardware itself.
	m_iop.enable_interrupt(6);
	// The 520's alphanumeric console: characters the OS writes to the
	// select code 0 display flow into the generic terminal, whose
	// keyboard feeds the keyboard board's keycode register back.
	if (m_terminal)
		m_console0.on_char = [this](u8 c) { m_terminal->write(c); };
	// hp9040 carries a probe in a spare slot to map what HP-UX's SERIAL
	// driver expects from a 98626/98644 card; FOCUS_SC1ID picks the
	// identity it answers.
	if (const char *e = getenv("FOCUS_SC1ID"))
	{
		m_probe1.id = strtoul(e, nullptr, 0);
		m_probe1.pa = 1;
		m_probe1.log = getenv("FOCUS_SC1LOG") != nullptr;
		m_iop.attach(1, &m_probe1);
	}
	m_cpu->set_io(&m_iop);

	// The powerup module's final IXIT unwinds a frame whose delta-Q word
	// is the loader's MPB inventory; the word must be cleared when the
	// module reaches that IXIT with the handoff stack still current --
	// what the real loader does to reconcile the two roles is unknown.
	// Same nudge as the bootrun harness, instruction-precise.
	m_cpu->set_pre_step([this](focus::cpu_core &core) {
		// Traps vector through CST entry 1 once the powerup module has
		// built the tables and pointed dedicated location 0 at them.
		if (!core.traps_enabled() && core.read_mem32(0) != 0)
			core.enable_traps(true);
		if (m_nudged || core.r.q != m_handoff_q || m_handoff_q == 0)
			return;
		const u32 w = core.read_mem32(core.r.p & ~3u);
		const u16 insn = (core.r.p & 2) ? u16(w) : u16(w >> 16);
		if (insn == 0x007c)
		{
			core.write_mem32(m_handoff_q - 3, 0);
			m_nudged = true;
			logerror("nudge: (Q) cleared for the final IXIT\n");
		}
	});

	// The IOP polls its interfaces continuously and runs the interval
	// timer; one emulated tick per millisecond is enough to get both
	// moving. Real timings are in the I/O ERS section 7.
	m_iop_timer = timer_alloc(FUNC(hp9000_5xx_state::iop_tick), this);
	m_iop_timer->adjust(attotime::from_msec(1), 0, attotime::from_msec(1));
	m_rtc_timer = timer_alloc(FUNC(hp9000_5xx_state::rtc_tick), this);
	m_rtc_timer->adjust(attotime::from_seconds(1), 0, attotime::from_seconds(1));
}


TIMER_CALLBACK_MEMBER(hp9000_5xx_state::iop_tick)
{
	m_iop.poll_and_service();
	// One timer count is 512 clock periods = 28.4 us; a 1 ms tick is
	// about 35 of them.
	m_iop.timer_tick(35);
	if (m_iop.take_timer_event())
		m_cpu->core().raise_message(13);

	// Every accepted device raise is one message at 16 + its level (the
	// microcode handler maps it back and pulls the device from the
	// level's list). The attention poll reaches the CPU as message 14
	// (I/O ERS section 2.1).
	const u32 raised = m_iop.take_raise_events();
	for (unsigned pa = 0; pa < 8; pa++)
		if (raised & (1u << pa))
			m_cpu->core().raise_message(16 + m_iop.interrupt_level(pa));
	if (m_iop.take_attention_event())
		m_cpu->core().raise_message(14);
}


TIMER_CALLBACK_MEMBER(hp9000_5xx_state::rtc_tick)
{
	m_console6.rtc_second_tick();
}


void hp9000_5xx_state::machine_reset()
{
	// iop_core::reset() rebuilds every channel_state, which drops the
	// interface attachments; hook everything back up afterwards.
	m_iop.reset();
	m_scm.reset();
	m_iop.attach(focus::scm_device::SELECT_CODE, &m_scm);
	m_iop.attach(0, &m_console0);
	m_iop.attach(6, &m_console6);
	m_iop.enable_interrupt(6);
	m_prev_rq = 0;

	// A loader ROM image would be installed here. None is dumped, so the
	// SCM answers the select-code scan but has nothing to hand over; see
	// docs/SOFTWARE.md.
	const u8 *rom = memregion("loader") ? memregion("loader")->base() : nullptr;
	m_scm.set_rom(rom, focus::scm_device::ROM_SIZE);
}


// Boot a Series 500 image the way the (undumped) SCM loader ROM hands
// one over -- the contract bootrun reverse-engineered from SYSTEM
// POWERUP itself. The image loads high, the powerup module executes in
// place with PB = 0, and the loader's parameter block sits at Q: the
// MPB inventory doubling as the final IXIT's frame, the load address at
// Q+52, the top-of-memory word at Q+64.
QUICKLOAD_LOAD_MEMBER(hp9000_5xx_state::quickload_cb)
{
	constexpr u32 LOAD = 0x100000;
	const u64 size = image.length();
	if (size < 0x100 || LOAD + size > RAM_SIZE)
		return std::make_pair(image_error::INVALIDLENGTH,
				"image empty or larger than RAM allows");

	std::vector<u8> img(size);
	image.fread(img.data(), size);

	auto &core = m_cpu->core();
	focus::memory_interface &mem = m_cpu->memory();
	for (u64 i = 0; i < size; i++)
		mem.write8(LOAD + u32(i), img[i]);

	auto beword = [&img](u32 off) {
		return (u32(img[off]) << 24) | (u32(img[off + 1]) << 16)
				| (u32(img[off + 2]) << 8) | img[off + 3];
	};
	u32 entry_off = 0;
	for (u32 off = 0x30; off + 4 <= 0x80; off += 4)
		if (beword(off) == 0xc4) { entry_off = off + 0x20; break; }
	if (!entry_off)
		return std::make_pair(image_error::INVALIDIMAGE,
				"no 000000c4 terminator in the module header");

	core.r.pb = 0;
	core.r.pl = LOAD + beword(8) + 3;
	core.r.p  = LOAD + entry_off;

	core.r.sb = 0xe00000;
	core.r.sl = 0xf7ffff | 3;
	core.r.q  = 0xf00203;
	core.r.s  = core.r.q + 0x200;
	core.r.db = 0;
	core.r.dl = 0xffffff | 3;
	core.r.dst = 1152;
	core.r.put(focus::sf::M, 1);
	core.r.mask = focus::ers32::set(0, 3, 3, 1);

	const u32 q = core.r.q;
	m_handoff_q = q;
	m_nudged = false;

	// The loader's parameter block; see bootrun.cpp for the full
	// derivation of every word.
	mem.write16(q - 3 + 0, 0x9004);   // MPB inventory: CPU, channel 1
	mem.write16(q - 3 + 2, 0xa002);   // IOP, channel 2
	mem.write16(q - 3 + 4, 0x8000);   // (Q+1) high / global DS pointer
	mem.write16(q - 3 + 6, 0x0000);   // inventory terminator
	mem.write32(q - 3 - 4, 0x00020000);   // (Q-1): privileged, CSN 0
	mem.write32(q - 3 - 8, 0x5c84);       // (Q-2): the dispatcher
	mem.write32(q - 3 - 12, 0);           // (Q-3): X
	mem.write32(q + 2 * 4 - 3, 0);        // (Q+2): previous mask
	mem.write32(q + 3 * 4 - 3, 0);        // (Q+3): device number
	mem.write32(q + 52 - 3, LOAD);        // where the image was loaded
	mem.write32(q + 64 - 3, RAM_SIZE);    // top of memory
	for (u32 i = 0; i < 20; i++)          // boot device record
		mem.write8(q + 65 + i, 0);
	mem.write32(q + 88 - 3, 0);
	mem.write32(q + 92 - 3, 0);
	core.push(u32(-2));                   // cold start

	core.enable_traps(false);
	logerror("boot image %s: %u bytes at %#x, entry %#x\n",
			image.filename(), u32(size), LOAD, core.r.p);
	return std::make_pair(std::error_condition(), std::string());
}


// A keystroke from the terminal becomes a keycode in the keyboard
// board's holding register (R10), with the service-request latch set.
// The real board sends keycodes from the keyswitch matrix, not ASCII,
// and the system translates them through a 256-byte table -- the same
// one in the diagnostic and BASIC images (diag.bin 0x23694), whose
// entries the emulated keyboard was read against: sweeping every
// keycode from 0x20 to 0x7e and reading the screen echo reproduced the
// table exactly -- 0x54 an S, 0x5c a D, 0x74 an R -- and 0x70 cleared
// the line, so that is Return. This is that half of the table
// inverted, with a lowercase letter arriving as its uppercase key.
// Keycodes below 0x20 index it through a modifier not yet identified
// (they echoed nothing of what the table promised), which is why the
// five characters reachable only there (, - . ; =) map to zero, along
// with the keys that are not characters at all.
static const u8 KEYCODES[128] =
{
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   // ........
	0x00, 0x00, 0x70, 0x00, 0x00, 0x70, 0x00, 0x00,   // ........
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   // ........
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   // ........
	0x7f, 0x79, 0x59, 0x2c, 0x7d, 0x20, 0x25, 0x3f,   //  !"#$%&'
	0x26, 0x2f, 0x2e, 0x29, 0x00, 0x00, 0x00, 0x3c,   // ()*+,-./
	0x41, 0x52, 0x4a, 0x53, 0x62, 0x5a, 0x63, 0x6a,   // 01234567
	0x72, 0x6b, 0x61, 0x00, 0x51, 0x00, 0x49, 0x45,   // 89:;<=>?
	0x24, 0x7c, 0x21, 0x4d, 0x5c, 0x60, 0x65, 0x36,   // @ABCDEFG
	0x35, 0x76, 0x5e, 0x67, 0x5f, 0x4f, 0x57, 0x6f,   // HIJKLMNO
	0x77, 0x78, 0x74, 0x54, 0x6d, 0x6e, 0x56, 0x3e,   // PQRSTUVW
	0x48, 0x75, 0x4c, 0x22, 0x73, 0x2b, 0x2d, 0x27,   // XYZ[\]^_
	0x5b, 0x7c, 0x21, 0x4d, 0x5c, 0x60, 0x65, 0x37,   // `abcdefg
	0x39, 0x31, 0x38, 0x30, 0x3a, 0x32, 0x3b, 0x6f,   // hijklmno
	0x77, 0x78, 0x74, 0x54, 0x6d, 0x6e, 0x33, 0x3e,   // pqrstuvw
	0x48, 0x75, 0x4c, 0x2a, 0x42, 0x23, 0x28, 0x00,   // xyz{|}~.
};

void hp9000_5xx_state::kbd_put(u8 data)
{
	const u8 code = KEYCODES[data & 0x7f];
	if (code)
		m_console6.type(code);
}


void hp9000_5xx_state::hp9020(machine_config &config)
{
	// 18 MHz, 55 ns microinstruction time (Hardware Technical Data).
	FOCUS(config, m_cpu, 18_MHz_XTAL);
	m_cpu->set_addrmap(AS_PROGRAM, &hp9000_5xx_state::mem_map);
	m_cpu->set_mpb_channel(1);

	// The 520's 98760A alpha display is 26 rows by 80 columns of 7x9
	// characters. Until its frame-buffer protocol is decoded, the
	// character stream renders on a generic terminal with a built-in
	// font, and the terminal's keyboard plays the keyswitch matrix.
	GENERIC_TERMINAL(config, m_terminal, 0);
	m_terminal->set_keyboard_callback(FUNC(hp9000_5xx_state::kbd_put));

	QUICKLOAD(config, "quickload", "bin").set_load_callback(
			FUNC(hp9000_5xx_state::quickload_cb));
}


void hp9000_5xx_state::hp9040(machine_config &config)
{
	// The 540 is the same processor stack in a free-standing cabinet, with
	// no integrated display; the console is an external terminal.
	FOCUS(config, m_cpu, 18_MHz_XTAL);
	m_cpu->set_addrmap(AS_PROGRAM, &hp9000_5xx_state::mem_map);
	m_cpu->set_mpb_channel(1);

	QUICKLOAD(config, "quickload", "bin").set_load_callback(
			FUNC(hp9000_5xx_state::quickload_cb));
}


// The System Control Module's 8 KB bootstrap loader ROM, 09020-80000 rev A
// or 09020-80001 rev B. No dump of either is known to exist; the museum's
// Series 500 collection does not include one. The documented alternative is
// to emulate the loader's behaviour at a high level, which is what the plan
// plumps for -- the protocol is fully described even though the code is not.
ROM_START(hp9020)
	ROM_REGION(0x2000, "loader", ROMREGION_ERASEFF)
	ROM_LOAD_OPTIONAL("09020-80001.bin", 0x0000, 0x2000, NO_DUMP)
ROM_END

ROM_START(hp9040)
	ROM_REGION(0x2000, "loader", ROMREGION_ERASEFF)
	ROM_LOAD_OPTIONAL("09020-80001.bin", 0x0000, 0x2000, NO_DUMP)
ROM_END

} // anonymous namespace


//   YEAR  NAME    PARENT  COMPAT  MACHINE  INPUT  CLASS             INIT        COMPANY            FULLNAME         FLAGS
COMP(1982, hp9020, 0,      0,      hp9020,  0,     hp9000_5xx_state, empty_init, "Hewlett-Packard", "HP 9000/520", MACHINE_NOT_WORKING | MACHINE_NO_SOUND)
COMP(1982, hp9040, 0,      0,      hp9040,  0,     hp9000_5xx_state, empty_init, "Hewlett-Packard", "HP 9000/540", MACHINE_NOT_WORKING | MACHINE_NO_SOUND)
