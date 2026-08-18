// license:BSD-3-Clause
#include <algorithm>
#include <string>
// Sweep candidate entry points into a SYSTEM POWERUP module and report how
// far each one runs before faulting.
//
// The loader ROM is not dumped, so where it transfers control is unknown. The
// module header carries a signature and a length but no entry vector, which
// suggests a fixed offset. Rather than guess, run every plausible offset and
// see which behaves like real code: a wrong entry lands in data and dies
// almost immediately, while the right one should run.
//
//   bootprobe <module.bin> [load_address]

#include "../cpu/focus/focusexec.h"
#include "../iop/focusiop.h"
#include "../iop/focusscm.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <vector>

using namespace focus;

namespace {

class flat_memory : public memory_interface
{
public:
	explicit flat_memory(u32 size) : m_d(size, 0) { }

	u32 read32(u32 a) override
	{
		a &= ~3u;
		return (a + 3 < m_d.size())
				? (u32(m_d[a]) << 24) | (u32(m_d[a+1]) << 16)
				  | (u32(m_d[a+2]) << 8) | m_d[a+3] : 0;
	}
	void write32(u32 a, u32 v) override
	{
		a &= ~3u;
		if (a + 3 >= m_d.size()) return;
		m_d[a] = u8(v >> 24); m_d[a+1] = u8(v >> 16);
		m_d[a+2] = u8(v >> 8); m_d[a+3] = u8(v);
	}
	void load(u32 at, const std::vector<u8> &img)
	{
		for (size_t i = 0; i < img.size() && at + i < m_d.size(); i++)
			m_d[at + i] = img[i];
	}
	std::vector<u8> m_d;
};

struct probe_host : public iop_core::host
{
	flat_memory &mem;
	explicit probe_host(flat_memory &m) : mem(m) { }
	memory_interface &iop_memory() override { return mem; }
	void iop_interrupt(bool) override { }
};

struct result
{
	unsigned steps = 0;
	exec_trap t = exec_trap::none;
	u32 fault_pc = 0;
	u32 cir = 0;
	unsigned distinct = 0;      // distinct addresses visited
};

result run_from(const std::vector<u8> &img, u32 base, u32 entry,
		u32 s_above_q, unsigned budget)
{
	flat_memory mem(16u << 20);
	mem.load(base, img);

	probe_host host(mem);
	iop_core iop(host, 1);
	iop.reset();
	scm_device scm;
	iop.attach(scm_device::SELECT_CODE, &scm);

	cpu_core cpu(mem);
	cpu.set_io(&iop);

	// `entry` doubles as the PB offset: the hypothesis under test is that
	// the loader sets PB past the module preamble and enters at PB+0.
	cpu.r.pb = entry;
	cpu.r.pl = base + u32(img.size() & ~3u) - 4;
	cpu.r.p  = entry;
	cpu.r.sb = 0x400000;
	cpu.r.sl = 0x47ffff | 3;
	cpu.r.q  = 0x400203;   // room below Q for the frame the loader leaves
	cpu.r.s  = cpu.r.q + s_above_q;
	cpu.r.db = 0x500000;
	cpu.r.dl = 0x50ffff | 3;
	cpu.r.dst = 0x600000;
	cpu.r.put(sf::M, 1);

	std::map<u32, unsigned> seen;
	result r;
	for (r.steps = 0; r.steps < budget; r.steps++)
	{
		seen[cpu.r.p]++;
		// A tight self-loop means it is spinning, not progressing.
		if (seen[cpu.r.p] > 200)
			break;
		r.fault_pc = cpu.r.p;
		const exec_trap t = cpu.step();
		if (t != exec_trap::none)
		{
			r.t = t;
			r.cir = cpu.cir();
			break;
		}
	}
	r.distinct = unsigned(seen.size());
	return r;
}

const char *trap_name(exec_trap t)
{
	switch (t)
	{
	case exec_trap::none:               return "-";
	case exec_trap::unimplemented:      return "unimplemented";
	case exec_trap::bounds_violation:   return "bounds";
	case exec_trap::code_bounds:        return "code bounds";
	case exec_trap::divide_by_zero:     return "div0";
	case exec_trap::address_error:      return "address";
	case exec_trap::stack_overflow:     return "stack";
	case exec_trap::pointer_conversion: return "ptr conv";
	case exec_trap::trapped:            return "trapped";
	case exec_trap::halted:             return "halted";
	}
	return "?";
}

} // anonymous namespace


int main(int argc, char **argv)
{
	if (argc < 2)
	{
		std::fprintf(stderr, "usage: bootprobe <module.bin> [load_address]\n");
		return 2;
	}
	const u32 base = (argc > 2) ? u32(strtoul(argv[2], nullptr, 0)) : 0x10000;

	std::FILE *f = std::fopen(argv[1], "rb");
	if (!f) { std::perror(argv[1]); return 2; }
	std::vector<u8> img;
	{
		u8 buf[4096];
		size_t n;
		while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
			img.insert(img.end(), buf, buf + n);
	}
	std::fclose(f);

	std::printf("module %s, %zu bytes, loaded at %#x\n\n", argv[1], img.size(), base);
	std::printf("%8s %8s %8s  %-14s %s\n",
			"entry", "steps", "distinct", "stopped by", "at");
	std::printf("%s\n", std::string(60, '-').c_str());

	// Sweep every halfword in the first 1 KB, which is where a fixed entry
	// would have to live, and report the ones that get anywhere.
	// Rank by how many *distinct* addresses were reached, not by step count:
	// a wrong entry usually finds a tight loop and racks up steps without
	// going anywhere, while real code sweeps forward through the module.
	std::vector<std::pair<unsigned, u32>> best;
	for (u32 off = 0; off < 0x400 && off + 2 <= img.size(); off += 2)
	{
		const result r = run_from(img, base, base + off, 0x200, 20000);
		if (r.distinct >= 30)
			best.push_back({r.distinct, off});
	}

	std::sort(best.rbegin(), best.rend());
	for (size_t i = 0; i < best.size() && i < 12; i++)
	{
		const u32 off = best[i].second;
		const result r = run_from(img, base, base + off, 0x200, 20000);
		std::printf("%8x %8u %8u  %-14s %08x cir=%08x\n",
				off, r.steps, r.distinct, trap_name(r.t), r.fault_pc, r.cir);
	}
	if (best.empty())
		std::printf("  no entry point ran for 40 instructions\n");

	return 0;
}
