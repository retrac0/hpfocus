// license:BSD-3-Clause
// copyright-holders:
/***************************************************************************

    HP 9000 Series 500 real-time clock and non-volatile memory

    The Series 500 puts the clock and the 2 KB of battery-backed CMOS on a
    different board depending on model:

      * on the 520 they are on the KEYBOARD ELECTRONICS BOARD, select code 6,
        alongside the key matrix, the beeper and the optional thermal printer
        (520 Service, "RTC/NVM Logic and Enable", sheet 2-33)
      * on the 530/540 they are on the SYSTEM CONTROL MODULE, select code 7,
        alongside the service panel and the loader ROM (530/540 Service,
        "Real-Time Clock (RTC) and Non-Volatile Memory (NVM)", sheet 2-29)

    Both boards present the same part through the same registers, and both
    manuals describe it in the same words: the RTC chip has sixteen four-bit
    locations behind a 32.768 kHz crystal, R12 latches an address, R9 moves
    RTC data and "succeeding reads or writes cause automatic increments of the
    address (address is complemented)"; the NVM chip has 2048 eight-bit bytes
    whose 16-bit address goes to R12 "in complemented form", data through R13.
    A NiCad assembly holds both for 30 days nominal and 10 days worst case,
    and a sample-and-hold at power-up reports a flat battery so the OS knows
    to restore what it finds.

    This header is that one part. Before it, each board carried its own copy
    of the arrays, its own second tick over a different set of cells, and its
    own address decode -- two expressions of one piece of hardware, with no
    way to see that they agreed.

    HOW WIDE IS A LOCATION? Both manuals say "16 four-bit locations" and in
    the same breath "a 4-bit multiplexed address/data bus provides I/O for the
    chip". A nibble cannot hold the 59 that HP-UX stores in a minute location
    or the 70 it stores in a year, and BASIC's driver undoes the bus inversion
    with a byte-wide NOT -- so the four bits modelled here are the BUS, and a
    location is a byte. HARDWARE.md 2.4 carries this as an open question; HP's
    own (7)KEYBOARD diagnostic (Rtc_update, Reset_time) is what settles it.

    No MAME dependency, like its siblings, so the standalone harnesses in
    src/test build against it unchanged.

***************************************************************************/

#ifndef MAME_IOP_FOCUSRTC_H
#define MAME_IOP_FOCUSRTC_H

#pragma once

#include "../cpu/focus/focusstate.h"

#include <cstdio>

namespace focus {

//**************************************************************************
//  THE 64-BYTE CONTENTS MAP
//**************************************************************************

// One table, printed identically in the 520 Service manual (sheet 3-16), the
// 530/540 Service manual (sheet 3-19) and the 9030/40 CE Handbook (chapter
// 10), under one heading: "Real-Time Clock/Non-Volatile Memory Contents".
// Entries 0-15 are the RTC's sixteen locations; 16-63 are NVM bytes.
//
// The running system keeps BOTH copies of the low sixteen. A battery file
// written by HP-UX 5.30 has the clock in the RTC part and, at the same map
// offsets in the NVM part, clock check 14, stack size 6, century 19 and year
// 70. That mirroring is why one numbering serves both parts, and why this
// lives in one place rather than once per board.
namespace nvloc {

inline constexpr unsigned SECOND           = 0;
inline constexpr unsigned CLOCK_CHECK      = 1;
inline constexpr unsigned MINUTE           = 2;
inline constexpr unsigned STACK_SIZE       = 3;   // two times the board count
inline constexpr unsigned HOUR             = 4;
inline constexpr unsigned CENTURY          = 5;
inline constexpr unsigned DAY_OF_WEEK      = 6;
inline constexpr unsigned DAY_OF_MONTH     = 7;
inline constexpr unsigned MONTH            = 8;
inline constexpr unsigned YEAR             = 9;
inline constexpr unsigned CONTROL          = 10;  // 10-13
inline constexpr unsigned CONTROL_N        = 4;
inline constexpr unsigned TIMER_RES        = 14;
inline constexpr unsigned TIMEOUT_SCAN_RES = 15;
// -- from here on the NVM part ------------------------------------------
inline constexpr unsigned KBD_INIT         = 16;
inline constexpr unsigned STARTUP_SUBSYS   = 17;  // 17-24
inline constexpr unsigned STARTUP_SUBSYS_N = 8;
inline constexpr unsigned DEFAULT_MSUS     = 25;  // 25-44
inline constexpr unsigned DEFAULT_MSUS_N   = 20;
inline constexpr unsigned IO_BUFFER_SIZE   = 45;
inline constexpr unsigned CHECKSUM         = 46;  // 46-47
inline constexpr unsigned CHECKSUM_N       = 2;
inline constexpr unsigned HEALER_CAM       = 48;  // 48-53
inline constexpr unsigned HEALER_CAM_N     = 6;
inline constexpr unsigned LAST_DOUBLE_BIT  = 54;  // 54-57
inline constexpr unsigned LAST_DOUBLE_BIT_N = 4;
inline constexpr unsigned MINUTES_ON       = 58;  // 58-60, ten-minute periods
inline constexpr unsigned MINUTES_ON_N     = 3;
inline constexpr unsigned OVERHEAT_CYCLES  = 61;
inline constexpr unsigned POWER_ON_CYCLES  = 62;  // 62-63
inline constexpr unsigned POWER_ON_CYCLES_N = 2;

inline constexpr unsigned MAP_SIZE         = 64;
inline constexpr unsigned RTC_LOCATIONS    = 16;  // what the chip really has

} // namespace nvloc

//**************************************************************************
//  ADDRESS DECODE
//**************************************************************************

// Reduced to essentials, the two boards differ in EXACTLY TWO ways, and
// naming them here rather than keeping two copies of the code is the point:
//
//                         SCM (SC7)        keyboard board (SC6)
//   RTC address           complemented     plain
//   RTC location mask     15               63
//   RTC data              complemented     complemented
//   RTC auto-step         decoded loc +1   decoded loc +1
//   NVM address           complemented     complemented
//   NVM data              plain            plain
//
// Neither difference is documented -- the manuals describe one part -- but
// both are observed and load-bearing: HP-UX's clock-set sweep on SC7 latches
// what look like plain ascending locations and its clock-check path latches
// ffff, fff1..., and BASIC's console open on SC6 latches 0, 2, 4, 7, 6, 8, 9,
// 5, 1, which under a plain decode is exactly the map above, read in the
// order second, minute, hour, day, day-of-week, month, year, century, check.
// Keeping them as fields makes each an assertion that can be swept rather
// than an accident buried in a switch.
struct rtc_decode
{
	bool     rtc_addr_complement = true;
	unsigned rtc_addr_mask       = 15;
	bool     has_day_of_week     = false;

	// The wrap constants the two boards' second ticks used before there was
	// a calendar here. They differ -- the SCM wraps the day at 32 and the
	// month at 13 with a floor of 1, because HP-UX's date validator rejects
	// a zero of either, while the keyboard board wraps at 31 and 12 with a
	// floor of 0 -- and they are kept only so this refactor changes nothing.
	// A real calendar replaces them.
	u8 day_wrap    = 32, day_floor   = 1;
	u8 month_wrap  = 13, month_floor = 1;

	static rtc_decode scm()
	{
		return rtc_decode{};
	}

	static rtc_decode keyboard()
	{
		rtc_decode d;
		d.rtc_addr_complement = false;
		d.rtc_addr_mask       = 63;
		d.has_day_of_week     = true;
		d.day_wrap = 31; d.day_floor = 0;
		d.month_wrap = 12; d.month_floor = 0;
		return d;
	}
};

//**************************************************************************
//  THE PART
//**************************************************************************

struct rtc_nvm
{
	// The chip has sixteen locations; 64 cells are stored because the
	// keyboard board decodes six address bits and the OS is free to use
	// them, and because one file format then serves both boards.
	static constexpr unsigned RTC_CELLS = 64;
	static constexpr unsigned NVM_SIZE  = 2048;

	explicit rtc_nvm(const rtc_decode &decode = rtc_decode::scm()) : d(decode) {}

	rtc_decode d;
	u8  rtc[RTC_CELLS] = {};
	u8  nvm[NVM_SIZE]  = {};
	u32 latch = 0;
	bool battery_fail = false;

	//----------------------------------------------------------------
	//  the map
	//----------------------------------------------------------------

	// Where a map offset lives in storage under this board's decode. The two
	// boards' second ticks used to name their cells directly -- 15, 13, 11,
	// 8, 7, 6 on the SCM and 0, 2, 4, 6, 7, 8, 9 on the keyboard board -- and
	// through this they are the same offsets: second, minute, hour, day of
	// month, month, year, plus the keyboard board's day of week. The two
	// boards always agreed on the map; the code just wrote it twice, in two
	// coordinate systems.
	unsigned index_for(unsigned map_offset) const
	{
		return d.rtc_addr_complement
				? ((~map_offset) & d.rtc_addr_mask)
				: (map_offset & d.rtc_addr_mask);
	}

	u8   at(unsigned map_offset) const { return rtc[index_for(map_offset)]; }
	void put(unsigned map_offset, u8 v) { rtc[index_for(map_offset)] = v; }

	//----------------------------------------------------------------
	//  the register plumbing behind R9 / R11 / R12 / R13
	//----------------------------------------------------------------

	// R12 write. The address arrives complemented on the bus (both manuals);
	// the inversion belongs to the decode, so what is latched is what was
	// written.
	void write_latch(u32 v) { latch = v & 0xffff; }
	u32  read_latch() const { return latch; }

	unsigned rtc_index() const
	{
		return d.rtc_addr_complement ? ((~latch) & d.rtc_addr_mask)
									 : (latch & d.rtc_addr_mask);
	}

	unsigned nvm_index() const { return (~latch) & (NVM_SIZE - 1); }

	// "Once the initial address is accessed, succeeding reads or writes cause
	// automatic increments of the address (address is complemented). This
	// enables blocks of data to be read or written without updating the
	// address from the IOP." The DECODED location is what increments, so a
	// complemented latch counts down and a plain one counts up.
	void step()
	{
		const u32 m = d.rtc_addr_mask;
		const u32 next = d.rtc_addr_complement ? (latch - 1) : (latch + 1);
		latch = (latch & ~m) | (next & m);
	}

	// R9. Data is complemented on the bus in both directions; BASIC's driver
	// undoes it with a NOT (trap seg 0x48072).
	u8 read_rtc()
	{
		const u8 v = u8(~rtc[rtc_index()]);
		step();
		return v;
	}

	void write_rtc(u32 v)
	{
		rtc[rtc_index()] = u8(~v);
		step();
	}

	// R13, and R11 alongside it on the SCM. The address is complemented and
	// the data is not, and neither board steps the latch for NVM.
	u8   read_nvm() const { return nvm[nvm_index()]; }
	void write_nvm(u32 v) { nvm[nvm_index()] = u8(v); }

	//----------------------------------------------------------------
	//  the clock
	//----------------------------------------------------------------

	// One second of wall time. The boot synchronises its software clock to
	// the next second boundary by polling the seconds location between 100 ms
	// sleeps (trap seg 0x476ec..0x47798), so a frozen clock parks it there
	// forever. Values count in binary: the boot's clock-sync completed with a
	// binary-counting seconds field, and a BCD experiment stalled it.
	void tick_second()
	{
		auto bump = [this](unsigned off, u8 wrap, u8 floor) -> bool {
			const unsigned i = index_for(off);
			rtc[i] = u8(rtc[i] + 1);
			if (rtc[i] < wrap)
				return false;
			rtc[i] = floor;
			return true;
		};
		if (!bump(nvloc::SECOND, 60, 0)) return;
		if (!bump(nvloc::MINUTE, 60, 0)) return;
		if (!bump(nvloc::HOUR, 24, 0)) return;
		if (d.has_day_of_week)
		{
			const unsigned i = index_for(nvloc::DAY_OF_WEEK);
			rtc[i] = u8((rtc[i] + 1) % 7);
		}
		if (!bump(nvloc::DAY_OF_MONTH, d.day_wrap, d.day_floor)) return;
		if (!bump(nvloc::MONTH, d.month_wrap, d.month_floor)) return;
		bump(nvloc::YEAR, 100, 0);
	}

	//----------------------------------------------------------------
	//  housekeeping
	//----------------------------------------------------------------

	// A reset is not a power cycle: the contents survive, which is what the
	// battery is for. Only the run state goes.
	void power_on_reset()
	{
		latch = 0;
		battery_fail = false;
	}

	void fill_rtc(u8 v) { for (u8 &b : rtc) b = v; }
	void fill_nvm(u8 v) { for (u8 &b : nvm) b = v; }

	bool battery_fail_visible() const { return battery_fail; }

	//----------------------------------------------------------------
	//  the battery
	//----------------------------------------------------------------

	// The contents survive power off for ten days at least, and that
	// persistence is load-bearing: the first boot's clock-not-set path SETS
	// the clock and writes a default configuration into the NVM, and only a
	// later boot, finding them already valid, takes the configured paths. A
	// file stands in for the battery.
	bool load(const char *path)
	{
		FILE *f = std::fopen(path, "rb");
		if (!f)
			return false;
		const bool ok = std::fread(rtc, 1, sizeof rtc, f) == sizeof rtc
				&& std::fread(nvm, 1, sizeof nvm, f) == sizeof nvm;
		std::fclose(f);
		return ok;
	}

	bool save(const char *path) const
	{
		FILE *f = std::fopen(path, "wb");
		if (!f)
			return false;
		const bool ok = std::fwrite(rtc, 1, sizeof rtc, f) == sizeof rtc
				&& std::fwrite(nvm, 1, sizeof nvm, f) == sizeof nvm;
		std::fclose(f);
		return ok;
	}
};

} // namespace focus

#endif // MAME_IOP_FOCUSRTC_H
