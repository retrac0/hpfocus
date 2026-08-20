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
	// present" and moves on. Real boot code additionally issues 000 and
	// 065, still refused until their effect is understood.
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
	virtual u16 dma_read16() { return u16((dma_read() << 8) | dma_read()); }
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
		m_int_event = false;
		m_int_message = 0;
		m_attn_mask = 0;
		m_attn_log = 0;
		m_attn_event = false;
		m_timer_start = 0;
		m_timer_count = 0;
		m_timer_enabled = false;
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

	// Where the processor is, for I/O logs: log lines that carry the P
	// register name the code that issued the operation, which is what a
	// protocol log is for. The host wires it to the CPU.
	std::function<u32()> trace_pc;
	u32 tpc() const { return trace_pc ? trace_pc() : 0; }

	//----------------------------------------------------------------
	//  MPB command entry points
	//----------------------------------------------------------------

	bool io_write(const target &t, u32 data) override
	{
		if (t.channel != m_channel)
			return false;
		channel_state &c = m_chan[t.pa & (CHANNELS - 1)];
		if (std::getenv("FOCUS_IOALL"))
			std::printf("[iop W op=%03o pa=%u d=%08x p=%08x]\n", t.opcode, t.pa, data, tpc());

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
			if (std::getenv("FOCUS_IOPLOG"))
				std::printf("[iop WAMK=%08x ch=%u pa=%u]\n", data, t.channel, t.pa);
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
					c.iface->write_fr(t.opcode >> 3, data & 0xffff);
				return true;
			}
			if (std::getenv("FOCUS_IOPLOG"))
				std::printf("[iop REFUSED W op=%03o pa=%u d=%08x]\n", t.opcode, t.pa, data);
			return false;
		}
	}

	bool io_read(const target &t, u32 &data) override
	{
		if (t.channel != m_channel)
			return false;
		channel_state &c = m_chan[t.pa & (CHANNELS - 1)];
		if (std::getenv("FOCUS_IOALL"))
			std::printf("[iop R op=%03o pa=%u p=%08x]\n", t.opcode, t.pa, tpc());

		switch (t.opcode)
		{
		case CMD_RDCS:
			// Remaining count in the HIGH half, termination status in the
			// low: the kernel's transfer classifier indexes a class table
			// with bits 13:15 of this word plus bit 0, and accepts only
			// class 4 -- bits 13:15 = 1, "terminated on count" -- while
			// its ISR treats bits 15, 14 and 0 as errors. The old
			// status-high/count-low reading came from a poor scan.
			data = ((c.dma_count & 0xffff) << 16) | c.term;
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
		case CMD_RIRV: data = REVISION; return true;
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
				data = c.iface ? (c.iface->read_fr(t.opcode >> 3) & 0xffff) : 0;
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
		if (std::getenv("FOCUS_IOALL"))
			std::printf("[iop C op=%03o pa=%u p=%08x]\n", t.opcode, t.pa, tpc());

		switch (t.opcode)
		{
		// Start DMA "sets a bit in the DMA Mask register" itself (I/O ERS
		// sheet 30) -- EDMA is not a prerequisite. SDC also clears the
		// PA's interrupt request bit, SDEC additionally enables its
		// interrupt, SRC is the RAMD flavour of SDC.
		case CMD_SD:
			c.dma_active = true;
			m_dma_mask |= (1u << t.pa);
			run_dma(t.pa);
			return true;
		case CMD_SDC:
		case CMD_SRC:
			c.dma_active = true;
			m_dma_mask |= (1u << t.pa);
			m_int_request &= ~(1u << t.pa);
			update_interrupt();
			if (std::getenv("FOCUS_IOPLOG"))
				std::printf("[iop SDC pa=%u addr=%08x cnt=%u st=%08x en=%d]\n",
						t.pa, c.dma_addr, c.dma_count, c.dma_status,
						c.int_enabled);
			run_dma(t.pa);
			return true;
		case CMD_SDEC:
			c.dma_active = true;
			m_dma_mask |= (1u << t.pa);
			c.int_enabled = true;
			m_int_request &= ~(1u << t.pa);
			update_interrupt();
			run_dma(t.pa);
			return true;
		case CMD_EDMA: m_dma_mask |= (1u << t.pa); return true;
		case CMD_DDMA: m_dma_mask &= ~(1u << t.pa); c.dma_active = false; return true;
		case CMD_CDMK: m_dma_mask = 0; return true;
		case CMD_CIRQ:
			m_int_request &= ~(1u << t.pa);
			update_interrupt();
			if (std::getenv("FOCUS_IOPLOG"))
				std::printf("[iop CIRQ pa=%u]\n", t.pa);
			return true;
		case CMD_CAAK: m_attn_log &= ~(1u << t.pa); return true;
		case CMD_ETMI:
			// Clears Timer Disable (I/O ERS sheet 33); underflows then
			// send message 13.
			m_timer_enabled = true;
			return true;
		case CMD_CALM: m_lockout = false; return true;
		case CMD_SALM: m_lockout = true; return true;
		case CMD_DCE:  m_enabled = false; return true;
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
			run_command_list(t.pa);
			return true;
		case CMD_RIOB:
			// Reset I/O Bus: drop every channel's DMA and clear pending
			// requests, as a power-up sequence expects.
			for (auto &c : m_chan) { c.dma_active = false; }
			m_dma_mask = 0;
			m_int_request = 0;
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
			m_int_mask = 0;
			update_interrupt();
			return true;
		default:
			// CIF n, the control strobe on interface register n.
			if ((t.opcode & 7) == 0 && t.opcode <= 0170)
			{
				if (c.iface)
					c.iface->control_fr(t.opcode >> 3);
				return true;
			}
			if (std::getenv("FOCUS_IOPLOG"))
				std::printf("[iop REFUSED C op=%03o pa=%u]\n", t.opcode, t.pa);
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
			run_dma(pa);
	}

	// Run a channel's DMA as far as it will go. A counted transfer
	// completes at bus speed on real hardware -- microseconds after the
	// start command, well inside a driver's completion spin -- so a
	// start command calls this directly rather than waiting for the
	// next poll round (the display driver quiesces the channel as a
	// timeout error if the completion is a poll period late). An
	// uncounted transfer moves one unit per call so the loop cannot
	// spin forever.
	void run_dma(unsigned pa)
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

		memory_interface &mem = m_host.iop_memory();
		// Direction and width come from the DMA status word (Table
		// 5.1): bit 27 (F1) set = output, memory to the card; bit 29
		// set = byte transfers, clear = halfword. The count is "the
		// length of the DMA block transfer in transfers".
		const bool output = ers32::get(c.dma_status, 27, 1) != 0;
		const bool bytes  = ers32::get(c.dma_status, 29, 1) != 0;
		u32 left = c.dma_count ? c.dma_count : 1;
		while (left-- && (c.dma_active ? c.iface->service_request()
				: (c.dma_count != 0 && c.iface->burst_request())))
		{
			if (bytes)
			{
				if (output)
					c.iface->dma_write(mem.read8(c.dma_current));
				else
					mem.write8(c.dma_current, c.iface->dma_read());
				c.dma_current++;
			}
			else
			{
				if (output)
					c.iface->dma_write16(mem.read16(c.dma_current));
				else
					mem.write16(c.dma_current, c.iface->dma_read16());
				c.dma_current += 2;
			}

			if (c.dma_count && --c.dma_count == 0)
			{
				c.dma_active = false;
				c.term = 0x2000;    // terminated on count, no error
				if (std::getenv("FOCUS_IOPLOG"))
					std::printf("[iop DMA-done pa=%u en=%d]\n", pa, c.int_enabled);
				c.iface->dma_complete(output);
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
	// and waits for the DMA completion interrupt. Opcodes 0115 and 074 end
	// the list -- 0115 carries a pointer whose meaning is not yet
	// established (a continuation block the kernel allocates right after
	// the list), 074 nothing. Both stop execution here.
	void run_command_list(unsigned pa)
	{
		memory_interface &mem = m_host.iop_memory();
		const u32 drt = 128u * m_channel + 16u * (pa & 7);
		u32 ptr = mem.read32(drt + 12);
		const bool log = std::getenv("FOCUS_IOPLOG") || std::getenv("FOCUS_ICEILOG");
		if (log)
			std::printf("[iop ICEI pa=%u list=%08x]\n", pa, ptr);
		for (unsigned step = 0; step < 32; step++, ptr += 8)
		{
			const u32 word = mem.read32(ptr);
			const u32 operand = mem.read32(ptr + 4);
			const u8 op = u8(word & 0x1ff);
			if (log)
				std::printf("[iop ICEI+%02u op=%03o operand=%08x]\n",
						step, op, operand);
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
			case 0115:
				// A pointer operand; the kernel stores the address of the
				// words just after the list. Treated as end-of-list until
				// its meaning is established.
				m_chan[pa & 7].chain = operand;
				return;
			case 074:
				return;
			default:
				// WIF n delivers the operand to the card's register n --
				// the first list entry is the card's command byte on FR8.
				if ((op & 7) == 2 && op <= 0172)
					ok = io_write(t, operand);
				if (!ok && log)
					std::printf("[iop ICEI STOP op=%03o]\n", op);
				if (!ok)
					return;
				break;
			}
			(void)ok;
		}
	}

	// Advance the interval timer by `units` counts (one count is
	// (1)*512 clock periods = 28.4 us on real hardware; hosts pass
	// however many of those their tick represents).
	void timer_tick(u32 units = 1)
	{
		if (m_timer_count == 0)
			return;
		if (m_timer_count <= units)
		{
			// Underflow "is the setting of the Timer Interrupt Request
			// message bit 13 and sending a message to the CPU" (I/O ERS
			// 7.1) -- a plain CPU message, not a device interrupt
			// through the DRT lists. The host forwards the event as
			// raise_message(13).
			m_timer_count = m_timer_start;
			if (m_timer_enabled)
				m_timer_event = true;
		}
		else
			m_timer_count -= units;
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
		if (std::getenv("FOCUS_IOPLOG"))
			std::printf("[iop raise pa=%u en=%d lvl=%u]\n", pa, c.int_enabled, c.int_level);
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
		if (tail != 0xffffffffu)                // -1 = locked by the CPU
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

		m_int_request |= (1u << pa);
		m_int_event = true;
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
	u32 attention_log() const { return m_attn_log; }

	// The interrupt level a device's requests are recorded at, which is
	// also which message bit (16 + level) announces them.
	unsigned interrupt_level(unsigned pa) const { return m_chan[pa & 7].int_level; }
	u32 dma_mask() const { return m_dma_mask; }

private:
	static constexpr u32 REVISION = 0x0001;
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
			if (m_chan[pa].iface && m_chan[pa].iface->service_request())
				v |= (1u << pa);
		return v;
	}

	void update_interrupt()
	{
		m_host.iop_interrupt(m_int_enable
				&& (m_int_request & ~m_int_mask) != 0);
	}

	host &m_host;
	channel_state m_chan[CHANNELS];
	u8 m_channel;
	u32 m_dma_mask = 0;
	u32 m_int_mask = 0;
	u32 m_int_request = 0;
	bool m_int_event = false;
	u32 m_int_message = 0;
	u32 m_attn_mask = 0;
	u32 m_attn_log = 0;     // TF0(16:8): PAs holding an unacknowledged ARD
	bool m_attn_event = false;
	u32 m_timer_start = 0;
	u32 m_timer_count = 0;
	bool m_timer_enabled = false;
	bool m_timer_event = false;
	u32 m_raise_events = 0;
	bool m_led = false;
	bool m_lockout = false;
	bool m_enabled = true;
	bool m_int_enable = true;
};

} // namespace focus

#endif // MAME_IOP_FOCUSIOP_H
