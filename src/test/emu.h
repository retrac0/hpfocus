// license:GPL-2.0+
// copyright-holders:Joel Heikkila
// Minimal stand-in for MAME's emu.h, sufficient to build and exercise
// focusdasm.cpp outside the MAME tree.
//
// This exists purely so the disassembler can be differentially tested against
// tools/focusdis.py before it goes anywhere near a MAME build. It is not a
// MAME compatibility layer and models only what focusdasm.cpp touches.

#ifndef HPFOCUS_TEST_EMU_H
#define HPFOCUS_TEST_EMU_H

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <ostream>
#include <string>
#include <vector>

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using s32 = std::int32_t;
using offs_t = std::uint32_t;

template <typename T> constexpr unsigned BIT(T x, unsigned n)
{
	return (x >> n) & 1U;
}

namespace util {

inline std::string string_format(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	return std::string(buf);
}

inline void stream_format(std::ostream &os, const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	os << buf;
}

// MAME's stream_format is variadic-template and accepts std::string for %s.
// The C varargs version above cannot, so provide the one overload we need.
inline void stream_format(std::ostream &os, const char *fmt,
		const std::string &s)
{
	char buf[512];
	snprintf(buf, sizeof(buf), fmt, s.c_str());
	os << buf;
}

class disasm_interface
{
public:
	// FOCUS is big-endian.
	class data_buffer
	{
	public:
		explicit data_buffer(const std::vector<u8> &d) : m_d(d) { }

		u16 r16(offs_t pc) const
		{
			return (pc + 1 < m_d.size())
					? u16((m_d[pc] << 8) | m_d[pc + 1]) : 0;
		}

		u32 r32(offs_t pc) const
		{
			return (pc + 3 < m_d.size())
					? (u32(m_d[pc]) << 24) | (u32(m_d[pc + 1]) << 16)
					  | (u32(m_d[pc + 2]) << 8) | u32(m_d[pc + 3]) : 0;
		}

	private:
		const std::vector<u8> &m_d;
	};

	static constexpr u32 SUPPORTED = 0x80000000;
	static constexpr u32 STEP_OVER = 0x40000000;
	static constexpr u32 STEP_OUT  = 0x20000000;
	static constexpr u32 LENGTHMASK = 0x0000ffff;

	virtual ~disasm_interface() = default;
	virtual u32 opcode_alignment() const = 0;
	virtual offs_t disassemble(std::ostream &stream, offs_t pc,
			const data_buffer &opcodes, const data_buffer &params) = 0;
};

} // namespace util

#endif // HPFOCUS_TEST_EMU_H
