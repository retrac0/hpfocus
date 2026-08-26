// license:BSD-3-Clause
// copyright-holders:
/***************************************************************************

    The 520's internal thermal printer.

    Unusually for this machine, the programming interface is documented.
    The 520 Service manual's Theory of Operation ("Thermal Printer",
    2-41..2-44) gives it outright:

      The printer shares select code 6 with the keyboard, RTC/NVM,
      optional input device and beeper. Its bus drivers are enabled when
      R0, R1, R2, R4, R5, R6 and R7 are accessed. R1 is shared, with the
      printer driving only the upper 4 bits.

      Writes to R0, R2, R4, R5, R6 and R7 clock data from the IOP into
      the data register. Interface commands are specified by writes to
      R5; the command is written to R5 and its parameters to R7.

      The IOP reads printer status by a read of R0, R2, R4, R5, R6 or
      R7. The printer responds by presenting the status byte.

    and the data path with it: a stream of ASCII bytes, separated into
    characters -- buffered into an 80-byte line printed on receipt of the
    81st byte or of an appropriate control code -- and control codes,
    which act immediately. Escape sequences start with code 27 and
    normally end with a capital letter; those that do not apply are
    ignored.

    The 9020 CE Handbook (5-6) gives the status byte, MSB first:

      PSFL DSTF MSTF OOPP HDFL VHLO PTTH HDTH

    power supply failure, dangerous self-test failure, don't care, out of
    paper, printhead control line failure, head voltage low, power
    transistor hot, printhead hot -- so a healthy printer with paper in
    it reads 0x00. The same page lists the powerup self test's failures,
    which is what grades this model: 02 is "flag did not go false after
    reading FR0", 05 "not responding to poll", 06 "failed self test",
    07 "responds to polls when disabled", 08 "status false".

    What is NOT documented anywhere in the collection is the escape
    sequence set -- these are hardware manuals, and the BASIC programming
    manuals are not in hand. The driver in HUMAN INTERFACE emits it, so
    it gets read off a run rather than guessed at; FOCUS_PRINTLOG is that
    instrument.

    This is deliberately not an iop_interface. Select code 6 is one
    interface with several devices behind it, so console_card owns the
    slot and routes the printer's registers here.

***************************************************************************/

#ifndef MAME_IOP_FOCUSPRINT_H
#define MAME_IOP_FOCUSPRINT_H

#pragma once

#include "focusiop.h"

#include <cstdio>
#include <functional>
#include <string>

namespace focus {

struct printer_device
{
	// 80 columns, and the manual is precise about when a line leaves the
	// buffer: on the 81st character, or on a control code that calls for
	// it.
	static constexpr unsigned COLUMNS = 80;

	// The CE Handbook's status byte. Zero is a healthy printer with paper
	// in it; the named bits are there so a fault can be injected.
	enum : u8 {
		HDTH = 0x01,   // printhead too hot
		PTTH = 0x02,   // power transistor too hot
		VHLO = 0x04,   // printhead supply voltage too low
		HDFL = 0x08,   // printhead control line failure
		OOPP = 0x10,   // out of paper
		MSTF = 0x20,   // don't care
		DSTF = 0x40,   // dangerous self test failure
		PSFL = 0x80    // power supply failure
	};
	u8 status = 0;

	knob::device_log log;
	// Which routine touched the printer. Select code 6 carries the
	// keyboard, the RTC and this, so the register number alone does not
	// say whose access it is, and the interesting question about an
	// undocumented command is always which driver sent it.
	std::function<std::string()> where;
	std::string site() const
	{ return (where && log) ? where() : std::string(); }

	// Every byte the printer is handed, in order and uninterpreted. This
	// is the artefact: the printer does true overprinting, inverse,
	// underline, overline, 150% tall and pixel-by-pixel graphics dumps,
	// and its font ROM is not dumped, so a text file cannot represent
	// what it prints. The host writes these to a file.
	std::function<void(u8)> on_byte;
	// A line's worth of printable characters, for the lossy convenience
	// dump and for tests that just want to read what came out.
	std::function<void(const std::string &)> on_line;

	// The last command written to R5 and its parameter from R7. Nothing
	// interprets them yet; they are logged so the command set can be
	// enumerated from a run.
	u8 command = 0;
	u8 parameter = 0;

	// ---- the interface side ------------------------------------------

	// Reads of R0, R2, R4, R5, R6, R7 all present the status byte.
	static bool is_status_register(unsigned n)
	{ return n == 0 || n == 2 || n == 4 || n == 5 || n == 6 || n == 7; }

	u8 read_status(unsigned n)
	{
		// In loopback the data register reads back instead of the status
		// byte. The self test walks a bit through it -- write R5 = 1,
		// then write and read R0 for each of 1, 2, 4 ... 128 -- and
		// reports failure 04 on any mismatch.
		if (m_loopback)
		{
			if (n == 0)
				m_flag_drops = true;
			log("[prn R r%u loop=%02x%s]\n", n, m_data, site().c_str());
			return m_data;
		}
		// The flag drops when the status is taken and comes back when the
		// printer has something to say again -- which, with no mechanism
		// to simulate, is on the next look. Both halves are pinned by the
		// self test: failure 02 is "flag did not go false after reading
		// FR0", and the test that follows waits for the flag and reports
		// failure 01 if it never returns. A flag that only fell, or only
		// stood, would fail one or the other.
		if (n == 0)
			m_flag_drops = true;
		m_polling = false;      // the request is answered; see transfer_complete
		log("[prn R r%u status=%02x%s]\n", n, status, site().c_str());
		return status;
	}

	void write_register(unsigned n, u32 v)
	{
		const u8 b = u8(v);
		switch (n)
		{
		case 5:
			command = b;
			// Command 1 is loopback; anything else leaves it.
			m_loopback = (b == 1);
			// The poll follows the loopback and nothing else. The self
			// test wants it set at exactly one point -- after the
			// loopback walking bit, before the interface reset -- and a
			// printer that polls at any other time takes the keyboard's
			// keystrokes with it, since select code 6 has one poll line
			// between them. Whether a real printer also polls to ask for
			// service during printing is not established; nothing has
			// needed it, and asserting it on spec cost a session's
			// typing.
			m_polling = m_loopback;
			log("[prn cmd r5=%02x%s%s]\n", b, m_loopback ? " loopback" : "",
					site().c_str());
			break;
		case 7:
			parameter = b;
			log("[prn par r7=%02x%s]\n", b, site().c_str());
			break;
		default:
			// R0, R2, R4, R6: data into the print stream -- unless the
			// printer is looped back, when it only has to remember it.
			m_data = b;
			if (!m_loopback)
				take(b);
			break;
		}
	}

	// The printer drives only the upper 4 bits of the shared R1 (520
	// Service 2-43), and the powerup self test says what two of them
	// mean. Its printer test opens with:
	//
	//     LDIS #14 ; IOR RIF1
	//     LSR 15 ; ANDI #1 ; TEST,B ; BCCS.E -> skip the whole test
	//     LSR 12 ; BRE -> continue, else ESCP 9
	//
	// so **bit 15 is PRESENT** -- clear and the machine never looks at
	// the printer again, which is exactly the behaviour of a machine
	// without the option -- and bit 12 must be CLEAR, the CE Handbook's
	// failure 09, "printer enabled for poll response after reset".
	static constexpr u32 R1_PRESENT = 0x8000;
	// Bit 13 is required SET by the driver's own fault decode. After a
	// transfer HUMAN INTERFACE +0x40d8 builds `status | (R1 & 0xF000)`
	// and takes it apart at +0x4144: status bit 4 (out of paper) escapes
	// 133, and R1 bit 13 CLEAR escapes 126 -- which is the "ERROR 81:
	// Device/Interface hardware failure" a printer without it reports
	// after printing the line perfectly well.
	static constexpr u32 R1_READY = 0x2000;
	static constexpr u32 R1_POLL_ENABLED = 0x1000;
	// Presenting the bit is what makes the machine run the printer's self
	// test at all, so it is separately switchable while its effect on the
	// keyboard -- which reads this same register thousands of times a
	// boot -- is being pinned down.
	bool present = true;
	u32 r1_bits() const
	{
		if (!present)
			return 0;
		return R1_PRESENT | R1_READY
				| (m_poll_enabled ? R1_POLL_ENABLED : 0);
	}

	// PFLG, the printer's flag line. See read_status: one look after the
	// status is taken finds it low.
	bool flag()
	{
		if (m_flag_drops)
		{
			m_flag_drops = false;
			return false;
		}
		return true;
	}
	// PSTS. A healthy printer asserts status; the self test's escape 08
	// is "printer status false".
	bool asserts_status() const { return status == 0; }

	// The interface poll. The self test brackets it: after the loopback
	// it requires this printer's bit SET (failure 05, "not responding to
	// poll"), then writes FR1 = 0 and requires it CLEAR (failure 07,
	// "responds to polls when disabled"). So talking to the printer arms
	// the poll and the interface reset disarms it.
	//
	// It has to be disarmed for the ordinary case too, and not merely to
	// satisfy a test: select code 6 has one poll line and the keyboard
	// shares it, so a printer that polls forever costs the machine its
	// keystrokes.
	bool poll_request() const { return m_polling; }

	// R1 as a control register. console_card routes writes here because
	// R1 is shared -- the keyboard's half stays its own.
	//
	// Zero is the interface reset. Bit 12 is the printer's poll-response
	// enable: the self test requires it CLEAR after a reset (its failure
	// 09, "printer enabled for poll response after reset"), and the
	// driver in HUMAN INTERFACE +0x40a4 SETS it immediately before it
	// starts a transfer -- which is how a line of print gets serviced,
	// since the data arrives by DMA rather than a byte at a time.
	void control(u32 v)
	{
		if (v == 0)
		{
			m_polling = false;
			m_poll_enabled = false;
			m_armed = false;
			m_loopback = false;
			log("[prn reset%s]\n", site().c_str());
			return;
		}
		if (v & R1_POLL_ENABLED)
		{
			m_poll_enabled = true;
			m_polling = true;
			m_armed = true;
			log("[prn poll armed%s]\n", site().c_str());
		}
	}

	// The printer only takes a transfer once the driver has armed it
	// (R1 bit 12). Accepting data before that let the IOP run the whole
	// block at set-up time, ahead of the arm -- the bytes arrived, but
	// the transfer was accounted in the wrong order and the driver read
	// a residual count that was never zero.
	bool accepts_data() const { return m_armed; }

	// The line arrives as DMA, which console_card hands on. Taking data
	// answers the request the poll was making, so the poll and its enable
	// drop here: the driver reads R1's upper nibble back as part of the
	// transfer's status (HUMAN INTERFACE +0x40d8 builds
	// `status | (R1 & 0xF000)`), and a poll-enable still standing there
	// reads as a fault.
	void dma_byte(u8 b) { take(b); }

	// The IOP tells the card when the block is over; that is when the
	// printer stops asking, and when R1's poll-enable has to be gone --
	// the driver reads the upper nibble back as part of the transfer's
	// status and a poll-enable still standing there reads as a fault.
	void transfer_complete()
	{
		// Everything stays standing. The interrupt service picks the
		// printer out of select code 6's several devices by ANDing R1
		// with 0x1000 -- the poll-enable -- and skipping the whole
		// completion path when it reads zero (HUMAN INTERFACE +0x3d68).
		// Clearing it here on the theory that a set bit would read as a
		// fault left the driver blocked on a semaphore nobody posted;
		// the fault decode does not look at bit 12 at all, only at
		// status bit 4 and R1 bit 13. The driver's own FR1 = 0 at the
		// end of the transfer is what clears it.
		// Stop taking data. The poll-enable is a different thing and has
		// to stand: leaving the card willing as well let the IOP start a
		// second transfer over the top of the first, and the residual
		// count the driver reads back was never zero again.
		m_armed = false;
		m_polling = true;
		flush();
		log("[prn transfer complete%s]\n", site().c_str());
	}

	// ---- the paper ---------------------------------------------------

	void take(u8 b)
	{
		if (on_byte)
			on_byte(b);
		if (b == 27)
		{
			m_escape = 1;
			m_seq = "ESC";
			return;
		}
		if (m_escape)
		{
			m_seq.push_back(char(b));
			// "An escape sequence ... normally ends with a capital
			// letter." Anything the printer does not implement is
			// ignored, which for now is all of it -- but the sequence is
			// logged, and that log is how the set gets enumerated.
			if (b >= 'A' && b <= 'Z')
			{
				log("[prn esc %s]\n", m_seq.c_str());
				m_escape = 0;
			}
			else if (++m_escape > 16)
			{
				log("[prn esc %s ... abandoned]\n", m_seq.c_str());
				m_escape = 0;
			}
			return;
		}
		if (b == '\r' || b == '\n' || b == '\f')
		{
			log("[prn %s]\n", b == '\r' ? "CR" : b == '\n' ? "LF" : "FF");
			flush();
			return;
		}
		if (b < 32)
		{
			log("[prn ctl %02x%s]\n", b, site().c_str());
			return;                 // undefined control codes are ignored
		}
		m_line.push_back(char(b));
		// The 81st character prints the line.
		if (m_line.size() >= COLUMNS)
			flush();
	}

	void flush()
	{
		if (m_line.empty())
			return;
		log("[prn line |%s|]\n", m_line.c_str());
		if (on_line)
			on_line(m_line);
		m_line.clear();
	}

private:
	std::string m_line;
	std::string m_seq;
	unsigned m_escape = 0;
	bool m_flag_drops = false;
	bool m_loopback = false;
	bool m_polling = false;
	bool m_armed = false;
	u8 m_data = 0;
	// Reset leaves the printer not answering polls; the self test checks
	// that (failure 09) before it does anything else.
	bool m_poll_enabled = false;
};

} // namespace focus

#endif // MAME_IOP_FOCUSPRINT_H
