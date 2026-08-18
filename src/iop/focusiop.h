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
	CMD_WTMS  = 040,   // W  write timer start
	CMD_WTMC  = 043,   // W  write timer count
	CMD_RTM   = 044,   // R  read timer count
	CMD_ETMI  = 045,   // C  enable timer interrupt

	// Interfaces
	CMD_IFPL  = 047,   // R  interface poll
	CMD_RISF  = 011,   // R  read interface status and flag (sheet 34):
	                   //    STATUS in bit 30, FLAG in bit 31, rest zero
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

	// A single DMA transfer in each direction.
	virtual u8 dma_read() { return 0; }
	virtual void dma_write(u8) { }

	// Direct CPU access to the card.
	virtual void write_data(u32) { }
	virtual u32 read_data() { return 0; }

	// Direct I/O: the interface's sixteen FR registers, reached with the
	// RIF n / WIF n commands (I/O ERS sheet 33: opcodes 8n+1 and 8n+2).
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
		for (auto &c : m_chan)
			c = channel_state();
		m_int_mask = 0;
		m_int_request = 0;
		m_int_message = 0;
		m_attn_mask = 0;
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

	u8 channel_number() const { return m_channel; }
	bool led() const { return m_led; }

	//----------------------------------------------------------------
	//  MPB command entry points
	//----------------------------------------------------------------

	bool io_write(const target &t, u32 data) override
	{
		if (t.channel != m_channel)
			return false;
		channel_state &c = m_chan[t.pa & (CHANNELS - 1)];

		switch (t.opcode)
		{
		case CMD_WDS:  c.dma_status = data; return true;
		case CMD_WDC:  c.dma_count = data & 0xffff; return true;
		case CMD_WDA:  c.dma_addr = data; c.dma_current = data; return true;
		case CMD_WDTF: c.termination = data; return true;
		case CMD_WDCA: c.dma_current = data; return true;
		case CMD_WDPA: c.dma_pa = u8(data & 7); return true;
		case CMD_WDB:  c.data_buffer = data; return true;
		case CMD_WIMK: m_int_mask = data; update_interrupt(); return true;
		case CMD_WIMG: m_int_message = data; return true;
		case CMD_SIL:  c.int_level = u8(data & 0xf); return true;
		case CMD_WTMS: m_timer_start = data; m_timer_count = data; return true;
		case CMD_WTMC: m_timer_count = data; return true;
		case CMD_WAMK: m_attn_mask = data; return true;
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
			return false;
		}
	}

	bool io_read(const target &t, u32 &data) override
	{
		if (t.channel != m_channel)
			return false;
		channel_state &c = m_chan[t.pa & (CHANNELS - 1)];

		switch (t.opcode)
		{
		case CMD_RDCS:
			// Current count in the low half, status in the high half.
			data = (c.dma_status & 0xffff0000u) | (c.dma_count & 0xffff);
			return true;
		case CMD_RDMK: data = m_dma_mask; return true;
		case CMD_RIMK: data = m_int_mask; return true;
		case CMD_RIRQ: data = m_int_request; return true;
		case CMD_RIMG: data = m_int_message; return true;
		case CMD_RTM:  data = m_timer_count; return true;
		case CMD_RDPA: data = c.dma_pa; return true;
		case CMD_RDB:  data = c.data_buffer; return true;
		case CMD_RDTF: data = c.termination; return true;
		case CMD_RIRV: data = REVISION; return true;
		case CMD_RIST: data = c.iface ? c.iface->status() : 0; return true;
		case CMD_RIFG: data = c.iface ? c.iface->flag() : 0; return true;
		case CMD_IFPL: data = poll(); return true;
		case CMD_RISF:
			// STATUS in bit 30, FLAG in bit 31, all else zero (sheet 34).
			data = c.iface
					? (((c.iface->status() & 1) << 1) | (c.iface->flag() & 1))
					: 0;
			return true;
		default:
			// RIF n, Read Interface FRn (sheet 33): opcode 8n+1 reads
			// the addressed card's register n into the low 16 bits. An
			// empty slot reads as an idle bus. RISF keeps 011.
			if ((t.opcode & 7) == 1 && t.opcode <= 0171 && t.opcode != CMD_RISF)
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

		switch (t.opcode)
		{
		case CMD_SD:   c.dma_active = true; return true;
		case CMD_EDMA: m_dma_mask |= (1u << t.pa); return true;
		case CMD_DDMA: m_dma_mask &= ~(1u << t.pa); c.dma_active = false; return true;
		case CMD_CDMK: m_dma_mask = 0; return true;
		case CMD_CIRQ: m_int_request &= ~(1u << t.pa); update_interrupt(); return true;
		case CMD_ETMI: m_timer_enabled = true; return true;
		case CMD_CALM: m_lockout = false; return true;
		case CMD_SALM: m_lockout = true; return true;
		case CMD_DCE:  m_enabled = false; return true;
		case CMD_LON:  m_led = true; return true;
		case CMD_LOFF: m_led = false; return true;
		case CMD_CAAK: m_attn_ack = false; return true;
		case CMD_EINT: m_int_enable = true;  update_interrupt(); return true;
		case CMD_DINT: m_int_enable = false; update_interrupt(); return true;
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
		default:       return false;
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

		for (unsigned pa = 0; pa < CHANNELS; pa++)
		{
			channel_state &c = m_chan[pa];
			if (!c.dma_active || !c.iface || !c.iface->service_request())
				continue;
			if (!(m_dma_mask & (1u << pa)))
				continue;

			// Direction lives in the DMA status word; bit 0 set means the
			// transfer runs from memory out to the card.
			memory_interface &mem = m_host.iop_memory();
			if (ers32::get(c.dma_status, 0, 1))
				c.iface->dma_write(mem.read8(c.dma_current));
			else
				mem.write8(c.dma_current, c.iface->dma_read());

			c.dma_current++;
			if (c.dma_count && --c.dma_count == 0)
			{
				c.dma_active = false;
				raise(pa);
			}
		}
	}

	// Advance the interval timer by one tick.
	void timer_tick()
	{
		if (m_timer_count == 0)
			return;
		if (--m_timer_count == 0)
		{
			// Timer interrupt is Message register bit 13 on the CPU side
			// (Instruction Set ERS sheet 13); here it is an ordinary IOP
			// interrupt request.
			m_timer_count = m_timer_start;
			if (m_timer_enabled)
				raise(TIMER_PA);
		}
	}

	// Let a card raise an interrupt of its own.
	void raise(unsigned pa)
	{
		m_int_request |= (1u << pa);
		update_interrupt();
	}

	u32 interrupt_request() const { return m_int_request; }
	u32 dma_mask() const { return m_dma_mask; }

private:
	static constexpr u32 REVISION = 0x0001;
	static constexpr unsigned TIMER_PA = 7;

	struct channel_state
	{
		iop_interface *iface = nullptr;
		u32 dma_addr = 0;
		u32 dma_current = 0;
		u32 dma_count = 0;
		u32 dma_status = 0;
		u32 termination = 0;
		u32 data_buffer = 0;
		u8 dma_pa = 0;
		u8 int_level = 0;
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
	u32 m_int_message = 0;
	u32 m_attn_mask = 0;
	u32 m_timer_start = 0;
	u32 m_timer_count = 0;
	bool m_timer_enabled = false;
	bool m_attn_ack = false;
	bool m_led = false;
	bool m_lockout = false;
	bool m_enabled = true;
	bool m_int_enable = true;
};

} // namespace focus

#endif // MAME_IOP_FOCUSIOP_H
