// license:BSD-3-Clause
// copyright-holders:
/***************************************************************************

    The Series 500 internal disc, select code 7.

    The 520's internal mass storage answers select code 7, and the system
    drives it through the module the boot image calls LCS80 -- a local
    CS/80 controller rather than anything on an HP-IB cable. Nothing here
    comes from documentation; every exchange below was read off the
    running system, and the ones still unknown are marked as such.

    What the powerup does, in order:

      WDS 0, CAAK          clear the channel
      RIF 1                read status
      RIF 3 (three times)  the identity register the other slots use --
                           this slot answers zero there
      WIF 0 <1<<7>         the presence pattern, as for every slot
      RIF 1                which must read the pattern back
      WAMK <1<<7>          include the slot in the attention poll
      RIF 1                and from here it is a status register

    and then it polls FR1 until the card's self test finishes -- bit 7
    alone means working, bit 6 as well means done. Answer that and the
    driver's initialisation runs:

      WIF 1 6              a command, meaning not yet known
      DCE, DDMA, DINT, CIRQ    quiesce the channel (twice over)
      WIF 1 1              a second command

    The slot is typed 29 whatever it answers, which is how the system
    knows an internal disc from a display (0x17) or a keyboard (0x0d);
    the type does not have to be earned.

    The identity this slot reports lives in FR0, not FR3 -- answer it and
    the internal disc driver binds where it otherwise reports ERROR 163,
    "Driver or interface not present".

***************************************************************************/

#ifndef MAME_IOP_FOCUSDISC_H
#define MAME_IOP_FOCUSDISC_H

#pragma once

#include "focusiop.h"

#include <cstdio>
#include <cstdlib>

namespace focus {

struct disc_card : public iop_interface
{
	iop_core *iop = nullptr;
	unsigned pa = 7;
	bool log = false;

	// The identity the slot answers on FR0. Which value means "internal
	// disc" is not established: any nonzero answer binds a driver.
	u32 identity = 1;
	// What FR1 reads once the presence test has had its echo. The driver
	// polls this as a status register; 0x140 is the value that gets it
	// through to its initialisation commands.
	// What FR1 reads once the presence test has had its echo. The system
	// polls it waiting for the card's own self test to finish: bit 7
	// alone reads as still working, and the poll runs until bit 6 joins
	// it. Answer 0x80 for a while and then 0xc0 and both readers are
	// satisfied -- the powerup's self test reports no error for select
	// code 7, and the internal disc driver stops polling and goes on.
	// Any other completion value the sweep tried (0x40, 0x140, 0x240)
	// draws "Self test error 1: IO address 007,0000" with that value as
	// its STATUS, so those bits are failure codes.
	u32 status_word = 0x80;
	u32 status_after = 0xc0;
	unsigned busy_reads = 20;

	u32 status() override { return 1; }     // status line asserted
	u32 flag() override { return 1; }       // done
	u32 status_reg() override { return 1; }

	u32 read_fr(unsigned n) override
	{
		u32 v = 0;
		switch (n)
		{
		case 0:
			v = identity;
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
			{
				v = 0;
			}
			else
			{
				v = (busy_reads && m_polls >= busy_reads)
						? status_after : status_word;
				m_polls++;
			}
			break;
		default:
			v = m_fr[n & 15];
			break;
		}
		if (log)
		{
			std::printf("[disc R fr%u = %04x]\n", n, v & 0xffff);
			std::fflush(stdout);
		}
		return v;
	}

	void write_fr(unsigned n, u32 v) override
	{
		if (log)
		{
			std::printf("[disc W fr%u = %04x]\n", n, v & 0xffff);
			std::fflush(stdout);
		}
		m_fr[n & 15] = v & 0xffff;
		if (n == 0)
		{
			m_last_fr0 = v & 0xffff;
			m_echo_pending = true;
		}
		// A command written to FR1 is answered: the driver's read then
		// reports "insufficient data" if the device never says it has
		// any. What the answer should be is the open question; for now
		// the card can be made to ask for service and to interrupt, to
		// see which of the two the driver is waiting on.
		if (n == 1 && m_present)
		{
			if (want_service)
				m_service = true;
			if (want_irq && iop)
				iop->raise(pa);
		}
	}

	void control_fr(unsigned n) override
	{
		if (log)
		{
			std::printf("[disc C fr%u  (fr9=%04x)]\n", n, m_fr[9]);
			std::fflush(stdout);
		}
	}

	bool want_service = false;   // assert the service-request line
	bool want_irq = false;       // and/or raise an interrupt
	bool service_request() override { return m_service; }
	// Data the card hands over, cycled; nothing is a real disc yet.
	u8 dma_read() override { return 0; }
	u16 dma_read16() override { return 0; }

private:
	u32 m_fr[16] = {};
	u32 m_last_fr0 = 0;
	bool m_echo_pending = false;
	bool m_present = false;
	bool m_service = false;
	unsigned m_polls = 0;
};

} // namespace focus

#endif // MAME_IOP_FOCUSDISC_H
