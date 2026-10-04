// license:GPL-2.0+
// copyright-holders:Joel Heikkila
// Standalone driver for focusdasm.cpp, used to differentially test the C++
// disassembler against tools/focusdis.py.
//
//   dasmtest <image> <start> <end>
//
// Emits one line per instruction: "%08x  %s", matching focusdis.py's
// address and text columns so the two can be diffed directly.

#include "emu.h"
#include "../cpu/focus/focusdasm.h"

#include <fstream>
#include <iostream>
#include <sstream>

int main(int argc, char **argv)
{
	if (argc != 4)
	{
		std::cerr << "usage: dasmtest <image> <start> <end>\n";
		return 2;
	}

	std::ifstream f(argv[1], std::ios::binary);
	if (!f)
	{
		std::cerr << "cannot open " << argv[1] << "\n";
		return 2;
	}
	std::vector<u8> data((std::istreambuf_iterator<char>(f)),
			std::istreambuf_iterator<char>());

	const offs_t start = offs_t(strtoul(argv[2], nullptr, 0));
	const offs_t end = offs_t(strtoul(argv[3], nullptr, 0));

	focus_disassembler dasm;
	util::disasm_interface::data_buffer buf(data);

	for (offs_t pc = start; pc < end && pc < data.size(); )
	{
		std::ostringstream ss;
		const offs_t r = dasm.disassemble(ss, pc, buf, buf);
		const offs_t len = r & util::disasm_interface::LENGTHMASK;
		printf("%08x  %s\n", pc, ss.str().c_str());
		pc += len ? len : 2;
	}
	return 0;
}
