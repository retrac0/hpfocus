// license:BSD-3-Clause
// copyright-holders:
/***************************************************************************

    HP FOCUS disassembler

    The FOCUS processor powered the HP 9000 Series 500 (models 520/530/540/550,
    1982). It is a 32-bit microcoded stack machine with no programmer-visible
    general purpose registers -- the HP 3000 architecture widened to 32 bits.

    Reference: Focus Machine Instruction Set ERS, A-1FE1-3020-B, 12/06/82,
    J. Fiasconaro. Opcode map from Appendix B (sheet 137); instruction formats
    from section 6.1 (sheet 53).

    NOTE ON BIT NUMBERING: the ERS numbers bits with 0 as the MOST significant,
    and writes a field as (N:M) meaning M bits starting at bit N. Comments here
    use ERS numbering; the code shifts accordingly.

***************************************************************************/

#ifndef MAME_CPU_FOCUS_FOCUSDASM_H
#define MAME_CPU_FOCUS_FOCUSDASM_H

#pragma once

class focus_disassembler : public util::disasm_interface
{
public:
	focus_disassembler() = default;
	virtual ~focus_disassembler() = default;

	virtual u32 opcode_alignment() const override { return 2; }
	virtual offs_t disassemble(std::ostream &stream, offs_t pc,
			const data_buffer &opcodes, const data_buffer &params) override;

private:
	// How an instruction's operand field is rendered.
	enum op_kind : u8
	{
		OK_NONE,        // no operand
		OK_MEM_WORD,    // base-relative, offset counts words
		OK_MEM_BYTE,    // base-relative, offset counts bytes
		OK_BRANCH_HW,   // halfword branch displacement, sign from the opcode
		OK_IMM,         // immediate
		OK_SHIFT,       // shift count
		OK_IO,          // I/O opcode
		OK_STT,         // segment transfer table entry
		OK_STT4,        // four times an STT entry number
		OK_NFIELD,      // EXIT/SXIT N field
		OK_LEVEL,       // lexical level plus offset
		OK_PARAM,       // debug-aid parameter
		OK_BITFIELD     // EXF/DPF start bit and width
	};

	struct op
	{
		const char *mnemonic;
		op_kind kind;
	};

	// BASE field, identical in formats 1 and 6. The offset is always
	// positive; the base selects the direction (ERS sheet 53).
	static const char *const s_bases[8];

	static const op s_fmt1[8];
	static const op s_fmt2[64];
	static const op s_fmt3[64];
	static const op s_fmt4[64];
	static const op s_fmt5[32];
	static const op s_fmt6[32];

	static void format_cc(std::ostream &stream, const char *mnemonic, u8 ccf);
	void format_mem(std::ostream &stream, u8 base, u32 offset,
			bool ind, bool idx) const;
};

#endif // MAME_CPU_FOCUS_FOCUSDASM_H
