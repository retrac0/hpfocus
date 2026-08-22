// license:BSD-3-Clause
// copyright-holders:
/***************************************************************************

    HP 27128A Asynchronous Serial Interface (ASI), and the 27130A eight
    channel multiplexer that shares its firmware.

    This is the 530/540's CONSOLE. Those machines have no integrated
    display: the CE handbook's installation procedure puts a "System
    Terminal (console) Interface Card in slot 0" -- select code 0 -- and
    sets the terminal to 9600 baud, 8 bits, no parity, with ENQ/ACK and
    XON/XOFF pacing (9030/40 CE Handbook, 2-3). HP-UX carries one driver
    module for both cards, named HP27128/30 in the kernel's own module
    directory, so the ASI and the MUX are two configurations of one
    protocol.

    Unlike the 520's console boards, this card is DOCUMENTED: HP 27128A
    Asynchronous Serial Interface Technical Reference (27132-90001,
    Jun 83) and the matching MUX reference (27132-90006). Sheet numbers
    below cite them. What is NOT documented anywhere is how a CIO card's
    channel operations -- write_control, write_command, write_order,
    read_data, write_data, read_status -- map onto the sixteen FR
    registers the FOCUS IOP addresses. The I/O ERS gives the FR
    mechanism, the card references give the protocol, and no document in
    hand joins them. The mapping below is therefore a MODEL, chosen to
    match the shapes the Series 500 boot code already uses on other
    cards, and it will need correcting against a driver that really
    drives it:

      FR0   read/write data          (read_data / write_data)
      FR1   read: status / control-register write, as everywhere else on
            this machine -- bit 0 ARD is the attention-request disable
            the IOP's own poll writes (I/O ERS 4.4), and the CIO
            write_control byte has ARD as bit 0 too (27110A 4-1), which
            is the one place the two vocabularies visibly agree
      FR3   read: card identity, 3 for the ASI and 5 for the MUX
            ("card ID = 3", 27128A 4-67; "card 10 = 5", 27130A 4-46)
      FR8   write: command byte  (write_command, opcode in bits 7:4)
      FR9   write: order byte    (write_order, opcode in bits 7:4)
      FR12  read: the posted Level 2 status byte (read_status), whose
            removal frees the card to post another

    The card's own model, which IS documented and is what the code below
    actually implements:

    - One posted status byte at a time. Every Attention Request must be
      acknowledged by a host read_status before another event can be
      reported; RQA from the host is answered with RFC (ready for
      command), spontaneous events with AES (27110A 4-3/4-4, the
      clearest statement of a mechanism all four cards share).
    - IDY answers an identity block: card ID, firmware ID, a 16-bit
      firmware revision, the multiplexing byte, ports-1, requests-1, and
      the maximum guaranteed data blocking size (27128A 4-67).
    - Transactions are opened by a Connect Logical Channel request block
      (27128A 4-3):

        byte 0  S | BLK | .. | request code (4 bits) | channel high
        byte 1-2  logical channel number
        byte 3  subfunction code
        byte 4  port ID          (ASI: always 0; MUX: the logical port)
        byte 5-6  data length, 16 bits

      request codes: 1 read device data, 2 write device data, 3 control
      device, 4 read card information, 5 write card configuration,
      6 control card.
    - The firmware owns the byte stream: received characters run a fixed
      pipeline (errors, signal characters, handshakes, edit mode,
      stripping, single and double text terminators, prompt sequences,
      end-on-count) and reach the host as terminated RECORDS carrying
      the code for whichever condition ended them (27128A 4-5, 4-66).
      Only the terminator behaviour matters to a host driver, so that is
      what is modelled: a record ends on the configured terminator, on
      the read length, or on the card's buffer limit.

***************************************************************************/

#ifndef MAME_IOP_FOCUSASI_H
#define MAME_IOP_FOCUSASI_H

#pragma once

#include "focusiop.h"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <vector>

namespace focus {

struct asi_card : public iop_interface
{
	iop_core *iop = nullptr;
	unsigned pa = 0;
	bool log = false;

	// A driver polling a status register runs to hundreds of thousands of
	// accesses in a few emulated seconds, and logging each one -- flushed,
	// so a killed run still says what it saw -- costs more time than the
	// emulation itself. Identical consecutive accesses are collapsed into
	// a repeat count, and the whole log stops after `log_max` lines.
	unsigned log_max = 2000;
	bool log_line(const char *dir, unsigned n, u32 v)
	{
		if (!log)
			return false;
		const u32 key = (u32(dir[0]) << 24) | (n << 16) | (v & 0xffff);
		if (key == m_log_last)
		{
			m_log_repeat++;
			return false;
		}
		if (m_log_repeat)
		{
			std::printf("[asi%u ... x%u more]\n", pa, m_log_repeat);
			m_log_repeat = 0;
		}
		m_log_last = key;
		if (m_log_lines++ >= log_max)
		{
			if (m_log_lines == log_max + 1)
			{
				std::printf("[asi%u log capped at %u lines]\n", pa, log_max);
				std::fflush(stdout);
			}
			return false;
		}
		return true;
	}

	// The identity block the IDY order answers (27128A 4-67). The MUX
	// answers the same shape with card ID 5, eight ports and its own
	// firmware revision (27130A 4-46), so one card models both.
	//
	// FR3 is NOT that card ID, which is what this register was first read
	// as. HP-UX's select-code scan (CONFIGURATION at runtime 0x152fc)
	// settles it: read FR1 and require (FR1 & 0xf30) == 0; read FR3, and
	// **if it reads 9, write 0 to FR1 and FR8** -- a reset; read FR3
	// again and require **zero**, skipping the slot otherwise; only then
	// write 1<<sc to FR0 and read it back from FR1 as the presence echo.
	// So FR3 says "this card is configured and ready" (0) or "reset me
	// first" (9), and a card that answers its ID here is never
	// registered at all. The disc controller's model already carried the
	// same note. The 9-then-reset path is not modelled: no card in hand
	// is known to need it.
	u8 card_id = 0;         // FR3: 0 = ready. FOCUS_ASIID sweeps it.
	u8 firmware_id = 0;
	u16 firmware_rev = 1;
	u8 mpx = 1;             // 0 subchannel, 1 in-channel, 2 both
	u8 ports = 1;           // number of active ports
	u8 requests = 3;        // maximum requests per port
	u16 blocking = 200;     // maximum guaranteed data blocking size

	// A constant FR1 answer, for sweeping what a driver polls for.
	u32 fr1_force = 0;

	// Event codes, dispatched by high nibble (the disc controller's
	// classification, which this card's driver shares): 1, 2 and 4 are
	// completion classes.
	enum : u8 { EVENT_DONE = 0x10 };
	// FOCUS_ASIEVENT overrides the completion event code, for finding
	// which class the driver acts on.
	u8 event_done = EVENT_DONE;
	unsigned event_delay_ms = 2;

	// The halfword the identify transaction answers, which is what types
	// the slot -- and the CARD ID goes in its HIGH byte. Read off the
	// driver: CORE SYSTEM +0x2f16 takes the halfword the two-byte DMA
	// delivered, stores its high byte at the device record's +0 and its
	// low byte at +2, then range-checks that high byte with
	// `CRB` against 1..7 and computed-branches through a seven-arm
	// table. Out of range stores error 30 and abandons the slot, which
	// is what 0x0003 -- the ID in the LOW byte -- used to do to every
	// card in the machine. 3 is the ASI's documented ID (27128A 4-67);
	// the low byte is the firmware ID. FOCUS_ASIIDENT sweeps it.
	u16 identity = 0x0300;

	// Where characters the host writes go, and where typed characters
	// come from. A host wires these to a terminal.
	std::function<void(u8)> on_char;
	void type(u8 c) { m_rx.push_back(c); }

	// The single text terminator the receive pipeline ends a record on.
	// A terminal's Return key is the one that matters; the driver can
	// reconfigure it through write-card-configuration subfunction 22.
	u8 terminator = '\r';

	//----------------------------------------------------------------
	//  CIO status bytes (27110A 4-3/4-4, shared across the card family)
	//----------------------------------------------------------------

	enum : u8
	{
		STAT_RFC = 0x01,    // ready for command, the answer to RQA
		STAT_AES = 0x02,    // asynchronous event sensed
		STAT_DONE = 0x03    // the transaction the host asked for finished
	};

	// Write Control bit assignments (27110A 4-1).
	enum : u8
	{
		CTL_ARD = 0x01,     // attention request disable
		CTL_ARE = 0x02,     // attention request enable
		CTL_RQA = 0x04,     // request attention
		CTL_DEN = 0x10,     // device enable
		CTL_DCL = 0x20      // device clear
	};

	// Write Order opcodes, in bits 7:4 of the order byte (27110A 4-2).
	enum : u8
	{
		ORD_IDY = 0,        // identify
		ORD_DLD = 1,        // download
		ORD_PAU = 2,        // pause
		ORD_DIS = 3,        // disconnect
		ORD_RS  = 4,        // read status
		ORD_WC  = 5,        // write control
		ORD_RD  = 6,        // read data
		ORD_WD  = 7         // write data
	};

	// Write Command opcodes, in bits 7:4 of the command byte.
	enum : u8
	{
		CMD_DSC = 0,        // disconnect subchannel, aborting any work
		CMD_AEK = 1,        // asynchronous event acknowledge
		CMD_CSC = 2,        // connect subchannel
		CMD_RSC = 4         // resume subchannel
	};

	//----------------------------------------------------------------
	//  iop_interface
	//----------------------------------------------------------------

	u32 status() override { return 1; }
	u32 status_reg() override { return 0; }   // RIST: zero is healthy

	// RIFG: the card has finished what it was asked to do. The powerup
	// self test's wait helper polls this and times out without it.
	u32 flag() override { return 1; }

	// The card asserts ASYNC while an unacknowledged status byte is
	// posted and attention requests are enabled -- "there is a one-to-one
	// correspondence between unacknowledged ARQs and posted status
	// bytes" (27110A 4-3). The IOP's poll answers with an ARD write to
	// FR1, which is the same disable the control byte's bit 0 carries.
	bool attention() override
	{ return !m_events.empty() && m_arq_enabled && !m_ard; }

	// Ready to move data whenever the channel offers it: the transfer
	// direction is the order's, and the firmware buffers both ways.
	bool service_request() override
	{ return m_order == ORD_WD || !m_out.empty(); }

	// The interface poll must be quiet unless the card wants the channel
	// to start something, as on the 520's boards -- the powerup self
	// test checks that nothing is requesting between its steps.
	bool poll_request() override { return false; }

	u32 read_fr(unsigned n) override
	{
		u32 v = 0;
		switch (n)
		{
		case 0:
			v = pop_out();
			break;
		case 1:
			// The presence test writes a pattern to FR0 and reads it
			// straight back here before this is a status register at
			// all, exactly as on the disc controller.
			if (m_echo_pending)
			{
				v = m_last_fr0;
				m_echo_pending = false;
				m_present = true;
			}
			else if (fr1_force)
				// FOCUS_ASIFR1: a constant answer, for finding which bit a
				// driver is really waiting on. It is tested before the
				// presence gate below, because a forced constant that
				// the gate can still override is not a constant.
				v = fr1_force & 0xffff;
			else if (!m_present)
				v = 0;
			else
				// The house status register of this machine's CIO cards,
				// established by sweeping constants past the HP-UX 5.30
				// driver: it polls here and only proceeds when bit 7 AND
				// bit 6 are set, which is the internal disc controller's
				// register exactly (bit 7 configured / self test passed,
				// bit 6 self test complete, bit 0 an event waiting). With
				// 0x80 alone the driver polls forever; with 0xc0 it goes
				// on to write FR8 and FR9 -- and the FR9 value it writes,
				// 0x20 = (2 << 4) | pa, is the same configuration word
				// LCS80 sends the disc. One protocol, two cards.
				v = 0xc0 | (m_events.empty() ? 0 : 0x01);
			break;
		case 3:
			v = card_id;
			break;
		case 9:
			// The event code the card is announcing. FR1 bit 0 says one
			// is waiting and this hands it over; codes are dispatched by
			// their high nibble -- 1, 2 and 4 completion classes, 3 and
			// anything above 0x4f an error (kernel handler at CORE
			// SYSTEM 0x26020). Handing the code over CONSUMES it: this
			// kernel writes no acknowledgement, and a code left standing
			// is read forever.
			if (!m_events.empty())
			{
				v = m_events.front();
				m_events.pop_front();
				m_ard = false;
			}
			break;
		case 12:
			// read_status: taking the byte frees the card to post
			// another and stands its attention request down.
			v = m_status;
			m_status_posted = false;
			m_ard = false;
			break;
		default:
			v = m_fr[n & 15];
			break;
		}
		if (log_line("R", n, v))
		{
			std::printf("[asi%u R fr%u = %04x p=%08x]\n", pa, n, v & 0xffff,
					iop ? iop->tpc() : 0);
			std::fflush(stdout);
		}
		return v;
	}

	void write_fr(unsigned n, u32 v) override
	{
		v &= 0xffff;
		if (log_line("W", n, v))
		{
			std::printf("[asi%u W fr%u = %04x p=%08x]\n", pa, n, v,
					iop ? iop->tpc() : 0);
			std::fflush(stdout);
		}
		m_fr[n & 15] = v;
		switch (n)
		{
		case 0:
			if (!m_present)      // the powerup presence echo
			{
				m_last_fr0 = v;
				m_echo_pending = true;
			}
			take_data(u8(v));
			break;
		case 1:
			write_control(u8(v));
			break;
		case 8:
			// The transaction command byte. HP-UX's driver delivers it
			// the way LCS80 delivers the disc's: as the first entry of an
			// ICEI command list, followed by SIL, the DMA setup and a
			// start -- so a command must leave its answer ready for the
			// leg the same list is about to run.
			transaction(u8(v));
			break;
		case 9:
			// The configuration word the driver writes before the first
			// transaction: 0x20 = (2 << 4) | pa, exactly what LCS80 sends
			// the disc controller. The high nibble's meaning is still
			// unestablished on either card.
			m_config = v;
			break;
		default:
			break;
		}
	}

	// DMA moves the data part of a transaction: the request block and
	// the write data go out, records and identity blocks come back.
	u8 dma_read() override { return pop_out(); }
	void dma_write(u8 b) override { take_data(b); }
	u16 dma_read16() override { return u16((dma_read() << 8) | dma_read()); }
	void dma_write16(u16 v) override { dma_write(u8(v >> 8)); dma_write(u8(v)); }

	void dma_complete(bool output) override
	{
		if (output && m_order == ORD_WC)
			take_request_block();
		// The transfer the command asked for is done, so the card
		// announces the completion the driver is waiting for.
		if (m_pending_event)
			m_event_timer = event_delay_ms ? event_delay_ms : 1;
		post(STAT_DONE);
	}

	//----------------------------------------------------------------
	//  the card's own behaviour
	//----------------------------------------------------------------

	// One millisecond of the card's time: a receive record that has been
	// standing complete gets announced to the host, which is how a typed
	// line reaches a driver that is not already reading.
	void tick()
	{
		// A completion the driver is not yet waiting for is discarded by
		// the kernel's handler (see focushpib.h), so the card takes a
		// tick or two to announce one, as real hardware does.
		if (m_event_timer && --m_event_timer == 0 && m_pending_event)
		{
			m_events.push_back(m_pending_event);
			m_pending_event = 0;
			if (iop)
				iop->raise(pa);
		}
		if (!m_rx.empty() && !m_status_posted && m_arq_enabled)
		{
			// "If no read request is pending, and the card has a record
			// available for the host, and the host interrupt mask allows
			// 'data available' interrupt, an asynchronous event is sent
			// to the host to start the read" (27128A 4-6).
			if (record_ready())
				post(STAT_AES);
		}
	}

private:
	void write_control(u8 c)
	{
		// A write of 2 acknowledges the event code standing in FR9 -- the
		// attention handler's loop pops one per acknowledgement, as on
		// the disc controller.
		if (c == 2 && !m_events.empty())
		{
			m_events.pop_front();
			m_ard = false;
			return;
		}
		if (c & CTL_ARD)
			m_ard = true;                 // the IOP's attention poll
		if (c & CTL_ARE)
			m_arq_enabled = true;
		if (c & CTL_RQA)
			post(STAT_RFC);               // answered with ready-for-command
		if (c & CTL_DCL)
			device_clear();
	}

	// A transaction command byte arrived on FR8. This is the machine's own
	// card protocol rather than the CIO document's -- read off HP-UX 5.30
	// driving this slot -- and it sits alongside the documented CIO
	// vocabulary below rather than replacing it.
	//
	// Command 0 is identify: the driver sets up a TWO BYTE input leg for
	// the answer (WDC 2 in its command list), and the halfword those bytes
	// form is what types the slot, exactly as on the disc controller.
	void transaction(u8 c)
	{
		m_command = c;
		m_in.clear();
		switch (c)
		{
		case 0:
			m_out.clear();
			m_out.push_back(u8(identity >> 8));
			m_out.push_back(u8(identity));
			m_pending_event = event_done;
			break;
		default:
			// Anything else is a command whose meaning is not yet known.
			// Leaving the leg unanswered is what the card would do if it
			// did not understand, and the driver's timeout path (DCE,
			// DDMA, DINT, CIRQ) is visible in a run when that happens.
			break;
		}
	}

	void write_command(u8 c)
	{
		// "The card will accept and discard (thus effectively
		// terminating) commands which are not supported" (27110A 4-2).
		switch (c >> 4)
		{
		case CMD_DSC:
			m_out.clear();
			m_order = 0xff;
			break;
		case CMD_AEK:
			m_status_posted = false;
			m_ard = false;
			break;
		case CMD_CSC:
			m_subchannel = c & 0x0f;
			m_connected = true;
			break;
		case CMD_RSC:
			break;
		default:
			break;
		}
	}

	void write_order(u8 o)
	{
		m_order = u8(o >> 4);
		m_in.clear();
		switch (m_order)
		{
		case ORD_IDY:
			m_out.clear();
			for (u8 b : identity_block())
				m_out.push_back(b);
			break;
		case ORD_RS:
			m_out.clear();
			m_out.push_back(m_status);
			m_status_posted = false;
			break;
		case ORD_RD:
			deliver_record();
			break;
		case ORD_DIS:
			m_connected = false;
			break;
		default:
			break;
		}
	}

	// The identity information block (27128A 4-67).
	std::vector<u8> identity_block() const
	{
		return {
			card_id,
			firmware_id,
			u8(firmware_rev >> 8), u8(firmware_rev),
			u8(mpx << 6),              // MPX in the top bits, bp mode in 0
			u8(ports - 1),
			u8(requests - 1),
			u8(blocking >> 8), u8(blocking)
		};
	}

	void take_data(u8 b)
	{
		// Under a write-data order the bytes are characters for the
		// terminal; under write-control they are the request block.
		if (m_order == ORD_WD)
		{
			if (on_char)
				on_char(b);
			return;
		}
		m_in.push_back(b);
	}

	// A Connect Logical Channel request block arrived (27128A 4-3).
	void take_request_block()
	{
		if (m_in.size() < 7)
			return;
		m_request = u8((m_in[0] >> 0) & 0x0f);
		m_channel = u16((m_in[1] << 8) | m_in[2]);
		m_subfunction = m_in[3];
		m_port = m_in[4];
		m_length = u16((m_in[5] << 8) | m_in[6]);
		if (log)
		{
			std::printf("[asi%u request code=%u channel=%u sf=%u port=%u "
					"len=%u]\n", pa, m_request, m_channel, m_subfunction,
					m_port, m_length);
			std::fflush(stdout);
		}
		if (m_request == 1)        // read device data
			deliver_record();
		m_in.clear();
	}

	// Is a complete receive record waiting? The pipeline ends one on the
	// single text terminator, on the read length, or on the card's own
	// buffer limit (27128A 4-5/4-6).
	bool record_ready() const
	{
		for (u8 c : m_rx)
			if (c == terminator)
				return true;
		return m_rx.size() >= blocking
				|| (m_length && m_rx.size() >= m_length);
	}

	// Hand the host the record it asked for.
	void deliver_record()
	{
		m_out.clear();
		while (!m_rx.empty())
		{
			const u8 c = m_rx.front();
			m_rx.pop_front();
			m_out.push_back(c);
			if (c == terminator)
				break;
			if (m_length && m_out.size() >= m_length)
				break;
		}
		if (!m_out.empty())
			post(STAT_DONE);
	}

	// Post a Level 2 status byte, which is what enables the attention
	// request. Only one may be outstanding: while the last is
	// unacknowledged the card holds everything else off ("EFFECT OF
	// FREEZE", 27110A 4-5).
	void post(u8 s)
	{
		if (m_status_posted)
			return;
		m_status = s;
		m_status_posted = true;
		m_ard = false;
		if (iop && m_arq_enabled)
			iop->raise(pa);
	}

	void device_clear()
	{
		// DCL "resets all of the I/O channel interface except the device
		// address" (27110A 4-6).
		m_in.clear();
		m_out.clear();
		m_status_posted = false;
		m_order = 0xff;
		m_connected = false;
	}

	u8 pop_out()
	{
		if (m_out.empty())
			return 0;
		const u8 b = m_out.front();
		m_out.pop_front();
		return b;
	}

	std::deque<u8> m_events;    // event codes waiting to be read from FR9
	u8 m_pending_event = 0;     // posted when the command's transfer ends
	unsigned m_event_timer = 0;
	std::deque<u8> m_rx;        // characters typed at the terminal
	std::deque<u8> m_out;       // what the host reads next
	std::vector<u8> m_in;       // what the host has written

	u32 m_fr[16] = {};
	u8 m_order = 0xff;
	u8 m_command = 0xff;
	u32 m_config = 0;
	u8 m_status = 0;
	bool m_status_posted = false;
	bool m_arq_enabled = true;
	bool m_ard = false;
	bool m_connected = false;
	u8 m_subchannel = 0;

	// The current request block's fields.
	u8 m_request = 0;
	u16 m_channel = 0;
	u8 m_subfunction = 0;
	u8 m_port = 0;
	u16 m_length = 0;

	// The powerup presence test's echo, as on every other card here.
	u32 m_log_last = 0xffffffffu;
	unsigned m_log_repeat = 0;
	unsigned m_log_lines = 0;
	bool m_present = false;
	bool m_echo_pending = false;
	u32 m_last_fr0 = 0;
};

} // namespace focus

#endif // MAME_IOP_FOCUSASI_H
