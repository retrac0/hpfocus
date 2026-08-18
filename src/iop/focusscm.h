// license:BSD-3-Clause
// copyright-holders:
/***************************************************************************

    HP 9000 Series 500 System Control Module

    The SCM sits on select code 7 and supplies the real-time clock, the
    service panel, high-temperature and power-fail warnings, 2 KB of
    battery-backed memory, and the bootstrap loader ROM.

    Reference: HP 9000 Models 530/540 Service Manual, 09040-90038, sheets
    2-27 to 2-31.

    A select code is an (IOP channel, peripheral address) pair: codes 0-6 are
    the I/O backplane, 7 is the SCM, 8-15 the first additional IOP and 16-23
    the second. So the SCM answers IOP channel 1, peripheral address 7.

    BIT NUMBERING. Unlike the Instruction Set ERS, the service manual numbers
    these hardware registers conventionally, with bit 0 least significant.
    That is the convention used here.

    THE LOADER ROM. No dump of 09020-80000 (rev A) or 09020-80001 (rev B) is
    known to exist. The fetch protocol is fully documented and implemented
    below, so a dump can simply be supplied; without one the ROM region reads
    as 0xFF and the high-level loader takes over instead.

***************************************************************************/

#ifndef MAME_IOP_FOCUSSCM_H
#define MAME_IOP_FOCUSSCM_H

#pragma once

#include "focusiop.h"

namespace focus {

class scm_device : public iop_interface
{
public:
	static constexpr unsigned SELECT_CODE = 7;
	static constexpr unsigned ROM_SIZE = 8 * 1024;
	static constexpr unsigned NVM_SIZE = 2 * 1024;

	// Register 1 command that arms the ROM read: it resets the address
	// counter to FFFF hex and enables DMA and the self-test bit
	// (Service Manual, sheet 2-31).
	static constexpr u8 ARM_ROM = 0x84;

	// The initial burst the CPU microcode pulls in before the loader itself
	// fetches the remaining 6 KB.
	static constexpr unsigned INITIAL_BURST = 2048;

	scm_device() { reset(); }

	void reset()
	{
		m_rom_addr = 0xffff;
		m_dma_enabled = false;
		m_self_test = false;
		m_battery_fail = false;
		m_power_fail = false;
		m_service_word = 0;
		m_addr_latch = 0;
		m_bytes_read = 0;
	}

	// Supply a loader ROM image if one is ever dumped.
	void set_rom(const u8 *data, unsigned len)
	{
		m_rom_present = (data != nullptr);
		for (unsigned i = 0; i < ROM_SIZE; i++)
			m_rom[i] = (data && i < len) ? data[i] : 0xff;
	}

	bool rom_present() const { return m_rom_present; }
	unsigned bytes_read() const { return m_bytes_read; }

	void set_power_fail(bool state) { m_power_fail = state; }
	void set_battery_fail(bool state) { m_battery_fail = state; }

	//----------------------------------------------------------------
	//  registers
	//----------------------------------------------------------------

	// The interface command field of the MPB channel address selects the
	// register, so the IOP passes it straight through.
	u32 read_register(u8 reg)
	{
		switch (reg)
		{
		case 2:
			// Status. Bit 1 power-fail warning, bit 3 self-test enabled,
			// bit 5 RTC/NVM battery fail. The battery bit is latched at
			// power-up and is only valid if read within four seconds.
			return (m_power_fail ? (1u << 1) : 0)
					| (m_self_test ? (1u << 3) : 0)
					| (m_battery_fail ? (1u << 5) : 0);

		case 3:
			// Bit 14 announces that a loader lives here. The CPU's
			// bootstrap scans select codes downward looking for it.
			return 1u << 14;

		case 4:
			// ROM data, address auto-decrementing after each read.
			return rom_byte();

		case 6:
			// The final byte of the burst comes from R6 rather than R4.
			return rom_byte();

		case 10:
			// Reserved for traffic between the service processor and the
			// main system software.
			return m_service_word;

		case 12:
			return m_addr_latch;

		default:
			return 0;
		}
	}

	void write_register(u8 reg, u32 data)
	{
		switch (reg)
		{
		case 1:
			if ((data & 0xff) == ARM_ROM)
			{
				m_rom_addr = 0xffff;
				m_dma_enabled = true;
				m_self_test = true;
				m_bytes_read = 0;
			}
			break;

		case 10:
			m_service_word = data;
			break;

		case 12:
			// The service manual notes the address is written in
			// complemented form.
			m_addr_latch = (~data) & 0xffff;
			break;

		default:
			break;
		}
	}

	//----------------------------------------------------------------
	//  iop_interface
	//----------------------------------------------------------------

	u32 status() override { return read_register(2); }
	u32 flag() override { return read_register(3); }

	// The SCM feeds the ROM out over DMA once armed.
	bool service_request() override { return m_dma_enabled; }
	u8 dma_read() override { return rom_byte(); }

	u32 read_data() override { return read_register(4); }
	void write_data(u32 v) override { write_register(1, v); }

private:
	u8 rom_byte()
	{
		if (!m_dma_enabled)
			return 0xff;
		const u8 v = m_rom[m_rom_addr & (ROM_SIZE - 1)];
		// The counter decrements after every read (Service Manual 2-30).
		m_rom_addr = (m_rom_addr - 1) & 0xffff;
		m_bytes_read++;
		if (m_bytes_read >= INITIAL_BURST)
			m_dma_enabled = false;
		return v;
	}

	u8 m_rom[ROM_SIZE] = {};
	u8 m_nvm[NVM_SIZE] = {};
	u32 m_rom_addr = 0xffff;
	u32 m_service_word = 0;
	u32 m_addr_latch = 0;
	unsigned m_bytes_read = 0;
	bool m_rom_present = false;
	bool m_dma_enabled = false;
	bool m_self_test = false;
	bool m_battery_fail = false;
	bool m_power_fail = false;
};

//**************************************************************************
//  BOOTSTRAP
//**************************************************************************

// The documented power-up sequence (Service Manual, sheet 2-31):
//
//   1  every processor stack chip runs self-test
//   2  the CPU microcode has the IOP read R3 of select code 7; bit 14 set
//      identifies a loader. If clear it tries SC6, and so on downward.
//   3  84 hex goes to R1 of that select code, resetting the ROM address
//      counter to FFFF hex and enabling DMA and the self-test bit
//   4  2048 bytes are DMA'd in from R4, the last byte from R6
//   5  that initial 2 KB pulls in the remaining 6 KB of loader
//
// Returns the select code holding the loader, or -1 if none answers.
inline int find_loader(iop_core &iop, u8 iop_channel)
{
	for (int sc = int(scm_device::SELECT_CODE); sc >= 0; sc--)
	{
		io_interface::target t;
		t.opcode = CMD_RIFG;      // read interface flag, which is R3 here
		t.channel = iop_channel;
		t.pa = u8(sc);
		t.ic = 3;
		u32 v = 0;
		if (iop.io_read(t, v) && (v & (1u << 14)))
			return sc;
	}
	return -1;
}

} // namespace focus

#endif // MAME_IOP_FOCUSSCM_H
