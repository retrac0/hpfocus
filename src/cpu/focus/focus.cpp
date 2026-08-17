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

focus::u32 focus_device::read32(focus::u32 addr)
{
	return m_program.read_dword(addr & ~3u);
}

void focus_device::write32(focus::u32 addr, focus::u32 data)
{
	m_program.write_dword(addr & ~3u, data);
}

focus::u8 focus_device::read8(focus::u32 addr)
{
	// Big-endian: byte 0 of a word is its most significant.
	return u8(m_program.read_dword(addr & ~3u) >> (8 * (3 - (addr & 3))));
}

void focus_device::write8(focus::u32 addr, focus::u8 data)
{
	const unsigned sh = 8 * (3 - (addr & 3));
	m_program.write_dword(addr & ~3u, u32(data) << sh, u32(0xff) << sh);
}

focus::u16 focus_device::read16(focus::u32 addr)
{
	const u32 w = m_program.read_dword(addr & ~3u);
	return u16((addr & 2) ? (w & 0xffff) : (w >> 16));
}

void focus_device::write16(focus::u32 addr, focus::u16 data)
{
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
	do
	{
		debugger_instruction_hook(m_core.r.p);

		const focus::exec_trap t = m_core.step();
		if (t != focus::exec_trap::none)
		{
			// Nothing can be done about a fault the core could not vector;
			// stop rather than spin, so the debugger shows where it died.
			logerror("%s: execution stopped, trap %d at P=%08X\n",
					tag(), int(t), m_core.r.p);
			m_icount = 0;
			break;
		}

		// The ERS gives no per-instruction timings beyond a handful of
		// examples (Hardware Technical Data lists a 32-bit integer multiply
		// at 2.92 us). One cycle per instruction until something needs
		// better.
		m_icount--;
	} while (m_icount > 0);
}


void focus_device::execute_set_input(int inputnum, int state)
{
	// External interrupts arrive as bits in the Message register, gated by
	// the Mask register and by STATUS bit I (ERS sheets 13, 38-43). The
	// interrupt handler and IXIT are not implemented yet, so the line is
	// recorded but not acted on.
	if (inputnum >= 0 && inputnum < 32)
	{
		if (state)
			m_core.r.msg |= (1u << (31 - inputnum));
		else
			m_core.r.msg &= ~(1u << (31 - inputnum));
	}
}
