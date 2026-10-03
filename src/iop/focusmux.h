// license:BSD-3-Clause
// copyright-holders:
/***************************************************************************

    HP 27140A Asynchronous 6-Channel Multiplexer with modem control -- the
    "6-port MUX" of HP-UX's mkdev(1M), and the terminal fan-out card whose
    driver the 5.30 kernel already carries (module HP27140, runtime CST 53).

    The card is DOCUMENTED at the level a host sees it: the firmware
    External Reference Specification (A-27140-81001-2, Apr 86; page
    numbers below are its printed ones), the Hardware Reference Manual
    (27140-90007, Jun 88) and the IMS. It is NOT the ASI with more ports.
    Its backplane protocol is an explicitly reduced subset of the HP-CIO
    smart-card protocol, "created to minimize the significant overhead of
    the backplane transactions for small buffer sizes (mainly
    character-at-a-time I/O from terminals)" (ERS p.27): one command, six
    orders and three ARQs, no logical channels, no request blocks.

      command   CSC n    connect subchannel: code 2 in the high nibble,
                         the SRQ address in the low one (p.27)
      orders    IDY 0    identify -- a SIX-byte block (p.41-42)
                DLD 1    download; option 0 data record, 1 control block
                DIS 3    disconnect subchannel
                RD  6    read data/status -- the card's whole state, as
                         4-byte character "quads" and status blocks,
                         ending in 0xFF, and NEVER empty (p.43-44)
                WD  7    write data/configuration/control (p.50-51)
                RDS 0xA  read download status (option 1)
      ARQs      RFC, AES (0x11 data available, 0x14 modem change), PER
                (0x71..0x78 protocol errors)

    How that vocabulary reaches the FR registers is the house protocol
    HP-UX 5.30 was watched driving the ASI and the HP-IB card with
    (HARDWARE.md 2.6.0), and this card settles one thing that table
    left open: the word the kernel writes to FR9 for every slot,
    (2 << 4) | pa, IS the Connect Subchannel command -- code 2, and the
    select code is the SRQ address the ERS says the low nibble must
    carry. So:

      FR0   read/write data, and the powerup presence echo
      FR1   read: 0xc0 | (an event code is waiting); write: 1 ARD, 2 ack
      FR3   read: 0 = ready
      FR8   write: the ORDER byte, opcode in bits 7:4, byte mode bit 3,
            option bits 2:0
      FR9   write: the CSC command; read: the event code, consumed
      FR6   read: the panic printer's byte port, same queue as FR0

    The event codes the kernel accepts (0x30 for a command, 0x10 for a
    completion, dispatched by high nibble at CORE SYSTEM 0x26020) are the
    ASI's; the ERS's own ARQ codes are what the firmware says, and which
    of the two the HP27140 driver wants is read off the driver, with the
    FOCUS_MUX*EVENT knobs as the sweep controls.

    Two firmwares live in one card (ERS ch.7). The EPROM one knows port 0
    only, answers firmware ID 2 with "active ports - 1 = 0", and exists
    to let the host download the full firmware: DLD control subfunctions
    1 initialise, 2 set RAM address and record length, 3 verify, 4 start
    RAM execution. HP-UX's HP27140 module carries the 5,330-byte Z80
    image itself (a length word 0x14d2 at module +0x31f4, code from
    +0x31f8). This model takes the download at transaction level -- the
    bytes are kept, verify answers 0, start flips the personality to
    firmware ID 3 with six ports, whose receivers are DISABLED until
    subfunction 148 enables each (ERS ch.6) -- and runs no Z80.

    What is modelled: the six orders, the RD block with quads and status
    blocks 128/141/142/143/144, the WD block with data blocks and every
    subfunction 1..8 and 128..155 (ERS 5.3.2), abort-on-first-error with
    the matching PER code, the host interrupt mask and one AES between
    reads (p.39), the transmit buffer of 255 bytes a port with X-OFF stop
    and restart, the receive silo of 1024 quads with the buffer-overflow
    attribute, and the modem lines as the host sees them. What is not:
    the modem timers' timing (they are stored), the ASI log-channel
    personality the EPROM switches to on an RTS or CLC order (p.C-1) --
    such an order is logged loudly and refused with PER -- and the
    firmware's own 16.7 ms clock, which the card's millisecond tick
    stands in for.

***************************************************************************/

#ifndef MAME_IOP_FOCUSMUX_H
#define MAME_IOP_FOCUSMUX_H

#pragma once

#include <algorithm>
#include "focusiop.h"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace focus {

struct mux_card : public iop_interface
{
	iop_core *iop = nullptr;
	unsigned pa = 0;
	knob::device_log log;
	static unsigned long log_key(char dir, unsigned n, u32 v)
	{
		return (u32(dir) << 24) | (n << 16) | (v & 0xffff);
	}
	static knob::channel &dbg()
	{
		static knob::channel c("FOCUS_MUXDBG");
		return c;
	}
	double stamp() const { return iop ? iop->tnow() : 0.0; }

	//----------------------------------------------------------------
	//  what the card answers -- the knobs
	//----------------------------------------------------------------

	// The identify halfword: card ID 7 in the high byte (the kernel's
	// seven-arm table at CORE SYSTEM +0x2f16 types the slot 32 from it),
	// firmware ID 2 for the EPROM firmware in the low one. FOCUS_MUXIDENT.
	u16 identity = 0x0702;
	// The 16-bit binary firmware revision date code, IDY bytes 2-3.
	u16 date_code = 0x8604;
	// A constant FR1 answer, for sweeping. FOCUS_MUXFR1.
	u32 fr1_force = 0;
	// The event announced for a CSC command on FR9, and for a completed
	// order. The ASI's codes to start with, since the kernel's generic
	// slot scan is the same code for every card. FOCUS_MUXCSCEVENT,
	// FOCUS_MUXEVENT.
	u8 event_csc = 0x30;
	u8 event_done = 0x10;
	// The asynchronous-event and protocol-error codes, as the ERS gives
	// them: AES 0x11 data available, 0x14 modem line changed, PER 0x7n.
	// FOCUS_MUXAESEVENT (the data code; modem is +3), FOCUS_MUXPER.
	u8 event_aes = 0x11;
	u8 event_per = 0x70;
	// The PER code an unsupported order gets (0x72 "unimplemented or
	// invalid order"). FOCUS_MUXPERORDER.
	u8 per_order = 2;
	unsigned event_delay_ms = 2;    // FOCUS_MUXEVDELAY
	// "There could be a delay of up to 10 milliseconds from the time the
	// card receive the first character to the time the host sees the
	// ARQ" (p.39). FOCUS_MUXAESDELAY.
	unsigned aes_delay_ms = 5;
	bool no_async = false;          // FOCUS_MUXNOASYNC
	// What the interface poll reports: 0 nothing, 1 the card wants its
	// next order (the ERS's SRQ), 2 whenever it could move data.
	// FOCUS_MUXPOLL.
	unsigned poll_mode = 1;

	//----------------------------------------------------------------
	//  the six ports, and the hooks a host wires them to
	//----------------------------------------------------------------

	enum : unsigned { PORTS = 6, SILO = 1024, TXBUF = 255,
			RDMAX = 8192, WDMAX = 4096 };

	// The modem lines as status 141 and the modem quad report them:
	// bit 0 TR, 2 RS, 4 DM, 5 RR, 6 IC, 7 CS (p.46, p.48).
	enum : u8 { LINE_TR = 0x01, LINE_RS = 0x04, LINE_DM = 0x10,
			LINE_RR = 0x20, LINE_IC = 0x40, LINE_CS = 0x80 };

	// Quad attribute bits (p.44-45).
	enum : u8 { ATTR_PARITY = 0x01, ATTR_OVERRUN = 0x02, ATTR_FRAMING = 0x04,
			ATTR_BREAK = 0x08, ATTR_MODEM = 0x10, ATTR_OVERFLOW = 0x20 };

	struct port_state
	{
		// Configuration, as the subfunctions store it (ERS ch.6 gives
		// the defaults: full duplex hardwired, 9600, 8 bits, 1 stop, no
		// parity, TR and RS cleared, SR set).
		u8 mode = 2;            // sf 1: 1 modem, 2 hardwired; bit 4 originate
		u8 baud = 0x0f;         // sf 2: 0x0f = 9600
		u8 frame = 0x30;        // sf 3: bits 5:4 data-5, 3:2 stop, 1:0 parity
		u8 timer_connect = 25;  // sf 4, seconds
		u16 timer_noactivity = 600; // sf 5, seconds
		u8 timer_rr = 40;       // sf 6, 10 ms units
		u8 xon_dev = 0xff, xoff_dev = 0xff;   // sf 7 (0xff = disabled)
		u8 xon_host = 0x11, xoff_host = 0x13; // sf 8
		bool rx_enabled = true; // sf 147/148
		bool tr = false, rs = false, sr = true;   // sf 128..137
		bool link = false;      // modem link open (sf 128/129)
		u8 lines = LINE_DM | LINE_RR | LINE_CS;   // inputs, as asserted
		bool tx_stopped = false;                  // sf 139/140, device X-OFF
		bool quads = false;     // sf 151/152: one modem quad per read
		u8 leds = 0;            // sf 145
		std::deque<u8> tx;      // the transmit buffer, TXBUF bytes
		unsigned last_tx_count = ~0u;   // for status 128 "on changes only"
		bool modem_changed = false;     // a quad is owed under sf 152
		// One-shot status blocks queued for the next read: code, then
		// the additional information.
		std::deque<std::vector<u8>> status;
	};
	port_state ports[PORTS];

	struct quad { u8 port, attr, ch; };

	// A byte the card has for the port's device. The sink returns false
	// while it cannot take one -- a UART with a byte still shifting out
	// -- and the card offers it again on a later tick; a sink that takes
	// everything drains the buffer at once.
	std::function<bool(unsigned, u8)> on_tx;
	// The port's line configuration changed (subfunctions 1, 2, 3).
	std::function<void(unsigned)> on_config;
	// The port's output lines TR/RS/SR changed.
	std::function<void(unsigned)> on_lines;
	// Break generation started or stopped on the port.
	std::function<void(unsigned, bool)> on_break;

	// A character arrived from the port's device, with its attribute
	// bits (parity, framing, break). Into the silo whatever the host is
	// doing; the AES that tells the host is the tick's business.
	void receive(unsigned port, u8 c, u8 attr = 0)
	{
		if (port >= active_ports())
			return;
		port_state &P = ports[port];
		if (!P.rx_enabled)
		{
			if (dbg())
				dbg()("[%8.3fs mux%u port %u rx %02x dropped: receiver "
						"disabled]\n", stamp(), pa, port, c);
			return;
		}
		if (m_silo.size() >= SILO)
		{
			// "The card ran out of buffer space when the character was
			// received for that port. The data character in the quad is
			// the last valid character for the port" (p.45) -- FOR THE
			// PORT: the newest quad of this port, not whichever port's
			// quad happens to be last in the silo.
			for (auto q = m_silo.rbegin(); q != m_silo.rend(); ++q)
				if (q->port == port)
				{
					q->attr |= ATTR_OVERFLOW;
					break;
				}
			return;
		}
		m_silo.push_back({u8(port), attr, c});
		if (dbg())
			dbg()("[%8.3fs mux%u port %u rx %02x attr %02x, silo %u]\n",
					stamp(), pa, port, c, attr, unsigned(m_silo.size()));
	}

	// The port's input lines changed: DM, RR, IC, CS as a LINE_ mask of
	// what is asserted now.
	void modem_lines(unsigned port, u8 asserted)
	{
		if (port >= PORTS)
			return;
		port_state &P = ports[port];
		const u8 now = u8(asserted & (LINE_DM | LINE_RR | LINE_IC | LINE_CS));
		const u8 was = u8(P.lines & (LINE_DM | LINE_RR | LINE_IC | LINE_CS));
		P.lines = u8((P.lines & (LINE_TR | LINE_RS)) | now);
		const u8 changed = u8(now ^ was);
		if (!changed || port >= active_ports())
			return;
		// "If the corresponding bit is cleared in the mask, the firmware
		// will turn off the interrupt for the modem signal. This will
		// also stop the firmware from reporting any changes" (p.23). In
		// full duplex modem mode every line is enabled (p.62).
		const u8 mask = P.mode == 1 ? 0xf0 : m_modem_mask;
		if (!(changed & mask))
			return;
		if (P.quads)
			P.modem_changed = true;
		else
			push_modem_quad(port, 0);
		m_modem_event = true;
	}

	// The next byte the port's transmitter should send, if any: a UART
	// that has just finished a byte asks here rather than waiting for
	// the tick.
	bool next_tx(unsigned port, u8 &c)
	{
		if (port >= PORTS)
			return false;
		port_state &P = ports[port];
		if (P.tx_stopped || P.tx.empty())
			return false;
		c = P.tx.front();
		P.tx.pop_front();
		return true;
	}

	unsigned active_ports() const { return m_downloaded ? unsigned(PORTS) : 1u; }
	bool downloaded() const { return m_downloaded; }
	const port_state &port(unsigned n) const { return ports[n]; }
	const std::vector<u8> &ram() const { return m_ram; }

	// Start in the downloaded personality without a download.
	// FOCUS_MUXDOWNLOADED.
	void start_downloaded() { become_downloaded(); }

	//----------------------------------------------------------------
	//  CIO vocabulary
	//----------------------------------------------------------------

	enum : u8 { ORD_IDY = 0, ORD_DLD = 1, ORD_PAU = 2, ORD_DIS = 3,
			ORD_RS = 4, ORD_RD = 6, ORD_WD = 7, ORD_RTS = 8, ORD_WTC = 9,
			ORD_RDS = 0xa, ORD_CLC = 0xb };

	// PER low nibbles (p.39-40).
	enum : u8 { PER_COMMAND = 1, PER_ORDER = 2, PER_CONNECTED = 3,
			PER_PORT = 4, PER_SUBFUNCTION = 5, PER_LENGTH = 6,
			PER_TXOVERFLOW = 7, PER_PARAMETER = 8 };

	enum : u8 { CTL_ARD = 0x01, CTL_ARE = 0x02, CTL_RQA = 0x04,
			CTL_DEN = 0x10, CTL_DCL = 0x20 };

	//----------------------------------------------------------------
	//  iop_interface
	//----------------------------------------------------------------

	u32 status() override { return 1; }
	u32 status_reg() override { return 0; }
	u32 flag() override { return 1; }
	bool attention() override { return !m_events.empty() && !m_ard; }
	bool poll_request() override
	{
		switch (poll_mode)
		{
		case 0: return false;
		case 2: return service_request();
		default: return m_want_order;
		}
	}
	// An order whose data phase runs host-to-card is asking for it from
	// the moment it arrives; an answer the card has prepared is offered
	// until it is read out.
	bool service_request() override
	{
		return m_order == ORD_WD || m_order == ORD_DLD || !m_out.empty();
	}
	bool data_end() override { return m_answering && m_out.empty(); }

	u32 read_fr(unsigned n) override
	{
		u32 v = 0;
		switch (n)
		{
		case 0:
		case 6:
			v = pop_out();
			break;
		case 1:
			if (!m_echo.take(v))
			{
				if (fr1_force)
					v = fr1_force & 0xffff;
				else if (!m_echo.present())
					v = 0;
				else
					v = 0xc0 | (m_events.empty() ? 0 : 0x01);
			}
			break;
		case 3:
			v = 0;          // ready
			break;
		case 9:
			if (!m_events.empty())
			{
				v = m_events.front();
				m_events.pop_front();
				m_ard = false;
				if (dbg())
					dbg()("[%8.3fs mux%u event %02x read from FR9]\n",
							stamp(), pa, u8(v));
			}
			break;
		case 12:
			v = 0;
			break;
		default:
			v = m_fr[n & 15];
			break;
		}
		if (log.gate(log_key('R', n, v)))
			log("[mux%u R fr%u = %04x p=%08x]\n", pa, n, v & 0xffff,
					iop ? iop->tpc() : 0);
		return v;
	}

	void write_fr(unsigned n, u32 v) override
	{
		v &= 0xffff;
		if (log.gate(log_key('W', n, v)))
			log("[mux%u W fr%u = %04x p=%08x]\n", pa, n, v,
					iop ? iop->tpc() : 0);
		m_fr[n & 15] = v;
		switch (n)
		{
		case 0:
			m_echo.note_write(v);
			break;
		case 1:
			write_control(u8(v));
			break;
		case 8:
			write_order(u8(v));
			break;
		case 9:
			connect(v);
			break;
		default:
			break;
		}
	}

	u8 dma_read() override { return pop_out(); }
	void dma_write(u8 b) override
	{
		// A write block is 4 KB at most, but a download record is the
		// firmware image itself -- HP-UX sends its 5,330 bytes in one
		// record -- so the inbound buffer is bounded by the card's RAM,
		// not by the block limit.
		if (m_in.size() < m_ram.size() + 2)
			m_in.push_back(b);
	}

	void dma_complete(bool output) override
	{
		if (!output)
		{
			// The host read what it wanted of the answer; whatever is
			// left goes with the transaction.
			m_out.clear();
			m_answering = false;
		}
		else if (m_order == ORD_WD)
			take_write_block();
		else if (m_order == ORD_DLD)
			take_download();
		else
			m_in.clear();
		// "<-- SRQ  Card requests next order" ends every transaction
		// (ERS 4.3-4.6), and the completion the driver waits for follows.
		m_want_order = true;
		arm_event(event_done, "transfer done");
		start_event_timer();
	}

	//----------------------------------------------------------------
	//  the card's own time
	//----------------------------------------------------------------

	void tick()
	{
		if (m_event_timer && --m_event_timer == 0 && !m_pending.empty())
		{
			const u8 code = m_pending.front();
			m_pending.pop_front();
			if (dbg())
				dbg()("[%8.3fs mux%u event %02x queued, %u pending]\n",
						stamp(), pa, code, unsigned(m_pending.size()));
			m_events.push_back(code);
			if (iop)
				iop->raise(pa);
			start_event_timer();
		}
		// The transmitters. A port with no sink drains into nothing, so
		// a card nobody is listening to never fills up and stalls.
		for (unsigned p = 0; p < active_ports(); p++)
		{
			port_state &P = ports[p];
			while (!P.tx_stopped && !P.tx.empty())
			{
				if (on_tx && !on_tx(p, P.tx.front()))
					break;
				P.tx.pop_front();
			}
		}
		// One asynchronous event between reads, sensed whatever the host
		// is doing: data waiting in the silo after the firmware's clock
		// delay, or a modem line that changed.
		if (!no_async && !m_aes_outstanding)
		{
			if ((m_host_mask & 0x01) && !m_silo.empty())
			{
				if (++m_aes_ticks >= (aes_delay_ms ? aes_delay_ms : 1))
				{
					m_aes_ticks = 0;
					m_aes_outstanding = true;
					arm_event(event_aes, "data available");
					start_event_timer();
				}
			}
			else if ((m_host_mask & 0x08) && m_modem_event)
			{
				m_modem_event = false;
				m_aes_outstanding = true;
				arm_event(u8(event_aes + 3), "modem line changed");
				start_event_timer();
			}
		}
	}

	std::deque<u8> m_events;    // event codes waiting to be read from FR9

private:
	void write_control(u8 c)
	{
		if (c == 2 && !m_events.empty())
		{
			m_events.pop_front();
			m_ard = false;
			return;
		}
		if (c & CTL_ARD)
			m_ard = true;
		if (c & CTL_DCL)
			device_clear();
	}

	// The CSC command. "The command should contain the SRQ address in the
	// low nibble of the byte. If this command is not issue, the card has
	// no way of doing an SRQ" (p.27). The kernel issues it to every slot
	// at the powerup scan and again at each open; a second connection is
	// PER 73 by the book, but the same kernel re-issues its FR9 handshake
	// three times to the ASI's console driver and expects an answer each
	// time, so a repeat is answered, not refused.
	void connect(u32 v)
	{
		m_config = v;
		m_connected = true;
		if (dbg())
			dbg()("[%8.3fs mux%u CSC %02x]\n", stamp(), pa, u8(v));
		arm_event(event_csc, "CSC");
		start_event_timer();
	}

	void write_order(u8 o)
	{
		m_order = u8(o >> 4);
		m_byte_mode = (o & 0x08) != 0;
		m_option = u8(o & 0x07);
		m_in.clear();
		m_out.clear();
		m_answering = false;
		m_want_order = false;
		if (dbg())
			dbg()("[%8.3fs mux%u order %02x (%s%s opt %u)]\n", stamp(), pa,
					o, order_name(m_order), m_byte_mode ? ",b" : "",
					m_option);
		switch (m_order)
		{
		case ORD_IDY:
			for (u8 b : identity_block())
				m_out.push_back(b);
			answer();
			break;
		case ORD_DLD:
		case ORD_WD:
			// The block follows on the outbound leg; dma_complete reads
			// it.
			break;
		case ORD_DIS:
			// "The card does this by removing the SRQ address. No SRQ is
			// sent to the host for the next order" (p.32).
			m_connected = false;
			arm_event(event_done, "disconnect");
			start_event_timer();
			break;
		case ORD_RD:
			build_read_block();
			answer();
			break;
		case ORD_RDS:
			// "If the read download status order is issued without a
			// previous download control request with subfunction 3, the
			// firmware will return one byte with all one's in place of
			// the status code indicating no status" (p.66).
			if (m_verified)
			{
				m_out.push_back(3);
				m_out.push_back(1);
				m_out.push_back(m_verify_result);
				m_verified = false;
			}
			else
				m_out.push_back(0xff);
			answer();
			break;
		default:
			// PAU, RS, RTS, WTC, CLC: "not used" by this protocol -- and
			// RTS or CLC would switch the real EPROM into its ASI
			// log-channel personality (p.C-1), which is not modelled. Say
			// so where it will be seen.
			dbg()("[%8.3fs mux%u UNSUPPORTED ORDER %02x (%s) -- refused "
					"with PER]\n", stamp(), pa, o, order_name(m_order));
			raise_per(per_order);
			break;
		}
	}

	static const char *order_name(u8 o)
	{
		switch (o)
		{
		case ORD_IDY: return "IDY";
		case ORD_DLD: return "DLD";
		case ORD_PAU: return "PAU";
		case ORD_DIS: return "DIS";
		case ORD_RS:  return "RS";
		case ORD_RD:  return "RD";
		case ORD_WD:  return "WD";
		case ORD_RTS: return "RTS";
		case ORD_WTC: return "WTC";
		case ORD_RDS: return "RDS";
		case ORD_CLC: return "CLC";
		default:      return "?";
		}
	}

	// The identity block (p.41-42): card ID, firmware ID, the date code,
	// MPX = 3 "for other backplane protocol" over MOD = 2 "for both byte
	// and word mode data transfers", and the active port count.
	std::vector<u8> identity_block() const
	{
		return {
			u8(identity >> 8),
			u8(m_downloaded ? 3 : (identity & 0xff)),
			u8(date_code >> 8), u8(date_code),
			u8((3 << 4) | 2),
			u8(active_ports() - 1)
		};
	}

	// The read data/status block (p.43-47).
	void build_read_block()
	{
		m_out.clear();
		while (!m_silo.empty() && m_out.size() + 4 + 1 <= RDMAX)
		{
			const quad q = m_silo.front();
			m_silo.pop_front();
			m_out.push_back(q.port);
			m_out.push_back(q.attr);
			m_out.push_back(1);
			m_out.push_back(q.ch);
		}
		for (unsigned p = 0; p < active_ports(); p++)
		{
			port_state &P = ports[p];
			// A modem quad owed under subfunction 152: "the quad will be
			// generated at the time the host post a read data/status
			// transaction and the quad will be returned in that
			// transaction" (p.22).
			if (P.modem_changed)
			{
				P.modem_changed = false;
				if (m_out.size() + 4 + 1 <= RDMAX)
				{
					m_out.push_back(u8(p));
					m_out.push_back(ATTR_MODEM);
					m_out.push_back(1);
					m_out.push_back(modem_status_byte(P, 0));
				}
			}
			while (!P.status.empty())
			{
				const std::vector<u8> &s = P.status.front();
				if (m_out.size() + 2 + s.size() + 1 > RDMAX)
					break;
				m_out.push_back(u8(0x80 | p));
				m_out.push_back(s[0]);
				m_out.push_back(u8(s.size() - 1));
				m_out.insert(m_out.end(), s.begin() + 1, s.end());
				P.status.pop_front();
			}
			// Status 128, "the number of bytes remaining in the transmit
			// buffer", on every read (sf 154, the default) or when it
			// changed (sf 155).
			const unsigned left = unsigned(P.tx.size());
			if ((m_status128_every || left != P.last_tx_count)
					&& m_out.size() + 4 + 1 <= RDMAX)
			{
				m_out.push_back(u8(0x80 | p));
				m_out.push_back(128);
				m_out.push_back(1);
				m_out.push_back(u8(left));
				P.last_tx_count = left;
			}
		}
		m_out.push_back(0xff);
		// Reading is what lets the next asynchronous event be told.
		m_aes_outstanding = false;
		m_aes_ticks = 0;
		if (dbg())
			dbg()("[%8.3fs mux%u RD block %u bytes, silo %u left]\n",
					stamp(), pa, unsigned(m_out.size()),
					unsigned(m_silo.size()));
	}

	// The modem status word a modem-change quad carries (p.46): the
	// current lines in bits 7:4 and the modem error code in 3:0.
	u8 modem_status_byte(const port_state &P, u8 code) const
	{
		return u8((P.lines & 0xf0) | (code & 0x0f));
	}

	void push_modem_quad(unsigned p, u8 code)
	{
		if (m_silo.size() >= SILO)
			return;
		m_silo.push_back({u8(p), ATTR_MODEM, modem_status_byte(ports[p], code)});
	}

	// The write data/configuration/control block (p.50-51). "THE
	// FIRMWARE STOPS PROCESSING THE WRITE DATA/CONFIGURATION/CONTROL
	// TRANSACTION UPON ENCOUNTERING THE FIRST ERROR" (p.40).
	void take_write_block()
	{
		if (log)
			log_block("WD");
		size_t i = 0;
		const size_t n = m_in.size();
		unsigned data_bytes = 0, controls = 0;
		while (i < n)
		{
			const u8 h = m_in[i];
			if (h == 0xff)
				break;
			const unsigned p = h & 0x7f;
			if (p >= active_ports())
			{
				raise_per(PER_PORT);
				break;
			}
			if (i + 3 > n)
			{
				raise_per(PER_LENGTH);
				break;
			}
			if (!(h & 0x80))
			{
				// A data block: reserved byte, length 1..255, the data.
				const unsigned len = m_in[i + 2];
				if (len == 0 || i + 3 + len > n)
				{
					raise_per(PER_LENGTH);
					break;
				}
				port_state &P = ports[p];
				if (P.tx.size() + len > TXBUF)
				{
					raise_per(PER_TXOVERFLOW);
					break;
				}
				for (unsigned k = 0; k < len; k++)
					P.tx.push_back(m_in[i + 3 + k]);
				data_bytes += len;
				i += 3 + len;
			}
			else
			{
				const u8 sf = m_in[i + 1];
				const unsigned cnt = m_in[i + 2];
				if (i + 3 + cnt > n)
				{
					raise_per(PER_LENGTH);
					break;
				}
				const u8 err = subfunction(p, sf, &m_in[i + 3], cnt);
				controls++;
				if (err)
				{
					if (dbg())
						dbg()("[%8.3fs mux%u port %u sf %u refused, PER %u]\n",
								stamp(), pa, p, sf, err);
					raise_per(err);
					break;
				}
				i += 3 + cnt;
			}
		}
		if (dbg() && (data_bytes || controls))
			dbg()("[%8.3fs mux%u WD: %u data bytes, %u control blocks]\n",
					stamp(), pa, data_bytes, controls);
		m_in.clear();
	}

	// One configuration or control subfunction (ERS 5.3.2). Returns the
	// PER low nibble, or 0.
	u8 subfunction(unsigned p, u8 sf, const u8 *a, unsigned n)
	{
		port_state &P = ports[p];
		auto need = [&](unsigned k) { return n >= k; };
		if (dbg())
		{
			std::string hex;
			char b2[4];
			for (unsigned k = 0; k < n && k < 16; k++)
			{
				std::snprintf(b2, sizeof b2, " %02x", a[k]);
				hex += b2;
			}
			dbg()("[%8.3fs mux%u port %u sf %u%s]\n", stamp(), pa, p, sf,
					hex.c_str());
		}
		switch (sf)
		{
		case 1:
		{
			if (!need(1)) return PER_LENGTH;
			const u8 mode = a[0] & 0x03;
			if (mode != 1 && mode != 2) return PER_PARAMETER;
			const u8 was = P.mode & 0x03;
			P.mode = u8(mode | (a[0] & 0x10));
			if (mode == 1 && was != 1)
			{
				// Hardwired to modem: with every modem signal asserted
				// the link is implicitly connected, else it is
				// disconnected with error code 7 (p.53). No AES either
				// way.
				const u8 up = LINE_DM | LINE_CS | LINE_RR;
				if (P.tr && P.rs && (P.lines & up) == up)
				{
					P.link = true;
					push_modem_quad(p, 1);
				}
				else
				{
					P.link = false;
					P.tr = P.rs = false;
					push_modem_quad(p, 7);
					if (on_lines) on_lines(p);
				}
			}
			if (on_config) on_config(p);
			return 0;
		}
		case 2:
			if (!need(1)) return PER_LENGTH;
			if ((a[0] & 0x1f) < 1 || (a[0] & 0x1f) > 0x11) return PER_PARAMETER;
			P.baud = u8(a[0] & 0x1f);
			if (on_config) on_config(p);
			return 0;
		case 3:
			if (!need(1)) return PER_LENGTH;
			if ((a[0] & 0x03) == 3 || ((a[0] >> 2) & 0x03) == 3) return PER_PARAMETER;
			P.frame = u8(a[0] & 0x3f);
			if (on_config) on_config(p);
			return 0;
		case 4:
			if (!need(1)) return PER_LENGTH;
			P.timer_connect = a[0];
			return 0;
		case 5:
			if (!need(2)) return PER_LENGTH;
			P.timer_noactivity = u16((a[0] << 8) | a[1]);
			return 0;
		case 6:
			if (!need(1)) return PER_LENGTH;
			P.timer_rr = a[0];
			return 0;
		case 7:
			if (!need(2)) return PER_LENGTH;
			if (a[0] == a[1] && a[0] != 0xff) return PER_PARAMETER;
			P.xon_dev = a[0];
			P.xoff_dev = a[1];
			return 0;
		case 8:
			if (!need(2)) return PER_LENGTH;
			if (a[0] == a[1]) return PER_PARAMETER;
			P.xon_host = a[0];
			P.xoff_host = a[1];
			return 0;
		case 128:
			// Open the modem link: a nop unless in modem mode (p.57).
			if ((P.mode & 3) == 1 && !P.link)
			{
				P.tr = P.rs = true;
				if (on_lines) on_lines(p);
				P.link = true;
				push_modem_quad(p, (P.mode & 0x10) ? 10 : 1);
				m_modem_event = true;
			}
			return 0;
		case 129:
			if ((P.mode & 3) == 1 && P.link)
			{
				P.tr = P.rs = false;
				P.link = false;
				P.tx.clear();
				if (on_lines) on_lines(p);
				push_modem_quad(p, 9);
				m_modem_event = true;
			}
			return 0;
		case 130:
			if (on_break) on_break(p, true);
			return 0;
		case 131:
			if (on_break) on_break(p, false);
			return 0;
		case 132: P.rs = false; if (on_lines) on_lines(p); return 0;
		case 133: P.rs = true;  if (on_lines) on_lines(p); return 0;
		case 134: P.tr = false; if (on_lines) on_lines(p); return 0;
		case 135: P.tr = true;  if (on_lines) on_lines(p); return 0;
		case 136: P.sr = false; if (on_lines) on_lines(p); return 0;
		case 137: P.sr = true;  if (on_lines) on_lines(p); return 0;
		case 138:
			P.tx.clear();
			return 0;
		case 139:
			// "The transmitter will be put into a state like if an X-OFF
			// was received."
			P.tx_stopped = true;
			return 0;
		case 140:
			P.tx_stopped = false;
			return 0;
		case 141:
			P.status.push_back({141, line_state(P)});
			return 0;
		case 142:
			// "If the transmission mode is full duplex hardwired, the
			// status will always be an one" (p.58).
			P.status.push_back({142, u8((P.mode & 3) == 1 ? (P.link ? 1 : 0) : 1)});
			return 0;
		case 143:
		{
			if (!need(3)) return PER_LENGTH;
			const unsigned addr = (a[0] << 8) | a[1];
			const unsigned len = a[2] ? a[2] : 256;
			std::vector<u8> s{143};
			for (unsigned k = 0; k < len; k++)
				s.push_back(m_ram[(addr + k) & 0xffff]);
			P.status.push_back(std::move(s));
			return 0;
		}
		case 144:
		{
			if (!need(1)) return PER_LENGTH;
			std::vector<u8> s{144};
			s.insert(s.end(), a, a + n);
			P.status.push_back(std::move(s));
			return 0;
		}
		case 145:
			if (!need(1)) return PER_LENGTH;
			P.leds = a[0];
			return 0;
		case 146:
			if (!need(1)) return PER_LENGTH;
			m_host_mask = a[0];
			if (dbg())
				dbg()("[%8.3fs mux%u host interrupt mask %02x]\n", stamp(),
						pa, m_host_mask);
			return 0;
		case 147: P.rx_enabled = false; return 0;
		case 148: P.rx_enabled = true;  return 0;
		case 149:
			P.tx.push_front(P.xon_host);
			P.tx_stopped = false;
			return 0;
		case 150:
			P.tx.push_front(P.xoff_host);
			return 0;
		case 151: P.quads = false; return 0;
		case 152: P.quads = true;  return 0;
		case 153:
			if (!need(1)) return PER_LENGTH;
			m_modem_mask = u8(a[0] & 0xf0);
			return 0;
		case 154: m_status128_every = true;  return 0;
		case 155: m_status128_every = false; return 0;
		default:
			return PER_SUBFUNCTION;
		}
	}

	// Status 141's byte: the output lines TR and RS as set by the host,
	// the input lines as the device drives them.
	static u8 line_state(const port_state &P)
	{
		return u8((P.lines & 0xf0) | (P.tr ? LINE_TR : 0) | (P.rs ? LINE_RS : 0));
	}

	// The download transaction (ERS 5.4).
	void take_download()
	{
		if (log)
			log_block("DLD");
		if (m_option == 1)
		{
			if (m_in.size() < 2)
			{
				raise_per(PER_LENGTH);
				m_in.clear();
				return;
			}
			const u8 sf = m_in[0];
			const unsigned cnt = m_in[1];
			const u8 *a = m_in.data() + 2;
			const bool whole = m_in.size() >= 2 + cnt;
			switch (sf)
			{
			case 1:
				// "Initialize the card for downloading by resetting the
				// SCC ... and by initializing the RAM variables required
				// by the downloader" (p.64).
				std::fill(m_ram.begin(), m_ram.end(), 0);
				m_verified = false;
				m_dl_addr = 0;
				m_dl_len = 0;
				if (dbg())
					dbg()("[%8.3fs mux%u download: initialise]\n", stamp(), pa);
				break;
			case 2:
				if (!whole || cnt < 8)
				{
					raise_per(PER_LENGTH);
					break;
				}
				m_dl_addr = unsigned((a[2] << 8) | a[3]);
				m_dl_len = unsigned((a[6] << 8) | a[7]);
				if (dbg())
					dbg()("[%8.3fs mux%u download: record at %04x, %u bytes]\n",
							stamp(), pa, m_dl_addr, m_dl_len);
				break;
			case 3:
				// "The card will perform a CRC checksum of the firmware
				// and return the result ... A zero value implys no error."
				m_verified = true;
				m_verify_result = 0;
				if (dbg())
					dbg()("[%8.3fs mux%u download: verify, %u bytes loaded]\n",
							stamp(), pa, m_dl_loaded);
				break;
			case 4:
			{
				const unsigned start = whole && cnt >= 4
						? unsigned((a[2] << 8) | a[3]) : 0;
				if (dbg())
					dbg()("[%8.3fs mux%u download: start RAM execution at "
							"%04x]\n", stamp(), pa, start);
				become_downloaded();
				break;
			}
			default:
				raise_per(PER_SUBFUNCTION);
				break;
			}
		}
		else if (m_option == 0)
		{
			// A data record: the binary image for the address the last
			// control request named. The IOP moves halfwords, so an
			// odd-length record arrives with one padding byte.
			if (m_in.size() > m_dl_len + 1)
			{
				raise_per(PER_LENGTH);
				m_in.clear();
				return;
			}
			const unsigned len = std::min<unsigned>(unsigned(m_in.size()), m_dl_len);
			for (unsigned k = 0; k < len; k++)
				m_ram[(m_dl_addr + k) & 0xffff] = m_in[k];
			m_dl_loaded += len;
			if (dbg())
				dbg()("[%8.3fs mux%u download: %u bytes into %04x]\n",
						stamp(), pa, len, m_dl_addr);
		}
		else
			raise_per(PER_ORDER);
		m_in.clear();
	}

	// "The downloaded firmware should be running at this point" (p.38):
	// six ports, firmware ID 3, and every receiver disabled until the
	// host enables it (ERS ch.6: "SCC receiver: enable for EPROM firmware
	// / disable for downloaded firmware").
	void become_downloaded()
	{
		m_downloaded = true;
		for (port_state &P : ports)
		{
			P.rx_enabled = false;
			P.tx.clear();
			P.tx_stopped = false;
			P.last_tx_count = ~0u;
		}
		m_host_mask = 0;
		m_modem_mask = 0;
		m_status128_every = true;
		m_silo.clear();
		m_aes_outstanding = false;
		m_modem_event = false;
		if (dbg())
			dbg()("[%8.3fs mux%u now running the downloaded firmware: 6 "
					"ports, receivers disabled]\n", stamp(), pa);
	}

	void raise_per(u8 code)
	{
		arm_event(u8(event_per | (code & 0x0f)), "protocol error");
		start_event_timer();
	}

	void log_block(const char *what)
	{
		std::string hex;
		char b2[4];
		for (size_t k = 0; k < m_in.size() && k < 96; k++)
		{
			std::snprintf(b2, sizeof b2, " %02x", m_in[k]);
			hex += b2;
		}
		log("[mux%u %s block %u bytes:%s%s]\n", pa, what,
				unsigned(m_in.size()), hex.c_str(),
				m_in.size() > 96 ? " ..." : "");
	}

	void arm_event(u8 code, const char *why)
	{
		if (std::find(m_pending.begin(), m_pending.end(), code)
				!= m_pending.end())
			return;
		if (dbg())
			dbg()("[%8.3fs mux%u event %02x armed (%s), %u pending]\n",
					stamp(), pa, code, why, unsigned(m_pending.size()) + 1);
		m_pending.push_back(code);
	}

	void start_event_timer()
	{
		if (!m_event_timer && !m_pending.empty())
			m_event_timer = event_delay_ms ? event_delay_ms : 1;
	}

	void answer()
	{
		m_answering = true;
		arm_event(event_done, "answer");
	}

	void device_clear()
	{
		m_in.clear();
		m_out.clear();
		m_answering = false;
		m_order = 0xff;
	}

	u8 pop_out()
	{
		if (m_out.empty())
			return 0;
		const u8 b = m_out.front();
		m_out.pop_front();
		return b;
	}

	presence_echo m_echo;
	u32 m_fr[16] = {};
	std::deque<u8> m_pending;
	unsigned m_event_timer = 0;
	std::deque<u8> m_out;
	std::vector<u8> m_in;
	std::deque<quad> m_silo;
	std::vector<u8> m_ram = std::vector<u8>(65536, 0);

	u8 m_order = 0xff;
	u8 m_option = 0;
	bool m_byte_mode = false;
	bool m_answering = false;
	bool m_want_order = false;
	bool m_connected = false;
	bool m_ard = false;
	u32 m_config = 0;

	bool m_downloaded = false;
	bool m_verified = false;
	u8 m_verify_result = 0;
	unsigned m_dl_addr = 0, m_dl_len = 0, m_dl_loaded = 0;

	u8 m_host_mask = 0;         // sf 146: bit 0 data, bit 3 modem
	u8 m_modem_mask = 0;        // sf 153: bits 7:4 CS/IC/RR/DM
	bool m_status128_every = true;
	bool m_aes_outstanding = false;
	bool m_modem_event = false;
	unsigned m_aes_ticks = 0;
};

} // namespace focus

#endif // MAME_IOP_FOCUSMUX_H
