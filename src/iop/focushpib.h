// license:GPL-2.0+
// copyright-holders:Joel Heikkila
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

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <string>
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
	// The 7914's geometry (docs/bitsavers/cs80describes/7914-0.txt).
	static constexpr u32 HEADS = 7;
	static constexpr u32 SECTORS = 64;

	// What Describe reports. The defaults are a 7914 -- the drive whose
	// unit save carries the HP-UX this emulates -- and can be overridden
	// to match another; real captured Describe blocks for several drives
	// are in docs/bitsavers/cs80describes/.
	u32 device_number = 0x079140;   // BCD product number, 6 digits
	u8 generic_type = 0;            // 0 fixed disc, 1 removable, 2 tape

	std::vector<u8> image;          // the medium, one byte per byte
	bool loaded() const { return !image.empty(); }

	// Told of every range of the medium a write has just changed, byte
	// offset and length, as the data lands. The host keeps the file the
	// medium came from up to date through it; nothing here needs it.
	std::function<void(u64, u64)> on_write;
	u32 blocks() const { return u32(image.size() / BLOCK); }

	// QSTAT (CS/80 3-13): 0 normal, 1 hard error -- ask Request Status,
	// 2 power on. A device answers 2 from cold until a Request Status
	// collects it.
	u8 qstat() const { return m_poweron ? 2 : (flags_up() ? 1 : 0); }

	// Run one CS/80 command message, appending whatever execution message
	// it produces to `out`.
	void command(const std::vector<u8> &cmd, std::deque<u8> &out,
			knob::device_log *log = nullptr)
	{
		for (size_t i = 0; i < cmd.size(); )
		{
			const u8 op = cmd[i++];
			// Set Unit 0x20+u, unit 15 the controller (CS/80 2-12).
			// "An illegal unit number (one that does not reside on the
			// addressed device) produces a Module Addressing status
			// error", and so does a volume the unit does not have (2-13).
			// Either is a reject error -- an error in the command message
			// (2-39) -- so nothing after it runs. It used to be flagged and
			// then carried out against unit 0's medium: a Describe of unit
			// 1 described the system disc, and a write to unit 1 would
			// have landed on it, reported only afterwards.
			if (op >= 0x20 && op <= 0x2f)
			{
				m_unit = op & 15;
				if (m_unit != 0 && m_unit != 15)
				{
					set_flag(6);        // module addressing
					return;
				}
				continue;
			}
			if (op >= 0x40 && op <= 0x47)
			{
				m_volume = op & 7;
				if (m_volume != 0)
				{
					set_flag(6);        // module addressing: volume 0 only
					return;
				}
				continue;
			}
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
			case 0x02: locate_write(); break;
			// Cold load read: the boot path's own Locate and Read. Only
			// this side decodes it -- LCS80 boots the internal disc a
			// different way and never sends one.
			case 0x0a: locate_read(out); break;   // cold load read
			case 0x35: describe(out); break;
			case 0x0d: request_status(out); break;
			// disc_card steps 0x33 as a three-byte command, which is what
			// the Instruction Set manual gives and what its driver sends;
			// nothing on this side has needed the distinction. Both
			// readings are recorded in VALIDATION.md section 4.
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
					(*log)("[cs80 op %02x unhandled]\n", op);
				return;
			}
		}
	}

	// A write's data arrived -- all of it, or the next part of it. "The
	// amount of data bytes defined by the power-on or last set length
	// value are accepted and written to the device" (CS/80 2-10, Locate and
	// Write): the execution message is as long as the Set Length said,
	// however the host chooses to feed it to the card, and HP-UX feeds it
	// a buffer at a time. A write of N file system blocks is ONE Locate
	// and Write with Set Length N KB, followed by N DMA legs of 1024
	// bytes from N different pages.
	//
	// This used to close the write on its first leg: the first kilobyte
	// went to the medium and the rest was thrown away under a QSTAT of 0.
	// `cp /bin/ls /x1; sync; cmp /bin/ls /x1` in a root session answered
	// "differ: char 2049", and on the image written back afterwards /x1,
	// a 29 KB copy, was wrong in 26 of its 29 blocks -- only the first
	// block of each extent had arrived. HP-UX's own i-node and free-map
	// updates of 2 and 3 KB, which /etc/rc makes on every boot, lost all
	// but their first kilobyte the same way.
	void commit_write(const std::vector<u8> &data)
	{
		if (!m_writing)
			return;
		const u64 off = u64(m_write_addr) * BLOCK + m_write_done;
		const u64 n = std::min<u64>(data.size(), m_write_len - m_write_done);
		for (u64 k = 0; k < n && off + k < image.size(); k++)
			image[off + k] = data[k];
		m_write_done += n;
		// ...and a write leaves the target after what it wrote, for the
		// same reason as locate_read.
		m_address = m_write_addr + u32((m_write_done + BLOCK - 1) / BLOCK);
		u64 stored = n;
		if (m_write_done >= m_write_len)
		{
			// The length is satisfied: the message is over. "If a partial
			// block is written, the remainder of the block is filled by
			// duplicating the last data byte of the transfer" (2-10).
			const u64 end = u64(m_write_addr) * BLOCK + m_write_done;
			if (n && (m_write_done % BLOCK) != 0)
				for (u64 k = end; k < end + BLOCK - m_write_done % BLOCK
						&& k < image.size(); k++, stored++)
					image[k] = data[n - 1];
			m_writing = false;
		}
		if (on_write && stored && off < image.size())
			on_write(off, std::min<u64>(stored, image.size() - off));
	}

	bool writing() const { return m_writing; }

	// The transaction a write belonged to is over -- the card has started
	// another -- so whatever arrives from now on is not its data. A write
	// the host abandoned short of its length must not take the next
	// transaction's bytes.
	void end_write() { m_writing = false; }

	// Read `count` bytes from block `block` through the device's own
	// Locate-and-Read path -- the bootstrap loader reads the system disc
	// the same way the driver later will, bounds checks and status flags
	// included, rather than reaching around the device into the file.
	bool read_blocks(u32 block, u32 count, std::vector<u8> &out)
	{
		// The real loader ROM drives the drive through the card and,
		// like every CS/80 host, collects the power-on condition with a
		// Request Status before it reads. Reading through here stands
		// for that whole dialogue, so the condition is consumed as it
		// would have been: HP-UX's first Describe otherwise came back
		// with QSTAT 2, which its HP27110 driver turns into escape 148
		// and the boot-device probe into "no disc here".
		m_poweron = false;
		std::deque<u8> data;
		m_address = block;
		m_length = count;
		locate_read(data);
		if (data.size() != count)
			return false;
		out.assign(data.begin(), data.end());
		return true;
	}

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
		// The target address moves with the data: after a Locate and
		// Read it names the block after the last one transferred, so a
		// following Locate and Read with a Set Length alone continues
		// where this one stopped. HP-UX's SDF mount reads the file
		// allocation area that way -- one Set Address, then two hundred
		// Set Length / Locate and Read pairs -- and its segment loader
		// reads sslib the same way. Leaving the address where it was
		// served every continuation the same first blocks: the mount
		// scanned one 5 KB window for eight seconds and the loader
		// relocated sslib segments out of the wrong blocks, which its
		// consistency checks turned into 328 and 315.
		m_address += (len + BLOCK - 1) / BLOCK;
	}

	// Locate and Write opens the execution message; commit_write takes its
	// data as it comes. The target is checked here, before any data, as a
	// read's is: a write that would run off the end of the volume is an
	// address bounds error (status bit 7), its data is sunk -- "the device
	// will sink data until the execution phase is complete" -- and QSTAT
	// says so in this transaction's own status block. It used to be
	// accepted and then clipped at the end of the image, a write that
	// vanished with a QSTAT of 0. A length of all ones is "a transfer size
	// equal to the selected volume" (2-16): from the target to the end.
	void locate_write()
	{
		m_writing = false;
		if (image.empty())
		{
			set_flag(35);               // not ready
			return;
		}
		const u64 off = u64(m_address) * BLOCK;
		if (off >= image.size())
		{
			set_flag(7);                // address bounds
			return;
		}
		const u64 len = (m_length == 0xffffffffu) ? image.size() - off : m_length;
		if (off + len > image.size())
		{
			set_flag(7);                // address bounds
			return;
		}
		if (len == 0)
			return;                     // a locate only: no data (2-16)
		m_writing = true;
		m_write_addr = m_address;
		m_write_len = len;
		m_write_done = 0;
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
		// volume. The internal disc's driver wants a block-addressed
		// volume to describe max cylinder, head and sector as zero
		// (focusdisc.h); HP-UX's CS80 driver is the opposite: it sizes
		// the volume from the geometry, and 0/0/0 made volume 0 a
		// zero-block volume that the swap open then refused with escape
		// 91. So the geometry is real: a 7914's 7 heads of 64 sectors,
		// and as many cylinders as the image needs (a 7914 has 1152;
		// tools/mkhpuxdisc.py pads its volume to whole cylinders).
		//
		// Whole cylinders only, rounded DOWN: an image that ends part way
		// through a cylinder describes the cylinders it has, and the
		// partial one at the end goes unused. Rounding up described
		// blocks the image does not hold -- HP-UX would allocate there,
		// and every write to them was dropped. The pristine volume is
		// exactly 1168 cylinders and describes as it always did. An
		// image smaller than one cylinder describes its own block count.
		const u32 sectors = u32(image.size() / BLOCK);
		const u32 cyls = sectors / (HEADS * SECTORS);
		const u32 maxcyl = cyls ? cyls - 1 : 0;
		b(u8(maxcyl >> 16)); b(u8(maxcyl >> 8)); b(u8(maxcyl));
		b(HEADS - 1);
		b(0); b(SECTORS - 1);
		const u32 maxblk = cyls ? cyls * HEADS * SECTORS - 1
				: (sectors ? sectors - 1 : 0);
		b(0); b(0); b(0);
		b(u8(maxblk >> 16)); b(u8(maxblk >> 8)); b(u8(maxblk));
		b(1);                               // current interleave
	}

	// Request Status (CS/80 2-35, table 2-5): 2 identification bytes,
	// 8 of error flags, 10 parameter bytes. Reading clears the report.
	// The two identification bytes are packed here and split in
	// disc_card; VALIDATION.md section 4 records why.
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
	u64 m_write_len = 0;            // bytes the open write will accept
	u64 m_write_done = 0;           // ...and how many have arrived
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
	knob::device_log log;
	// FOCUS_HPIBFR1 forces the status word FR1 answers (bits 7:1).
	u32 fr1_force = 0;

	// The halfword the identify transaction answers, which is what types
	// the slot: the card ID in the HIGH byte, the firmware ID in the low
	// one. HP-UX's scan range-checks the high byte against 1..7 and
	// abandons a slot outside it (see focusasi.h for the driver code the
	// reading comes from), so zero -- what this used to answer -- was
	// rejected. 2 is the HP-IB card's documented ID.
	// FOCUS_HPIBIDENT sweeps it.
	u16 identity = 0x0200;

	// The device on the bus, at its HP-IB address. The install procedure
	// sets the system disc to address 0.
	cs80_device disc;
	u8 device_address = 0;

	// The two bytes the drive answers an AMIGO identify with. CS/80 discs
	// answer 0x02 in the first byte -- the byte HP-UX's boot-device probe
	// tests to choose its CS80 driver -- and a model code in the second;
	// 0x020a is the 7914. FOCUS_CS80ID sweeps it.
	u16 amigo_id = 0x020a;

	// Switch S6, "System Controller", UP from the factory (27110A 2-3).
	// Bit 5 of FR1 reports it: the HP27110 and HP27125A drivers both
	// refuse request 13 with escape 127 unless it is set, and nothing
	// else in the image looks at that bit. FOCUS_HPIBSC=0 clears it.
	bool system_controller = true;

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

	// Write Order opcodes, in bits 7:4 of the order byte (27110A 4-2).
	enum : u8
	{
		ORD_IDY = 0,        // identify
		ORD_DLD = 1,        // download (not supported)
		ORD_PAU = 2,        // pause
		ORD_DIS = 3,        // disconnect
		ORD_RS  = 4,        // read status
		ORD_WC  = 5,        // write control
		ORD_RD  = 6,        // read data
		ORD_WD  = 7         // write data
	};

	// The card's own commentary, in emulated time: what it decided and
	// when. FOCUS_HPIBDBG.
	static knob::channel &dbg()
	{
		static knob::channel c("FOCUS_HPIBDBG");
		return c;
	}
	double stamp() const { return iop ? iop->tnow() : 0.0; }

	// One millisecond of the card's own time.
	void tick()
	{
		if (m_event_timer && --m_event_timer == 0 && m_pending_event)
		{
			if (dbg())
				dbg()("[%8.3fs hpib%u event %02x queued]\n",
						stamp(), pa, m_pending_event);
			m_events.push_back(m_pending_event);
			m_pending_event = 0;
			if (iop)
				iop->raise(pa);
		}
		// The pause the PAU order asked for, counted out; when it runs
		// down the card asks for its next order.
		if (m_pause_ticks && --m_pause_ticks == 0)
			m_want_order = true;
	}

	// Collapsing and capping come from knob::device_log, for the same
	// reason as the serial card: a driver polling status outruns any
	// uncapped log long before the interesting part of a dialogue.
	static unsigned long log_key(char dir, unsigned n, u32 v)
	{
		return (u32(dir) << 24) | (n << 16) | (v & 0xffff);
	}

	//----------------------------------------------------------------
	//  iop_interface
	//----------------------------------------------------------------

	u32 status() override { return 1; }
	u32 status_reg() override { return 0; }
	u32 flag() override { return 1; }

	bool attention() override { return !m_events.empty() && !m_ard; }

	// A transaction is three legs -- Control, Data, Status -- and each
	// is a separate order from the host (27110A 4-8). The card is asking
	// the channel for service whenever an order whose data runs
	// HOST-to-CARD is open (the control block after WC, a write's data
	// block after WD), or whenever it has bytes to give.
	bool service_request() override
	{
		return m_order == ORD_WC || m_order == ORD_WD || !m_out.empty();
	}

	// Device End: the answer this order prepared has been read out in
	// full. The host sizes its count for the longest reply, so a leg
	// ending here is the normal case.
	bool data_end() override { return m_answering && m_out.empty(); }

	// "The CLC request is in response to an SRQ signal, requesting the
	// next order" (27128A 4-2) -- the same BIC on both cards. Between the
	// legs of a transaction the card asks for its next order, and the
	// driver's command list, suspended on ICEI after each order, resumes
	// when the interface poll says so. Without this the list never got
	// past its control block: the data and status legs were never run,
	// the completion arrived anyway, and the driver read a status block
	// nobody had written.
	bool poll_request() override { return m_want_order; }

	u32 read_fr(unsigned n) override
	{
		u32 v = 0;
		switch (n)
		{
		case 0:
		case 6:
			// The data path a byte at a time. The driver's list reads
			// the status block this way: `WIF8 RS; ICEI; WRIF FR6 ->
			// memory; DCEI`, and the byte that arrives is the TSTAT its
			// 44-arm state table dispatches on.
			v = pop_out();
			break;
		case 1:
			// The powerup presence echo, then the status this becomes.
			if (!m_echo.take(v))
			{
				if (fr1_force)
					v = fr1_force & 0xffff;
				else if (!m_echo.present())
					v = 0;
				else
					// The house status register: bit 7 configured, bit 6
					// self test complete, bit 5 System Controller, bit 0
					// an event waiting.
					v = 0xc0 | (system_controller ? 0x20 : 0)
							| (m_events.empty() ? 0 : 0x01);
			}
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
		if (log.gate(log_key('R', n, v)))
			log("[hpib%u R fr%u = %04x p=%08x]\n", pa, n, v & 0xffff,
					iop ? iop->tpc() : 0);
		return v;
	}

	void write_fr(unsigned n, u32 v) override
	{
		v &= 0xffff;
		if (log.gate(log_key('W', n, v)))
			log("[hpib%u W fr%u = %04x p=%08x]\n", pa, n, v,
					iop ? iop->tpc() : 0);
		m_fr[n & 15] = v;
		switch (n)
		{
		case 0:
			m_echo.note_write(v);   // the powerup presence echo
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
			order(u8(v));
			break;
		case 9:
			m_config = v;               // (2 << 4) | pa
			break;
		default:
			break;
		}
	}

	u8 dma_read() override { return pop_out(); }
	void dma_write(u8 b) override { m_in.push_back(b); }

	void dma_complete(bool output) override
	{
		if (output)
		{
			if (m_order == ORD_WC)
			{
				// The control block, which opens the transaction. The
				// card then wants its next order: RD for the data, or RS
				// straight away when there is none.
				request_block(m_in);
				m_in.clear();
				m_want_order = true;
			}
			else if (m_order == ORD_WD)
			{
				// A write's data block.
				data_block(m_in);
				m_in.clear();
				m_want_order = true;
			}
		}
		else
		{
			m_answering = false;
			switch (m_order)
			{
			case ORD_IDY:
				// The identify's two bytes have gone: the completion the
				// powerup scan waits for.
				start_event_timer();
				break;
			case ORD_RD:
				// The data leg is over; the status leg is next.
				m_want_order = true;
				break;
			case ORD_RS:
				// A status block read by DMA rather than through FR6.
				finish();
				break;
			default:
				break;
			}
		}
	}

private:
	//----------------------------------------------------------------
	//  orders
	//----------------------------------------------------------------

	void order(u8 c)
	{
		m_order = u8(c >> 4);
		m_want_order = false;
		m_answering = false;
		if (dbg())
			dbg()("[%8.3fs hpib%u order %02x]\n", stamp(), pa, c);
		switch (m_order)
		{
		case ORD_IDY:
			// Identify, as on every card here: the answer is one
			// halfword and it types the slot.
			m_out.clear();
			m_out.push_back(u8(identity >> 8));
			m_out.push_back(u8(identity));
			m_answering = true;
			m_pending_event = event_done;
			break;
		case ORD_WC:
			// The control block arrives on the next outbound leg.
			m_in.clear();
			m_out.clear();
			m_status.clear();
			m_tstat = 0;
			break;
		case ORD_RD:
			// The data block the control block prepared.
			m_answering = true;
			break;
		case ORD_WD:
			m_in.clear();
			break;
		case ORD_RS:
			// The status block: TSTAT first, then whatever the
			// transaction has to add (27110A 4-11). The driver's list
			// goes straight on to read it, so the card wants its next
			// order at once.
			m_out.clear();
			for (u8 b : m_status)
				m_out.push_back(b);
			if (m_out.empty())
				m_out.push_back(m_tstat);
			if (dbg())
			{
				std::string hex;
				char b2[4];
				for (u8 b : m_out)
				{
					std::snprintf(b2, sizeof b2, " %02x", b);
					hex += b2;
				}
				dbg()("[%8.3fs hpib%u status block%s]\n", stamp(), pa,
						hex.c_str());
			}
			m_answering = true;
			m_want_order = true;
			break;
		case ORD_PAU:
			m_pause_ticks = 1;
			break;
		case ORD_DIS:
			m_out.clear();
			m_status.clear();
			break;
		default:
			// "The card will accept and discard (thus effectively
			// terminating) commands which are not supported" (4-2).
			break;
		}
	}

	// The transaction is over: its status has been read. The completion
	// event follows, a tick or two later.
	void finish()
	{
		if (m_done)
			return;
		m_done = true;
		if (dbg())
			dbg()("[%8.3fs hpib%u transaction done, tstat %02x]\n",
					stamp(), pa, m_tstat);
		m_pending_event = event_done;
		start_event_timer();
	}

	void start_event_timer()
	{
		if (!m_event_timer && m_pending_event)
			m_event_timer = event_delay_ms ? event_delay_ms : 1;
	}

	u8 pop_out()
	{
		if (m_out.empty())
			return 0;
		const u8 b = m_out.front();
		m_out.pop_front();
		if (m_order == ORD_RS && m_out.empty())
			finish();
		return b;
	}

	//----------------------------------------------------------------
	//  the card's transactions
	//----------------------------------------------------------------

	// A request block arrived (27110A Tech Ref, 4-8/4-9). The first byte
	// routes on bits 7:6.
	void request_block(const std::vector<u8> &raw)
	{
		if (raw.empty())
			return;
		const u8 fc = raw[0];
		if (log)
		{
			std::string hex;
			char b2[4];
			for (size_t k = 1; k < raw.size() && k < 24; k++)
			{
				std::snprintf(b2, sizeof b2, " %02x", raw[k]);
				hex += b2;
			}
			log("[hpib%u request %02x:%s]\n", pa, fc, hex.c_str());
		}
		if (dbg())
		{
			std::string hex;
			char b2[4];
			for (size_t k = 1; k < raw.size() && k < 24; k++)
			{
				std::snprintf(b2, sizeof b2, " %02x", raw[k]);
				hex += b2;
			}
			dbg()("[%8.3fs hpib%u request %02x:%s]\n", stamp(), pa, fc,
					hex.c_str());
		}
		m_out.clear();
		m_status.clear();
		m_tstat = 0;
		m_write_pending = false;
		m_done = false;
		switch (fc >> 6)
		{
		case 2:
			if (fc & 0x20)
			{
				m_tstat = 0x0a;         // mag tape: not supported here
				break;
			}
			cs80_transaction(fc, raw);
			break;
		case 0:
			general(fc & 0x3f, raw);
			break;
		case 3:
			bus_transfer(fc, raw);
			break;
		default:
			m_tstat = 0x0a;             // reserved
			break;
		}
		if (m_status.empty())
			m_status.push_back(m_tstat);
	}

	// A write's data block arrived on the WD leg.
	void data_block(const std::vector<u8> &raw)
	{
		if (log)
		{
			std::string hex;
			char b2[4];
			for (size_t k = 0; k < raw.size() && k < 32; k++)
			{
				std::snprintf(b2, sizeof b2, " %02x", raw[k]);
				hex += b2;
			}
			log("[hpib%u data block %u bytes:%s%s]\n", pa,
					unsigned(raw.size()), hex.c_str(),
					raw.size() > 32 ? " ..." : "");
		}
		if (dbg())
			dbg()("[%8.3fs hpib%u data block %u bytes%s]\n", stamp(), pa,
					unsigned(raw.size()),
					m_write_pending && disc.writing() ? " (written)" : "");
		// One leg of the write's data. A long write comes as several legs
		// under one WD order, and the drive knows from its Set Length when
		// the last has arrived; until then the write stays pending.
		if (m_write_pending && disc.writing())
			disc.commit_write(raw);
		if (!disc.writing())
			m_write_pending = false;
	}

	// A CS/80 transaction (4-30/4-31): bytes 1-8 are the card's own
	// header -- flags, then the device's bus address and a secondary --
	// and the CS/80 command message follows from byte 9.
	void cs80_transaction(u8 fc, const std::vector<u8> &raw)
	{
		if (raw.size() < 9)
		{
			m_tstat = 0x0b;             // the card's "bad request" family
			return;
		}
		const u8 addr = raw[6];
		std::vector<u8> cmd(raw.begin() + 8, raw.end());
		if ((addr & 0x1f) != device_address)
		{
			// Nothing at that bus address answers.
			m_tstat = 0x0e;
			return;
		}
		// A new transaction: a write the last one left short of its
		// length is over, and none of what follows is its data.
		disc.end_write();
		disc.command(cmd, m_out, &log);
		if (log && !m_out.empty())
		{
			std::string hex;
			char b2[4];
			size_t k = 0;
			for (auto it = m_out.begin(); it != m_out.end() && k < 32; ++it, ++k)
			{
				std::snprintf(b2, sizeof b2, " %02x", *it);
				hex += b2;
			}
			log("[hpib%u read data %u bytes:%s%s]\n", pa,
					unsigned(m_out.size()), hex.c_str(),
					m_out.size() > 32 ? " ..." : "");
		}
		// A write's data comes on the WD leg that follows.
		m_write_pending = disc.writing() && (fc & 0x08) && !(fc & 0x04);
		m_tstat = 0;

		// The status block: TSTAT, then the device's QSTAT, and if that
		// is not zero the card fetches Request Status by itself and
		// appends the report (4-31).
		// "Byte 1: TSTAT. Byte 2: CS/80 QSTAT. Byte 3 to up to 22: the
		// CS/80 Request Status execution message if QSTAT <> 0. Byte n:
		// QSTAT of CS/80 Request Status if QSTAT <> 0" (4-31) -- twenty-
		// three bytes when the device has something to report, and the
		// driver reads exactly that many: a report without its closing
		// QSTAT is one byte short, which its count arithmetic turns into
		// escape 148.
		const u8 q = disc.qstat();
		m_status.push_back(m_tstat);
		m_status.push_back(q);
		if (q != 0)
		{
			std::deque<u8> report;
			const std::vector<u8> rs{ 0x0d };
			disc.command(rs, report, &log);
			for (u8 b : report)
				m_status.push_back(b);
			m_status.push_back(disc.qstat());
		}
	}

	// Data transfer between the card and another HP-IB device (4-17..
	// 4-21). The function code's low bits say whether there is a data
	// block and which way it runs; the parameters are a flag byte, a
	// four-byte length, the device ID, the match byte, and then the
	// interface message the card sends with ATN before the data moves.
	// An AMIGO identify is exactly this -- UNL, MLA, UNT, then the
	// secondary 0x60+address, and a two-byte read (4-35) -- and it is
	// how HP-UX's boot-device probe asks each bus address what it is.
	void bus_transfer(u8 fc, const std::vector<u8> &raw)
	{
		const bool data = (fc & 0x08) != 0;
		const bool read = (fc & 0x04) != 0;
		u32 length = 0;
		for (size_t k = 2; k < 6 && k < raw.size(); k++)
			length = (length << 8) | raw[k];
		std::vector<u8> msg;
		if (raw.size() > 8)
			msg.assign(raw.begin() + 8, raw.end());
		if (dbg())
		{
			std::string hex;
			char b2[4];
			for (u8 b : msg)
			{
				std::snprintf(b2, sizeof b2, " %02x", b);
				hex += b2;
			}
			dbg()("[%8.3fs hpib%u bus %s%s len=%u msg%s]\n", stamp(), pa,
					data ? (read ? "read" : "write") : "no-data",
					data ? "" : " (interface message only)", length,
					hex.c_str());
		}
		// Run the interface message across the bus. The card is CIC; the
		// one device on the bus answers to its address.
		bool identify = false;
		for (u8 b : msg)
		{
			const u8 cmd = b & 0x7f;
			if (cmd == 0x3f)                            // UNL
				m_device_listens = false;
			else if (cmd >= 0x20 && cmd < 0x3f)         // LAD n
			{
				if ((cmd & 0x1f) == device_address)
					m_device_listens = true;
			}
			else if (cmd == 0x5f)                       // UNT
			{
				m_device_talks = false;
				m_after_unt = true;
				continue;
			}
			else if (cmd >= 0x40 && cmd < 0x5f)         // TAD n
				m_device_talks = ((cmd & 0x1f) == device_address);
			else if (cmd >= 0x60)                       // secondary
			{
				if (m_after_unt)
				{
					// "UNT followed by a secondary" is the AMIGO
					// identify of the address the secondary carries.
					identify = ((cmd & 0x1f) == device_address);
				}
				else
					m_secondary = cmd & 0x1f;
			}
			m_after_unt = false;
		}
		if (data && read)
		{
			if (identify)
			{
				m_out.push_back(u8(amigo_id >> 8));
				m_out.push_back(u8(amigo_id));
				m_tstat = 0x01;         // terminated by EOI
			}
			else if (m_device_talks)
			{
				// The device's execution message for whatever the last
				// command was. Not reached by anything yet; logged.
				if (log)
					log("[hpib%u bus read from device, secondary %02x: "
							"not modelled]\n", pa, m_secondary);
				m_tstat = 0x01;
			}
			else
			{
				// Nobody addressed to talk. On a real bus the handshake
				// would hang until the host gave up; here the read ends
				// with nothing, which the probe's length check rejects
				// in the same way and a great deal faster.
				if (dbg())
					dbg()("[%8.3fs hpib%u bus read: no talker]\n",
							stamp(), pa);
				m_tstat = 0x00;
			}
			while (m_out.size() > length && length)
				m_out.pop_back();
		}
		else if (data && !read)
		{
			// The data block follows on the WD leg. Where it goes is
			// decided by the addressing above; nothing consumes it yet.
			m_tstat = 0x00;
		}
		else
			m_tstat = 0x00;
	}

	// General card control (table 4-1). None of these need a bus to be
	// answered usefully; the card records what it was told.
	void general(u8 opcode, const std::vector<u8> &raw)
	{
		m_tstat = 0;
		switch (opcode)
		{
		case 0x01: case 0x02:           // set / clear REN
			if (!system_controller)
				m_tstat = 0x0b;         // requires the card to be SC
			break;
		case 0x03:                      // send IFC
			if (!system_controller)
				m_tstat = 0x0b;
			m_cic = true;               // "The System Controller becomes
			break;                      // the CIC by asserting IFC"
		case 0x0b: m_online = true; break;
		case 0x0c: m_online = false; break;
		case 0x11:                      // configure the card's HP-IB address
			if (raw.size() > 1)
				m_card_address = raw[1] & 0x1f;
			break;
		case 0x12:
			// Read HP-IB address configuration and current status
			// (4-25): OML|TA|LA|D1 -- the not-CIC address -- then D2,
			// the CIC address, always 30, then the state flags. The
			// HP27110 driver takes the card's own address from the
			// second byte, and it matters: the boot-device probe skips
			// the address it believes is the card's, and an answer of
			// zero there made it skip the system disc.
			m_out.push_back(u8((m_online ? 0x80 : 0) | (m_card_address & 0x1f)));
			m_out.push_back(30);
			m_out.push_back(u8((system_controller ? 0x80 : 0) | (m_cic ? 0x40 : 0)));
			break;
		case 0x07:                      // read reason for interrupt
			m_out.push_back(0);
			break;
		case 0x10:                      // conduct PPOLL and read response
			m_out.push_back(0);
			break;
		case 0x05:                      // read PPOLL response configuration
			m_out.push_back(0);
			break;
		case 0x09:                      // read interrupt mask
			m_out.push_back(0);
			m_out.push_back(0);
			break;
		case 0x14:                      // last secondary address
			m_out.push_back(m_secondary);
			break;
		default:
			break;
		}
	}

	u32 m_fr[16] = {};
	std::deque<u8> m_out;               // what the host reads next
	std::vector<u8> m_in;               // what the host wrote
	std::vector<u8> m_status;           // the transaction's status block
	std::deque<u8> m_events;
	u8 m_pending_event = 0;
	u8 m_order = 0xff;
	u8 m_tstat = 0;
	u32 m_config = 0;
	u8 m_card_address = 30;             // the card talks at address 30 (4-31)
	u8 m_secondary = 0;
	bool m_online = false;              // the card powers up offline (4-7)
	bool m_cic = true;
	bool m_answering = false;
	bool m_want_order = false;
	bool m_write_pending = false;
	bool m_done = false;                // the completion has been announced
	bool m_device_listens = false;
	bool m_device_talks = false;
	bool m_after_unt = false;
	unsigned m_event_timer = 0;
	unsigned m_pause_ticks = 0;
	bool m_ard = false;
	presence_echo m_echo;
};

} // namespace focus

#endif // MAME_IOP_FOCUSHPIB_H
