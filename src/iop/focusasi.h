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
    hand joins them. The mapping below has since been driven by HP-UX
    5.30 itself (HARDWARE.md sec 2.6.3) and is confirmed, not a guess:

      FR0   read/write data          (read_data / write_data)
      FR1   read: status / control-register write, as everywhere else on
            this machine -- bit 0 ARD is the attention-request disable
            the IOP's own poll writes (I/O ERS 4.4), and the CIO
            write_control byte has ARD as bit 0 too (27110A 4-1), which
            is the one place the two vocabularies visibly agree
      FR3   read: card identity, 3 for the ASI and 5 for the MUX
            ("card ID = 3", 27128A 4-67; "card 10 = 5", 27130A 4-46)
      FR8   write: the ORDER byte  (write_order, opcode in bits 7:4 --
            IDY/PAU/WC/RTS are the values HP-UX actually writes here;
            see transaction()/write_order() below)
      FR9   write: a configuration/command word, mode<<4|pa, that the
            card answers with an event (see the case 9 handler below)
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

#include <algorithm>
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
	// The card's log. Collapsing and capping live in knob::device_log,
	// shared with the HP-IB card, because a driver polling a status
	// register runs to hundreds of thousands of accesses in a few
	// emulated seconds and neither card can afford to print them all.
	knob::device_log log;
	// What makes one register access different from the last: the
	// direction, the register and the value.
	static unsigned long log_key(char dir, unsigned n, u32 v)
	{
		return (u32(dir) << 24) | (n << 16) | (v & 0xffff);
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
	enum : u8 { EVENT_DONE = 0x10, EVENT_CONFIGURED = 0x30 };
	// FOCUS_ASIEVENT overrides the completion event code, for finding
	// which class the driver acts on.
	u8 event_done = EVENT_DONE;
	// The answer to a configuration write on FR9, which is a class of its
	// own: the kernel's generic event handler does nothing for high
	// nibble 3, and the console driver's reset path waits for exactly
	// that nibble. FOCUS_ASICFGEVENT sweeps it.
	u8 event_configured = EVENT_CONFIGURED;
	unsigned event_delay_ms = 2;
	// How long a Pause order pauses for, in milliseconds. The driver's
	// idle loop is PAU then read transparent status, so this sets its
	// cadence: 1 ms reproduces the ~1000 status reads a second the real
	// driver makes (999 in the second before the console open).
	// FOCUS_ASIPAUSE sweeps it.
	unsigned pause_ms = 1;

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

	// ...and the halfword is only the FIRST TWO BYTES of the answer. The
	// IDY order returns a NINE-byte **identity information block**
	// (27128A Tech Ref 4-67), modelled by `identity_block()` below:
	//
	//        7                                 0
	//   byte 0            card ID = 3
	//        1           firmware ID = 0
	//        2        16-bit binary number
	//        3      firmware revision code
	//        4   MPX                    bp mode
	//        5   No. of active ports - 1 = 0
	//        6   max requests/port - 1 = 2
	//        7        maximum guaranteed
	//        8     data blocking size = 200
	//
	// The powerup scan reads only the first two bytes (`WDC 2`) and types
	// the slot from them, which is why answering just the halfword got
	// that far. **The console open asks for all nine** (`WDC 9`, the same
	// command 0), and a card that delivers two leaves the transfer short:
	// no completion, the driver's timed wait expires with exception 56,
	// the HP27128/30 driver re-raises it, the TTY subsystem retries and
	// then binds the NULL driver instead, and the first request to that
	// stub raises 116 -- "System halted due to system error."
	//
	// The 27110A is NOT the same: its Level-2 identify message is two
	// bytes, id code then capability/revision nibbles (27110A 4-6). The
	// nine-byte block belongs to the Z-80 cards, ASI and MUX alike
	// (27130A 4-46 gives the identical layout with card ID 5).

	// Where characters the host writes go, and where typed characters
	// come from. A host wires these to a terminal.
	std::function<void(u8)> on_char;
	// A character typed at the terminal joins the receive queue; the tick
	// senses the completed record and the host is told at its next Read
	// Transparent Status.
	void type(u8 c)
	{
		m_rx.push_back(c);
		if (dbg())
			dbg()("[%8.3fs asi%u typed %02x, rx=%u]\n", stamp(), pa, c,
					unsigned(m_rx.size()));
	}

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
		ORD_WD  = 7,        // write data
		ORD_RTS = 8,        // read transparent status
		ORD_WTC = 9,        // write transparent control
		// Connect Logical Channel. Not in the 27110A's table -- it lists
		// 10-15 as unsupported, and an HP-IB card has no logical
		// channels -- but the ASI opens every read and write with "a
		// Connect Logical Channel (CLC) order followed by a request
		// block from the host computer" (27128A 4-2), and HP-UX's
		// console driver has a list entry that writes 0xb8 to FR8 and
		// then DMAs seven bytes OUT. Order 11 with an outbound request
		// block is that, and nothing else fits.
		ORD_CLC = 11
	};

	// CLC request codes (27128A 4-4).
	enum : u8
	{
		REQ_READ_DATA       = 1,    // read device data
		REQ_WRITE_DATA      = 2,    // write device data
		REQ_CONTROL_DEVICE  = 3,
		REQ_READ_CARD_INFO  = 4,
		REQ_WRITE_CARD_CFG  = 5,
		REQ_CONTROL_CARD    = 6
	};

	// Read Transparent Status opcodes (27128A 4-60).
	enum : u8
	{
		RTS_IDL = 0,        // nothing to do
		RTS_SWI = 1,        // switch to the logical channel in the LCN
		RTS_EDD = 2,        // end of data for that transaction
		RTS_LCD = 3,        // logical channel destroyed
		RTS_AES = 4,        // asynchronous event sensed; block replaces LCN
		RTS_ERT = 5         // error trap in that transaction
	};

	// Asynchronous event codes, byte 1 of the event block an AES report
	// carries in place of the LCN (27128A 4-61..4-63). Only the ones this
	// card can actually raise are named.
	enum : u8
	{
		AEV_DATA     = 1,   // a data message was received from the device
		AEV_BREAK    = 2,   // break received from the device
		AEV_TX_EMPTY = 3,   // the transmit buffer is empty
		AEV_LINK     = 4    // an opened link was closed or disconnected
	};

	// Host interrupt mask, byte 0 of write-card-configuration subfunction
	// 21 and byte 22 of the whole configuration block (27128A 4-42, 4-53).
	// "IF ALL INTERRUPT CONDITIONS ARE DISABLED, NO ASYNCHRONOUS INTERRUPT
	// WILL OCCUR" (4-32), so the mask is what decides whether an event the
	// firmware senses ever reaches the host.
	enum : u8
	{
		MASK_BREAK    = 0x01,
		MASK_DATA     = 0x02,
		MASK_TX_EMPTY = 0x04,
		MASK_LINK     = 0x08,
		MASK_SIG1     = 0x10,
		MASK_SIG2     = 0x20
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
	// Ready to move data whenever the channel offers it. An order whose
	// data phase runs HOST-to-CARD -- write device data, write control,
	// write transparent control, and the CLC request block that opens
	// every transaction -- is requesting service from the moment the
	// order arrives, because the card is waiting to be told something.
	// Only counting outbound bytes the card already HAS (`m_out`) left
	// the CLC's request block undelivered: the driver issued the order,
	// the channel never moved the seven bytes, and the transaction the
	// whole console open is built on never opened.
	bool service_request() override
	{
		return m_order == ORD_WD || m_order == ORD_WC
				|| m_order == ORD_WTC || m_order == ORD_CLC
				|| !out_now().empty();
	}

	// Device End: the answer this command prepared has been read out in
	// full. The host sizes its count for the largest reply an order can
	// give -- 17 bytes for transparent status, of which an idle card has
	// 3 to say -- so ending the leg here is the normal case, not an
	// error.
	bool data_end() override { return m_answering && out_now().empty(); }

	// Which outbound queue the order in progress is reading. Everything
	// that moves bytes to the host -- the DMA leg, FR0 and the panic
	// printer's byte-at-a-time FR6 -- goes through here, so a status
	// read and a data phase cannot reach into each other's bytes.
	std::deque<u8> &out_now() { return m_order == ORD_RTS ? m_report : m_out; }
	const std::deque<u8> &out_now() const
	{ return m_order == ORD_RTS ? m_report : m_out; }

	// The interface poll must be quiet unless the card wants the channel
	// to start something, as on the 520's boards -- the powerup self
	// test checks that nothing is requesting between its steps.
	// "Each read or write is preceded by a Connect Logical Channel (CLC)
	// order followed by a request block from the host computer. **The CLC
	// request is in response to an SRQ signal, requesting the next order,
	// from the ASI**" (27128A 4-2). So a card that has been told to pause
	// and has nothing in flight is asking for its next order, and that
	// request is what an interface poll reports.
	bool poll_request() override { return m_want_order; }
	// Emulated seconds for the log lines below.
	double stamp() const { return iop ? iop->tnow() : 0.0; }

	// The card's own running commentary, distinct from the FR-register
	// trace above: what it decided and when, in emulated time. One
	// channel serves every card in the machine, as the switch does.
	static knob::channel &dbg()
	{
		static knob::channel c("FOCUS_ASIDBG");
		return c;
	}

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
			if (!m_echo.take(v))
			{
				if (fr1_force)
					// FOCUS_ASIFR1: a constant answer, for finding which
					// bit a driver is really waiting on. It is tested
					// before the presence gate below, because a forced
					// constant the gate can still override is not a
					// constant.
					v = fr1_force & 0xffff;
				else if (!m_echo.present())
					v = 0;
				else
					// The house status register of this machine's CIO
					// cards, established by sweeping constants past the
					// HP-UX 5.30 driver: it polls here and only proceeds
					// when bit 7 AND bit 6 are set, which is the internal
					// disc controller's register exactly (bit 7 configured
					// / self test passed, bit 6 self test complete, bit 0
					// an event waiting). With 0x80 alone the driver polls
					// forever; with 0xc0 it goes on to write FR8 and FR9
					// -- and the FR9 value it writes, 0x20 = (2 << 4) | pa,
					// is the same configuration word LCS80 sends the disc.
					// One protocol, two cards.
					v = 0xc0 | (m_events.empty() ? 0 : 0x01);
			}
			break;
		case 3:
			v = card_id;
			break;
		case 6:
			// The card's byte-at-a-time read port: the register-level
			// twin of an inbound DMA leg, and the same queue behind it.
			// HP-UX's panic printer uses it because a halted machine
			// cannot set up DMA -- it writes an order to FR8, waits for
			// the interface poll unless the order was 0x28, then reads
			// the answer one byte at a time from here
			// (HP-UX KERNEL +0xc67c, `IOR io=0x31` = RIF6).
			v = pop_out();
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
				if (dbg())
					dbg()("[%8.3fs asi%u event %02x read from FR9]\n",
							stamp(), pa, u8(v));
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
		if (log.gate(log_key('R', n, v)))
			log("[asi%u R fr%u = %04x p=%08x]\n", pa, n, v & 0xffff,
					iop ? iop->tpc() : 0);
		return v;
	}

	void write_fr(unsigned n, u32 v) override
	{
		v &= 0xffff;
		if (log.gate(log_key('W', n, v)))
			log("[asi%u W fr%u = %04x p=%08x]\n", pa, n, v,
					iop ? iop->tpc() : 0);
		m_fr[n & 15] = v;
		switch (n)
		{
		case 0:
			m_echo.note_write(v);   // the powerup presence echo
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
			// The configuration word: high nibble a mode, low nibble the
			// peripheral address -- 0x20 = (2 << 4) | pa, exactly what
			// LCS80 sends the disc controller.
			//
			// **A write here is a command, and the card must answer it.**
			// The console driver's reset path (HP-UX KERNEL +0xc63c) is
			// explicit: DCL, DEN, poll FR1 bit 6 (self test done) then
			// bit 7 (configured), write FR9 = (2 << 4) | pa, then
			// `RIF1 / BRO bit 0` in a tight loop until an event is
			// waiting, `RIF9`, and require its HIGH NIBBLE to be 3 --
			// anything else and it goes back to polling. It then repeats
			// the whole handshake for mode 0 and mode 2 again. With the
			// write recorded and nothing announced, that loop is where
			// the console bring-up stopped: no console, and interrupts
			// disabled around it, so nothing else ran either.
			m_config = v;
			arm_event(event_configured, "FR9 config");
			start_event_timer();
			break;
		default:
			break;
		}
	}

	// DMA moves the data part of a transaction: the request block and
	// the write data go out, records and identity blocks come back.
	// The read-status block reports "the length in bytes of the data
	// transfer initiated by the card for the host read or write request"
	// (27128A 4-64), so only the DATA PHASE counts. Counting every leg
	// put the three bytes of a transparent-status read into it, and the
	// console open's read-card-information then reported a transfer of
	// four bytes where the driver asked for one -- HP27128/30 +0x5d8
	// compares the field against the requested length and, on a
	// mismatch, writes 91 into the request's result cell, which +0x1822
	// then raises because it wanted 1.
	bool data_phase() const { return m_order == ORD_RD || m_order == ORD_WD; }
	u8 dma_read() override
	{
		// Only bytes the card has: the IOP's halfword legs pull a pad
		// byte past an odd-length record (an empty pop), which the card
		// did not send -- 'ls\r' was reported as a four-byte transfer.
		// The write side already caps the same way.
		if (data_phase() && !out_now().empty())
			m_transferred++;
		return pop_out();
	}
	void dma_write(u8 b) override
	{
		if (data_phase())
		{
			// The IOP's DMA moves halfwords, so an odd-length write
			// arrives with one padding byte past the request's data
			// transfer length -- whatever happened to follow the record
			// in the host's buffer. The card "initiates" exactly the
			// length the request block asked for (4-64): the padding is
			// neither sent to the device nor counted. Counting it made a
			// 23-byte write report 24, and HP27128/30 +0x2f52 turns that
			// mismatch into 91; the first odd-length console write HP-UX
			// makes is the boot date line, and its 91 unwound the boot
			// task before it brought up the VM device.
			if (m_length && m_transferred >= m_length)
				return;
			m_transferred++;
		}
		take_data(b);
	}

	void dma_complete(bool output) override
	{
		// The CLC order is the one that opens a transaction, and its
		// request block is the outbound leg that follows it (27128A
		// 4-2/4-3). WC carries one too.
		if (output && (m_order == ORD_CLC || m_order == ORD_WC))
			take_request_block();
		// The host decides how much of an answer it wants: the powerup
		// scan asks for the two bytes of the identity block that type
		// the slot, the console open for all nine. Whatever is left when
		// the channel stops goes with the transaction rather than
		// standing as data the card is still offering -- service_request
		// reads m_out, and a leftover tail would claim the card wants
		// another leg.
		if (!output && m_answering)
		{
			out_now().clear();
			m_answering = false;
		}
		// A data phase that has just finished owes the host an EDD:
		// "End of the data transfer for the transaction given in the LCN
		// field" (27128A 4-60). SWI told it to switch to this logical
		// channel and run the transfer; EDD tells it the transfer is
		// over and the status block is next. Reporting IDL there says
		// "nothing to do" about a transaction the host believes is
		// running, and its interrupt handler aborts the channel --
		// DCE, DDMA, DINT, CIRQ, and 66 into every outstanding request.
		if ((m_order == ORD_RD || m_order == ORD_WD) && m_request)
			m_rts_report = RTS_EDD;
		// The data phase of a request that sends the card a block rather
		// than characters: read it now, while it is still whole, and
		// leave nothing behind -- a block still sitting in m_in when the
		// next Connect Logical Channel arrives would be read as that
		// transaction's request block.
		if (output && m_order == ORD_WD && m_request != REQ_WRITE_DATA)
		{
			if (m_request == REQ_WRITE_CARD_CFG)
				take_card_configuration();
			m_in.clear();
		}
		// "The transmit buffer is empty" -- event 3, and the one the tty
		// subsystem's drain waits for. Every character of this write has
		// been handed to the terminal, which on this card is as far as
		// the SIO ever gets (27128A 4-31, 4-62). Without it the console
		// driver's event daemon is never woken, the drain flag in its
		// channel record is set by every write and cleared by nothing,
		// and the boot stops in TTY SUBSYSTEM entry 2 the moment it has
		// finished printing the boot report. See docs/HPUX.md 8.
		if (output && m_order == ORD_WD && m_request == REQ_WRITE_DATA)
			raise_async(AEV_TX_EMPTY, MASK_TX_EMPTY, "write device data done");
		// The transfer the command asked for is done, so the card
		// announces the completion the driver is waiting for.
		start_event_timer();
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
		if (m_event_timer && --m_event_timer == 0 && !m_pending.empty())
		{
			const u8 code = m_pending.front();
			m_pending.pop_front();
			if (dbg())
				dbg()("[%8.3fs asi%u event %02x queued, %u pending]\n",
						stamp(), pa, code, unsigned(m_pending.size()));
			m_events.push_back(code);
			if (iop)
				iop->raise(pa);
			// One announcement per tick; whatever is left keeps the
			// clock running so nothing is stranded.
			start_event_timer();
		}
		// The pause the PAU order asked for, counted out; when it runs
		// down the card asks for its next order.
		if (m_pause_ticks && --m_pause_ticks == 0)
			m_want_order = true;
		if (!m_rx.empty() && m_arq_enabled && !m_request && record_ready())
		{
			// "If no read request is pending, and the card has a record
			// available for the host, and the host interrupt mask allows
			// 'data available' interrupt, an asynchronous event is sent
			// to the host to start the read" (27128A 4-6). The status
			// byte is only half of that: the event itself is told
			// through the next Read Transparent Status, as event 1.
			//
			// SENSING the event and POSTING the status are two different
			// things, and only the second is a handshake with the host.
			// Gating both on "no status outstanding" made the terminal
			// deaf: this driver polls the card continuously (PAU, read
			// transparent status, repeat) and every one of those reads
			// answers with a completion event of its own, so a status is
			// outstanding almost always -- with a character waiting in
			// the receive queue, `posted` was 1 on every tick from the
			// keystroke to the end of the run and the shell never saw
			// the line. The firmware notices the record whatever the
			// host is in the middle of; the status post still waits its
			// turn, and the next Read Transparent Status tells it.
			raise_async(AEV_DATA, MASK_DATA, "record ready");
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
	// FR8 carries the **order byte**, and the opcode is in bits 7:4 --
	// the documented CIO vocabulary, not a private one. Three of three
	// values HP-UX writes here decode under it and nothing else:
	//
	//   0x00  IDY  identify   -> the nine-byte identity block, and the
	//                           powerup scan reads its first two bytes
	//   0x88  RTS  read transparent status -> the driver asks for 17
	//                           bytes, the length of the longest reply
	//   0x58  WC   write control -> the HP-IB card gets this one, and
	//                           the request block follows on an OUTPUT
	//                           leg, which is exactly what WC means
	//
	// This used to be modelled as a separate "transaction command"
	// register with its own two-case switch beside an order decoder on
	// FR9 that nothing ever wrote. One vocabulary, one decoder.
	void transaction(u8 c)
	{
		m_command = c;
		write_order(c);
	}

	void write_order(u8 o)
	{
		m_order = u8(o >> 4);
		m_in.clear();
		// The SRQ this models asks for one thing: "The CLC request is in
		// response to an SRQ signal, requesting the next order, from the
		// ASI" (4-2). So a CLC answers it and nothing else does --
		// clearing it on every order let a status read satisfy a request
		// meant for the transaction that follows, and two pieces of host
		// code polling the same card then stole the signal from each
		// other.
		if (u8(o >> 4) == ORD_CLC)
			m_want_order = false;
		if (dbg())
			dbg()("[%8.3fs asi%u order %02x want_order=%d]\n",
					stamp(), pa, o, m_want_order);
		switch (m_order)
		{
		case ORD_IDY:
			m_out.clear();
			for (u8 b : identity_block())
				m_out.push_back(b);
			answer();
			break;
		case ORD_RS:
			read_status_block();
			break;
		case ORD_RD:
			// The data phase of whatever transaction is open. A CLC has
			// already said what that is: a read-device-data request
			// wants a record off the terminal, but read-card-information
			// (and any other request that prepares its own answer) has
			// ALREADY put the bytes in m_out, and fetching a terminal
			// record here would throw them away -- which is exactly what
			// happened to the console open's first transaction, the
			// switch byte it asks for with subfunction 253.
			if (m_out.empty())
				deliver_record();
			if (!m_out.empty())
				answer();
			break;
		case ORD_RTS:
			// Read Transparent Status: `opcode`, then a 16-bit logical
			// channel number -- except under AES, where an event block
			// of up to six bytes replaces the LCN (27128A 4-60/4-61).
			// Opcode 0 is IDL, "nothing to do".
			//
			// This is the card's side of the conversation: RTS is how it
			// tells the host what it wants, and the host's interrupt
			// service dispatches on the opcode through a six-arm table
			// (CORE SYSTEM +0x384c, arms 0..5). FOCUS_ASIRTS forces the
			// opcode and FOCUS_ASIEVENT2 the event code under AES, which
			// is how the arms get identified.
			m_report.clear();
			{
			// A report the card owes the host takes precedence over the
			// swept default, and is consumed by the read.
			u8 op = m_rts_report ? m_rts_report : rts_opcode;
			// An asynchronous event the firmware has sensed is told
			// here, and only here -- but never over the top of a
			// transaction: the SWI/EDD/status conversation belongs to
			// the logical channel that is open, and AES replaces the
			// LCN field, so it has to wait until that channel has been
			// released (27128A 4-60/4-61).
			if (!m_rts_report && op == RTS_IDL && !m_request
					&& !m_async.empty())
				op = RTS_AES;
			m_rts_report = 0;
			if (dbg())
				dbg()("[%8.3fs asi%u RTS reports %u lcn=%u]\n",
						stamp(), pa, op,
						op == RTS_AES ? 0u : unsigned(rts_lcn));
			m_report.push_back(op);
			if (op == RTS_AES)
			{
				// The event block: code, then the port ID, then whatever
				// the code carries with it -- nothing, for the events
				// this card raises. FOCUS_ASIEVENT2 still names the code
				// when the opcode was forced rather than sensed.
				u8 code = rts_event;
				if (!m_async.empty())
				{
					code = m_async.front();
					m_async.pop_front();
				}
				m_report.push_back(code);         // event code
				m_report.push_back(0);            // port ID: the ASI has one
			}
			else
			{
				m_report.push_back(u8(rts_lcn >> 8));
				m_report.push_back(u8(rts_lcn));
			}
			}
			answer();
			break;		case ORD_DIS:
			break;
		case ORD_WC:
			// The request block arrives on the next OUTBOUND leg;
			// dma_complete hands it to take_request_block.
			break;
		case ORD_PAU:
			// Pause, and it is a WAIT, not a no-op: the driver's polling
			// list is PAU, read transparent status, repeat, and the only
			// thing that decides how fast that loop runs is how long the
			// card takes to ask for its next order afterwards. Answering
			// instantly used to be harmless because the IOP's poll ran
			// once a millisecond and set the cadence; once a list may
			// resume the moment the poll is up (focusiop.h, CMD_ICEI),
			// an instant answer turns the loop into a tight ring of
			// interrupt, ICEI, status, interrupt -- and the console
			// driver's own task never runs, so its open times out.
			// Measured: the whole open moved from 2.32 s to 4.42 s and
			// the machine went from 147% speed to 75%.
			m_want_order = false;
			m_pause_ticks = pause_ms ? pause_ms : 1;
			break;
		default:
			// The card "will accept and discard (thus effectively
			// terminating) commands which are not supported"
			// (27110A 4-2).
			break;
		}
	}

	// Arm a completion code: at most one of each kind outstanding.
	//
	// Both halves matter. Appending rather than overwriting is what
	// keeps the printer's `configured` alive when the driver's list arms
	// `done` a few microseconds later -- the bug this replaces. But a
	// plain queue is just as wrong the other way: `answer()` arms `done`
	// on every transparent-status read, a thousand times a boot, so the
	// backlog ran to 137 entries and the printer's code sat behind all
	// of them at one announcement per tick. Collapsing repeats of the
	// same code is what the old single slot did correctly, and it bounds
	// the queue at the number of distinct codes -- two or three here.
	void arm_event(u8 code, const char *why)
	{
		if (std::find(m_pending.begin(), m_pending.end(), code)
				!= m_pending.end())
			return;
		if (dbg())
			dbg()("[%8.3fs asi%u event %02x armed (%s), %u pending]\n",
					stamp(), pa, code, why,
					unsigned(m_pending.size()) + 1);
		m_pending.push_back(code);
	}

	// Start the announcement clock if it is not already running. The
	// codes armed by answer() and the write family wait for the data to
	// actually move -- dma_complete starts it for them -- while an FR9
	// configuration write is its own completion and starts it at once.
	void start_event_timer()
	{
		if (!m_event_timer && !m_pending.empty())
			m_event_timer = event_delay_ms ? event_delay_ms : 1;
	}

	// The order has an answer ready: the host will read it out, and the
	// completion event follows.
	void answer()
	{
		m_answering = true;
		arm_event(event_done, "answer");
	}

	// The identity information block (27128A 4-67).
	std::vector<u8> identity_block() const
	{
		// Byte 0 is the CARD ID and byte 1 the firmware ID -- exactly the
		// halfword the powerup scan types the slot from, so both come
		// from `identity` rather than from `card_id`, which is FR3's
		// readiness value and a different thing entirely.
		//
		// The diagram draws MPX at the bit-7 end of byte 4 and `bp mode`
		// at the bit-0 end without giving either field's width, so the
		// shift here is a choice the document does not make.
		return {
			u8(identity >> 8),
			u8(identity),
			u8(firmware_rev >> 8), u8(firmware_rev),
			u8(mpx << 6),
			u8(ports - 1),
			u8(requests - 1),
			u8(blocking >> 8), u8(blocking)
		};
	}

	void take_data(u8 b)
	{
		// Under a write-data order the bytes are the DATA PHASE of
		// whatever request the CLC opened, and only write device data's
		// are characters for the terminal. Write card configuration,
		// control device and control card send their data blocks on the
		// same order (27128A 4-4), and the card has to READ those: taking
		// every WD byte for a character threw the configuration away --
		// with the visible symptom that the 58-byte block the console
		// open writes was printed to the terminal as garbage ahead of the
		// boot banner. Under write-control the bytes are the request
		// block, as before.
		if (m_order == ORD_WD && m_request == REQ_WRITE_DATA)
		{
			if (on_char)
				on_char(b);
			return;
		}
		m_in.push_back(b);
	}

	// The configuration block a write-card-configuration request just
	// delivered. Only the host and modem interrupt masks are kept: they
	// are what gates every asynchronous event, and the rest of the block
	// describes a real UART's framing, which this card does not model.
	// Subfunction 0 writes the whole block, where the masks are bytes 22
	// and 23; subfunction 21 writes just the two of them (27128A 4-42,
	// 4-53).
	void take_card_configuration()
	{
		unsigned at = ~0u;
		if (m_subfunction == 0 && m_in.size() > 23)
			at = 22;
		else if (m_subfunction == 21 && m_in.size() >= 2)
			at = 0;
		if (at != ~0u)
		{
			m_host_mask = m_in[at];
			m_modem_mask = m_in[at + 1];
			if (dbg())
				dbg()("[%8.3fs asi%u interrupt mask host=%02x modem=%02x"
						" (sf %u)]\n", stamp(), pa, m_host_mask,
						m_modem_mask, m_subfunction);
		}
		m_in.clear();
	}

	// Sense an asynchronous event. The mask decides whether it is
	// announced at all; a code already waiting is not repeated, which is
	// the same collapsing arm_event() does and for the same reason -- the
	// firmware has one event of each kind to tell, not a backlog.
	void raise_async(u8 code, u8 mask, const char *why)
	{
		if (no_async || !(m_host_mask & mask))
			return;
		if (std::find(m_async.begin(), m_async.end(), code) != m_async.end())
			return;
		if (dbg())
			dbg()("[%8.3fs asi%u async event %u sensed (%s)]\n",
					stamp(), pa, code, why);
		m_async.push_back(code);
	}

	// A Connect Logical Channel request block arrived (27128A 4-3).
	//
	// The field order is the one the prose lists it in -- logical channel
	// number, then the S bit, BLK and the request code, then subfunction,
	// port and length -- so the request code is in BYTE 2, not byte 0:
	//
	//   byte 0-1  logical channel number, host-assigned
	//   byte 2    S | BLK | -- | -- | request code (4 bits)
	//   byte 3    subfunction code
	//   byte 4    port ID (the ASI has one port, so always 0)
	//   byte 5-6  data length, 16-bit unsigned
	//
	// The console driver's first request settles it: `00 00 04 fd 00 00
	// 01` reads as request 4 "read card information", subfunction 253
	// "get the card switch setting", one byte -- coherent, and the only
	// coherent reading. Taking the code from byte 0 gives request 0,
	// which the same page marks "not used".
	void take_request_block()
	{
		if (m_in.size() < 7)
			return;
		m_channel     = u16((m_in[0] << 8) | m_in[1]);
		m_keep_record = (m_in[2] & 0x80) != 0;      // the S bit
		m_block       = (m_in[2] & 0x40) != 0;      // BLK
		m_request     = u8(m_in[2] & 0x0f);
		m_subfunction = m_in[3];
		m_port        = m_in[4];
		m_length      = u16((m_in[5] << 8) | m_in[6]);
		if (log)
		{
			std::string hex;
			char b2[4];
			for (u8 b : m_in)
			{
				std::snprintf(b2, sizeof b2, " %02x", b);
				hex += b2;
			}
			log("[asi%u request code=%u channel=%u sf=%u port=%u "
					"len=%u raw%s]\n", pa, m_request, m_channel,
					m_subfunction, m_port, m_length, hex.c_str());
		}
		m_in.clear();
		m_transferred = 0;
		m_request_status = 0;       // no error unless something says so
		// "SWI (switch) -- Switch to the logical channel given in the LCN
		// field and continue the transaction there" (4-60). A CLC opens
		// a transaction on a logical channel; SWI is how the card tells
		// the host that transaction is ready to run, and the host will
		// keep re-issuing the CLC until it hears it.
		m_rts_report = clc_report ? clc_report : RTS_SWI;
		rts_lcn = lcn_force ? lcn_force : m_channel;
		// A read request ends the way a write request does: with the card
		// asking for its next order. "The CLC request is in response to
		// an SRQ signal, requesting the next order" (27128A 4-2), and the
		// sibling firmware's order processing "exit[s] ... [and]
		// initiates an SRQ for the next order" (27130A firmware IMS
		// 13.2.3.3). Only the write family did. After a CLC for a read
		// the card sat silent with its SWI ready, and the read went ahead
		// only if one of the driver's timed PAUs happened to end in
		// between and raise the request; when the driver ran the RTS and
		// the RD back to back, the RD's command list stopped at its next
		// ICEI waiting for a request that never came, and the console
		// took no more input for the rest of the run (keystrokes queued,
		// card disarmed by the driver's DCE/DINT at CORE SYSTEM
		// +0x4134). Measured on six console logins: three died that way
		// before this, none after. Read card information is left as it
		// was: raising the request there too broke the console open at
		// FOCUS_IOPPOLL 300 and 600, so for that request the rule is not
		// established.
		switch (m_request)
		{
		case REQ_READ_DATA:
			deliver_record();
			m_want_order = true;
			break;
		case REQ_READ_CARD_INFO:
			read_card_information();
			break;
		default:
			// Write device data, control device, write card
			// configuration and control card all take their data on a
			// following leg, which the host sends as the next order --
			// and the card has to ASK for that order: "The CLC request
			// is in response to an SRQ signal, requesting the next
			// order, from the ASI" (4-2). Without the ask, the host has
			// opened a transaction and is waiting for a card that is
			// waiting for it.
			m_want_order = true;
			arm_event(event_done, "write-family");
			break;
		}
	}

	// The Read Status block (27128A 4-64), returned "during the status
	// part of a transaction. Following the status block, the logical
	// channel is automatically disconnected and the LCN is released."
	//
	//   byte 0    card dependent request status (0 = no error)
	//             -- ends here for all CONTROL requests
	//   byte 1-2  length of data transfer
	//             -- ends here for all WRITE requests
	//   byte 3-4  length of data remaining in the current record
	//             -- ends here for READ CARD INFORMATION
	//   byte 5    message type for a read request
	//   byte 6    text terminator for message type 1
	//             -- ends here for READ DEVICE DATA
	//
	// So the block is 1, 3, 5 or 7 bytes by request type, and the host
	// reads it with a count sized for the longest -- the short ones end
	// on Device End. This is the status the console open waits for; a
	// single byte where seven were due left it hanging.
	void read_status_block()
	{
		m_out.clear();
		m_out.push_back(m_request_status);
		if (m_request != REQ_CONTROL_DEVICE && m_request != REQ_CONTROL_CARD)
		{
			m_out.push_back(u8(m_transferred >> 8));
			m_out.push_back(u8(m_transferred));
			if (m_request != REQ_WRITE_DATA && m_request != REQ_WRITE_CARD_CFG)
			{
				const u16 left = u16(m_rx.size());
				m_out.push_back(u8(left >> 8));
				m_out.push_back(u8(left));
				if (m_request == REQ_READ_DATA)
				{
					// Message type 1 is "text terminated by the single
					// text terminator" (4-65/4-66); byte 6 is the
					// terminator that ended the record.
					m_out.push_back(m_message_type);
					m_out.push_back(terminator);
				}
			}
		}
		m_status_posted = false;
		answer();
		// "Following the status block, the logical channel is
		// automatically disconnected and the LCN is released." With the
		// transaction over the card wants its next order, and saying so
		// is what lets the host's suspended command list run again and
		// refresh the report buffer -- its interrupt handler loops back
		// and re-dispatches whatever that buffer still holds.
		m_request = 0;
		m_want_order = true;
		// ...and it owes the host NOTHING further. dma_complete arms EDD
		// when the data phase ends, which is right between the data and
		// the status block; once the status block has been read the
		// transaction is over and the next transparent status must read
		// IDL. Leaving EDD standing makes the driver's handler take its
		// EDD arm, which is not an acknowledgement -- it ARMS A TRANSFER
		// (CORE SYSTEM +0x3c88, CRB against states 4..16, state := 24) --
		// against a channel the completion has already closed.
		m_rts_report = 0;
	}

	// Read Card Information (request code 4, 27128A 4-38..4-41). The
	// answer is truncated to the length the host asked for; the residue
	// is what the read status block reports.
	void read_card_information()
	{
		m_out.clear();
		switch (m_subfunction)
		{
		case 253:
			// Get the card switch setting, "1 -> open", D7..D0 = SW1..SW8:
			// single text termination, hard-wired, no parity, 8 data
			// bits, then four baud-rate bits (2-3/2-5, table 2-2).
			m_out.push_back(switches);
			break;
		case 254:
			// Get card status, cleared by the read.
			m_out.push_back(m_card_status);
			m_card_status = 0;
			break;
		case 255:
			// Read modem line status: which line changed, then the
			// current state (4-41). Nothing here has a modem, but a
			// hard-wired terminal still presents its handshake lines
			// asserted, and the driver reads them at open: with every
			// line reported down, HP-UX's tty layer treated the console
			// as a modem port waiting for carrier -- its writes sat in a
			// half-second retry loop and init never reached the screen.
			m_out.push_back(0);
			m_out.push_back(0x1f);
			break;
		default:
			// Subfunctions 0 and 1..32 read back the configuration
			// written by the matching write-card-configuration
			// subfunction; none is modelled yet, so answer the length
			// asked for with zeros rather than stalling the leg.
			for (unsigned k = 0; k < m_length && k < 64; k++)
				m_out.push_back(0);
			break;
		}
		while (m_out.size() > m_length && m_length)
			m_out.pop_back();
		answer();
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
		m_report.clear();
		m_async.clear();
		m_status_posted = false;
		m_order = 0xff;
	}

	u8 pop_out()
	{
		std::deque<u8> &q = out_now();
		if (q.empty())
			return 0;
		const u8 b = q.front();
		q.pop_front();
		return b;
	}

	std::deque<u8> m_events;    // event codes waiting to be read from FR9
	// Codes armed but not yet announced. This is a QUEUE, not a slot:
	// two conversations can be open on this card at once -- the driver's
	// IOP command list and the kernel's register-level panic printer --
	// and with one slot the second arm destroyed the first. Measured:
	// the printer writes FR9 and arms `configured` (0x30), the list arms
	// `done` (0x10) a few microseconds later, 0x10 is what gets queued,
	// and the printer rejects it (it requires high nibble 3) and polls
	// FR1 for ever. See docs/HPUX.md 6.
	std::deque<u8> m_pending;   // posted when the command's transfer ends
	unsigned m_event_timer = 0;
	std::deque<u8> m_rx;        // characters typed at the terminal
	std::deque<u8> m_out;       // what the host reads next
	// Read Transparent Status answers out of its OWN buffer, and has to.
	// RTS is the card's out-of-band channel -- "what do you want next?"
	// -- asked between the legs of a transaction the card is already in
	// the middle of, and answering it used to `m_out.clear()`. So the
	// console open went: CLC prepares the switch byte subfunction 253
	// asks for; the driver's polling list reads transparent status and
	// throws that byte away; the RD leg that should have carried it then
	// has nothing to give, does not assert Device End, and never
	// completes at all -- measured, no DMA-done and no raise for the
	// whole leg. The guard in ORD_RD below was written to protect that
	// byte and cannot, because it is already gone by the time RD runs.
	std::deque<u8> m_report;    // ...and what it reads for an RTS
	std::vector<u8> m_in;       // what the host has written
	// Asynchronous events the firmware has sensed and not yet told, in
	// the order it sensed them; an AES report takes one off the front.
	std::deque<u8> m_async;
	u8 m_host_mask = 0;         // which events may be announced...
	u8 m_modem_mask = 0;        // ...and which modem lines, unused here

	u32 m_fr[16] = {};
	u8 m_order = 0xff;
	// True while a command's prepared answer is being read out; see
	// data_end().
	bool m_answering = false;
	bool m_keep_record = false; // the request block's S bit
	bool m_block = false;       // ...and BLK
	u8 m_card_status = 0;       // what subfunction 254 reports
	unsigned m_pause_ticks = 0; // milliseconds left of a PAU order
	u8 m_rts_report = 0;        // a report owed to the host, or 0
	u8 m_request_status = 0;    // byte 0 of the read-status block
	u16 m_transferred = 0;      // ...and its length-of-transfer field
	u8 m_message_type = 1;      // text terminated by the terminator
	// The card wants its next order; the interface poll reports it.
	bool m_want_order = false;

	u8 m_command = 0xff;
	u32 m_config = 0;
	u8 m_status = 0;
	bool m_status_posted = false;
	bool m_arq_enabled = true;
	bool m_ard = false;

	// The current request block's fields.
	u8 m_request = 0;
	u16 m_channel = 0;
	u8 m_subfunction = 0;
	u8 m_port = 0;
	u16 m_length = 0;

	// The powerup presence test's echo, as on every other card here.
	presence_echo m_echo;

public:
	// What the card reports through Read Transparent Status, and the
	// event code it carries under AES. Sweepable; see write_order().
	// The card's configuration switches, D7..D0 = SW1..SW8, a set bit
	// meaning OPEN (2-3, table 2-2): single text termination enabled,
	// hard-wired (a terminal is directly connected and modem control is
	// off -- with this one CLOSED the card will not move data until the
	// host makes a line connection request), no parity, 8 data bits, and
	// 1110 for 9600 baud. That is the 530/540 console exactly as the CE
	// handbook has the operator set the terminal up. FOCUS_ASISWITCHES
	// sweeps it.
	u8 switches = 0xfe;

	u8 rts_opcode = RTS_IDL;
	u8 rts_event = 0;
	u16 rts_lcn = 0;
	// FOCUS_ASILCN overrides the logical channel number the card echoes
	// in its SWI report, for identifying what the kernel expects there.
	u16 lcn_force = 0;
	// FOCUS_ASICLCRTS overrides what the card reports after a CLC.
	u8 clc_report = 0;
	// FOCUS_ASINOASYNC suppresses every asynchronous event the firmware
	// senses, which is the card as it behaved before they were modelled:
	// the A/B control for the console drain (docs/HPUX.md 8).
	bool no_async = false;
};

} // namespace focus

#endif // MAME_IOP_FOCUSASI_H
