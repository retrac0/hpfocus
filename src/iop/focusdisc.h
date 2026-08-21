// license:BSD-3-Clause
// copyright-holders:
/***************************************************************************

    The Series 500 internal disc, select code 7.

    The 520's internal mass storage answers select code 7, and the system
    drives it through the module the boot image calls LCS80 -- a local
    CS/80 controller rather than anything on an HP-IB cable. Nothing here
    comes from documentation; every exchange below was read off the
    running system, and the ones still unknown are marked as such.

    The powerup presence test is as for every slot: FR1 must read zero,
    FR3 zero (three reads), then 1<<7 written to FR0 must read straight
    back from FR1. From there FR1 is the card's status register:

      bit 7 (0x80)  configured / passed its own self test
      bit 6 (0x40)  self test complete
      bit 0 (0x01)  an event code is waiting in FR9

    The kernel's device init polls FR1 until bit 6 sets, then -- with
    bit 7 already set -- writes FR9 = (2 << 4) | pa, a configuration
    word whose high nibble's meaning is not yet established, and runs
    its first transaction. A transaction is a command list executed by
    the IOP (ICEI, opcode 070): a command byte for the card arrives as
    a WIF8 write, then the list sets up and starts a DMA leg, and the
    completion is the DMA interrupt through the device's DRT entry.

    Command 0 is identify: the card answers two bytes; the halfword
    they form types the slot -- 1..6 map to internal type codes 6, 5,
    26, 7, 27 and 18, and anything else types the slot 29, which the
    kernel applies as the default.

    Writes to FR1 are commands in their own right: 1 is the attention
    request disable the IOP's poll issues, 2 acknowledges the event
    code in FR9 (the attention handler's loop pops one per ack), and 6
    asks an unconfigured card to configure itself -- answered by
    setting bit 7 and posting an event with high nibble 1, which is
    the "command complete" class of the FR9 dispatch. Event codes are
    dispatched by their high nibble: 1, 2 and 4 are completion classes,
    3 and anything above 0x4f is an error.

***************************************************************************/

#ifndef MAME_IOP_FOCUSDISC_H
#define MAME_IOP_FOCUSDISC_H

#pragma once

#include "focusiop.h"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <vector>

namespace focus {

struct disc_card : public iop_interface
{
	iop_core *iop = nullptr;
	unsigned pa = 7;
	bool log = false;

	// The halfword the identify transaction answers. Values 1..6 map to
	// internal device types 6, 5, 26, 7, 27 and 18; anything else types
	// the slot 29.
	u16 identity = 0;

	// The identity register FR3 answers per selected device: the flexible
	// disc controller answers 4, a Winchester 5, the bootstrap loader
	// card 8. The kernel's typing table maps 4 to type 12 (flexible), 5
	// to 3 (fixed) and 8 to 25, and LCS80 binds the slots typed 3, 12 or
	// 25. Select code 7 is shared: FR8 bit 8 switches between the
	// flexible disc controller and the fixed disc / bootstrap loader
	// card (520 service manual 2-47).
	u16 id_fr3 = 4;
	u16 id_fr3_alt = 8;
	u16 id_fr3_fixed = 5;

	// The card's self test takes time, not a number of reads: the host
	// ticks this once a millisecond and the status changes when the time
	// is up.
	unsigned busy_ms = 50;

	u32 status() override { return 1; }     // status line asserted
	u32 flag() override { return 0; }       // LCS80's init requires 0
	u32 status_reg() override { return 0; } // RIST: zero is healthy

	u32 read_fr(unsigned n) override
	{
		u32 v = 0;
		// The bootstrap loader card is unit 1 behind this select code, and
		// its ROM carries the machine ID "for protection of software
		// rights". Whether the running system ever reads it is an open
		// question (HARDWARE.md §2.5.1) -- report any access while unit 1
		// is selected, so a licence check would show itself.
		if (m_device && getenv("FOCUS_UNIT1"))
		{
			std::printf("[disc UNIT1 R fr%u p=%08x]\n", n,
					iop ? iop->tpc() : 0);
			std::fflush(stdout);
		}
		switch (n)
		{
		case 3:
			v = m_device ? (m_fixed.empty() ? id_fr3_alt : id_fr3_fixed)
					: id_fr3;
			break;
		case 1:
			// Before the presence test this register reads zero -- the
			// scan gives up on a slot that answers anything else. The
			// test then writes a pattern to FR0 and reads it straight
			// back here, and only afterwards is this the status.
			if (m_echo_pending)
			{
				v = m_last_fr0;
				m_echo_pending = false;
				m_present = true;
			}
			else if (!m_present)
				v = 0;
			else if (m_busy_left)
				v = 0x80;
			else
				v = 0xc0 | (m_events.empty() ? 0 : 1);
			break;
		case 0:
		case 6:
			// The card's data latch: LCS80's init walks a bit pattern
			// through here (FR0 on the flexible disc, FR6 on the other
			// device) and expects the exact value back, and the status
			// phase deposits the transaction's QSTAT here. Reading it
			// consumes the card's service request.
			v = m_fr0;
			break;
		case 9:
			v = m_events.empty() ? 0 : m_events.front();
			break;
		default:
			v = m_fr[n & 15];
			break;
		}
		if (log)
		{
			std::printf("[disc R fr%u = %04x p=%08x]\n", n, v & 0xffff,
					iop ? iop->tpc() : 0);
			std::fflush(stdout);
		}
		return v;
	}

	void write_fr(unsigned n, u32 v) override
	{
		if (log)
		{
			std::printf("[disc W fr%u = %04x p=%08x]\n", n, v & 0xffff,
					iop ? iop->tpc() : 0);
			std::fflush(stdout);
		}
		m_fr[n & 15] = v & 0xffff;
		switch (n)
		{
		case 0:
		case 6:
			// The latch echoes what is written, and the 8039 raises its
			// service request to say it has -- the init's loopback sweep
			// waits on that between the write and the read-back. The
			// powerup presence test's FR1 echo is armed only until the
			// slot has answered it once.
			if (!m_present)
			{
				m_last_fr0 = v & 0xffff;
				m_echo_pending = true;
			}
			m_fr0 = v & 0xffff;
			m_announce = true;
			break;
		case 1:
			fr1_command(v & 0xffff);
			break;
		case 8:
			// Select code sharing: bit 8 switches between the flexible
			// disc controller and the fixed disc / bootstrap loader card.
			// The driver writes the complement of the device number in
			// the high byte: 0xff00 selects device 0, 0xfe00 device 1.
			m_device = (~v >> 8) & 1;
			if (m_device && getenv("FOCUS_UNIT1"))
			{
				std::printf("[disc UNIT1 selected p=%08x]\n",
						iop ? iop->tpc() : 0);
				std::fflush(stdout);
			}
			break;
		case 9:
			m_config = v & 0xffff;
			break;
		default:
			break;
		}
	}

	void control_fr(unsigned n) override
	{
		// Opcode 070 is the IOP's own ICEI, not a strobe on this card;
		// nothing else is known to strobe registers here, so a strobe
		// arriving is something new to learn from.
		if (log)
		{
			std::printf("[disc C fr%u  (fr9=%04x)]\n", n, m_fr[9]);
			std::fflush(stdout);
		}
	}

	// The card asserts ASYNC while it holds event codes; the IOP's
	// attention poll answers with an ARD write to FR1 and the CPU hears
	// message 14. The handler reads FR9 and acknowledges with FR1 = 2.
	bool attention() override { return !m_events.empty(); }

	// Ready to transfer: accepting a commanded output leg, or holding
	// data for an input leg. The service-request line is also how the
	// card announces itself once its power-on self test completes --
	// LCS80's init polls for exactly that and reports "self test
	// error" without it; the first command the driver sends stands the
	// announcement down.
	bool service_request() override
	{ return m_announce || m_want_burst || !m_out.empty(); }

	// The controller raises the burst-request line while a commanded
	// transfer is in progress; the IOP moves the data with no start
	// command.
	bool burst_request() override { return m_want_burst; }
	void dma_complete(bool output) override
	{
		m_want_burst = false;
		if (output && m_phase == 1)
		{
			if (log)
			{
				std::printf("[%u disc block mode=%x:", m_seq++, m_go_mode);
				for (u8 b : m_in)
					std::printf(" %02x", b);
				std::printf("]\n");
				std::fflush(stdout);
			}
			command_block(m_in);
			m_in.clear();
		}
		else if (output && m_phase == 2)
		{
			if (log)
			{
				std::printf("[disc data-in %zu bytes]\n", m_in.size());
				std::fflush(stdout);
			}
			commit_write();
			m_in.clear();
		}
		m_phase = 0;
	}
	u8 dma_read() override
	{
		if (m_out.empty())
			return 0;
		const u8 b = m_out.front();
		m_out.pop_front();
		return b;
	}
	u16 dma_read16() override
	{
		return u16((dma_read() << 8) | dma_read());
	}
	void dma_write(u8 b) override
	{
		m_in.push_back(b);
		if (log)
		{
			std::printf("[disc DMA-in %02x]\n", b);
			std::fflush(stdout);
		}
	}
	void dma_write16(u16 v) override
	{
		dma_write(u8(v >> 8));
		dma_write(u8(v));
	}

	// One millisecond of the card's own time.
	void tick()
	{
		if (m_busy_left && --m_busy_left == 0)
			m_announce = true;
	}
	void reset_self_test() { m_busy_left = busy_ms; }

	// Put a medium in the drive. The bootstrap loader changes discs the
	// way an operator does, one volume of a set after another.
	void mount(std::vector<u8> medium)
	{
		m_image = std::move(medium);
		m_poweron = true;
	}
	bool loaded() const { return !m_image.empty(); }
	u32 blocks() const { return u32(m_image.size() / BLOCK); }

	// The 520's fixed disc. Select code 7 is shared: the flexible
	// controller is device 0 and the Winchester is device 1, switched by
	// the FR8 write, and when a Winchester is fitted the bootstrap loader
	// circuitry lives on ITS controller rather than on a separate loader
	// card (520 service manual). So mounting a fixed disc changes what
	// device 1 is: identity 5, a Winchester, instead of identity 8, a
	// bare loader card.
	void mount_fixed(std::vector<u8> medium)
	{
		m_fixed = std::move(medium);
		m_poweron = true;
	}
	bool fixed_loaded() const { return !m_fixed.empty(); }
	// The loader selects the device it boots from; the driver writes the
	// same selection the system later makes through FR8.
	void select_device(unsigned d) { m_device = d & 1; }
	u32 fixed_blocks() const { return u32(m_fixed.size() / BLOCK); }

	// Which medium the selected device is reading. Everything below goes
	// through this so one command path serves both drives.
	std::vector<u8> &medium()
	{
		// Device 1 is the fixed disc when one is fitted. A machine with a
		// fixed disc and nothing in the flexible drive reads the fixed
		// disc either way -- there is no other medium to mean.
		if (!m_fixed.empty() && (m_device || m_image.empty()))
			return m_fixed;
		return m_image;
	}

	// Read `count` bytes from block `block`, through the controller's own
	// Locate-and-Read path -- the bootstrap loader gets its data the same
	// way the driver does, bounds checks and status flags included, rather
	// than reaching around the card into the file.
	bool read_blocks(u32 block, u32 count, std::vector<u8> &out)
	{
		m_address = block;
		m_length = count;
		m_out.clear();
		locate_read();
		if (m_out.size() != count)
			return false;
		out.assign(m_out.begin(), m_out.end());
		m_out.clear();
		return true;
	}

private:
	void fr1_command(u32 v)
	{
		// R1 is the controller's control register. Bit 5 resets it (520
		// service manual: "the setting of NIOD bit 5 of output register
		// R1"). The driver's transaction, read off its own code: 4 (or
		// 6) announces the command block, which arrives as an output DMA
		// leg; 1 announces the data leg; 3 asks for status -- the card
		// interrupts when it is ready and the driver reads the status
		// byte from FR6 (zero is success); 7 follows a reset. What
		// separates 6 from 4, and what 7 asks, is not yet established.
		if (v & 0x20)
		{
			m_want_burst = false;
			m_in.clear();
			m_out.clear();
			m_announce = true;      // the 8039 comes back up idle
			return;
		}
		switch (v)
		{
		case 4: case 6: case 7:
			m_go_mode = v;
			m_in.clear();
			m_phase = 1;
			m_want_burst = true;
			m_announce = false;     // a transaction begins
			break;
		case 1:
			m_phase = 2;
			m_want_burst = true;
			break;
		case 3:
			// Status phase: the QSTAT lands in the data latch and the
			// card interrupts. QSTAT is 2 -- CS/80's power-fail value --
			// from power-on until the first Request Status reports it.
			m_want_burst = false;
			m_announce = false;
			m_fr0 = m_status;
			if (iop)
				iop->raise(pa);
			break;
		default:
			if (log)
			{
				std::printf("[disc FR1 command %02x unhandled]\n", v);
				std::fflush(stdout);
			}
			break;
		}
	}

	// A command block arrived over DMA. The language is CS/80's
	// complementary command set -- the module driving this card is named
	// LCS80, a local CS/80 controller: Set Unit is 0x20+u (unit 15 the
	// controller), Set Volume 0x40+v, Set Address 0x10 + 6 bytes, Set
	// Length 0x18 + 4 bytes, Locate and Read 0x00, Locate and Write
	// 0x02, Describe 0x35 (the driver reads back 19 halfwords = the
	// 5+19+13 byte controller/unit/volume description), Request Status
	// 0x0d, Release 0x0e/0x0f, Set Release 0x3b + 1. Opcodes it sends
	// that are not yet understood -- 0x09 among them -- are accepted
	// and logged.
	void command_block(const std::vector<u8> &raw)
	{
		// QSTAT: 0 = transaction clean, 1 = error status waiting, 2 =
		// power fail. A CS/80 device holds its power-on status until a
		// Request Status reports it, and every transaction before that
		// answers 2.
		m_out.clear();
		// Wire framing: a length byte, then that many bytes of CS/80
		// ops. The flexible disc's blocks carry one leading zero byte
		// before the length and pad to a halfword after.
		size_t start = 0;
		if (!raw.empty() && raw[0] == 0)
			start = 1;
		if (start >= raw.size())
			return;
		const size_t len = raw[start];
		std::vector<u8> cmd(raw.begin() + start + 1,
				raw.begin() + start + 1 + std::min(len, raw.size() - start - 1));
		for (size_t i = 0; i < cmd.size(); )
		{
			const u8 op = cmd[i++];
			if (op >= 0x20 && op <= 0x2f)
			{
				m_unit = op & 15;
				// Only unit 0 (the drive) and unit 15 (the controller)
				// exist; addressing any other raises status bit 15,
				// which the driver maps to its tolerated "no such
				// unit" escape.
				if (m_unit != 0 && m_unit != 15)
					set_flag(15);
				continue;
			}
			if (op >= 0x40 && op <= 0x47) { m_volume = op & 7; continue; }
			switch (op)
			{
			case 0x10:      // set address (single-vector, 6 bytes)
				m_address = 0;
				for (unsigned k = 0; k < 6 && i < cmd.size(); k++)
					m_address = (m_address << 8) | cmd[i++];
				break;
			case 0x18:      // set length (4 bytes; all-ones = whole)
				m_length = 0;
				for (unsigned k = 0; k < 4 && i < cmd.size(); k++)
					m_length = (m_length << 8) | cmd[i++];
				break;
			case 0x00:      // locate and read
				locate_read();
				break;
			case 0x02:      // locate and write
				m_writing = true;
				m_write_addr = m_address;
				break;
			case 0x35:      // describe
				describe();
				break;
			case 0x0d:      // request status
				request_status();
				m_status = 0;
				break;
			case 0x33:      // set retry time (2 bytes) -- assumed
				i += 2;
				break;
			case 0x3b:      // set release (1 byte)
				i += 1;
				break;
			case 0x34:      // no op
			case 0x0e:      // release
			case 0x0f:      // release denied
				break;
			case 0x09:      // sent as SetUnit+0x09 by the request wrapper;
							// meaning not yet established, accepted
				break;
			default:
				if (log)
				{
					std::printf("[disc CS80 op %02x unhandled]\n", op);
					std::fflush(stdout);
				}
				i = cmd.size();
				break;
			}
		}
		m_status = m_poweron ? 2 : (flags_up() ? 1 : 0);
	}

	void locate_read()
	{
		if (medium().empty())
		{
			// CS/80 status bit 35 is NOT READY -- "the selected unit is
			// not ready for access at this time"; 36 is WRITE PROTECT
			// (Instruction Set manual, status bit list). An empty drive
			// is the former, and reporting the latter made every access
			// to a machine with no flexible volume mounted come back
			// "ERROR 83: Write protected".
			set_flag(35);   // not ready: no medium
			return;
		}
		u64 off = u64(m_address) * BLOCK;
		u32 len = m_length;
		if (len == 0xffffffffu)
			len = BLOCK;
		if (off + len > medium().size())
		{
			set_flag(8);    // address bounds
			return;
		}
		for (u32 k = 0; k < len; k++)
			m_out.push_back(medium()[off + k]);
	}

	void commit_write()
	{
		if (!m_writing)
			return;
		m_writing = false;
		if (medium().empty())
		{
			set_flag(35);   // not ready: no medium
			return;
		}
		u64 off = u64(m_write_addr) * BLOCK;
		for (size_t k = 0; k < m_in.size() && off + k < medium().size(); k++)
			medium()[off + k] = m_in[k];
	}

	// The CS/80 describe reply for a 9130K flexible disc drive: 5 bytes
	// of controller description, 19 of unit, 13 of volume. The medium is
	// 35 cylinders x 2 heads x 16 sectors of 256 bytes = 1120 blocks.
	void describe()
	{
		auto b = [this](u8 v) { m_out.push_back(v); };
		// controller: installed units (bit 15 = the controller itself),
		// transfer rate in kB/s, controller type
		b(0x80); b(0x01);
		b(0x00); b(0x1e);
		b(0x05);
		if (m_unit == 15)
			return;
		// unit: type 1 = flexible disc, device number 009130, bytes per
		// block, blocks buffered, burst size, block time, rate, retry
		// time, access time, max interleave, fixed/removable volumes
		const bool fixed = m_device && !m_fixed.empty();
		if (fixed)
		{ b(0); b(0x00); b(0x79); b(0x08); }   // type 0, device 007908
		else
		{ b(1); b(0x00); b(0x91); b(0x30); }   // type 1, device 009130
		b(BLOCK >> 8); b(BLOCK & 0xff);
		b(1); b(0);
		b(0x20); b(0x00);
		b(0x00); b(0x1e);
		b(0x00); b(0x64);
		b(0x00); b(0x64);
		b(2);
		b(fixed ? 0x01 : 0x00);   // fixed volumes
		b(fixed ? 0x00 : 0x01);   // removable volumes
		// volume: the driver's post-describe checks read max head and max
		// sector and take the cylinder-addressed path when they are set,
		// so the flexible disc describes itself block-addressed: max
		// cylinder, head and sector all zero, only the single-vector
		// maximum address real.
		b(0); b(0); b(0);
		b(0);
		b(0); b(0);
		const u32 maxblk = u32(medium().empty()
				? 1119 : medium().size() / BLOCK - 1);
		b(0); b(0); b(0);
		b(u8(maxblk >> 16)); b(u8(maxblk >> 8)); b(u8(maxblk));
		b(1);
	}

	// Request Status: 20 bytes -- unit and volume identifiers, eight
	// status-flag bytes (bits 0-63), ten parameter bytes. Until the
	// first request the power-fail bit (30) is up, as a freshly
	// powered CS/80 device reports; reporting clears it.
	void request_status()
	{
		m_out.push_back(u8(m_unit));
		m_out.push_back(u8(m_volume));
		if (m_poweron)
			m_flags[30 / 8] |= u8(0x80 >> (30 & 7));
		for (unsigned k = 0; k < 8; k++)
			m_out.push_back(m_flags[k]);
		for (unsigned k = 0; k < 10; k++)
			m_out.push_back(0);
		m_poweron = false;
		for (auto &f : m_flags)
			f = 0;
	}

	void set_flag(unsigned bit)
	{
		m_flags[bit / 8] |= u8(0x80 >> (bit & 7));
	}

	bool flags_up() const
	{
		for (u8 f : m_flags)
			if (f)
				return true;
		return false;
	}

	void post_event(u8 code)
	{
		m_events.push_back(code);
	}

	u32 m_fr[16] = {};
	u32 m_fr0 = 0;              // the data latch behind FR0/FR6
	// The service manual says interface clear leaves device 1 (the fixed
	// disc / loader card) selected, but the powerup scan hangs against
	// an id of 8 or 5 -- not yet understood -- so the card powers up
	// with the flexible disc selected.
	unsigned m_device = 0;
	bool m_want_burst = false;
	unsigned m_go_mode = 0;
public:
	unsigned m_seq = 0;         // command-block counter, for ordering logs
private:
	u32 m_status = 2;           // QSTAT; power-on state until reported
	unsigned m_unit = 0;
	unsigned m_volume = 0;
	u32 m_address = 0;
	u32 m_length = 0xffffffff;
	bool m_writing = false;
	bool m_poweron = true;
	bool m_announce = false;
	u8 m_flags[8] = {};         // the CS/80 status-report flag bytes
	u32 m_write_addr = 0;
	unsigned m_phase = 0;       // 1 = command block expected, 2 = data leg
public:
	static constexpr u32 BLOCK = 256;
	std::vector<u8> m_image;    // the flexible medium, one byte per byte
	std::vector<u8> m_fixed;    // the fixed disc, when one is fitted
private:
	u32 m_last_fr0 = 0;
	u32 m_config = 0;
	bool m_echo_pending = false;
	bool m_present = false;
	bool m_configured = false;
	std::deque<u8> m_events;
	std::deque<u8> m_out;       // bytes the next DMA read leg supplies
	std::vector<u8> m_in;       // bytes a DMA write leg delivered
	unsigned m_busy_left = 50;
};

} // namespace focus

#endif // MAME_IOP_FOCUSDISC_H
