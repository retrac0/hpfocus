// license:BSD-3-Clause
// copyright-holders:
/***************************************************************************

    HP FOCUS disassembler

    Instruction formats (ERS section 6.1, sheet 53; field widths confirmed
    against Appendix B and against real code -- see docs/FOCUS.md Appendix B).
    Bit 0 is the most significant bit throughout.

      Fmt 1 (16b, OP1=2..6)   OP1(0:3) I(3) X(4) BASE(5:3) OFFSET(8:8)
      Fmt 2 (16b, OP1=0)      000 b(3) OP2(4:6) OPERAND(10:6)
      Fmt 3 (OP1=0, OP2=0)    000 b(3) 000000   OP3(10:6)
      Fmt 4 (OP1=0, OP2=1)    000 b(3) 000001   OP3(10:6)
      Fmt 5 (16b, OP1=1)      001 OP2(3:5) OPERAND(8:8)   -- no I or X bit
      Fmt 6 (32b, OP1=7)      111 OP2(3:5) I(8) X(9) BASE(10:3) OFFSET(13:19)

***************************************************************************/

#include "emu.h"
#include "focusdasm.h"

const char *const focus_disassembler::s_bases[8] =
{
	"P+", "P-", "DB+", "DL-", "Q+", "Q-", "SB+", "S-"
};

#include "focusdasm_tables.ipp"

// Opcodes needing step-over / step-out hints in the debugger.
namespace {

constexpr u8 FMT6_PCL  = 003;   // procedure call
constexpr u8 FMT6_BCC  = 031;   // branch on condition code, 32-bit
constexpr u8 FMT6_EXF  = 000;   // extract field
constexpr u8 FMT6_DPF  = 001;   // deposit field
constexpr u8 FMT5_PCLS = 022;   // procedure call, short
constexpr u8 FMT5_EXIT = 023;
constexpr u8 FMT5_SXIT = 024;
constexpr u8 FMT4_IXIT = 074;

} // anonymous namespace


//  Render a branch mnemonic with its condition-code mask appended.
//  ERS sheet 91: CCF(0:1) selects CCG, CCF(1:1) CCE, CCF(2:1) CCL.
void focus_disassembler::format_cc(std::ostream &stream, const char *mnemonic,
		u8 ccf)
{
	if (ccf == 7)
		util::stream_format(stream, "%-8s", util::string_format("%s.ALW", mnemonic));
	else if (ccf == 0)
		util::stream_format(stream, "%-8s", util::string_format("%s.NVR", mnemonic));
	else
		util::stream_format(stream, "%-8s", util::string_format("%s.%s%s%s",
				mnemonic,
				BIT(ccf, 2) ? "G" : "",
				BIT(ccf, 1) ? "E" : "",
				BIT(ccf, 0) ? "L" : ""));
}


void focus_disassembler::format_mem(std::ostream &stream, u8 base, u32 offset,
		bool ind, bool idx) const
{
	util::stream_format(stream, "%s%d", s_bases[base], offset);
	if (ind)
		stream << ",I";
	if (idx)
		stream << ",X";
}


offs_t focus_disassembler::disassemble(std::ostream &stream, offs_t pc,
		const data_buffer &opcodes, const data_buffer &params)
{
	const u16 w = opcodes.r16(pc);
	const u8 op1 = (w >> 13) & 7;

	//-----------------------------------------------------------------
	// Format 6: 32 bits. Always word aligned (ERS sheet 53).
	//-----------------------------------------------------------------
	if (op1 == 7)
	{
		const u32 d = opcodes.r32(pc);
		const u8 op2 = (d >> 24) & 0x1f;
		const op &e = s_fmt6[op2];

		if (e.mnemonic == nullptr)
		{
			util::stream_format(stream, "%-8s0x%08x", ".word", d);
			return 4 | SUPPORTED;
		}

		// BCC replaces I/X/BASE with a condition mask and a sign bit.
		if (op2 == FMT6_BCC)
		{
			const u8 ccf = (d >> 20) & 7;
			const u32 off = d & 0x7ffff;
			const s32 delta = BIT(d, 19) ? -s32(off) : s32(off);
			format_cc(stream, e.mnemonic, ccf);
			util::stream_format(stream, "0x%08x", u32(pc + delta));
			return 4 | SUPPORTED;
		}

		// EXF/DPF: J = CIR(27:5) start bit, K = CIR(22:5) = 32 - width,
		// bit 8 sign-extends (EXF), bit 9 indexes (ERS sheet 73).
		if (op2 == FMT6_EXF || op2 == FMT6_DPF)
		{
			const u8 j = d & 0x1f;
			const u8 k = (d >> 5) & 0x1f;
			util::stream_format(stream, "%-8sstart=%d,width=%d",
					e.mnemonic, j, 32 - k);
			if (op2 == FMT6_EXF && BIT(d, 23))
				stream << ",S";
			if (BIT(d, 22))
				stream << ",X";
			return 4 | SUPPORTED;
		}

		const bool ind = BIT(d, 23), idx = BIT(d, 22);
		const u8 base = (d >> 19) & 7;
		const u32 off = d & 0x7ffff;

		util::stream_format(stream, "%-8s", e.mnemonic);
		switch (e.kind)
		{
		case OK_MEM_BYTE:
		case OK_MEM_WORD:
			format_mem(stream, base, off, ind, idx);
			break;
		case OK_IMM:
			// LDI/LNI immediates are CIR(8:24) -- the full 24 bits after
			// the opcode (ERS sheet 62), not the 19-bit offset field.
			util::stream_format(stream, "#%d", d & 0xffffff);
			break;
		case OK_STT4:
			util::stream_format(stream, "STT[%d]", off / 4);
			break;
		case OK_LEVEL:
			util::stream_format(stream, "lev=%d,%d", base, off);
			break;
		default:
			util::stream_format(stream, "%d", off);
			break;
		}
		return 4 | ((op2 == FMT6_PCL) ? STEP_OVER : 0) | SUPPORTED;
	}

	//-----------------------------------------------------------------
	// Format 5: 8-bit operand. No index or indirect bit.
	//-----------------------------------------------------------------
	if (op1 == 1)
	{
		const u8 op2 = (w >> 8) & 0x1f;
		const u8 operand = w & 0xff;
		const op &e = s_fmt5[op2];

		if (e.mnemonic == nullptr)
		{
			util::stream_format(stream, "%-8s0x%04x", ".half", w);
			return 2 | SUPPORTED;
		}

		util::stream_format(stream, "%-8s", e.mnemonic);
		switch (e.kind)
		{
		case OK_STT:    util::stream_format(stream, "STT[%d]", operand); break;
		case OK_IO:     util::stream_format(stream, "io=0x%02x", operand); break;
		case OK_NFIELD: util::stream_format(stream, "N=%d", operand); break;
		case OK_LEVEL:  util::stream_format(stream, "lev=%d,%d",
								operand >> 5, operand & 0x1f); break;
		default:        util::stream_format(stream, "#%d", operand); break;
		}

		u32 flags = 0;
		if (op2 == FMT5_PCLS)
			flags = STEP_OVER;
		else if (op2 == FMT5_EXIT || op2 == FMT5_SXIT)
			flags = STEP_OUT;
		return 2 | flags | SUPPORTED;
	}

	//-----------------------------------------------------------------
	// Formats 2, 3 and 4: OP1 == 0. In all three, bit 3 is an indirect
	// bit, an index bit, or an instruction-specific indicator depending
	// on the instruction (ERS sheet 54).
	//-----------------------------------------------------------------
	if (op1 == 0)
	{
		const bool b3 = BIT(w, 12);
		const u8 op2 = (w >> 6) & 0x3f;
		const u8 operand = w & 0x3f;

		if (op2 == 0 || op2 == 1)
		{
			const op &e = (op2 == 0) ? s_fmt3[operand] : s_fmt4[operand];
			if (e.mnemonic == nullptr)
			{
				util::stream_format(stream, "%-8s0x%04x", ".half", w);
				return 2 | SUPPORTED;
			}
			stream << e.mnemonic;
			if (b3)
				stream << ",B";
			const u32 flags = (op2 == 1 && operand == FMT4_IXIT) ? STEP_OUT : 0;
			return 2 | flags | SUPPORTED;
		}

		const op &e = s_fmt2[op2];
		if (e.mnemonic == nullptr)
		{
			util::stream_format(stream, "%-8s0x%04x", ".half", w);
			return 2 | SUPPORTED;
		}

		if (e.kind == OK_BRANCH_HW)
		{
			// Backward variants have bit 2 of the opcode set: 044/045,
			// 064..067, 074..077 (Appendix B).
			const s32 delta = (BIT(op2, 2) ? -s32(operand) : s32(operand)) * 2;
			util::stream_format(stream, "%-8s0x%08x", e.mnemonic,
					u32(pc + delta));
			if (b3)
				stream << ",I";
			return 2 | SUPPORTED;
		}

		util::stream_format(stream, "%-8s", e.mnemonic);
		if (e.kind == OK_SHIFT)
		{
			util::stream_format(stream, "%d", operand);
			if (b3)
				stream << ",X";
		}
		else
		{
			util::stream_format(stream, "%d", operand);
			if (b3)
				stream << ",B";
		}
		return 2 | SUPPORTED;
	}

	//-----------------------------------------------------------------
	// Format 1: OP1 == 2..6.
	//-----------------------------------------------------------------
	const op &e = s_fmt1[op1];
	if (e.mnemonic == nullptr)
	{
		util::stream_format(stream, "%-8s0x%04x", ".half", w);
		return 2 | SUPPORTED;
	}

	// BCCS: CCF = CIR(20:3) = instruction bits 4..6; sign = CIR(23:1) =
	// bit 7, 0 is + and 1 is -; offset counts halfwords (ERS sheet 90).
	if (e.kind == OK_BRANCH_HW)
	{
		const u8 ccf = (w >> 9) & 7;
		const u8 off = w & 0xff;
		const s32 delta = (BIT(w, 8) ? -s32(off) : s32(off)) * 2;
		format_cc(stream, e.mnemonic, ccf);
		util::stream_format(stream, "0x%08x", u32(pc + delta));
		if (BIT(w, 12))
			stream << ",I";
		return 2 | SUPPORTED;
	}

	const bool ind = BIT(w, 12), idx = BIT(w, 11);
	const u8 base = (w >> 8) & 7;
	const u32 off = u32(w & 0xff) * ((e.kind == OK_MEM_WORD) ? 4 : 1);

	util::stream_format(stream, "%-8s", e.mnemonic);
	format_mem(stream, base, off, ind, idx);
	return 2 | SUPPORTED;
}
