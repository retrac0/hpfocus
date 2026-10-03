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
#include "focusrtc.h"

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
		m_power_fail = false;
		m_service_word = 0;
		m_bytes_read = 0;
		// The RTC and NVM keep their contents across a reset -- that is
		// what the battery is for; power_on_reset() clears the run state
		// and nothing else.
		m_nv.power_on_reset();
	}

	// One second of wall time for the RTC. On this board that is the digit
	// clock of focusrtc.h -- seconds, minutes, hours, day, day of week and
	// month as BCD digits in the locations HP-UX 5.30's driver reads them
	// from, counting while the OS has it started -- and no year: the OS
	// keeps that in the NVM.
	void rtc_second_tick() { m_nv.tick_second(); }

	// The RTC and NVM are battery-backed: their contents survive power
	// off, for ten days at least, and that persistence is load-bearing --
	// the first boot's clock-not-set path SETS the clock and writes a
	// default configuration into the NVM, and only a later boot, finding
	// them already valid, takes the configured paths. A file stands in
	// for the battery.
	nv_load_result load_nv(const char *path)
	{ return m_nv.load(path, SELECT_CODE); }
	bool save_nv(const char *path) { return m_nv.save(path, SELECT_CODE); }
	bool nv_dirty() const { return m_nv.dirty; }
	rtc_nvm &nv() { return m_nv; }

	// The counterpart of FOCUS_RAMFILL for non-volatile storage.
	void fill_nvm(u8 v) { m_nv.fill_nvm(v); }

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
	void set_battery_fail(bool state) { m_nv.battery_fail = state; }

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
					| (m_nv.battery_fail_visible() ? (1u << 5) : 0);

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
			return m_nv.read_latch();

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
			// The address is written in complemented form (Service Manual
			// 2-30) -- and the inversion belongs to the decode, which
			// applies it on every access. Latching the complement HERE as
			// well inverted it twice, so this entry point disagreed with
			// the FR path every guest actually uses.
			m_nv.write_latch(data);
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
	// match the Service Manual's register allocations, 2-29/2-30). The
	// decode itself lives in focusrtc.h; what belongs here is which register
	// carries what:
	//
	//   W R12   RTC/NVM address latch. The address travels COMPLEMENTED
	//           (Service Manual 2-30) for the clock and the NVM alike: the
	//           driver writes NOT location before every digit and NOT
	//           address before every NVM byte
	//   W R9    RTC data for the latched location, COMPLEMENTED on the bus
	//   R R9    RTC data back, complemented likewise; four bits of it
	//   R R1    read before every exchange. Bit 7 is not a ready bit: it
	//           tells the driver WHICH clock part the board carries. Set, it
	//           takes the digit path this board implements (complemented
	//           addresses, one BCD digit a location, NVM through R13);
	//           clear, a byte-wide path with plain addresses that reads
	//           the map's clock offsets through R9 and the rest through R11
	//           (CORE SYSTEM +0x8814, +0x88b0). Before this answered 0x80
	//           the kernel spun 200,000 times in that other path, which is
	//           how bit 7 came to be called "ready".
	//   R/W R11 NVM data on the driver's OTHER path; with bit 7 set HP-UX
	//           never touches it, and it answers like R13 so that either
	//           path finds the same bytes
	//   R/W R13 NVM data byte
	//
	// WHAT IT ACTUALLY STORES. The clock chip holds the time of day, the day,
	// the day of week and the month -- as digits -- and has no year. The year
	// and the century are NVM bytes 9 and 5, and NVM byte 1 is the check
	// byte that says whether the clock was ever set: 240 set, 15 initialised
	// but never set, 14 set once and since found wrong. Anything but 240 is
	// escape 941, "CLOCK AND DATE NOT SET", and a software clock started
	// from zero -- 1 January 1900. See focusrtc.h, "THE DIGIT CLOCK". (An
	// earlier note here had the clock-set sweep latching "plain 0, 2, 4...":
	// that was the same driver's map-offset routine, which sends offsets 1, 5
	// and 9 to the NVM and the rest to the digit locations.)
	u32 read_fr(unsigned n) override
	{
		switch (n)
		{
		case 1:  return 0x80;                  // a digit clock
		case 2:  return read_register(2);
		case 3:  return read_register(3);
		case 4:  return read_register(4);
		case 6:  return read_register(6);
		case 9:  return m_nv.read_rtc();
		case 10: return m_service_word;
		// "Writing a 16-bit address to R12 in complemented form loads the
		// address of the NVM location" (Service Manual 2-30). The boot
		// bears it out: it sweeps R12 from ffff down through fff800, which
		// complemented is 0 through 07ff -- the whole 2 KB part, in order.
		case 11:
		case 13: return m_nv.read_nvm();
		case 12: return m_nv.read_latch();     // data-bus test: echo
		default: return m_scratch[n & 15];
		}
	}

	void write_fr(unsigned n, u32 v) override
	{
		v &= 0xffff;
		switch (n)
		{
		case 1:  write_register(1, v); break;
		case 9:  m_nv.write_rtc(v); break;
		case 10: m_service_word = v; break;
		case 11:
		case 13: m_nv.write_nvm(v); break;
		case 12: m_nv.write_latch(v); break;
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
	// The clock and the configuration memory, shared with the 520's keyboard
	// board (focusrtc.h). All-zero is "midnight, clock never set", which the
	// unset-clock path detects and initializes.
	rtc_nvm m_nv{rtc_decode::scm()};
	// Registers with no modelled behaviour remember what was written, as
	// the keyboard board's self test taught.
	u32 m_scratch[16] = {};
	u32 m_rom_addr = 0xffff;
	u32 m_service_word = 0;
	unsigned m_bytes_read = 0;
	bool m_rom_present = false;
	bool m_dma_enabled = false;
	bool m_self_test = false;
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
