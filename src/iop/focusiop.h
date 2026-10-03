// license:BSD-3-Clause
// copyright-holders:
/***************************************************************************

    HP FOCUS I/O Processor

    The IOP is one of the five chips of the FOCUS set. It presents eight CIO
    channels, each with DMA, and handles interrupt collection and the system
    interval timer. The CPU talks to it over the Memory Processor Bus with
    the IOW, IOR and IOC instructions.

    Reference: FOCUS I/O System ERS, F. Gross, 09/20/82. Command opcodes are
    from section 5 (sheets 29-42) and are quoted in octal, as the ERS does.

    Kept free of MAME dependency so it can be unit tested; the MAME device
    supplies memory access and the interrupt line.

***************************************************************************/

#ifndef MAME_IOP_FOCUSIOP_H
#define MAME_IOP_FOCUSIOP_H

#pragma once

#include "../cpu/focus/focusaddr.h"

#include <cstdio>
#include <cstdlib>
#include <functional>

namespace focus {

//**************************************************************************
//  MPB I/O COMMANDS
//**************************************************************************

// Each opcode indexes one of 128 words of IOP control store, which form a
// jump table to the command routines (ERS sheet 29). R, W and C mark reads,
// writes and controls.
enum iop_cmd : u8
{
	// DMA
	CMD_WDS   = 003,   // W  write DMA status
	CMD_WDC   = 004,   // W  write DMA count
	CMD_WDA   = 005,   // W  write DMA start address
	CMD_WDTF  = 006,   // W  write DMA termination field
	CMD_RDCS  = 010,   // R  read DMA current count and status
	CMD_RDMK  = 013,   // R  read DMA mask
	CMD_CDMK  = 014,   // C  clear DMA mask
	CMD_EDMA  = 015,   // C  enable DMA
	CMD_DDMA  = 016,   // C  disable DMA
	CMD_SD    = 023,   // C  start DMA

	// Interrupts
	CMD_WIMK  = 025,   // W  write interrupt mask
	CMD_RIMK  = 026,   // R  read interrupt mask
	CMD_RINT  = 027,   // C  request interrupt
	CMD_CIRQ  = 030,   // C  clear interrupt request
	CMD_WIMG  = 033,   // W  write interrupt message
	CMD_RIRQ  = 034,   // R  read interrupt request
	CMD_SIL   = 035,   // W  set interrupt level
	CMD_EINT  = 036,   // C  enable interrupt
	CMD_DINT  = 037,   // C  disable interrupt

	// Interval timer
	CMD_SDEC  = 017,   // C  start DMA, enable interrupt, clear IRQ
	CMD_SDC   = 020,   // C  start DMA, clear IRQ
	CMD_SRC   = 024,   // C  start RAMD, clear IRQ
	CMD_WTMS  = 040,   // W  write timer start
	CMD_WTMC  = 043,   // W  write timer count
	CMD_RTM   = 044,   // R  read timer count
	CMD_ETMI  = 045,   // C  enable timer interrupt

	// Command lists (channel programs)
	CMD_ICEI  = 070,   // C  initialize command execution from interrupt

	// Interfaces
	CMD_IFPL  = 047,   // R  interface poll
	CMD_RIF1  = 011,   // R  read interface FR1, by the 8n+1 rule
	//
	// The HP-UX 5.30 powerup module's device scan also issues opcodes
	// 002, 012, 031, 0102 and 0132, none of which the Dec-82 ERS names --
	// the shipped IOP evidently postdates it. They are accepted as
	// no-ops (reads return zero), which the scan reads as "no device
	// present" and moves on. Real boot code additionally issues 000,
	// still refused until its effect is understood. (065 is no longer
	// among them: it is Increment and Branch, one of the list-only
	// opcodes -- see run_command_list.)
	CMD_WDPA  = 054,   // W  write DMA peripheral address
	CMD_RDPA  = 055,   // R  read DMA peripheral address
	CMD_RDB   = 056,   // R  read data buffer
	CMD_RIRV  = 057,   // R  read revision
	CMD_RIOB  = 050,   // C  reset I/O bus
	CMD_RIOP  = 053,   // C  reset the IOP itself
	CMD_WMCN  = 060,   // W  write MPB channel number
	CMD_CALM  = 063,   // C  clear address lockout mode
	CMD_SALM  = 064,   // C  set address lockout mode
	CMD_DCE   = 073,   // C  disable IOP command execution
	CMD_LON   = 075,   // C  turn LED on
	CMD_LOFF  = 076,   // C  turn LED off
	CMD_RIST  = 0103,  // R  read interface status
	CMD_RIFG  = 0104,  // R  read interface flag
	CMD_RDTF  = 0105,  // R  read DMA termination field
	CMD_WAMK  = 0106,  // W  write attention poll mask
	CMD_CAAK  = 0107,  // C  clear attention acknowledge bit
	CMD_WDB   = 0117,  // W  write DMA data buffer
	CMD_WDCA  = 0120,  // W  write DMA current address
	CMD_RIMG  = 0123   // R  read interrupt message
};

//**************************************************************************
//  INTERFACE ATTACHMENT
//**************************************************************************

// One CIO interface card. The IOP polls these and moves their data.
class iop_interface
{
public:
	virtual ~iop_interface() = default;

	// Status and flag words the CPU can read back.
	virtual u32 status() { return 0; }
	virtual u32 flag() { return 0; }

	// True when the card wants a DMA transfer.
	virtual bool service_request() { return false; }

	// True when the card is ending the data phase of the current order
	// short of the channel's count -- the DEND line. A card answers an
	// order with exactly as much as that order has to say, and the host
	// programs a count big enough for the largest possible answer, so
	// almost every inbound leg ends this way rather than on count.
	virtual bool data_end() { return false; }

	// What the interface poll (IFPL) reports for this card. It defaults
	// to the transfer request, but the two are not the same thing: the
	// powerup self test drives the poll bit through the card's control
	// registers and expects it to follow them, while a display stands
	// ready to take data whenever the IOP offers it.
	virtual bool poll_request() { return service_request(); }

	// True when the card is raising the burst-request line: with the
	// channel's DMA registers programmed, a burst request moves data
	// without any start command. The internal disc driver relies on this
	// -- it programs WDS/WDC/WDA, commands the card through FR1, and
	// waits for the completion interrupt without ever issuing SD.
	virtual bool burst_request() { return false; }

	// The transfer the channel was counting is complete (the count
	// reached zero). Lets a card interpret a command block it was sent.
	virtual void dma_complete(bool output) { }

	// True while the card is asserting ASYNC outside a transfer -- an
	// attention request, e.g. a keystroke waiting (I/O ERS section 4.4).
	virtual bool attention() { return false; }

	// A single DMA transfer in each direction; the halfword forms serve
	// status-word bit 29 clear and default to byte pairs.
	virtual u8 dma_read() { return 0; }
	virtual void dma_write(u8) { }
	// High byte first, both ways: FOCUS is big-endian, and dma_write16
	// below has always sent the high byte first because two statements
	// are sequenced.
	//
	// The read had to be written the same way rather than as
	// `u16((dma_read() << 8) | dma_read())`, which is what it was. The
	// operands of | are NOT sequenced against each other in C++, so which
	// of those two calls ran first was up to the compiler -- and each one
	// takes a byte off the card's queue, so the choice decided the byte
	// order of every halfword the IOP read. GCC takes the left one first
	// and the emulator was built on that; nothing in the language says it
	// must, and a compiler that chose otherwise would have swapped every
	// halfword with no diagnostic anywhere.
	virtual u16 dma_read16()
	{
		const u8 hi = dma_read();
		const u8 lo = dma_read();
		return u16((hi << 8) | lo);
	}
	virtual void dma_write16(u16 v) { dma_write(u8(v >> 8)); dma_write(u8(v)); }

	// Direct CPU access to the card.
	virtual void write_data(u32) { }
	virtual u32 read_data() { return 0; }

	// Direct I/O: the interface's sixteen FR registers, reached with the
	// RIF n / WIF n commands (I/O ERS sheet 33: opcodes 8n+1 and 8n+2).
	// A control strobe on one of the interface's registers. The read and
	// write forms of a register are opcodes 8n+1 and 8n+2 (I/O ERS sheet
	// 33), and the internal disc driver issues 8n as well -- FR9 written,
	// then control 070 -- so this reads as the third form. The meaning is
	// inferred from that pattern, not from the ERS.
	virtual void control_fr(unsigned) { }
	// The interface status register, read with RIST -- a full word the
	// card composes, distinct from the one-bit STATUS line RISF and the
	// poll report. The self test prints it verbatim ("Self test error 1:
	// IO address 006,0002 STATUS: 00000001"), so zero is healthy.
	virtual u32 status_reg() { return 0; }
	virtual u32 read_fr(unsigned) { return 0; }
	virtual void write_fr(unsigned, u32) { }
};

//**************************************************************************
//  PRESENCE ECHO
//**************************************************************************

// The powerup presence test, which every card in the backplane answers
// the same way. Before a slot has proven itself FR1 reads zero -- the
// scan gives up on one that answers anything else. The scan then writes
// a pattern to the card's data latch and reads it straight back from
// FR1, and only after that echo is FR1 a status register at all.
//
// Three cards carried their own copy of the handshake, down to the same
// three members; the comment on the ASI's copy read "exactly as on the
// disc controller". This is that one copy. A card keeps it as a plain
// member and asks it first in FR1, so what the register means for the
// rest of its life is the card's own business.
class presence_echo
{
public:
	// The write side: arm the echo, but only until the slot has
	// answered one. A card that has proven itself latches nothing.
	void note_write(u32 v)
	{
		if (!m_present)
		{
			m_last = v & 0xffff;
			m_pending = true;
		}
	}

	// The read side: true when this FR1 read is the echo, which leaves
	// the value to answer in `v` and the card present from here on.
	bool take(u32 &v)
	{
		if (!m_pending)
			return false;
		v = m_last;
		m_pending = false;
		m_present = true;
		return true;
	}

	bool present() const { return m_present; }

private:
	bool m_present = false;
	bool m_pending = false;
	u32 m_last = 0;
};

//**************************************************************************
//  IOP
//**************************************************************************

class iop_core : public io_interface
{
public:
	static constexpr unsigned CHANNELS = 8;      // peripheral addresses 0-7

	// The IOP moves data to and from main memory on the card's behalf, and
	// pulls the CPU's interrupt line.
	class host
	{
	public:
		virtual ~host() = default;
		virtual memory_interface &iop_memory() = 0;
		virtual void iop_interrupt(bool state) = 0;
	};

	explicit iop_core(host &h, u8 channel = 1) : m_host(h), m_channel(channel) { }

	void reset()
	{
		// A reset clears the channel registers but does not unplug the
		// cards: what is in a slot stays there.
		for (auto &c : m_chan)
		{
			iop_interface *const iface = c.iface;
			c = channel_state();
			c.iface = iface;
		}
		m_int_mask = 0;
		for (auto &c : m_chan)
			c.int_enabled = false;
		m_int_request = 0;
		m_int_posted = 0;
		m_int_event = false;
		m_int_message = 0;
		m_attn_mask = 0;
		m_attn_log = 0;
		m_attn_event = false;
		m_timer_start = 0;
		m_timer_count = 0;
		m_timer_enabled = false;
		m_timer_frac = 0;
		m_led = false;
		m_lockout = false;
		m_enabled = true;
		m_int_enable = true;
		update_interrupt();
	}

	void attach(unsigned pa, iop_interface *iface)
	{
		if (pa < CHANNELS)
			m_chan[pa].iface = iface;
	}

	// Hardware-default interrupt enable: "Select code 6 is enabled for
	// keyboard and printer interrupts at all times" (520 service manual
	// 2-30) -- some channels come up armed without any EINT.
	void enable_interrupt(unsigned pa)
	{
		if (pa < CHANNELS)
			m_chan[pa].int_enabled = true;
	}

	u8 channel_number() const { return m_channel; }
	bool led() const { return m_led; }

	// Which of a card's sixteen registers an operation names. There are
	// two ways to say it and the system uses both. The opcode form is
	// 8n+1 / 8n+2 (I/O ERS sheet 33). The other is the ADDRESS: the four
	// NIC lines the 520 manual describes ("one of 16 input/output
	// registers can be selected by the four NIC lines", 2-47) are the
	// IC field of the interface address, and the kernel's own register
	// primitives build their operand as `select code | register << 6`
	// and then use plain read/write -- opcodes 001 and 002. Reading the
	// register from the opcode alone sent every one of those accesses to
	// register 0, which is why the keyboard board's self test wrote a
	// walking bit at register 15 and read back nothing like it.
	static unsigned register_of(const target &t)
	{
		const unsigned from_opcode = t.opcode >> 3;
		return from_opcode ? from_opcode : t.ic;
	}

	// Where the processor is, for I/O logs: log lines that carry the P
	// register name the code that issued the operation, which is what a
	// protocol log is for. The host wires it to the CPU.
	std::function<u32()> trace_pc;
	u32 tpc() const { return trace_pc ? trace_pc() : 0; }
	// Emulated seconds, for the card logs. Without it FOCUS_ASIDBG is a
	// bare sequence of orders with no way to line them up against
	// anything else, and partitioning it by eye is how two wrong
	// conclusions got drawn -- see docs/HPUX.md 6.
	std::function<double()> trace_time;
	double tnow() const { return trace_time ? trace_time() : 0.0; }

	//----------------------------------------------------------------
	//  MPB command entry points
	//----------------------------------------------------------------

	// The three log channels the IOP writes on. Buffered: FOCUS_IOALL
	// alone runs to twenty thousand lines on an HP-UX boot.
	static knob::channel &ioall()
	{
		static knob::channel c("FOCUS_IOALL", knob::sync::buffered);
		return c;
	}
	static knob::channel &ioplog()
	{
		static knob::channel c("FOCUS_IOPLOG", knob::sync::buffered);
		return c;
	}
	static knob::channel &irqlog()
	{
		static knob::channel c("FOCUS_IRQLOG", knob::sync::buffered);
		return c;
	}

	bool io_write(const target &t, u32 data) override
	{
		if (t.channel != m_channel)
			return false;
		channel_state &c = m_chan[t.pa & (CHANNELS - 1)];
		if (ioall())
			ioall()("[%8.3fs iop W op=%03o pa=%u d=%08x p=%08x]\n",
					tnow(), t.opcode, t.pa, data, tpc());

		switch (t.opcode)
		{
		case CMD_WDS:  c.dma_status = data; c.term = 0; return true;
		case CMD_WDC:  c.dma_count = data & 0xffff; return true;
		case CMD_WDA:  c.dma_addr = data; c.dma_current = data; return true;
		case CMD_WDTF: c.termination = data; return true;
		case CMD_WDCA: c.dma_current = data; return true;
		case CMD_WDPA: c.dma_pa = u8(data & 7); return true;
		case CMD_WDB:  c.data_buffer = data; return true;
		case CMD_WIMK: m_int_mask = data; update_interrupt(); return true;
		case CMD_WIMG: m_int_message = data; return true;
		case CMD_SIL:  c.int_level = u8(data & 0xf); return true;
		// Timer Start/Count live in the word's bits 0:24 -- the ERS loads
		// "bits 0-23 of Timer Start" on underflow and ignores bits 24-31,
		// and the interval is (TimerStart(0:24)+1)*512 clock periods
		// (7.1/7.2). The OS writes (n-1)<<8 for an n-count interval.
		case CMD_WTMS: m_timer_start = data >> 8; m_timer_count = data >> 8; return true;
		case CMD_WTMC: m_timer_count = data >> 8; return true;
		case CMD_WAMK:
			m_attn_mask = data;
			if (ioplog())
				ioplog()("[iop WAMK=%08x ch=%u pa=%u]\n", data, t.channel, t.pa);
			return true;
		case CMD_WMCN:
			// Bits 3-5 of the data word become the IOP's MPB identification;
			// the ERS requires every other bit to be zero (sheet 35).
			m_channel = u8(ers32::get(data, 3, 3));
			return true;
		default:
			// WIF n, Write Interface FRn (I/O ERS sheet 33): opcode
			// 8n+2 puts the low 16 data bits on the addressed card's
			// register n. An empty slot swallows the write.
			if ((t.opcode & 7) == 2 && t.opcode <= 0172)
			{
				if (c.iface)
					c.iface->write_fr(register_of(t), data & 0xffff);
				return true;
			}
			if (ioplog())
				ioplog()("[iop REFUSED W op=%03o pa=%u d=%08x]\n", t.opcode, t.pa, data);
			return false;
		}
	}

	bool io_read(const target &t, u32 &data) override
	{
		const bool ok = io_read_inner(t, data);
		if (ioall())
			ioall()("[%8.3fs iop R op=%03o pa=%u -> %08x%s p=%08x]\n",
					tnow(), t.opcode, t.pa, data, ok ? "" : " REFUSED", tpc());
		return ok;
	}

	bool io_read_inner(const target &t, u32 &data)
	{
		if (t.channel != m_channel)
			return false;
		channel_state &c = m_chan[t.pa & (CHANNELS - 1)];

		switch (t.opcode)
		{
		case CMD_RDCS:
			// Remaining count in the HIGH half, termination status in the
			// low: the kernel's transfer classifier indexes a class table
			// with bits 13:15 of this word plus bit 0, and accepts only
			// class 4 -- bits 13:15 = 1, "terminated on count" -- while
			// its ISR treats bits 15, 14 and 0 as errors. The old
			// status-high/count-low reading came from a poor scan.
			//
			// And the count is BIASED BY TWO. I/O ERS sheet 5.2: "Current
			// count is two less than the number of transfers remaining
			// once a Start DMA has been issued. If DMA terminates on
			// count ... count will be -2 ... If DMA terminates on Device
			// End, the number not transferred is count+2." The kernel's
			// transfer report (entry 14) adds exactly that 2 back, so a
			// count that reaches plain zero tells the driver two
			// transfers never happened -- which is how a thermal printer
			// line that printed perfectly still came back as "ERROR 81:
			// Device/Interface hardware failure".
			data = (((c.dma_count - 2) & 0xffff) << 16) | c.term;
			return true;
		case CMD_RDMK: data = m_dma_mask; return true;
		case CMD_RIMK: data = m_int_mask; return true;
		case CMD_RIRQ: data = m_int_request; return true;
		case CMD_RIMG: data = m_int_message; return true;
		case CMD_RTM:  data = m_timer_count << 8; return true;
		case CMD_RDPA: data = c.dma_pa; return true;
		case CMD_RDB:  data = c.data_buffer; return true;
		case CMD_RDTF:
			// TF0 doubles as the attention state (I/O ERS section 4.4):
			// the poll mask in (0:8) and the log of PAs given an ARD in
			// (16:8). Only channel 0's field carries them.
			data = c.termination;
			if (t.pa == 0)
				data |= ((m_attn_mask & 0xff) << 24)
						| ((m_attn_log & 0xff) << 8);
			return true;
		case CMD_RIRV: {
			// "The date the IOP microcode was released ... YYYYMMDD in
			// hex" (sheet 39). HP-UX 5.30 compares it with 0x19810630 and
			// keeps a flag, CORE SYSTEM's DB+140, that is true only for
			// microcode NEWER than that; every IOP in the machine has to
			// agree (escape 69) and without the flag the kernel will not
			// open a device on the HP-IB card at all (escape 91, then
			// 185 and a halt). An IOP that answered 1 here was an IOP
			// from before June 1981. The default is this ERS's own
			// revision date; FOCUS_IOPREV sweeps it.
			static const knob::number rev("FOCUS_IOPREV", REVISION, 16);
			data = u32(rev);
			return true;
		}
		case CMD_RIST: data = c.iface ? c.iface->status_reg() : 0; return true;
		case CMD_RIFG: data = c.iface ? c.iface->flag() : 0; return true;
		case CMD_IFPL: data = poll(); return true;
		case CMD_RIF1:
			// Opcode 011 is RIF 1 by the 8n+1 rule, and the powerup self
			// test reads it as one: it writes 1<<pa to FR0 with WIF 0
			// (opcode 002) and then reads here, reporting what it finds
			// as "Self test error 1: IO address 003,0000 STATUS:
			// 00000008" when the card just echoes the pattern back --
			// which a card that answered its own status would not do.
			// The status-and-flag reading of this opcode, taken from a
			// poor scan of sheet 34, does not survive that.
			data = c.iface ? (c.iface->read_fr(1) & 0xffff) : 0;
			return true;
		default:
			// RIF n, Read Interface FRn (sheet 33): opcode 8n+1 reads
			// the addressed card's register n into the low 16 bits. An
			// empty slot reads as an idle bus. RISF keeps 011.
			if ((t.opcode & 7) == 1 && t.opcode <= 0171 && t.opcode != CMD_RIF1)
			{
				data = c.iface ? (c.iface->read_fr(register_of(t)) & 0xffff) : 0;
				return true;
			}
			return false;
		}
	}

	bool io_control(const target &t) override
	{
		if (t.channel != m_channel)
			return false;
		channel_state &c = m_chan[t.pa & (CHANNELS - 1)];
		if (ioall())
			ioall()("[%8.3fs iop C op=%03o pa=%u p=%08x]\n",
					tnow(), t.opcode, t.pa, tpc());

		switch (t.opcode)
		{
		// Start DMA "sets a bit in the DMA Mask register" itself (I/O ERS
		// sheet 30) -- EDMA is not a prerequisite. SDC also clears the
		// PA's interrupt request bit, SDEC additionally enables its
		// interrupt, SRC is the RAMD flavour of SDC.
		case CMD_SD:
			c.dma_active = true;
			m_dma_mask |= (1u << t.pa);
			if (!defer_dma())
				run_dma(t.pa);
			return true;
		case CMD_SDC:
		case CMD_SRC:
			c.dma_active = true;
			m_dma_mask |= (1u << t.pa);
			m_int_request &= ~(1u << t.pa);
			m_int_posted  &= ~(1u << t.pa);
			update_interrupt();
			if (ioplog())
				ioplog()("[iop SDC pa=%u addr=%08x cnt=%u st=%08x en=%d]\n",
						t.pa, c.dma_addr, c.dma_count, c.dma_status,
						c.int_enabled);
			if (!defer_dma())
				run_dma(t.pa);
			return true;
		case CMD_SDEC:
			c.dma_active = true;
			m_dma_mask |= (1u << t.pa);
			c.int_enabled = true;
			m_int_request &= ~(1u << t.pa);
			m_int_posted  &= ~(1u << t.pa);
			update_interrupt();
			if (!defer_dma())
				run_dma(t.pa);
			return true;
		case CMD_EDMA: m_dma_mask |= (1u << t.pa); return true;
		case CMD_DDMA: m_dma_mask &= ~(1u << t.pa); c.dma_active = false; return true;
		case CMD_CDMK: m_dma_mask = 0; return true;
		case CMD_RINT:
			// "The bit corresponding to the PA in the channel address is
			// set in the Interrupt Request register" (I/O ERS sheet 31)
			// -- the exact mirror of CIRQ below, and a driver's way of
			// raising a service request for a device that cannot. The
			// keyboard board's register exchange takes this path when
			// the interface flag does not come back on its own, which
			// is what the internal printer makes it do.
			m_int_request |= (1u << t.pa);
			update_interrupt();
			if (ioplog())
				ioplog()("[iop RINT pa=%u]\n", t.pa);
			return true;
		case CMD_CIRQ:
			m_int_request &= ~(1u << t.pa);
			m_int_posted  &= ~(1u << t.pa);
			update_interrupt();
			if (ioplog())
				ioplog()("[iop CIRQ pa=%u]\n", t.pa);
			return true;
		case CMD_CAAK: m_attn_log &= ~(1u << t.pa); return true;
		case CMD_ETMI:
			// Clears Timer Disable (I/O ERS sheet 33); underflows then
			// send message 13.
			m_timer_enabled = true;
			return true;
		case CMD_CALM: m_lockout = false; return true;
		case CMD_SALM: m_lockout = true; return true;
		case CMD_DCE:
			// Disable IOP Command Execution: "This command clears a bit
			// in the Interrupt Request register and clears the Command
			// Execution bit (bit 20 in the Status register) **both
			// corresponding to the PA in the channel address**" (I/O ERS
			// sheet 38). It is PER-PA, and it disables COMMAND
			// EXECUTION -- not the IOP.
			//
			// Modelling it as a global off-switch cost the 540 its
			// console. The driver polls the card for a second, arms its
			// real transaction in the command list, and issues DCE to
			// stop the polling list -- at which point this IOP stopped
			// servicing every channel, including the suspended list it
			// had just been asked to run, so the armed transaction never
			// executed and the open timed out four seconds later.
			c.cmd_exec = false;
			c.await_poll = false;
			m_int_request &= ~(1u << t.pa);
			m_int_posted  &= ~(1u << t.pa);
			update_interrupt();
			return true;
		case CMD_LON:  m_led = true; return true;
		case CMD_LOFF: m_led = false; return true;
		// Interrupt enable is per device: the ERS's request sequence ends
		// with "the interrupt mask on the IOP now disables the device that
		// requested from requesting again", so EINT is how the CPU re-arms
		// a device after servicing it (I/O ERS section 4.1.4).
		case CMD_EINT:
			c.int_enabled = true;
			update_interrupt();
			if (c.int_pending)      // a latched request delivers now
			{
				c.int_pending = false;
				raise(t.pa);
			}
			return true;
		case CMD_DINT: c.int_enabled = false; update_interrupt(); return true;
		case CMD_ICEI:
			// Initialize Command Execution from Interrupt: the IOP fetches
			// a command list and executes it against the addressed channel.
			// The kernel's transaction layer builds the list in the device
			// record (a WIF8 command byte for the card, then SIL, DDMA,
			// WDS, WDA, WDC, SDEC to set up and start the DMA leg), points
			// the device's DRT entry word 3 at it, and issues this command.
			//
			// It also "enables Command Execution (bit 20 in the Status
			// register)... corresponding to the PA" (sheet 36), which is
			// the bit DCE clears.
			c.cmd_exec = true;
			m_int_request &= ~(1u << t.pa);
			m_int_posted  &= ~(1u << t.pa);
			run_command_list(t.pa);
			return true;
		case CMD_RIOB:
			// Reset I/O Bus: drop every channel's DMA and clear pending
			// requests, as a power-up sequence expects.
			for (auto &c : m_chan) { c.dma_active = false; }
			m_dma_mask = 0;
			m_int_request = 0;
			m_int_posted = 0;
			update_interrupt();
			return true;
		case CMD_RIOP:
			// Reset IOP (I/O ERS sheet 35). Forces the initial state and
			// resumes immediately: interrupt request disabled, timer
			// disabled, DMA mask cleared, Interrupt Request and Interrupt
			// Mask cleared. "All other registers remain unchanged", so the
			// channel DMA registers and the MPB channel number survive --
			// which is what separates this from reset().
			m_int_enable = false;
			m_timer_enabled = false;
			m_dma_mask = 0;
			for (auto &c : m_chan) { c.dma_active = false; }
			m_int_request = 0;
			m_int_posted = 0;
			m_int_mask = 0;
			update_interrupt();
			return true;
		default:
			// CIF n, the control strobe on interface register n.
			if ((t.opcode & 7) == 0 && t.opcode <= 0170)
			{
				if (c.iface)
					c.iface->control_fr(register_of(t));
				return true;
			}
			if (ioplog())
				ioplog()("[iop REFUSED C op=%03o pa=%u]\n", t.opcode, t.pa);
			return false;
		}
	}

	//----------------------------------------------------------------
	//  periodic work
	//----------------------------------------------------------------

	// One poll round: service any channel whose card wants a transfer.
	// Figure 2.1 of the I/O ERS has the IOP polling the interfaces
	// continuously and executing MPB commands between rounds.
	void poll_and_service()
	{
		if (!m_enabled)
			return;

		// The attention poll (I/O ERS section 4.4). A card asserting
		// ASYNC outside a transfer is priority encoded by PA, the winner
		// gets a write of 1 to its FR1 -- the Attention Request Disable,
		// which stops it asserting until re-armed -- and its PA is logged
		// in TF0(16:8). The mask excludes unwanted PAs ("to exclude any
		// unwanted PA's"), so a zero mask, which is what the powerup
		// writes, leaves every card eligible. The CPU hears about it as
		// message 14, "CSG attention requests" (section 2.1).
		for (unsigned pa = CHANNELS; pa-- > 0; )
		{
			channel_state &c = m_chan[pa];
			if (!c.iface || !c.iface->attention())
				continue;
			// The mask's sense: a set bit makes the PA eligible. The
			// device scan writes 1<<7 before commanding the internal
			// disc and expects its completion through this poll; the
			// "exclude unwanted PAs" reading of the (poorly scanned)
			// ERS text, with 0 meaning everyone, matched nothing the
			// running system does -- powerup writes 0 first, when it
			// wants quiet.
			if (!((m_attn_mask >> pa) & 1))     // not armed
				continue;
			if ((m_attn_log >> pa) & 1)         // ARD already given
				continue;
			c.iface->write_fr(1, 1);            // Attention Request Disable
			m_attn_log |= (1u << pa);
			m_attn_event = true;
			break;
		}

		for (unsigned pa = 0; pa < CHANNELS; pa++)
			run_dma(pa, false);

		// A command list suspended on ICEI resumes when its interface
		// responds to the poll.
		// One line per thousand poll rounds; the sampling is here, so the
		// channel itself is uncapped.
		static knob::channel dbg("FOCUS_IOPDBG", knob::sync::buffered);
		if (dbg && (++m_dbg_ticks % 1000) == 0)
			dbg("[iop tick %u: pa0 await=%d iface=%d poll=%d "
					"cmdexec=%d]\n", m_dbg_ticks, m_chan[0].await_poll,
					m_chan[0].iface != nullptr,
					m_chan[0].iface && m_chan[0].iface->poll_request(),
					int(m_enabled));
		for (unsigned pa = 0; pa < CHANNELS; pa++)
		{
			channel_state &c = m_chan[pa];
			if (!c.await_poll || !c.iface || !c.cmd_exec)
				continue;
			if (!c.iface->poll_request())
			{
				if (dbg && (++c.stall_ticks % 500) == 1)
					dbg("[iop pa=%u suspended, poll quiet "
							"(%u ticks)]\n", pa, c.stall_ticks);
				continue;
			}
			c.stall_ticks = 0;
			c.await_poll = false;
			run_command_list(pa);
		}
	}

	// Run a channel's DMA as far as it will go. A counted transfer
	// completes at bus speed on real hardware -- microseconds after the
	// start command, well inside a driver's completion spin -- so a
	// start command calls this directly rather than waiting for the
	// next poll round (the display driver quiesces the channel as a
	// timeout error if the completion is a poll period late). An
	// uncounted transfer moves one unit per call so the loop cannot
	// spin forever.
	// FOCUS_IOPDEFER makes a start command ARM the transfer and leave it
	// to the next poll round instead of moving the data inside the
	// command itself. Real hardware needs microseconds for a leg and the
	// interrupt lands later; here a leg issued from inside an interrupt
	// handler completes before that handler has finished, so its
	// completion path runs against state the handler is still using.
	static bool defer_dma()
	{
		static const knob::flag on("FOCUS_IOPDEFER");
		return on.on();
	}

	// What the DMA status word says this transfer is. Direction is bits
	// 27 and 28 TOGETHER, and Table 5.1 of the I/O ERS spells out all
	// four combinations -- only one of which is an output:
	//
	//     F1=0 F2=0  Input
	//     F1=0 F2=1  Input, terminate on Match
	//     F1=1 F2=0  Output
	//     F1=1 F2=1  Input, Verify
	//
	// Reading F1 alone as the direction, which is what this did, turns
	// the last of those into an output: the card is never asked for a
	// word and the transfer moves nothing. That is how the display's
	// memory test read back 98,304 bytes of nothing and still passed --
	// the comparison is the IOP's job here, not the software's, which is
	// why PWP SELF TEST does not do it.
	struct dma_mode
	{
		bool output;      // the card is being written to
		bool verify;      // compare against memory, do not store
		bool on_match;    // compare, and stop when it agrees
		bool bytes;       // bit 29 set is bytes, clear halfwords
	};

	static dma_mode decode_dma_mode(u32 status)
	{
		const bool f1 = ers32::get(status, 27, 1) != 0;
		const bool f2 = ers32::get(status, 28, 1) != 0;
		return dma_mode{ f1 && !f2, f1 && f2, !f1 && f2,
				ers32::get(status, 29, 1) != 0 };
	}

	// One unit across the channel: a byte or a halfword, out of memory or
	// into it, or -- on a verify or a search -- compared against what is
	// there and not stored at all. Returns true when the comparison says
	// the transfer should stop here, leaving what disagreed in got/want
	// for the log. Advances `current` by the width either way.
	//
	// It takes the card and the address cursor rather than the whole
	// channel because those are what moving one unit needs; the counting
	// and the terminations belong to run_dma.
	static bool run_dma_unit(iop_interface &card, u32 &current,
			const dma_mode &m, memory_interface &mem,
			u32 &got_v, u32 &want_v)
	{
		bool stop = false;
		if (m.bytes)
		{
			if (m.output)
				card.dma_write(mem.read8(current));
			else
			{
				const u8 got = card.dma_read();
				if (m.verify || m.on_match)
				{
					got_v = got; want_v = mem.read8(current);
					stop = (got_v == want_v) == m.on_match;
				}
				else
					mem.write8(current, got);
			}
			current++;
		}
		else
		{
			if (m.output)
				card.dma_write16(mem.read16(current));
			else
			{
				const u16 got = card.dma_read16();
				if (m.verify || m.on_match)
				{
					got_v = got; want_v = mem.read16(current);
					stop = (got_v == want_v) == m.on_match;
				}
				else
					mem.write16(current, got);
			}
			current += 2;
		}
		return stop;
	}

	// `uncounted` says whether a transfer with no count may advance. A
	// COUNTED transfer completes inside one call whatever the poll rate,
	// so polling faster changes only how promptly it finishes -- which is
	// the whole point. An UNCOUNTED one moves a unit per call, so its
	// throughput IS the poll rate, and the interfaces that use one -- the
	// display's line DMA, the internal disc -- are paced by the device
	// rather than by how often the IOP happens to look. Those advance on
	// the millisecond beat instead (pace_uncounted_dma), which is what
	// keeps the 520's boot identical while the console's counted legs
	// finish in microseconds.
	void run_dma(unsigned pa, bool uncounted = true)
	{
		channel_state &c = m_chan[pa];
		if (!c.iface)
			return;
		// A burst request moves data on a programmed channel without a
		// start command; otherwise a transfer needs the start command's
		// active state and the DMA mask bit.
		const bool burst = c.dma_count != 0 && c.iface->burst_request();
		if (!burst && (!c.dma_active || !(m_dma_mask & (1u << pa))))
			return;
		if (!uncounted && c.dma_count == 0)
			return;

		memory_interface &mem = m_host.iop_memory();
		// The count is "the length of the DMA block transfer in
		// transfers"; what a transfer IS comes off the status word.
		const dma_mode m = decode_dma_mode(c.dma_status);
		u32 left = c.dma_count ? c.dma_count : 1;
		while (left--)
		{
			if (!(c.dma_active ? c.iface->service_request()
					: (c.dma_count != 0 && c.iface->burst_request())))
			{
				// The card has stopped supplying. If it is asserting
				// Device End the transfer is OVER, short of its count,
				// and the channel has to say so -- otherwise the driver
				// waits for a completion that never comes. That is what
				// stranded HP-UX's console open: it asks for 17 bytes of
				// transparent status and the card has 3 to give.
				//
				// Table 5.1 of the I/O ERS puts "DMA Terminated on
				// Device End" at bit 31, the least significant bit of
				// the status half RDCS returns -- 0x0001 here, against
				// 0x2000 (bit 18) for terminated on count. The remaining
				// count is left standing in `dma_count`, which is how
				// the driver learns how much really arrived.
				if (c.dma_active && c.dma_count && c.iface->data_end())
				{
					c.dma_active = false;
					c.term = 0x0001;
					if (ioplog())
						ioplog()("[iop DMA-dend pa=%u left=%u]\n",
								pa, c.dma_count);
					c.iface->dma_complete(m.output);
					raise(pa);
				}
				break;
			}
			u32 got_v = 0, want_v = 0;
			const bool stop = run_dma_unit(*c.iface, c.dma_current, m,
					mem, got_v, want_v);

			// A verify that disagrees, or a search that agrees, ends the
			// transfer where it stands. Bit 17 of the status half is
			// "DMA Terminated on Match or Verify Error" (Table 5.1),
			// which is 0x4000 in the word RDCS returns.
			if (stop)
			{
				if (c.dma_count)
					c.dma_count--;
				c.dma_active = false;
				c.term = 0x4000;
				if (ioplog())
					ioplog()("[iop DMA-%s pa=%u at=%08x got=%04x want=%04x "
							"left=%u]\n", m.verify ? "verifyfail" : "match",
							pa, c.dma_current, got_v, want_v, c.dma_count);
				c.iface->dma_complete(false);
				raise(pa);
				break;
			}

			if (c.dma_count && --c.dma_count == 0)
			{
				c.dma_active = false;
				c.term = 0x2000;    // terminated on count, no error
				if (ioplog())
					ioplog()("[iop DMA-done pa=%u en=%d]\n", pa, c.int_enabled);
				c.iface->dma_complete(m.output);
				raise(pa);
			}
		}
	}

	// Execute a command list (ICEI, opcode 070). The list is found through
	// the device's DRT entry: word 3 points at (command, operand) pairs of
	// words, the command in the low bits -- the kernel packs the select
	// code into bits 3:6 of the first word as well -- and the operand in
	// the word after. Everything here is read off the running system: the
	// kernel's transaction builder emits
	//
	//     WIF8 <command byte for the card>
	//     SIL  <level>          DDMA
	//     WDS  <dma status>     WDA <buffer>       WDC <count>
	//     SDEC                  0115 <link>        074
	//
	// and waits for the DMA completion interrupt. 074 (DCEI) ends the
	// list.
	//
	// Three opcodes are legal ONLY inside a list -- the ERS groups them
	// under "INSTRUCTIONS FOR COMMAND EXECUTION ONLY" (sheets 38-39) and
	// they are the list's control flow:
	//
	//   065  Increment and Branch      no mnemonic
	//   0114 Write RIF Result to Memory      WRIF
	//   0115 Write Count and Status to Memory WCSM
	//
	// 0115 was read here as an unexplained end-of-list pointer, which it
	// is not: it is a STORE, and the address it carries is where the
	// driver expects to find how its transfer went. 065 was refused
	// outright, and refusing it is what stranded HP-UX's console: the
	// driver's next list begins with one, the walker stopped on it, the
	// transaction never ran and the open timed out.
	void run_command_list(unsigned pa)
	{
		memory_interface &mem = m_host.iop_memory();
		const u32 drt = 128u * m_channel + 16u * (pa & 7);
		u32 ptr = mem.read32(drt + 12);
		// The command-list walk answers to either switch: FOCUS_ICEILOG
		// for the lists alone, FOCUS_IOPLOG for them alongside everything
		// else the IOP does.
		static knob::channel iceilog("FOCUS_ICEILOG", knob::sync::buffered);
		knob::channel &log = iceilog.on() ? iceilog : ioplog();
		if (log)
			log("[iop ICEI pa=%u list=%08x]\n", pa, ptr);
		// The list pointer belongs to the IOP, and it ADVANCES: the ERS
		// says so in passing where it describes Skip on Status False --
		// "The MPB command list pointer in the DRT points to the next
		// instruction... The command list pointer remains pointing to
		// the skip command." A walker that only ever reads the pointer
		// restarts the list from the top every time the CPU issues ICEI,
		// which is exactly what the HP-IB card's transaction was doing:
		// the same seven-command block over and over.
		struct list_pointer
		{
			memory_interface &mem;
			u32 drt, &ptr;
			~list_pointer() { mem.write32(drt + 12, ptr); }
		} keep{ mem, drt, ptr };
		for (unsigned step = 0; step < 64; step++)
		{
			const u32 word = mem.read32(ptr);
			const u32 operand = mem.read32(ptr + 4);
			const u8 op = u8(word & 0x1ff);
			const u32 here = ptr;
			ptr += 8;
			if (log)
				log("[iop ICEI+%02u @%08x op=%03o operand=%08x]\n",
						step, here, op, operand);
			target t{};
			t.opcode = op;
			t.channel = m_channel;
			t.pa = u8(pa & 7);
			bool ok = false;
			switch (op)
			{
			case CMD_WDS: case CMD_WDC: case CMD_WDA: case CMD_WDTF:
			case CMD_WDCA: case CMD_WDPA: case CMD_WDB: case CMD_WIMK:
			case CMD_WIMG: case CMD_SIL: case CMD_WAMK:
				ok = io_write(t, operand);
				break;
			case CMD_CDMK: case CMD_EDMA: case CMD_DDMA: case CMD_SD:
			case CMD_SDC: case CMD_SDEC: case CMD_SRC: case CMD_CIRQ:
			case CMD_CAAK: case CMD_EINT: case CMD_DINT:
				ok = io_control(t);
				break;
			case CMD_ICEI:
				// "This command suspends execution of commands until the
				// interface responds to a poll" (ERS sheet 36). Inside a
				// list that is a WAIT, not an end: the pointer left
				// behind names the entry to resume at, and
				// poll_and_service starts the list again when the card
				// answers. Stopping dead here is what left the console's
				// transaction half-issued.
				// ...and if the interface is ALREADY responding there
				// is nothing to suspend for. The IOP polls continuously
				// (I/O ERS figure 2.1); this one polls once an emulated
				// millisecond, which is three orders of magnitude slower
				// than the bus, and the gap is long enough for the CPU
				// to run its whole interrupt service inside it. That is
				// what killed HP-UX's console open: the RS leg's DMA
				// completion is an accepted interrupt, the list parks
				// one entry short of the Read Transparent Status that
				// would refresh the report buffer, and the driver's
				// handler -- whose every arm ends "ICEI; IXIT", one
				// interrupt per freshly-DMA'd report -- dispatches the
				// SWI still standing in that buffer against a channel
				// its own completion has just closed. State 0 is the
				// error arm, so the transaction that had already earned
				// status 55 is torn down with 66.
				{
					// FOCUS_IOPPARKALWAYS restores the always-park
					// behaviour, which is the A/B control for everything
					// this changes.
					static const knob::flag park("FOCUS_IOPPARKALWAYS");
					const channel_state &ic = m_chan[pa & 7];
					if (!park.on() && ic.iface && ic.iface->poll_request())
					{
						if (log)
							log("[iop ICEI pa=%u poll already up, "
									"resume=%08x]\n", pa, ptr);
						continue;
					}
				}
				m_chan[pa & 7].await_poll = true;
				if (log)
					log("[iop ICEI pa=%u suspend, resume=%08x]\n",
							pa, ptr);
				return;
			case 065: {
				// Increment and Branch (ERS sheet 39): "test the count,
				// the contents of the second memory location following
				// this command, and branch to the absolute address in
				// the first memory location following this command if
				// the value was not zero. It will also increment and
				// replace the count if the count is still negative. If
				// the count is zero, execution will continue at the
				// third memory location following this command."
				//
				// So the entry is three words wide, not two, and it is
				// how a list loops or arms itself for a fixed number of
				// passes: a count of -1 takes the branch once and falls
				// through ever after.
				const u32 count_at = here + 8;
				const s32 count = s32(mem.read32(count_at));
				if (count != 0)
				{
					if (count < 0)
						mem.write32(count_at, u32(count + 1));
					ptr = operand;
				}
				else
					ptr = here + 12;
				continue;
			}
			case 0114: {
				// Write RIF Result to Memory: "do a Read Interface using
				// the PA and IC lines from the command word and record
				// the results in the absolute word address found in the
				// first memory location following the command". The
				// register number is the IC field, bits 9:4 of the MPB
				// command word (Figure 5.1).
				target r{};
				r.opcode = 0;
				r.channel = m_channel;
				r.pa = u8(pa & 7);
				r.ic = u8(ers32::get(word, 9, 4));
				u32 v = 0;
				if (io_read(r, v))
					mem.write32(operand, v);
				break;
			}
			case 0115: {
				// Write Count and Status to Memory: "record the contents
				// of the Current Count and Status register in the
				// absolute word address found in the first memory
				// location following the command" -- the same word RDCS
				// returns, so the driver can read its transfer's outcome
				// out of memory instead of asking for it.
				//
				// The count half follows RDCS exactly: "current count is
				// two less than the number of transfers remaining ... if
				// DMA terminates on count, count will be -2; if on Device
				// End, the number not transferred is count+2" (sheet 29).
				// HP-UX's HP27110 driver reads its transfer lengths out of
				// this word and does that arithmetic itself -- `length -
				// count - 2` for a data leg, `count + 2` for the status
				// leg -- so writing the raw count here made a full read
				// look two bytes long and a short status block look
				// shorter still, and the driver refused every CS/80
				// Describe with escape 148.
				const channel_state &cs = m_chan[pa & 7];
				mem.write32(operand,
						(((cs.dma_count - 2) & 0xffff) << 16) | cs.term);
				m_chan[pa & 7].chain = operand;
				break;
			}
			case 074:
				// DCEI, Disable Command Execution and Enable Interrupt:
				// bit 20 cleared and the interrupt mask bit set for this
				// PA (I/O ERS sheet 37), so the list ends here and the
				// CPU is told about it.
				m_chan[pa & 7].cmd_exec = false;
				m_chan[pa & 7].await_poll = false;
				m_chan[pa & 7].int_enabled = true;
				return;
			default:
				// WIF n delivers the operand to the card's register n --
				// the first list entry is the card's command byte on FR8.
				if ((op & 7) == 2 && op <= 0172)
					ok = io_write(t, operand);
				if (!ok && log)
					log("[iop ICEI STOP op=%03o]\n", op);
				if (!ok)
					return;
				break;
			}
			(void)ok;
		}
	}

	// One millisecond's worth of the transfers the poll deliberately
	// leaves alone: see run_dma's `uncounted`.
	void pace_uncounted_dma()
	{
		for (unsigned pa = 0; pa < CHANNELS; pa++)
			if (m_chan[pa].dma_count == 0)
				run_dma(pa, true);
	}

	// Advance the interval timer by `units` counts (one count is 512
	// periods of the 18 MHz clock, 28.4 us; hosts pass however many of
	// those their tick represents).
	//
	// "The length of the interval is exactly (Timer Start(0:24)+1)*512*(one
	// clock period)" (I/O ERS 7.2): the count runs down from Start through
	// zero, and the NEXT count is the underflow that reloads it, so a
	// period is Start+1 counts, and whatever is left of this call's counts
	// after an underflow belongs to the next period. This used to reload on
	// `count <= units` and drop the remainder, which made every period a
	// whole number of calls: HP-UX's Start of 350 came out as ten 35-count
	// calls where the hardware takes 351 counts, a Start of 351 would have
	// taken eleven, and no period could be shorter than one call. A count
	// that reached zero also used to stop the timer for good.
	//
	// A timer nobody has programmed (start and count both zero) stays quiet,
	// as it always has.
	void timer_tick(u32 units = 1)
	{
		if (m_timer_count == 0 && m_timer_start == 0)
			return;
		while (units > m_timer_count)
		{
			// Underflow "is the setting of the Timer Interrupt Request
			// message bit 13 and sending a message to the CPU" (I/O ERS
			// 7.1) -- a plain CPU message, not a device interrupt
			// through the DRT lists. The host forwards the event as
			// raise_message(13).
			units -= m_timer_count + 1;
			m_timer_count = m_timer_start;
			if (m_timer_enabled)
				m_timer_event = true;
		}
		m_timer_count -= units;
	}

	// A millisecond of the timer. 18 MHz / 512 is 35.15625 counts a
	// millisecond -- 2250/64 exactly -- and the sixty-fourths are carried
	// from call to call. A flat 35 a millisecond ran the timer 0.44% slow;
	// HP-UX programs Start = 350, adds the 9984 us that 351 counts really
	// are to its software clock on every tick, and so lost 0.16% (with the
	// old rounding) against emulated time: two and a quarter minutes a day.
	void timer_tick_ms()
	{
		m_timer_frac += 2250;
		timer_tick(m_timer_frac >> 6);
		m_timer_frac &= 63;
	}

	// True once per timer underflow; the host forwards it to the CPU
	// as message 13.
	bool take_timer_event() { const bool e = m_timer_event; m_timer_event = false; return e; }

	// One bit per accepted device raise since the last call; the host
	// forwards each as message 16 + the device's interrupt level.
	u32 take_raise_events() { const u32 e = m_raise_events; m_raise_events = 0; return e; }

	// Let a card raise an interrupt of its own.
	void raise(unsigned pa)
	{
		channel_state &c = m_chan[pa];
		// FOCUS_IRQLOG: every raise, with the device's enable and both
		// latches, for telling "the card keeps interrupting" apart from
		// "the line never fell".
		if (irqlog())
			irqlog()("[iop raise pa=%u en=%d req=%02x posted=%02x]\n",
					pa, c.int_enabled, m_int_request, m_int_posted);
		if (ioplog())
			ioplog()("[iop raise pa=%u en=%d lvl=%u]\n", pa, c.int_enabled, c.int_level);
		// The Interrupt REQUEST register records the event whatever the
		// mask says: "DMA termination or a non-DMA service request will
		// cause a bit in the Interrupt Request register corresponding to
		// the proper PA to be set" (I/O ERS 4.1.1). The mask decides
		// only whether the IOP goes on to POST an interrupt, which is a
		// separate latch here (`m_int_posted`).
		//
		// Setting the request inside the enabled path made the register
		// invisible to anyone polling it with interrupts off -- which is
		// exactly how the kernel's panic printer works. It clears the
		// whole mask with WIMK, starts its transfer with SDC (start DMA,
		// clear IRQ -- note: NOT the SDEC that would enable the
		// interrupt), and then spins on `RIRQ & 1<<pa`. With the bit
		// never set it spun 2800 times for a transfer that had already
		// finished, gave up, and retried forever: the 540 could not
		// report its own panic.
		m_int_request |= (1u << pa);
		if (!c.int_enabled)
		{
			// The request latches while the device is disabled; EINT
			// delivers it then. A DMA completion can beat the driver's
			// SIL/EINT pair, which lands right after the start command
			// -- on real hardware the transfer takes long enough that
			// the completion interrupt always finds the enable set.
			c.int_pending = true;
			return;
		}
		m_int_event = true;
		// "The interrupt mask on the IOP now disables the device that
		// requested from requesting again" (I/O ERS 4.1.4): an accepted
		// raise disarms the device until the service routine's EINT, and
		// each accepted raise is one message to the CPU -- the request
		// LATCH (cleared only by CIRQ) is separate bookkeeping, so the
		// host forwards raise events, not latch edges.
		c.int_pending = false;
		c.int_enabled = false;
		m_raise_events |= (1u << pa);

		// Record the request in memory (I/O ERS section 4.1.4): link the
		// device's DRT entry onto the list for its priority level. The
		// sixteen lists live in dedicated locations 2000-2177 octal, a
		// (tail, head) pair of absolute pointers per level; the link
		// field is the left halfword of the entry's first word, and a
		// zero link ends the list. Figure 4.1's worked example pins the
		// device number arithmetic: CH4 PA4 sits at octal 1100 = 576 =
		// 128*4 + 16*4.
		const u32 device = 128u * m_channel + 16u * pa;
		const u32 tail_addr = 01000 * 2 / 2;    // octal 2000 = 1024
		memory_interface &mem = m_host.iop_memory();
		const u32 ta = 1024 + 8u * c.int_level;
		const u32 tail = mem.read32(ta);
		// A device already on the list must not be linked onto it again:
		// "the interrupt mask is used to prevent interrupt requests ...
		// from devices which have interrupt requests pending at the CPU
		// already" (I/O ERS 4.1.2). One PA is one bit in the request
		// register and one entry on its level's list, however many times
		// it asks before the CPU gets to it -- and the CPU may not get to
		// it for a while, because a driver that restarts its transfer
		// from inside an interrupt service (SDEC re-arms the device) has
		// the completion raised here while its level is still masked.
		//
		// The first form of this guard compared against the TAIL only,
		// which covers a device queued alone. With a second device queued
		// behind it the append ran anyway: it zeroed the link field of an
		// entry that was in the middle of the list, hung the entry on the
		// tail a second time, and so cut every device queued behind it
		// off the list. The CPU's walk then found one entry with a zero
		// link, emptied the list, and the other device's interrupt was
		// never taken. HP-UX's transaction layer counts those interrupts
		// -- a card's DRT routine is a V on the semaphore the driver
		// waits on after every ICEI (CORE SYSTEM +0x10dc) -- so each one
		// lost left the 27140A's semaphore a unit short, and with the
		// console's card interrupting every millisecond under a login
		// session the multiplexer's poll parked for good within seconds:
		// its gettys died of the driver's timeout and every reopen
		// answered EIO.
		bool queued = false;
		if (tail != 0xffffffffu)
		{
			u32 e = mem.read32(ta + 4);
			for (unsigned n = 0; e != 0 && e != 0xffffffffu && n < 64; n++)
			{
				if (e == device)
				{
					queued = true;
					break;
				}
				e = mem.read32(e) >> 16;
			}
		}
		if (tail != 0xffffffffu && !queued)   // -1 = locked by CPU
		{
			// The new last entry's own link must read zero.
			mem.write32(device,
					mem.read32(device) & 0x0000ffffu);
			if (tail != 0)
			{
				mem.write32(tail, (mem.read32(tail) & 0x0000ffffu)
						| (device << 16));
				mem.write32(ta, device);
			}
			else
			{
				mem.write32(ta + 4, device);    // head first,
				mem.write32(ta, device);        // tail last (step 4)
			}
		}
		(void)tail_addr;

		// "...except that the interrupt mask on the IOP now disables the
		// device that requested from requesting again."
		c.int_enabled = false;

		m_int_posted |= (1u << pa);
		update_interrupt();
	}

	u32 interrupt_request() const { return m_int_request; }

	// True once for each raise. The request word is a latch that only
	// CIRQ clears, so its level means "a request is pending", not "a
	// request just arrived" -- and a device that interrupts twice without
	// being serviced moves the level not at all. A host waking a STOPped
	// CPU wants the event.
	bool take_request_event() { const bool e = m_int_event; m_int_event = false; return e; }

	// True once per attention the poll latched; the host forwards it to
	// the CPU as message 14.
	bool take_attention_event() { const bool e = m_attn_event; m_attn_event = false; return e; }

	// The interrupt level a device's requests are recorded at, which is
	// also which message bit (16 + level) announces them.
	unsigned interrupt_level(unsigned pa) const { return m_chan[pa & 7].int_level; }
	u32 dma_mask() const { return m_dma_mask; }

private:
	static constexpr u32 REVISION = 0x19820920;   // I/O ERS A-1FE1-3030-9, 09/20/82
	static constexpr unsigned TIMER_PA = 7;

	struct channel_state
	{
		iop_interface *iface = nullptr;
		u32 dma_addr = 0;
		u32 dma_current = 0;
		bool int_pending = false;   // request latched while disabled
		u32 dma_count = 0;
		u32 dma_status = 0;
		u32 termination = 0;
		u32 term = 0;       // RDCS low half: how the last transfer ended
		u32 data_buffer = 0;
		u8 dma_pa = 0;
		u8 int_level = 0;
		u32 chain = 0;      // the 0115 list terminator's pointer operand
		bool await_poll = false;    // a list suspended on ICEI
		// Status bit 20, Command Execution: set by ICEI, cleared by DCE
		// and DCEI, and per-PA in all three.
		bool cmd_exec = false;
		unsigned stall_ticks = 0;   // FOCUS_IOPDBG bookkeeping
		// Devices power up with interrupt requests disabled: the CPU
		// arms each one with EINT after it has installed the device's
		// DRT entry -- vectoring through an empty entry lands at
		// absolute zero, so the hardware cannot default to armed. The
		// boot logs bear it out: the OS runs its whole device scan
		// without ever issuing an EINT.
		bool int_enabled = false;
		bool dma_active = false;
	};

	// Interface Poll returns a word with a bit per channel wanting service.
	u32 poll() const
	{
		u32 v = 0;
		for (unsigned pa = 0; pa < CHANNELS; pa++)
			if (m_chan[pa].iface && m_chan[pa].iface->poll_request())
				v |= (1u << pa);
		return v;
	}

	// The host's interrupt line follows the requests the IOP actually
	// POSTED -- an accepted raise on an armed device -- not every bit in
	// the request register, which a device is free to set with the mask
	// against it and a driver is free to poll.
	void update_interrupt()
	{
		// Only on a CHANGE. The host wires this to a CPU input line, and
		// that line sets a bit in the Message register; asserting it
		// again re-sets the bit the interrupt service has just cleared,
		// so a level that never falls becomes a storm -- the handler
		// exits, the CPU reaches its dispatcher STOP, the same interrupt
		// fires immediately, ten thousand times a second. A line is a
		// level, and it is the transitions that are events.
		const bool level = m_int_enable
				&& (m_int_posted & ~m_int_mask) != 0;
		if (level == m_int_line)
			return;
		m_int_line = level;
		m_host.iop_interrupt(level);
	}

	host &m_host;
	channel_state m_chan[CHANNELS];
	u8 m_channel;
	u32 m_dma_mask = 0;
	u32 m_int_mask = 0;
	u32 m_int_request = 0;
	u32 m_int_posted = 0;   // the requests the IOP posted to the CPU
	bool m_int_line = false;    // the level currently on the host line
	unsigned m_dbg_ticks = 0;   // FOCUS_IOPDBG heartbeat
	bool m_int_event = false;
	u32 m_int_message = 0;
	u32 m_attn_mask = 0;
	u32 m_attn_log = 0;     // TF0(16:8): PAs holding an unacknowledged ARD
	bool m_attn_event = false;
	u32 m_timer_start = 0;
	u32 m_timer_count = 0;
	bool m_timer_enabled = false;
	bool m_timer_event = false;
	u32 m_timer_frac = 0;   // sixty-fourths of a count, for timer_tick_ms()
	u32 m_raise_events = 0;
	bool m_led = false;
	bool m_lockout = false;
	bool m_enabled = true;
	bool m_int_enable = true;
};

} // namespace focus

#endif // MAME_IOP_FOCUSIOP_H
