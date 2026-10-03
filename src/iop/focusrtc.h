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

    HOW WIDE IS A LOCATION? Four bits, as both manuals say: "the RTC chip
    stores clock and calendar data in 16 four-bit locations". This used to be
    modelled as a byte per location, on the reading that a nibble cannot hold
    the 59 HP-UX stores in a minute -- but HP-UX does not store a 59 anywhere.
    Its clock driver (and BASIC's, and HP-UX 5.01's on the 520) reads R1 of
    the board first and, with bit 7 set -- which both boards answer -- keeps
    ONE BCD DIGIT per location: the units of a field at one location and the
    tens at the next. The chip has no year; the OS keeps that in the NVM. See
    "THE DIGIT CLOCK" below, which is what the guests' own code says.

    No MAME dependency, like its siblings, so the standalone harnesses in
    src/test build against it unchanged.

***************************************************************************/

#ifndef MAME_IOP_FOCUSRTC_H
#define MAME_IOP_FOCUSRTC_H

#pragma once

#include "../cpu/focus/focusstate.h"

#include <cstdio>
#include <cstring>
#include <ctime>

namespace focus {

//**************************************************************************
//  THE 64-BYTE CONTENTS MAP
//**************************************************************************

// One table, printed identically in the 520 Service manual (sheet 3-16), the
// 530/540 Service manual (sheet 3-19) and the 9030/40 CE Handbook (chapter
// 10), under one heading: "Real-Time Clock/Non-Volatile Memory Contents".
// The table reads as if entries 0-15 were the RTC's sixteen locations and
// 16-63 NVM bytes -- and that is the layout of a byte-wide clock with RAM
// beside it, the kind of part the guests' drivers still carry a path for
// (R1 bit 7 clear; see "THE DIGIT CLOCK"). On the boards both manuals
// describe, and both boards here answer for, the clock is a digit chip and
// EVERY offset of this map is an NVM byte: the ones the clock fills in
// (second, minute, hour, days, month) are simply unused there, while the
// check byte, the century and the year at 1, 5 and 9 are what the OS reads
// alongside the chip. A battery file written by HP-UX 5.30 holds exactly
// that: check 14, stack size 6, century 19, year 70 at those NVM offsets.
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
//  THE CALENDAR
//**************************************************************************

// A moment, kept apart from the storage so the arithmetic can be tested
// without a board. The map splits the year across two locations -- century
// at offset 5 and year at 9 -- so 1985 is century 19, year 85, which is
// exactly what a battery file written by HP-UX holds (19 and 70).
struct calendar
{
	int year   = 0;      // full, e.g. 1985; 0 with month 0 means "never set"
	u8  month  = 0;      // 1-12
	u8  day    = 0;      // 1-31
	u8  hour   = 0, minute = 0, second = 0;
	u8  weekday = 0;     // 0 = Sunday; derived, never trusted from storage

	static bool leap(int y)
	{ return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

	// The length of a month, and 31 for a month number that is not one --
	// an unset clock reads month 0, and it still has to count.
	static u8 month_length(int y, unsigned m)
	{
		static const u8 len[12] =
				{ 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
		if (m < 1 || m > 12)
			return 31;
		return (m == 2 && leap(y)) ? 29 : len[m - 1];
	}

	bool valid() const
	{
		return month >= 1 && month <= 12
				&& day >= 1 && day <= month_length(year, month)
				&& hour < 24 && minute < 60 && second < 60;
	}

	// Sakamoto's method, proleptic Gregorian. 0 = Sunday.
	u8 day_of_week() const
	{
		static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
		if (month < 1 || month > 12)
			return 0;
		const int y = year - (month < 3 ? 1 : 0);
		return u8((((y + y / 4 - y / 100 + y / 400 + t[month - 1] + day) % 7)
				+ 7) % 7);
	}

	// Broken-down UTC from a Unix timestamp, and back. std::gmtime would do
	// it, but this header is used by harnesses that want no locale and no
	// static buffer, and the arithmetic is four lines either way.
	static calendar from_epoch(long long t)
	{
		calendar c;
		long long days = t / 86400, rem = t % 86400;
		if (rem < 0) { rem += 86400; days--; }
		c.hour = u8(rem / 3600);
		c.minute = u8((rem / 60) % 60);
		c.second = u8(rem % 60);
		c.year = 1970; c.month = 1; c.day = 1;
		while (days < 0 || days >= (leap(c.year) ? 366 : 365))
		{
			const int len = leap(c.year) ? 366 : 365;
			if (days < 0) { c.year--; days += leap(c.year) ? 366 : 365; }
			else          { days -= len; c.year++; }
		}
		while (days >= month_length(c.year, c.month))
		{
			days -= month_length(c.year, c.month);
			c.month++;
		}
		c.day = u8(days + 1);
		c.weekday = c.day_of_week();
		return c;
	}

	long long to_epoch() const
	{
		long long days = 0;
		for (int y = 1970; y < year; y++) days += leap(y) ? 366 : 365;
		for (int y = year; y < 1970; y++) days -= leap(y) ? 366 : 365;
		for (unsigned m = 1; m < month; m++) days += month_length(year, m);
		days += (day ? day : 1) - 1;
		return days * 86400 + hour * 3600LL + minute * 60LL + second;
	}

	// One second on. The date fields roll by real month lengths and real
	// leap years -- the two boards used to wrap the day at 31 or 32 and the
	// month at 12 or 13, which put 31 February on the clock every year.
	void advance(long long seconds)
	{
		long long s = second + 60LL * minute + 3600LL * hour + seconds;
		long long days = s / 86400;
		s %= 86400;
		if (s < 0) { s += 86400; days--; }
		hour = u8(s / 3600); minute = u8((s / 60) % 60); second = u8(s % 60);
		if (day < 1) day = 1;
		if (month < 1 || month > 12) month = 1;
		while (days-- > 0)
		{
			if (day < month_length(year, month)) { day++; continue; }
			day = 1;
			if (++month > 12) { month = 1; year++; }
		}
		weekday = day_of_week();
	}
};

//**************************************************************************
//  ADDRESS DECODE
//**************************************************************************

// The manuals describe one part on both boards, and the guests agree:
//
//                         SCM (SC7)        keyboard board (SC6)
//   RTC address           complemented     complemented
//   RTC location mask     15               15
//   RTC location          one BCD digit    one BCD digit
//   RTC data              complemented     complemented
//   RTC auto-step         decoded loc +1   decoded loc +1
//   NVM address           complemented     complemented
//   NVM data              plain            plain
//
// The keyboard board used to be decoded plainly over six bits with a byte
// per location, on the strength of an early BASIC trace (0, 2, 4, 7, 6, 8, 9,
// 5, 1 -- the map above, read as a byte clock). Neither BASIC nor HP-UX 5.01
// does that today: on SC6, with R1 bit 7 set, both latch ffff, fffe, fffd...
// -- the complement of locations 0, 1, 2 -- and mask what comes back to four
// bits, exactly as HP-UX 5.30 does on SC7. Under the plain decode those reads
// landed on cells 63, 62, 61, which nothing ever wrote, so both systems on
// the 520 reported "CLOCK AND DATE NOT SET" whatever the battery held. The
// byte path (`digits` false) is kept because it is the other half of the
// drivers' R1 bit 7 test, and a board that answers bit 7 clear would want it.
struct rtc_decode
{
	bool     rtc_addr_complement = true;
	unsigned rtc_addr_mask       = 15;
	// Whether a location is a four-bit BCD digit of the clock described in
	// "THE DIGIT CLOCK" below, or a byte holding a whole binary field.
	bool     digits              = true;

	static rtc_decode scm()
	{
		return rtc_decode{};
	}

	static rtc_decode keyboard()
	{
		return rtc_decode{};
	}

	// The byte-wide clock the drivers take when R1 bit 7 is clear: plain
	// addresses over the whole 64-byte map. No board here answers that way.
	static rtc_decode byte_clock()
	{
		rtc_decode d;
		d.rtc_addr_complement = false;
		d.rtc_addr_mask       = 63;
		d.digits              = false;
		return d;
	}
};

//**************************************************************************
//  THE DIGIT CLOCK
//**************************************************************************

// "The RTC chip stores clock and calendar data in 16 four-bit locations"
// (520 Service 3-16; 530/540 Service 2-29). HP-UX 5.30's clock driver (CORE
// SYSTEM +0x8814..+0x8e98, behind its local STT 29/30/31 and 1) reads R1 of
// the board first; with bit 7 set it treats every location as ONE DIGIT:
//
//     W12 <- NOT location ; R9 -> NOT, AND 15          (STT 31)
//
// and reads a two-digit field as the units at one location and the tens at
// the NEXT without re-latching -- the manual's auto-increment (STT 1), with
// the tenths location read before and after, a change between the two being
// escape 941 and a retry. The locations, read off that code:
//
//      0     test                    written 0 before a set
//      1     tenths of seconds       the rollover guard above
//      2,3   seconds, units, tens    never written: a stop zeroes them
//      4,5   minutes
//      6,7   hours, 24-hour
//      8,9   day of month
//     10     day of week             1 = Monday ... 7 = Sunday
//     11,12  month
//     13     leap-year status        8 on a leap year, then 1, 2, 4
//     14     stop/start              0 to stop, 1 to run
//     15     interrupt control
//
// That is National's MM58174A register for register -- an identification
// from the layout, not from a parts list. It has NO YEAR. The year and the
// century live in the NVM at the contents map's offsets 9 and 5, and the
// "Clock Check Byte" at offset 1 says whether to believe any of it: HP-UX
// writes 240 (0xF0) there when it sets the clock, 15 when it initialises a
// blank NVM, 14 when a set clock fails its checks, and raises escape 941 --
// "CLOCK AND DATE NOT SET", and a software clock started at 1 January 1900,
// which `date` shows as February 2036 -- for anything but 240. It checks the
// day of week against the date, and a mismatch that the NEXT year's calendar
// explains is how it notices that the chip rolled over a new year.
//
// BASIC and HP-UX 5.01 drive SC6 through the same sequence (ffff, fffe,
// fffd... on R12; their helper at trap seg 0x48066 branches on the same R1
// bit), so the keyboard board carries the same chip.
//
// The emulator used to keep binary bytes -- 0..59 in one cell -- at the map's
// offsets: on the SCM seconds in cell 15, minutes in 13, hours in 11. None of
// those is where HP-UX looks, so the time it read never moved, and cell 11 is
// its month-units digit, so every emulated hour moved the month on by one.
namespace rtcdigit {

inline constexpr unsigned TEST    = 0;
inline constexpr unsigned TENTHS  = 1;
inline constexpr unsigned SECOND  = 2;    // units; the tens are one further on
inline constexpr unsigned MINUTE  = 4;
inline constexpr unsigned HOUR    = 6;
inline constexpr unsigned DAY     = 8;
inline constexpr unsigned WEEKDAY = 10;   // 1 = Monday ... 7 = Sunday
inline constexpr unsigned MONTH   = 11;
inline constexpr unsigned LEAP    = 13;
inline constexpr unsigned RUN     = 14;
inline constexpr unsigned IRQ     = 15;

inline constexpr u8 LEAP_YEAR     = 8;
// The clock check byte, NVM offset 1, as HP-UX 5.30 keeps it.
inline constexpr u8 CHECK_SET     = 0xf0; // the clock has been set
inline constexpr u8 CHECK_INIT    = 15;   // NVM initialised, clock never set
inline constexpr u8 CHECK_FAILED  = 14;   // a set clock failed its checks

// The leap status the OS writes for a year: one bit, walking up to 8 on the
// leap year itself (CORE SYSTEM +0x8cc2: 1 << ((year - 1) mod 4), with the
// century years that are not leap years forced to the bottom).
inline u8 leap_status(int year)
{
	unsigned r = unsigned(((year - 1) % 4 + 4) % 4);
	if (year % 100 == 0 && (year / 100) % 4 != 0)
		r = 0;
	return u8(1u << r);
}

} // namespace rtcdigit

//**************************************************************************
//  THE BATTERY FILE
//**************************************************************************

// The contents survive power off for ten days at least (30 nominal), and
// that persistence is load-bearing: the first boot's clock-not-set path SETS
// the clock and writes a default configuration into the NVM, and only a
// later boot, finding them already valid, takes the configured paths. A file
// stands in for the battery.
//
// The first version of this wrote the two arrays raw and read them back the
// same way. That is fine until the file is short, at which point the first
// fread has already overwritten the clock and the second fails -- a half
// loaded battery, reported by a bool the caller ignored. So the file carries
// a header now, the load is all-or-nothing, and the save goes through a
// temporary and a rename so a run killed mid-write cannot leave a torn one.
//
//   off  size  contents
//     0     8  magic "FOCUSNV\0"
//     8     2  format version, little-endian
//    10     1  the select code the file came from, 6 or 7
//    11     1  flags: bit 0 = the battery was good when this was saved
//    12     2  RTC cell count
//    14     2  reserved
//    16     4  NVM size
//    20     8  save time, Unix epoch seconds -- the battery's reference for
//              how long the machine was off
//    28     4  CRC-32 of the payload
//    32  2112  payload: the RTC cells, then the NVM
//
// A file of exactly 2112 bytes that does not start with the magic is one of
// the old headerless ones and is read as the payload alone.

enum class nv_error
{
	none, no_file, unreadable, bad_size, bad_version, bad_crc,
};

inline const char *nv_error_text(nv_error e)
{
	switch (e)
	{
	case nv_error::none:        return "no error";
	case nv_error::no_file:     return "no such file";
	case nv_error::unreadable:  return "cannot be read";
	case nv_error::bad_size:    return "not the size of a battery file";
	case nv_error::bad_version: return "written by a newer format";
	case nv_error::bad_crc:     return "checksum mismatch, contents corrupt";
	}
	return "unknown";
}

struct nv_load_result
{
	bool      ok       = false;
	nv_error  error    = nv_error::no_file;
	bool      legacy   = false;   // headerless, from before the format
	long long saved_at = 0;       // Unix epoch seconds; 0 when unknown
	unsigned  board    = 0;       // the select code the file claims
	long long off_seconds = 0;    // how long the battery had to hold
	bool      battery_expired = false;
};

// CRC-32, the zip and PNG polynomial, table-less so this header keeps its
// short dependency list.
inline u32 nv_crc32(const u8 *p, unsigned n)
{
	u32 c = 0xffffffffu;
	for (unsigned i = 0; i < n; i++)
	{
		c ^= p[i];
		for (int k = 0; k < 8; k++)
			c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
	}
	return ~c;
}

namespace nvfile {

inline constexpr char     MAGIC[8]    = { 'F','O','C','U','S','N','V','\0' };
inline constexpr unsigned HEADER      = 32;
inline constexpr unsigned VERSION     = 1;
inline constexpr unsigned RTC_CELLS   = 64;
inline constexpr unsigned NVM_SIZE    = 2048;
inline constexpr unsigned PAYLOAD     = RTC_CELLS + NVM_SIZE;   // 2112
inline constexpr unsigned SIZE        = HEADER + PAYLOAD;       // 2144

// Little-endian by hand, so a battery written on one host reads on another.
inline void put_le(u8 *p, unsigned long long v, unsigned n)
{ for (unsigned i = 0; i < n; i++) p[i] = u8(v >> (8 * i)); }

inline unsigned long long get_le(const u8 *p, unsigned n)
{
	unsigned long long v = 0;
	for (unsigned i = 0; i < n; i++) v |= (unsigned long long)(p[i]) << (8 * i);
	return v;
}

} // namespace nvfile

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
	// Whether the battery-fail bit self-clears. "This bit must be read
	// within four seconds of powerup" (530/540 Service 2-30, 520 Service
	// 2-34) -- so on real hardware it is a sample-and-hold that goes away.
	// The emulator has always latched it for the whole run; FOCUS_RTCWINDOW
	// turns the real behaviour on.
	bool battery_window = false;
	unsigned seconds_since_power_on = 0;
	// Whether anything has been written since the last save, so a periodic
	// write-back can skip a part nobody has touched.
	bool dirty = false;

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

	// R9. Data is complemented on the bus in both directions; the guests undo
	// it with a NOT. A digit location is four bits wide, so only those come
	// back; the drivers mask the rest off anyway (NOT; ANDI #15).
	u8 read_rtc()
	{
		const u8 cell = d.digits ? u8(rtc[rtc_index()] & 15) : rtc[rtc_index()];
		const u8 v = u8(~cell);
		step();
		return v;
	}

	void write_rtc(u32 v)
	{
		if (d.digits)
			write_digit(rtc_index(), u8(~v) & 15);
		else
			rtc[rtc_index()] = u8(~v);
		dirty = true;
		step();
	}

	// R13, and R11 alongside it on the SCM. The address is complemented and
	// the data is not, and neither board steps the latch for NVM.
	u8   read_nvm() const { return nvm[nvm_index()]; }
	void write_nvm(u32 v) { nvm[nvm_index()] = u8(v); dirty = true; }

	//----------------------------------------------------------------
	//  the digit clock (rtc_decode::digits)
	//----------------------------------------------------------------

	// A register of the digit clock lives in the cell of the same number:
	// the OS latches the COMPLEMENT of the register it wants and the decode
	// undoes it.
	unsigned pair(unsigned reg) const
	{
		return (rtc[reg] & 15u) + 10u * (rtc[reg + 1] & 15u);
	}

	void put_pair(unsigned reg, unsigned v)
	{
		rtc[reg]     = u8(v % 10);
		rtc[reg + 1] = u8((v / 10) % 10);
	}

	bool running() const { return (rtc[rtcdigit::RUN] & 1) != 0; }

	// A write from the bus. The counters below the minutes cannot be
	// written: the OS never tries, and its set sequence relies on the stop
	// (location 14 written 0) to zero them -- it stops the chip, writes the
	// fields, and starts it again on a whole minute.
	void write_digit(unsigned reg, u8 v)
	{
		switch (reg)
		{
		case rtcdigit::TENTHS:
		case rtcdigit::SECOND:
		case rtcdigit::SECOND + 1:
			return;
		case rtcdigit::RUN:
			rtc[reg] = v & 1;
			if (!(v & 1))
				rtc[rtcdigit::TENTHS] = rtc[rtcdigit::SECOND]
						= rtc[rtcdigit::SECOND + 1] = 0;
			return;
		default:
			rtc[reg] = v;
			return;
		}
	}

	// One second on a running digit clock: BCD, real month lengths (the
	// leap status says whether February has 29), the day of week alongside
	// the date, and at the year's end the leap status moves on. The YEAR
	// does not, because the chip has none: the OS notices the new year from
	// the day of week and writes it to the NVM itself.
	void tick_digits()
	{
		if (!running())
			return;
		dirty = true;
		auto bump = [this](unsigned reg, unsigned wrap) -> bool {
			const unsigned v = pair(reg) + 1;
			put_pair(reg, v < wrap ? v : 0);
			return v >= wrap;
		};
		if (!bump(rtcdigit::SECOND, 60)) return;
		if (!bump(rtcdigit::MINUTE, 60)) return;
		if (!bump(rtcdigit::HOUR, 24)) return;
		const unsigned wd = rtc[rtcdigit::WEEKDAY] & 15u;
		rtc[rtcdigit::WEEKDAY] = u8((wd >= 1 && wd < 7) ? wd + 1 : 1);
		const unsigned month = pair(rtcdigit::MONTH);
		const bool leap = (rtc[rtcdigit::LEAP] & rtcdigit::LEAP_YEAR) != 0;
		const unsigned len = calendar::month_length(leap ? 2000 : 2001, month);
		const unsigned day = pair(rtcdigit::DAY);
		if (day >= 1 && day < len)
		{
			put_pair(rtcdigit::DAY, day + 1);
			return;
		}
		put_pair(rtcdigit::DAY, 1);
		if (month >= 1 && month < 12)
		{
			put_pair(rtcdigit::MONTH, month + 1);
			return;
		}
		put_pair(rtcdigit::MONTH, 1);
		const u8 ls = rtc[rtcdigit::LEAP] & 15;
		rtc[rtcdigit::LEAP] = u8((ls & 8) ? 1 : ((ls << 1) & 15));
	}

	// The moment into the chip's fields, the day of week and leap status
	// that go with it, and the year and century into the NVM where the OS
	// keeps them. The clock is left running.
	void put_digits(const calendar &c)
	{
		rtc[rtcdigit::TEST] = 0;
		rtc[rtcdigit::TENTHS] = 0;
		put_pair(rtcdigit::SECOND, c.second);
		put_pair(rtcdigit::MINUTE, c.minute);
		put_pair(rtcdigit::HOUR,   c.hour);
		put_pair(rtcdigit::DAY,    c.day);
		put_pair(rtcdigit::MONTH,  c.month);
		const u8 wd = c.day_of_week();               // 0 = Sunday
		rtc[rtcdigit::WEEKDAY] = u8(wd ? wd : 7);    // 1 = Monday
		rtc[rtcdigit::LEAP] = rtcdigit::leap_status(c.year);
		rtc[rtcdigit::RUN] = 1;
		nvm[nvloc::CENTURY] = u8(c.year / 100);
		nvm[nvloc::YEAR]    = u8(c.year % 100);
		dirty = true;
	}

	//----------------------------------------------------------------
	//  the clock
	//----------------------------------------------------------------

	// What the clock reads. On the digit clock the year comes from the NVM,
	// as it does for the OS; on the byte clock the map splits it across two
	// locations, so a battery holding 19 and 70 reads as 1970.
	calendar time() const
	{
		calendar c;
		if (d.digits)
		{
			c.year    = int(nvm[nvloc::CENTURY]) * 100 + int(nvm[nvloc::YEAR]);
			c.month   = u8(pair(rtcdigit::MONTH));
			c.day     = u8(pair(rtcdigit::DAY));
			c.hour    = u8(pair(rtcdigit::HOUR));
			c.minute  = u8(pair(rtcdigit::MINUTE));
			c.second  = u8(pair(rtcdigit::SECOND));
			c.weekday = u8((rtc[rtcdigit::WEEKDAY] & 15u) % 7);  // 7 = Sunday
			return c;
		}
		c.year    = int(at(nvloc::CENTURY)) * 100 + int(at(nvloc::YEAR));
		c.month   = at(nvloc::MONTH);
		c.day     = at(nvloc::DAY_OF_MONTH);
		c.hour    = at(nvloc::HOUR);
		c.minute  = at(nvloc::MINUTE);
		c.second  = at(nvloc::SECOND);
		c.weekday = at(nvloc::DAY_OF_WEEK);
		return c;
	}

	// Set the clock the way the operator setting it leaves it. On the digit
	// clock that is what the OS's own set sequence writes (CORE SYSTEM
	// +0x8c08..+0x8d2a, read off the code): the fields, the day of week it
	// checks the date against, the leap status, the clock started, the year
	// and century into the NVM -- and 240 into the clock check byte at NVM
	// offset 1, without which HP-UX believes none of it.
	void set_time(const calendar &c)
	{
		if (d.digits)
		{
			put_digits(c);
			nvm[nvloc::CLOCK_CHECK] = rtcdigit::CHECK_SET;
			return;
		}
		put(nvloc::CENTURY,      u8(c.year / 100));
		put(nvloc::YEAR,         u8(c.year % 100));
		put(nvloc::MONTH,        c.month);
		put(nvloc::DAY_OF_MONTH, c.day);
		put(nvloc::HOUR,         c.hour);
		put(nvloc::MINUTE,       c.minute);
		put(nvloc::SECOND,       c.second);
		put(nvloc::DAY_OF_WEEK,  c.day_of_week());
		dirty = true;
	}

	// The same moment into the NVM's copy of the low sixteen, for the byte
	// clock, whose map has the OS keep one there too. The digit clock's
	// set_time() has already put everything the OS reads where it reads it.
	void set_time_mirror(const calendar &c)
	{
		if (d.digits)
			return;
		nvm[nvloc::CENTURY]      = u8(c.year / 100);
		nvm[nvloc::YEAR]         = u8(c.year % 100);
		nvm[nvloc::MONTH]        = c.month;
		nvm[nvloc::DAY_OF_MONTH] = c.day;
		nvm[nvloc::HOUR]         = c.hour;
		nvm[nvloc::MINUTE]       = c.minute;
		nvm[nvloc::SECOND]       = c.second;
		nvm[nvloc::DAY_OF_WEEK]  = c.day_of_week();
		dirty = true;
	}

	// Whether the OS will believe the clock: on the digit clock, a running
	// chip holding a real date and the check byte HP-UX writes when it sets
	// it. A blank battery is "never set" on both, and HP-UX reports escape
	// 941 and BASIC "CLOCK AND DATE NOT SET".
	bool clock_set() const
	{
		if (d.digits)
			return running() && time().valid()
					&& nvm[nvloc::CLOCK_CHECK] == rtcdigit::CHECK_SET;
		return time().valid();
	}

	// HP-UX 5.30's checksum over the NVM (CORE SYSTEM +0x8ee0): one, plus
	// the stack size at 3, plus every byte from 14 up except the checksum's
	// own two at 46 and 47, modulo 16384, kept high byte first at 46. A
	// mismatch at boot makes the kernel rewrite the whole part with its
	// defaults -- the clock check byte included, so a seeded clock does not
	// survive a boot on a battery HP-UX has never initialised. Checked
	// against both battery files HP-UX wrote here (stored 189 and 8).
	bool hpux_checksum_ok() const
	{
		unsigned sum = 1 + nvm[nvloc::STACK_SIZE];
		for (unsigned a = 14; a < NVM_SIZE; a++)
			if (a != nvloc::CHECKSUM && a != nvloc::CHECKSUM + 1)
				sum += nvm[a];
		return (sum % 16384) == unsigned(nvm[nvloc::CHECKSUM]) * 256
				+ nvm[nvloc::CHECKSUM + 1];
	}

	// One second of wall time. A digit clock counts only while it runs --
	// HP-UX stops it to set it and starts it on the minute -- and one that
	// has never been started (a blank battery) stands still, which the
	// drivers that read it through the digit path cope with: they find the
	// check byte wrong and never wait on the seconds.
	//
	// The byte clock's seconds, minutes and hours are bumped in place, and
	// count even unset: a driver on that path synchronises to the next
	// second boundary by polling the seconds location between 100 ms sleeps
	// (trap seg 0x476ec..0x47798), so a frozen clock parks it there forever.
	// Only a day rollover pays for the date arithmetic -- and it is real
	// arithmetic now: the two boards used to wrap the day at 31 or 32 and
	// the month at 12 or 13, so February had 31 days on one and the month
	// could reach 0 on the other.
	void tick_second()
	{
		if (seconds_since_power_on < ~0u)
			seconds_since_power_on++;
		if (d.digits)
		{
			tick_digits();
			return;
		}
		auto bump = [this](unsigned off, u8 wrap) -> bool {
			const unsigned i = index_for(off);
			rtc[i] = u8(rtc[i] + 1);
			if (rtc[i] < wrap)
				return false;
			rtc[i] = 0;
			return true;
		};
		dirty = true;
		if (!bump(nvloc::SECOND, 60)) return;
		if (!bump(nvloc::MINUTE, 60)) return;
		if (!bump(nvloc::HOUR, 24)) return;
		calendar c = time();
		c.hour = c.minute = c.second = 0;   // already carried, above
		c.advance(0);                       // normalise, then roll one day
		if (c.day < calendar::month_length(c.year, c.month))
			c.day++;
		else
		{
			c.day = 1;
			if (++c.month > 12) { c.month = 1; c.year++; }
		}
		put(nvloc::MONTH,        c.month);
		put(nvloc::DAY_OF_MONTH, c.day);
		put(nvloc::CENTURY,      u8(c.year / 100));
		put(nvloc::YEAR,         u8(c.year % 100));
		put(nvloc::DAY_OF_WEEK,  c.day_of_week());
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
		seconds_since_power_on = 0;
	}

	void fill_nvm(u8 v) { for (u8 &b : nvm) b = v; }

	// The bit as the OS can actually see it: latched for the run, or, with
	// the manuals' rule turned on, only for the first four seconds.
	bool battery_fail_visible() const
	{
		return battery_fail
				&& (!battery_window || seconds_since_power_on < 4);
	}

	//----------------------------------------------------------------
	//  the battery
	//----------------------------------------------------------------

	// All-or-nothing: the file is read whole and checked whole before a
	// single byte of the live part is touched, so a short or corrupt
	// battery leaves the machine on the clock it already had rather than on
	// half of somebody else's.
	nv_load_result load(const char *path, unsigned select_code)
	{
		nv_load_result r;
		FILE *f = std::fopen(path, "rb");
		if (!f)
		{
			r.error = nv_error::no_file;
			return r;
		}
		u8 buf[nvfile::SIZE];
		const unsigned n = unsigned(std::fread(buf, 1, sizeof buf, f));
		const bool more = std::fgetc(f) != EOF;
		std::fclose(f);

		const u8 *payload = nullptr;
		if (n == nvfile::PAYLOAD && !more
				&& std::memcmp(buf, nvfile::MAGIC, sizeof nvfile::MAGIC) != 0)
		{
			// One of the old headerless files.
			r.legacy = true;
			r.board = select_code;
			payload = buf;
		}
		else if (n == nvfile::SIZE && !more
				&& std::memcmp(buf, nvfile::MAGIC, sizeof nvfile::MAGIC) == 0)
		{
			const unsigned ver = unsigned(nvfile::get_le(buf + 8, 2));
			if (ver > nvfile::VERSION)
			{
				r.error = nv_error::bad_version;
				return r;
			}
			r.board = buf[10];
			r.saved_at = (long long)nvfile::get_le(buf + 20, 8);
			const u32 want = u32(nvfile::get_le(buf + 28, 4));
			if (nv_crc32(buf + nvfile::HEADER, nvfile::PAYLOAD) != want)
			{
				r.error = nv_error::bad_crc;
				return r;
			}
			payload = buf + nvfile::HEADER;
		}
		else
		{
			r.error = n ? nv_error::bad_size : nv_error::unreadable;
			return r;
		}

		std::memcpy(rtc, payload, sizeof rtc);
		std::memcpy(nvm, payload + sizeof rtc, sizeof nvm);
		dirty = false;
		r.ok = true;
		r.error = nv_error::none;
		return r;
	}

	// Through a temporary and a rename, so a run that is killed while this
	// is running cannot leave a torn battery behind.
	bool save(const char *path, unsigned select_code)
	{
		u8 buf[nvfile::SIZE] = {};
		std::memcpy(buf, nvfile::MAGIC, sizeof nvfile::MAGIC);
		nvfile::put_le(buf + 8, nvfile::VERSION, 2);
		buf[10] = u8(select_code);
		buf[11] = battery_fail ? 0 : 1;
		nvfile::put_le(buf + 12, sizeof rtc, 2);
		nvfile::put_le(buf + 16, sizeof nvm, 4);
		nvfile::put_le(buf + 20, (unsigned long long)std::time(nullptr), 8);
		std::memcpy(buf + nvfile::HEADER, rtc, sizeof rtc);
		std::memcpy(buf + nvfile::HEADER + sizeof rtc, nvm, sizeof nvm);
		nvfile::put_le(buf + 28,
				nv_crc32(buf + nvfile::HEADER, nvfile::PAYLOAD), 4);

		char tmp[1024];
		std::snprintf(tmp, sizeof tmp, "%s.new", path);
		FILE *f = std::fopen(tmp, "wb");
		if (!f)
			return false;
		const bool ok = std::fwrite(buf, 1, sizeof buf, f) == sizeof buf;
		const bool closed = std::fclose(f) == 0;
		if (!ok || !closed || std::rename(tmp, path) != 0)
		{
			std::remove(tmp);
			return false;
		}
		dirty = false;
		return true;
	}

	// How long the battery was asked to hold, and whether it could. The
	// manuals promise 30 days nominal and 10 days worst case; past the
	// limit the contents "may be invalid and should be restored by the
	// operating system", which is what the battery-fail bit says.
	static constexpr long long BATTERY_DAYS = 10;

	void run_battery(nv_load_result &r, long long limit_days)
	{
		if (!r.ok || r.saved_at <= 0)
			return;
		const long long now = (long long)std::time(nullptr);
		r.off_seconds = now - r.saved_at;
		if (r.off_seconds <= 0)
			return;
		if (limit_days > 0 && r.off_seconds > limit_days * 86400)
		{
			r.battery_expired = true;
			battery_fail = true;
			return;
		}
		calendar c = time();
		if (!c.valid())
			return;              // a clock that was never set stays unset
		c.advance(r.off_seconds);
		if (d.digits)
		{
			// A stopped chip stays stopped, and the battery is not the
			// operator: what the OS made of the check byte stands.
			if (running())
				put_digits(c);
			return;
		}
		set_time(c);
		dirty = true;
	}
};

} // namespace focus

#endif // MAME_IOP_FOCUSRTC_H
