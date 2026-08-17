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
#include "screen.h"

namespace {

class hp9000_5xx_state : public driver_device
{
public:
	hp9000_5xx_state(const machine_config &mconfig, device_type type,
			const char *tag)
		: driver_device(mconfig, type, tag)
		, m_cpu(*this, "cpu")
		, m_ram(*this, "ram")
	{
	}

	void hp9020(machine_config &config) ATTR_COLD;
	void hp9040(machine_config &config) ATTR_COLD;

protected:
	virtual void machine_start() override ATTR_COLD;
	virtual void machine_reset() override ATTR_COLD;

private:
	void mem_map(address_map &map) ATTR_COLD;
	u32 screen_update(screen_device &screen, bitmap_rgb32 &bitmap,
			const rectangle &cliprect);

	required_device<focus_device> m_cpu;
	required_shared_ptr<u32> m_ram;
};


// The FOCUS address space is 29 bits of byte address. A 520 shipped with far
// less than that populated; 2 MB is a reasonable starting configuration and
// well within the 10 MB ceiling.
static constexpr u32 RAM_SIZE = 2 * 1024 * 1024;

void hp9000_5xx_state::mem_map(address_map &map)
{
	map.unmap_value_high();
	map(0x0000000, 0x0000000 + RAM_SIZE - 1).ram().share("ram");
}


void hp9000_5xx_state::machine_start()
{
}


void hp9000_5xx_state::machine_reset()
{
	// Nothing supplies a boot image yet. The real machine finds its loader
	// by scanning select codes downward from 7 for a device with bit 14 of
	// R3 set, then DMAs 2 KB of loader ROM in from R4 (530/540 Service,
	// sheet 2-31). That belongs with the IOP and the System Control Module.
}


u32 hp9000_5xx_state::screen_update(screen_device &screen,
		bitmap_rgb32 &bitmap, const rectangle &cliprect)
{
	bitmap.fill(rgb_t::black(), cliprect);
	return 0;
}


void hp9000_5xx_state::hp9020(machine_config &config)
{
	// 18 MHz, 55 ns microinstruction time (Hardware Technical Data).
	FOCUS(config, m_cpu, 18_MHz_XTAL);
	m_cpu->set_addrmap(AS_PROGRAM, &hp9000_5xx_state::mem_map);
	m_cpu->set_mpb_channel(1);

	// The 9020B's 12 inch monochrome display is 560 x 455. The 9020A is
	// 512 x 390 colour and the 9020C 560 x 455 colour; none is driven yet.
	screen_device &screen(SCREEN(config, "screen"));
	screen.set_raw(18_MHz_XTAL, 720, 0, 560, 500, 0, 455);
	screen.set_screen_update(FUNC(hp9000_5xx_state::screen_update));
}


void hp9000_5xx_state::hp9040(machine_config &config)
{
	// The 540 is the same processor stack in a free-standing cabinet, with
	// no integrated display; the console is an external terminal.
	FOCUS(config, m_cpu, 18_MHz_XTAL);
	m_cpu->set_addrmap(AS_PROGRAM, &hp9000_5xx_state::mem_map);
	m_cpu->set_mpb_channel(1);
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
