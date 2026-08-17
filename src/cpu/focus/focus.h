// license:BSD-3-Clause
// copyright-holders:
/***************************************************************************

    HP FOCUS CPU device

    The processor of the HP 9000 Series 500 (models 520/530/540/550, 1982):
    a 32-bit microcoded stack machine with no programmer-visible general
    purpose registers, being the HP 3000 architecture widened to 32 bits.

    The architectural work lives in header-only classes that carry no MAME
    dependency -- focusstate.h, focusaddr.h, focusdst.h and focusexec.h -- so
    that they can be unit tested on their own. This device supplies the
    address space, the scheduler hook and the debugger state.

***************************************************************************/

#ifndef MAME_CPU_FOCUS_FOCUS_H
#define MAME_CPU_FOCUS_FOCUS_H

#pragma once

#include "focusexec.h"

//**************************************************************************
//  TYPE DEFINITIONS
//**************************************************************************

class focus_device : public cpu_device, protected focus::memory_interface
{
public:
	focus_device(const machine_config &mconfig, const char *tag,
			device_t *owner, u32 clock);

	// The MPB channel number this CPU answers to, held in MASK bits 3-5.
	void set_mpb_channel(u8 channel) { m_mpb_channel = channel; }

protected:
	// device_t
	virtual void device_start() override ATTR_COLD;
	virtual void device_reset() override ATTR_COLD;

	// device_execute_interface
	virtual u32 execute_min_cycles() const noexcept override { return 1; }
	virtual u32 execute_max_cycles() const noexcept override { return 64; }
	virtual void execute_run() override;
	virtual void execute_set_input(int inputnum, int state) override;

	// device_memory_interface
	virtual space_config_vector memory_space_config() const override;

	// device_disasm_interface
	virtual std::unique_ptr<util::disasm_interface> create_disassembler() override;

	// device_state_interface
	virtual void state_string_export(const device_state_entry &entry,
			std::string &str) const override;

	// focus::memory_interface -- routes the core's accesses to the space.
	virtual focus::u32 read32(focus::u32 addr) override;
	virtual void write32(focus::u32 addr, focus::u32 data) override;
	virtual focus::u8 read8(focus::u32 addr) override;
	virtual void write8(focus::u32 addr, focus::u8 data) override;
	virtual focus::u16 read16(focus::u32 addr) override;
	virtual void write16(focus::u32 addr, focus::u16 data) override;

private:
	// The three MSBs of every absolute address must be zero (ERS sheet 54),
	// so the space is 29 bits of byte address over 32-bit big-endian words.
	static constexpr int ADDR_WIDTH = 29;

	address_space_config m_program_config;
	memory_access<ADDR_WIDTH, 2, 0, ENDIANNESS_BIG>::specific m_program;

	focus::cpu_core m_core;
	int m_icount;
	u8 m_mpb_channel;

	// Debugger-visible mirrors, refreshed from the core each step.
	u32 m_dbg_flags;
};

DECLARE_DEVICE_TYPE(FOCUS, focus_device)

//**************************************************************************
//  REGISTER ENUMERATION
//**************************************************************************

enum
{
	FOCUS_P = 1, FOCUS_PB, FOCUS_PL,
	FOCUS_SB, FOCUS_SL, FOCUS_S, FOCUS_Q,
	FOCUS_DB, FOCUS_DL, FOCUS_DST,
	FOCUS_X, FOCUS_STATUS, FOCUS_FLAGS,
	FOCUS_MSG, FOCUS_MASK, FOCUS_BRKPT,
	FOCUS_SAR, FOCUS_SDR,
	FOCUS_A, FOCUS_B, FOCUS_C, FOCUS_D    // top of stack, read only
};

#endif // MAME_CPU_FOCUS_FOCUS_H
