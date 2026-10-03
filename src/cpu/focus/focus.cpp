// license:BSD-3-Clause
// copyright-holders:
/***************************************************************************

    HP FOCUS CPU device

***************************************************************************/

#include "emu.h"
#include "focus.h"
#include "focusdasm.h"

DEFINE_DEVICE_TYPE(FOCUS, focus_device, "focus", "HP FOCUS")


focus_device::focus_device(const machine_config &mconfig, const char *tag,
		device_t *owner, u32 clock)
	: cpu_device(mconfig, FOCUS, tag, owner, clock)
	, m_program_config("program", ENDIANNESS_BIG, 32, ADDR_WIDTH, 0)
	, m_core(*this)
	, m_icount(0)
	, m_mpb_channel(1)
	, m_dbg_flags(0)
{
}


device_memory_interface::space_config_vector focus_device::memory_space_config() const
{
	return space_config_vector{ std::make_pair(AS_PROGRAM, &m_program_config) };
}


std::unique_ptr<util::disasm_interface> focus_device::create_disassembler()
{
	return std::make_unique<focus_disassembler>();
}


//**************************************************************************
//  MEMORY
//**************************************************************************

// FOCUS_RWATCH=<lo>:<hi>[ FOCUS_RWFROM=<seconds>] reports every read of a
// byte-address range. The write side has FOCUS_WWATCH; this is the other
// half, and it exists because the driver's on_ea hook only sees addresses
// the standard effective-address path computes -- a table reached through
// PLDA/PSTA, as the per-task handler table is, is invisible to it.
// Instruction fetches come through here too, so keep the range to data.
void focus_device::check_rwatch(focus::u32 addr, focus::u32 data)
{
	static const focus::knob::range where("FOCUS_RWATCH");
	static const focus::knob::seconds from("FOCUS_RWFROM", 0.0);
	static focus::knob::channel log("FOCUS_RWATCH", 200);

	const double now = machine().time().as_double();
	if (!where || addr + 4 <= where.lo() || addr > where.hi() || !log
			|| now < from)
		return;
	// The last line the budget allows says so, rather than the report
	// simply stopping and leaving the reader to wonder.
	const char *const note = log.left() == 1 ? " (cap reached)" : "";
	log("[rwatch %08x -> %08x P=%08x PB=%08x t=%.3f]%s\n",
			addr, data, m_core.r.p, m_core.r.pb, now, note);
}

focus::u32 focus_device::read32(focus::u32 addr)
{
	const focus::u32 v = m_program.read_dword(addr & ~3u);
	check_rwatch(addr & ~3u, v);
	return v;
}

// FOCUS_WWATCH=<lo>:<hi>[ FOCUS_WWFROM=<seconds>] reports every store into
// a byte-address range, unlike the driver's FOCUS_WPOINT change-poller,
// which cannot see a value overwritten with itself. The report names P, so
// a store from DMA or another non-CPU agent shows P of whatever the CPU was
// doing at the time -- attribution still needs the caller to check that P's
// neighbourhood really contains a store instruction.
void focus_device::check_wwatch(focus::u32 addr, focus::u32 data, unsigned bytes)
{
	static const focus::knob::range where("FOCUS_WWATCH");
	static const focus::knob::seconds from("FOCUS_WWFROM", 0.0);
	static focus::knob::channel log("FOCUS_WWATCH", 400);

	const double now = machine().time().as_double();
	if (!where || addr + bytes <= where.lo() || addr > where.hi() || !log
			|| now < from)
		return;
	const char *const note = log.left() == 1 ? " (cap reached)" : "";
	log("[wwatch %08x <- %0*x (%u) P=%08x PB=%08x t=%.3f]%s\n",
			addr, int(bytes * 2), data, bytes, m_core.r.p, m_core.r.pb,
			now, note);
}

void focus_device::write32(focus::u32 addr, focus::u32 data)
{
	check_wwatch(addr & ~3u, data, 4);
	m_program.write_dword(addr & ~3u, data);
}

focus::u8 focus_device::read8(focus::u32 addr)
{
	// Big-endian: byte 0 of a word is its most significant.
	const u32 w = m_program.read_dword(addr & ~3u);
	// The watch covers the narrow reads too. It has to: a driver that
	// types a card by the halfword its identify transaction answered
	// reads that halfword, and a word-only watch reports nothing at all
	// while the comparison it is hunting for happens in front of it.
	// The address reported is the one asked for, not the word it lands
	// in -- which half of a word a driver reads is exactly the question
	// a narrow read is usually being watched to answer.
	check_rwatch(addr, w);
	return u8(w >> (8 * (3 - (addr & 3))));
}

void focus_device::write8(focus::u32 addr, focus::u8 data)
{
	check_wwatch(addr, data, 1);
	const unsigned sh = 8 * (3 - (addr & 3));
	m_program.write_dword(addr & ~3u, u32(data) << sh, u32(0xff) << sh);
}

focus::u16 focus_device::read16(focus::u32 addr)
{
	const u32 w = m_program.read_dword(addr & ~3u);
	check_rwatch(addr, w);
	return u16((addr & 2) ? (w & 0xffff) : (w >> 16));
}

void focus_device::write16(focus::u32 addr, focus::u16 data)
{
	check_wwatch(addr & ~1u, data, 2);
	if (addr & 2)
		m_program.write_dword(addr & ~3u, data, 0x0000ffffu);
	else
		m_program.write_dword(addr & ~3u, u32(data) << 16, 0xffff0000u);
}


//**************************************************************************
//  DEVICE
//**************************************************************************

void focus_device::device_start()
{
	space(AS_PROGRAM).specific(m_program);

	set_icountptr(m_icount);

	// Registers, in the order the ERS describes them (sheets 10-14).
	state_add(FOCUS_P,      "P",      m_core.r.p);
	state_add(STATE_GENPC,  "GENPC",  m_core.r.p).noshow();
	state_add(STATE_GENPCBASE, "CURPC", m_core.r.p).noshow();
	state_add(FOCUS_PB,     "PB",     m_core.r.pb);
	state_add(FOCUS_PL,     "PL",     m_core.r.pl);
	state_add(FOCUS_SB,     "SB",     m_core.r.sb);
	state_add(FOCUS_SL,     "SL",     m_core.r.sl);
	state_add(FOCUS_S,      "S",      m_core.r.s);
	state_add(FOCUS_Q,      "Q",      m_core.r.q);
	state_add(FOCUS_DB,     "DB",     m_core.r.db);
	state_add(FOCUS_DL,     "DL",     m_core.r.dl);
	state_add(FOCUS_DST,    "DST",    m_core.r.dst);
	state_add(FOCUS_X,      "X",      m_core.r.x);
	state_add(FOCUS_STATUS, "STATUS", m_core.r.status);
	state_add(STATE_GENFLAGS, "GENFLAGS", m_dbg_flags).noshow().formatstr("%18s");
	state_add(FOCUS_FLAGS,  "FLAGS",  m_core.r.flags);
	state_add(FOCUS_MSG,    "MSG",    m_core.r.msg);
	state_add(FOCUS_MASK,   "MASK",   m_core.r.mask);
	state_add(FOCUS_BRKPT,  "BRKPT",  m_core.r.brkpt);
	state_add(FOCUS_SAR,    "SAR",    m_core.r.sar);
	state_add(FOCUS_SDR,    "SDR",    m_core.r.sdr);

	save_item(NAME(m_core.r.p));
	save_item(NAME(m_core.r.pb));
	save_item(NAME(m_core.r.pl));
	save_item(NAME(m_core.r.sb));
	save_item(NAME(m_core.r.sl));
	save_item(NAME(m_core.r.s));
	save_item(NAME(m_core.r.q));
	save_item(NAME(m_core.r.db));
	save_item(NAME(m_core.r.dl));
	save_item(NAME(m_core.r.dst));
	save_item(NAME(m_core.r.x));
	save_item(NAME(m_core.r.status));
	save_item(NAME(m_core.r.flags));
	save_item(NAME(m_core.r.msg));
	save_item(NAME(m_core.r.mask));
	save_item(NAME(m_core.r.brkpt));
	save_item(NAME(m_core.r.sar));
	save_item(NAME(m_core.r.sdr));
	save_item(NAME(m_mpb_channel));
}


void focus_device::device_reset()
{
	m_core.reset();
	m_stopped = false;

	// MASK bits 3-5 carry this CPU's MPB channel number (ERS sheet 13).
	m_core.r.mask = focus::ers32::set(0, 3, 3, m_mpb_channel);

	// The reset trap enters in secondary address mode on CPU 4.0 and
	// earlier (ERS Appendix G); nothing models address modes yet.
	m_core.enable_traps(false);
}


void focus_device::state_string_export(const device_state_entry &entry,
		std::string &str) const
{
	using namespace focus;

	switch (entry.index())
	{
	case STATE_GENFLAGS:
	{
		static const char *const cc[4] = { "G", "L", "E", "L" };
		str = string_format("%c%c%c%c %s RM%d CS%03X",
				m_core.r.test(sf::M) ? 'M' : '.',
				m_core.r.test(sf::I) ? 'I' : '.',
				m_core.r.test(sf::C) ? 'C' : '.',
				m_core.r.test(sf::O) ? 'O' : '.',
				cc[m_core.r.condition_code() & 3],
				m_core.r.get(sf::RM),
				m_core.r.get(sf::CSN));
		break;
	}
	}
}


//**************************************************************************
//  EXECUTION
//**************************************************************************

void focus_device::execute_run()
{
	if (m_stopped)
	{
		m_icount = 0;
		return;
	}

	do
	{
		// A STOPped machine idles awaiting a message; raise_message
		// un-halts the core when an unmasked bit arrives. Burn the
		// timeslice rather than latching -- the dispatcher's idle loop
		// is STOP and a wake, not a fault.
		if (m_core.halted())
		{
			if (!m_halt_logged)
			{
				logerror("%s: STOP at P=%08X, MASK=%08X MSG=%08X\n",
						tag(), m_core.r.p, m_core.r.mask, m_core.r.msg);
				m_halt_logged = true;
			}
			m_icount = 0;
			return;
		}
		m_halt_logged = false;

		if (m_pre_step)
			m_pre_step(m_core);

		debugger_instruction_hook(m_core.r.p);

		// The microcode interrupt handler runs between instructions:
		// with STATUS.I set and an unmasked I/O message pending, pull
		// the first waiting device from its level's list and vector
		// through its DRT entry (ERS section 5.1.3).
		m_core.service_interrupts();

		const focus::exec_trap t = m_core.step();
		if (t == focus::exec_trap::halted)
		{
			m_icount = 0;
			return;
		}
		if (t != focus::exec_trap::none)
		{
			// A fault the core could not vector -- no handler segment, or an
			// opcode this core does not model. Latch a stop so the machine
			// stays put and the debugger shows where it died, rather than
			// re-executing the same instruction every timeslice.
			logerror("%s: stopped, trap %d at P=%08X (CIR=%08X)\n",
					tag(), int(t), m_core.r.p, m_core.cir());
			m_stopped = true;
			m_icount = 0;
			return;
		}

		// Charge what the instruction cost. The Hardware Technical Data
		// sheet's four measured figures (a 550 ns load, a 1.17 us FP add,
		// a 1.28 us FP multiply and a 2.92 us integer multiply) are 10,
		// 21, 23 and 53 cycles at 18 MHz; the core sets them and every
		// other instruction costs the typical 10. This used to charge one
		// cycle flat, which ran emulated time about ten times too fast.
		m_icount -= int(m_core.cycles);
	} while (m_icount > 0);
}


void focus_device::execute_set_input(int inputnum, int state)
{
	// External interrupts arrive as bits in the Message register, gated by
	// the Mask register and by STATUS bit I (ERS sheets 13, 38-43) -- but
	// not through here. The IOP posts each accepted request as a message
	// at 16 + the device's level (raise_message, from the driver's
	// deliver_iop_interrupts), which is what the microcode interrupt
	// handler consumes, level by level, clearing a bit only when that
	// level's list is empty. The IOP's level line, which the driver still
	// drives on input 16, used to be mirrored into MSG bit 16 here: every
	// post set the level-0 request whatever the device's level was, and
	// every de-assert after a CIRQ or DCE cleared it, erasing a genuine
	// pending level-0 request of another device still linked on the
	// list. An input line carries nothing the Message register does not
	// already have, so it is accepted and ignored.
	(void)inputnum;
	(void)state;
}
