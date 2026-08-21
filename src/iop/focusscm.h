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

#include <cstdio>

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
		// The RTC and NVM keep their contents across a reset -- that is
		// what the battery is for.
	}

	// One second of wall time for the RTC. The cell layout is the one the
	// HP-UX 5.30 clock driver defines by its own writes, read under the
	// uniform complemented address decode below: initializing an unset
	// clock it stores second, minute, hour, day, month, year at cells
	// 15, 13, 11, 8, 7, 6 -- 0, 0, 0, 1, 1, 70: midnight, 1 January
	// 1970 -- and its date validator (CORE SYSTEM +0x7b7c, the Julian
	// arithmetic with the 1461 and 153 constants) rejects anything
	// outside day 1-31 and month 1-12 with escape 254.
	void rtc_second_tick()
	{
		auto bump = [this](unsigned loc, u8 wrap, u8 floor = 0) -> bool {
			m_rtc[loc] = u8(m_rtc[loc] + 1);
			if (m_rtc[loc] < wrap)
				return false;
			m_rtc[loc] = floor;
			return true;
		};
		if (!bump(15, 60)) return;        // second
		if (!bump(13, 60)) return;        // minute
		if (!bump(11, 24)) return;        // hour
		if (!bump(8, 32, 1)) return;      // day of month (approximate)
		if (!bump(7, 13, 1)) return;      // month
		bump(6, 100);                     // year
	}

	// The RTC and NVM are battery-backed: their contents survive power
	// off, for ten days at least, and that persistence is load-bearing --
	// the first boot's clock-not-set path SETS the clock and writes a
	// default configuration into the NVM, and only a later boot, finding
	// them already valid, takes the configured paths. A file stands in
	// for the battery.
	bool load_nv(const char *path)
	{
		FILE *f = std::fopen(path, "rb");
		if (!f)
			return false;
		const bool ok = std::fread(m_rtc, 1, sizeof m_rtc, f) == sizeof m_rtc
				&& std::fread(m_nvm, 1, sizeof m_nvm, f) == sizeof m_nvm;
		std::fclose(f);
		return ok;
	}

	bool save_nv(const char *path) const
	{
		FILE *f = std::fopen(path, "wb");
		if (!f)
			return false;
		const bool ok = std::fwrite(m_rtc, 1, sizeof m_rtc, f) == sizeof m_rtc
				&& std::fwrite(m_nvm, 1, sizeof m_nvm, f) == sizeof m_nvm;
		std::fclose(f);
		return ok;
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
			// The self-identity code, 4007 hex (Service Manual 2-29). Bit
			// 14 set is what the CPU's bootstrap scans select codes
			// downward looking for; the low bits keep the powerup device
			// scan from registering the SCM as an I/O card (its gate wants
			// the low byte zero).
			return 0x4007;

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

	// The RTC and NVM behind the register file, as the HP-UX 5.30 clock
	// driver actually drives them (read off the running system; the shapes
	// match the Service Manual's register allocations, 2-29/2-30):
	//
	//   W R12   RTC/NVM address latch. The address travels COMPLEMENTED
	//           (Service Manual 2-30), and uniformly: the clock-set
	//           sweep writes what look like plain 0, 2, 4... and the
	//           clock-check path writes ffff, fff1... -- only one
	//           inversion applied to both makes the reader find the
	//           writer's cells, and the date validator agrees
	//   W R9    RTC data for the latched location, COMPLEMENTED on the bus
	//           -- the unset-clock path writes ~0, ~0, ~0, ~1, ~1, ~70:
	//           midnight, 1 January 1970
	//   R R9    RTC data back, complemented likewise
	//   R R1    polled before every exchange; bit 7 = ready, as on the
	//           520's keyboard-board RTC (R1 has no documented read side)
	//   R/W R11 the boot's 2 KB configuration readout -- NVM data for the
	//           latched address (the manual assigns NVM to R13; the driver
	//           reads its bulk configuration through R11, so both answer)
	//   R/W R13 NVM data byte (Service Manual: address in complemented
	//           form -- but the driver latches plain ascending addresses,
	//           so the latch is taken as written)
	u32 read_fr(unsigned n) override
	{
		switch (n)
		{
		case 1:  return 0x80;                  // ready
		case 2:  return read_register(2);
		case 3:  return read_register(3);
		case 4:  return read_register(4);
		case 6:  return read_register(6);
		case 9:
		{
			const u32 v = u32(~m_rtc[(~m_addr_latch) & 15]) & 0xff;
			// "Succeeding reads or writes cause automatic increments of
			// the address (address is complemented)" -- the complemented
			// value counts down so the decoded location counts up.
			m_addr_latch = (m_addr_latch & ~15u) | ((m_addr_latch - 1) & 15);
			return v;
		}
		case 10: return m_service_word;
		// "Writing a 16-bit address to R12 in complemented form loads the
		// address of the NVM location" (Service Manual 2-30). The boot
		// bears it out: it sweeps R12 from ffff down through fff800, which
		// complemented is 0 through 07ff -- the whole 2 KB part, in order.
		case 11:
		case 13: return m_nvm[(~m_addr_latch) & (NVM_SIZE - 1)];
		case 12: return m_addr_latch;          // data-bus test: echo
		default: return m_scratch[n & 15];
		}
	}

	void write_fr(unsigned n, u32 v) override
	{
		v &= 0xffff;
		switch (n)
		{
		case 1:  write_register(1, v); break;
		case 9:
			m_rtc[(~m_addr_latch) & 15] = u8(~v);
			m_addr_latch = (m_addr_latch & ~15u) | ((m_addr_latch - 1) & 15);
			break;
		case 10: m_service_word = v; break;
		case 11:
		case 13: m_nvm[(~m_addr_latch) & (NVM_SIZE - 1)] = u8(v); break;
		case 12: m_addr_latch = v; break;
		default: m_scratch[n & 15] = v; break;
		}
	}

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
	// The battery-backed clock. All-zero is "midnight, clock never set",
	// which the unset-clock path detects and initializes.
	u8 m_rtc[64] = {};
	// Registers with no modelled behaviour remember what was written, as
	// the keyboard board's self test taught.
	u32 m_scratch[16] = {};
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
