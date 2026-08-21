// license:BSD-3-Clause
// copyright-holders:
/***************************************************************************

    HP 27110A HP-IB Interface, and the CS/80 disc on the other end of it.

    This is the 530/540's MASS STORAGE path. The CE handbook's install
    procedure puts the system disc's HP-IB card in slot 5 with the drive
    at HP-IB address 0 (9030/40 CE Handbook, 2-3); HP-UX carries the
    driver as module HP27110, and reaches the machine's built-in drive
    through a second one it calls INTERNAL_HP-IB. Even the loader ROM
    speaks this language -- its error codes are "CS80 DEVICE: SELECT CODE
    NN" and "HPIB CARD: SELECT CODE NN" (530/540 Service, 3-13).

    Two documents meet in this file. The card's own protocol is from the
    HP 27110A Technical Reference (27132-90002, Jun 83), cited by sheet;
    what it carries is CS/80, from the Instruction Set Programming Manual
    (5955-3442), cited by section. How either reaches the IOP's FR
    registers is the house protocol recovered by driving the serial card
    (focusasi.h) -- FR1 status with bits 7 and 6, FR9 configuration,
    FR8 command, ICEI command lists, event codes read back from FR9.

    THE CARD (27110A Tech Ref, 4-8..4-31). A host request is a
    transaction of up to three parts -- Control block, Data block, Status
    block, "CDS" -- and the control block's first byte routes on bits
    7:6:

      11  raw HP-IB data transfer
      10  CS/80 (bit 5 clear) or mag tape (bit 5 set)
      01  reserved
      00  general card control, opcode in bits 5:0

    with bits 3:0 carrying data-present, read/write and CRC flags. The
    general opcodes are table 4-1: 1/2 set/clear REN, 3 send IFC, 4/5
    configure/read the PPOLL response, 6 configure SRQ, 7 read reason for
    interrupt, 8/9 write/read interrupt mask, A wait for interrupt,
    B go online, C go offline, D/E assert/deassert ATN, F PPOLL sense,
    10 conduct PPOLL, 11/12 configure/read the HP-IB address, 13 AMIGO
    identify configuration, 14 last secondary address, 15/16 freeze on
    parity failure on and off.

    A CS/80 transaction (4-30) carries a don't-wait flag and SPD/UNT/UNL
    bits, then the device's bus address and a secondary -- F2 control or
    E5 transparent -- and then the CS/80 command message itself. The card
    runs the whole bus dialogue on the host's behalf: talk address, UNL,
    listen address, secondary, data, PPOLL wait. It answers with a status
    block of TSTAT, then the device's QSTAT, and -- if QSTAT is not zero
    -- the 20-byte Request Status report it fetched by itself. That
    auto-fetch is the card's most useful behaviour and is modelled here.

    TSTAT is the card's own view of how the transfer ended (table 4-2):
    0 no exceptional condition, 1/2 terminated by EOI, 3/4 by count, 5/6
    by IF, 7/8 by MSA, the even values meaning an odd byte count.

    THE DEVICE. A CS/80 unit sits behind the card at a bus address. Its
    command set, status report and Describe layout are the manual's, and
    the same rules the internal disc's controller taught apply: a volume
    that means to be block-addressed must describe max cylinder, head and
    sector as zero, and QSTAT reads 2 from cold until a Request Status
    collects the power-on condition.

***************************************************************************/

#ifndef MAME_IOP_FOCUSHPIB_H
#define MAME_IOP_FOCUSHPIB_H

#pragma once

#include "focusiop.h"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <vector>

namespace focus {

//**************************************************************************
//  THE DEVICE ON THE BUS
//**************************************************************************

// One CS/80 device: a disc with units and volumes. The card below drives
// it; nothing here knows about the FOCUS side.
struct cs80_device
{
	static constexpr u32 BLOCK = 256;

	// What Describe reports. The defaults are a 7914 -- the drive whose
	// unit save carries the HP-UX this emulates -- and can be overridden
	// to match another; real captured Describe blocks for several drives
	// are in docs/bitsavers/cs80describes/.
	u32 device_number = 0x079140;   // BCD product number, 6 digits
	u8 generic_type = 0;            // 0 fixed disc, 1 removable, 2 tape

	std::vector<u8> image;          // the medium, one byte per byte
	bool loaded() const { return !image.empty(); }
	u32 blocks() const { return u32(image.size() / BLOCK); }

	// QSTAT (CS/80 3-13): 0 normal, 1 hard error -- ask Request Status,
	// 2 power on. A device answers 2 from cold until a Request Status
	// collects it.
	u8 qstat() const { return m_poweron ? 2 : (flags_up() ? 1 : 0); }

	// Run one CS/80 command message, appending whatever execution message
	// it produces to `out`.
	void command(const std::vector<u8> &cmd, std::deque<u8> &out, bool log = false)
	{
		for (size_t i = 0; i < cmd.size(); )
		{
			const u8 op = cmd[i++];
			// Set Unit 0x20+u, unit 15 the controller (CS/80 2-12).
			if (op >= 0x20 && op <= 0x2f)
			{
				m_unit = op & 15;
				if (m_unit != 0 && m_unit != 15)
					set_flag(6);        // module addressing
				continue;
			}
			if (op >= 0x40 && op <= 0x47) { m_volume = op & 7; continue; }
			if (op >= 0x48 && op <= 0x4f) { continue; }  // return addressing mode
			switch (op)
			{
			case 0x10: case 0x11:       // set address, 6-byte parameter
				m_address = 0;
				for (unsigned k = 0; k < 6 && i < cmd.size(); k++)
					m_address = (m_address << 8) | cmd[i++];
				break;
			case 0x18:                  // set length, 4 bytes
				m_length = 0;
				for (unsigned k = 0; k < 4 && i < cmd.size(); k++)
					m_length = (m_length << 8) | cmd[i++];
				break;
			case 0x00: locate_read(out); break;
			case 0x02: m_writing = true; m_write_addr = m_address; break;
			case 0x0a: locate_read(out); break;   // cold load read
			case 0x35: describe(out); break;
			case 0x0d: request_status(out); break;
			case 0x30: case 0x31: case 0x32: case 0x33:
				i += 8;                 // initiate utility, up to 8 bytes
				break;
			case 0x38: i += 1; break;   // set options
			case 0x39: i += 4; break;   // set RPS
			case 0x3a: i += 2; break;   // set retry time
			case 0x3b: i += 1; break;   // set release
			case 0x3c: case 0x3d: i += 1; break;   // set burst
			case 0x3e: i += 8; break;   // set status mask
			case 0x12: i += 6; break;   // set block displacement
			case 0x34:                  // no op
			case 0x0e: case 0x0f:       // release / release denied
				break;
			case 0x09:
				// Sent by the 520's request wrapper and absent from the
				// CS/80 standard's own opcode table (appendix B has no
				// 0x09). Accepted, as the real device evidently does.
				break;
			default:
				set_flag(5);            // illegal opcode
				if (log)
				{
					std::printf("[cs80 op %02x unhandled]\n", op);
					std::fflush(stdout);
				}
				return;
			}
		}
	}

	// A write's data arrived.
	void commit_write(const std::vector<u8> &data)
	{
		if (!m_writing)
			return;
		m_writing = false;
		if (image.empty())
		{
			set_flag(35);               // not ready
			return;
		}
		u64 off = u64(m_write_addr) * BLOCK;
		for (size_t k = 0; k < data.size() && off + k < image.size(); k++)
			image[off + k] = data[k];
	}

	bool writing() const { return m_writing; }

private:
	void locate_read(std::deque<u8> &out)
	{
		if (image.empty())
		{
			set_flag(35);               // not ready
			return;
		}
		const u64 off = u64(m_address) * BLOCK;
		u32 len = m_length;
		if (len == 0xffffffffu)
			len = BLOCK;
		if (off + len > image.size())
		{
			set_flag(7);                // address bounds
			return;
		}
		for (u32 k = 0; k < len; k++)
			out.push_back(image[off + k]);
	}

	// Describe (CS/80 2-25, table 2-4): 5 controller bytes, 19 unit,
	// 13 volume. Addressed to the controller only the first field comes
	// back; addressed to a unit, all three.
	void describe(std::deque<u8> &out)
	{
		auto b = [&out](u8 v) { out.push_back(v); };
		// controller: installed-unit mask, max instantaneous transfer
		// rate in kB/s, controller type (1 = integrated multi-unit)
		b(0x00); b(0x01);
		b(0x00); b(0x2d);
		b(0x01);
		if (m_unit == 15)
			return;
		// unit
		b(generic_type);
		b(u8(device_number >> 16)); b(u8(device_number >> 8)); b(u8(device_number));
		b(BLOCK >> 8); b(BLOCK & 0xff);     // bytes per block
		b(1);                               // blocks bufferable
		b(0);                               // recommended burst size
		b(0x00); b(0x20);                   // block time, microseconds
		b(0x00); b(0x2d);                   // continuous transfer rate
		b(0x00); b(0x0a);                   // optimal retry time
		b(0x00); b(0x0a);                   // access time
		b(1);                               // max interleave
		b(0x01);                            // fixed volumes: volume 0
		b(0x00);                            // removable volumes: none
		// volume: block-addressed, so max cylinder, head and sector are
		// all zero and only the single-vector maximum is real -- the
		// rule the internal disc's driver enforces (see focusdisc.h).
		b(0); b(0); b(0);
		b(0);
		b(0); b(0);
		const u32 maxblk = image.empty() ? 0 : u32(image.size() / BLOCK - 1);
		b(0); b(0); b(0);
		b(u8(maxblk >> 16)); b(u8(maxblk >> 8)); b(u8(maxblk));
		b(1);                               // current interleave
	}

	// Request Status (CS/80 2-35, table 2-5): 2 identification bytes,
	// 8 of error flags, 10 parameter bytes. Reading clears the report.
	void request_status(std::deque<u8> &out)
	{
		out.push_back(u8((m_volume << 4) | (m_unit & 15)));
		out.push_back(0);
		if (m_poweron)
			set_flag(30);               // power fail
		for (unsigned k = 0; k < 8; k++)
			out.push_back(m_flags[k]);
		for (unsigned k = 0; k < 10; k++)
			out.push_back(0);
		m_poweron = false;
		for (auto &f : m_flags)
			f = 0;
	}

	void set_flag(unsigned bit) { m_flags[bit / 8] |= u8(0x80 >> (bit & 7)); }
	bool flags_up() const
	{
		for (u8 f : m_flags)
			if (f)
				return true;
		return false;
	}

	unsigned m_unit = 0;
	unsigned m_volume = 0;
	u32 m_address = 0;
	u32 m_length = 0xffffffffu;
	u32 m_write_addr = 0;
	bool m_writing = false;
	bool m_poweron = true;
	u8 m_flags[8] = {};
};


//**************************************************************************
//  THE CARD
//**************************************************************************

struct hpib_card : public iop_interface
{
	iop_core *iop = nullptr;
	unsigned pa = 5;
	bool log = false;

	// The halfword the identify transaction answers, which is what types
	// the slot. Which value binds HP-UX's HP27110 driver is not yet
	// known; FOCUS_HPIBIDENT sweeps it.
	u16 identity = 0;

	// The device on the bus, at its HP-IB address. The install procedure
	// sets the system disc to address 0.
	cs80_device disc;
	u8 device_address = 0;

	// Event codes, classified by high nibble as on every card here. The
	// kernel's handler (CORE SYSTEM 0x26020) reads the code, masks 0xf0
	// and computed-branches on it: 0x00 and 0x30 do nothing, 0x10, 0x20
	// and 0x40 are completion classes, above 0x40 is an error.
	enum : u8 { EVENT_DONE = 0x10 };
	u8 event_done = EVENT_DONE;

	// A completion must not arrive before the driver is waiting for it.
	// The handler's completion path tests a byte at the device record's
	// +201 -- "a transfer is outstanding" -- and returns silently when it
	// is clear, so an event posted synchronously inside the transfer is
	// simply thrown away. Real hardware takes microseconds to move the
	// data and the driver is always parked by the time it lands; here the
	// card takes a millisecond or two of the host's tick instead.
	unsigned event_delay_ms = 2;

	// One millisecond of the card's own time.
	void tick()
	{
		if (m_event_timer && --m_event_timer == 0 && m_pending_event)
		{
			m_events.push_back(m_pending_event);
			m_pending_event = 0;
			if (iop)
				iop->raise(pa);
		}
	}

	// Log capping, for the same reason as the serial card: a driver
	// polling status outruns any useful log in a second of emulated time.
	unsigned log_max = 2000;
	bool log_line(char dir, unsigned n, u32 v)
	{
		if (!log)
			return false;
		const u32 key = (u32(dir) << 24) | (n << 16) | (v & 0xffff);
		if (key == m_log_last) { m_log_repeat++; return false; }
		if (m_log_repeat)
		{
			std::printf("[hpib%u ... x%u more]\n", pa, m_log_repeat);
			m_log_repeat = 0;
		}
		m_log_last = key;
		if (m_log_lines++ >= log_max)
		{
			if (m_log_lines == log_max + 1)
			{
				std::printf("[hpib%u log capped]\n", pa);
				std::fflush(stdout);
			}
			return false;
		}
		return true;
	}

	//----------------------------------------------------------------
	//  iop_interface
	//----------------------------------------------------------------

	u32 status() override { return 1; }
	u32 status_reg() override { return 0; }
	u32 flag() override { return 1; }

	bool attention() override { return !m_events.empty() && !m_ard; }

	bool service_request() override { return !m_out.empty() || m_want_in; }
	bool poll_request() override { return false; }

	u32 read_fr(unsigned n) override
	{
		u32 v = 0;
		switch (n)
		{
		case 0:
			v = m_out.empty() ? 0 : m_out.front();
			break;
		case 1:
			if (m_echo_pending)
			{
				v = m_last_fr0;
				m_echo_pending = false;
				m_present = true;
			}
			else if (!m_present)
				v = 0;
			else
				// The house status register: bit 7 configured, bit 6
				// self test complete, bit 0 an event waiting.
				v = 0xc0 | (m_events.empty() ? 0 : 0x01);
			break;
		case 3:
			// The registration gate wants zero here.
			v = 0;
			break;
		case 9:
			// Handing the code over consumes it. The kernel's handler
			// reads FR1 for the waiting bit and then reads here, and it
			// writes no acknowledgement back -- with the code left
			// standing it reads the same event forever. (The internal
			// disc's BASIC driver does acknowledge, with a write of 2 to
			// FR1, which is still honoured.)
			if (!m_events.empty())
			{
				v = m_events.front();
				m_events.pop_front();
				m_ard = false;
			}
			break;
		default:
			v = m_fr[n & 15];
			break;
		}
		if (log_line('R', n, v))
		{
			std::printf("[hpib%u R fr%u = %04x p=%08x]\n", pa, n, v & 0xffff,
					iop ? iop->tpc() : 0);
			std::fflush(stdout);
		}
		return v;
	}

	void write_fr(unsigned n, u32 v) override
	{
		v &= 0xffff;
		if (log_line('W', n, v))
		{
			std::printf("[hpib%u W fr%u = %04x p=%08x]\n", pa, n, v,
					iop ? iop->tpc() : 0);
			std::fflush(stdout);
		}
		m_fr[n & 15] = v;
		switch (n)
		{
		case 0:
			if (!m_present)
			{
				m_last_fr0 = v;
				m_echo_pending = true;
			}
			break;
		case 1:
			if (v == 2 && !m_events.empty())
			{
				m_events.pop_front();   // acknowledge the event in FR9
				m_ard = false;
			}
			else if (v & 1)
				m_ard = true;           // the IOP poll's attention disable
			break;
		case 8:
			transaction(u8(v));
			break;
		case 9:
			m_config = v;               // (2 << 4) | pa
			break;
		default:
			break;
		}
	}

	u8 dma_read() override
	{
		if (m_out.empty())
			return 0;
		const u8 b = m_out.front();
		m_out.pop_front();
		return b;
	}
	void dma_write(u8 b) override { m_in.push_back(b); }
	u16 dma_read16() override { return u16((dma_read() << 8) | dma_read()); }
	void dma_write16(u16 v) override { dma_write(u8(v >> 8)); dma_write(u8(v)); }

	void dma_complete(bool output) override
	{
		if (output && !m_in.empty())
		{
			// An outbound leg carried the request block.
			request_block(m_in);
			m_in.clear();
		}
		m_want_in = false;
		if (m_pending_event)
			m_event_timer = event_delay_ms ? event_delay_ms : 1;
	}

private:
	//----------------------------------------------------------------
	//  the card's transactions
	//----------------------------------------------------------------

	void transaction(u8 c)
	{
		m_command = c;
		m_in.clear();
		switch (c)
		{
		case 0:
			// Identify, as on every card here: the answer is one
			// halfword and it types the slot.
			m_out.clear();
			m_out.push_back(u8(identity >> 8));
			m_out.push_back(u8(identity));
			m_pending_event = event_done;
			break;
		default:
			// Any other command opens a transaction whose control block
			// arrives on the next outbound leg.
			m_want_in = true;
			break;
		}
	}

	// A request block arrived (27110A Tech Ref, 4-8/4-9). The first byte
	// routes on bits 7:6.
	void request_block(const std::vector<u8> &raw)
	{
		if (raw.empty())
			return;
		const u8 fc = raw[0];
		if (log)
		{
			std::printf("[hpib%u request %02x:", pa, fc);
			for (size_t k = 1; k < raw.size() && k < 24; k++)
				std::printf(" %02x", raw[k]);
			std::printf("]\n");
			std::fflush(stdout);
		}
		switch (fc >> 6)
		{
		case 2:
			if (fc & 0x20)
				break;                  // mag tape, not modelled
			cs80_transaction(raw);
			break;
		case 0:
			general(fc & 0x3f, raw);
			break;
		case 3:
			// Raw HP-IB data transfer: without a bus model there is
			// nothing to carry, so the transaction ends cleanly with
			// nothing transferred.
			m_tstat = 0;
			break;
		default:
			break;
		}
		m_pending_event = event_done;
	}

	// A CS/80 transaction (4-30/4-31): bytes 1-8 are the card's own
	// header -- flags, then the device's bus address and a secondary --
	// and the CS/80 command message follows from byte 9.
	void cs80_transaction(const std::vector<u8> &raw)
	{
		if (raw.size() < 9)
		{
			m_tstat = 0x0b;             // the card's "bad request" family
			return;
		}
		const u8 addr = raw[6];
		std::vector<u8> cmd(raw.begin() + 8, raw.end());
		m_out.clear();
		if ((addr & 7) != device_address)
		{
			// Nothing at that bus address answers.
			m_tstat = 0x0e;
			return;
		}
		disc.command(cmd, m_out, log);
		m_tstat = 0;

		// The status block: TSTAT, then the device's QSTAT, and if that
		// is not zero the card fetches Request Status by itself and
		// appends the report (4-31).
		const u8 q = disc.qstat();
		m_status_block.clear();
		m_status_block.push_back(m_tstat);
		m_status_block.push_back(q);
		if (q != 0)
		{
			std::deque<u8> report;
			const std::vector<u8> rs{ 0x0d };
			disc.command(rs, report, log);
			for (u8 b : report)
				m_status_block.push_back(b);
		}
		// The status block follows the data on the same read path.
		for (u8 b : m_status_block)
			m_out.push_back(b);
	}

	// General card control (table 4-1). None of these need a bus to be
	// answered usefully; the card records what it was told.
	void general(u8 opcode, const std::vector<u8> &raw)
	{
		switch (opcode)
		{
		case 0x0b: m_online = true; break;
		case 0x0c: m_online = false; break;
		case 0x11:                      // configure the card's HP-IB address
			if (raw.size() > 1)
				m_card_address = raw[1] & 0x1f;
			break;
		case 0x12:                      // read address and current status
			m_out.clear();
			m_out.push_back(m_card_address);
			m_out.push_back(m_online ? 1 : 0);
			break;
		case 0x07:                      // read reason for interrupt
			m_out.clear();
			m_out.push_back(0);
			break;
		default:
			break;
		}
		m_tstat = 0;
	}

	u32 m_fr[16] = {};
	std::deque<u8> m_out;
	std::vector<u8> m_in;
	std::vector<u8> m_status_block;
	std::deque<u8> m_events;
	u8 m_pending_event = 0;
	u8 m_command = 0xff;
	u8 m_tstat = 0;
	u32 m_config = 0;
	u8 m_card_address = 30;             // the card talks at address 30 (4-31)
	bool m_online = false;              // the card powers up offline (4-7)
	unsigned m_event_timer = 0;
	bool m_want_in = false;
	bool m_ard = false;
	bool m_present = false;
	bool m_echo_pending = false;
	u32 m_last_fr0 = 0;
	u32 m_log_last = 0xffffffffu;
	unsigned m_log_repeat = 0;
	unsigned m_log_lines = 0;
};

} // namespace focus

#endif // MAME_IOP_FOCUSHPIB_H
