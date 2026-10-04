// license:GPL-2.0+
// copyright-holders:Joel Heikkila
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

// Seed value for the loader's parameter block; overridden by --run's 7th arg.
focus::u32 param_q64 = 0;

// The IOP's MPB channel number. The CPU's own channel is 1 (see MASK below),
// and two MPB devices cannot share one, so the IOP's is a separate unknown --
// WMCN exists precisely to assign it, which implies a power-up default the
// ERS does not state. FOCUS_IOP_CHANNEL sweeps it.
unsigned iop_channel = 1;

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
	focus::trap at = focus::trap::none;   // which addressing trap, if any
	u32 tos = 0, tos1 = 0, tos2 = 0;      // the stack the faulting op saw
};

// The memory image is reused across trials -- allocating and zeroing several
// megabytes per candidate dominated the sweep otherwise. Only the regions a
// trial can dirty are reset.
// When `tos` is supplied the loader's parameter is pushed before entry, and
// `trail` collects the last addresses executed.
result run_from(flat_memory &mem, const std::vector<u8> &img, u32 base,
		u32 pb, u32 entry, unsigned budget, std::vector<u16> &visits,
		bool push_tos = false, u32 tos = 0, std::vector<u32> *trail = nullptr)
{
	std::fill(mem.m_d.begin() + 0x400000, mem.m_d.begin() + 0x520000, 0);
	mem.load(base, img);

	probe_host host(mem);
	iop_core iop(host, u8(iop_channel));
	iop.reset();
	scm_device scm;
	iop.attach(scm_device::SELECT_CODE, &scm);

	cpu_core cpu(mem);
	cpu.set_io(&iop);

	cpu.r.pb = pb;
	cpu.r.pl = base + u32(img.size() & ~3u) - 4;
	cpu.r.p  = entry;
	cpu.r.sb = 0x400000;
	cpu.r.sl = 0x47ffff | 3;
	cpu.r.q  = 0x400203;   // room below Q for the frame the loader leaves
	cpu.r.s  = cpu.r.q + 0x200;
	cpu.r.db = 0x500000;
	// The kernel addresses global offsets past 0x11900 within its first
	// hundred instructions, so a 64 KB global segment is too small to be a
	// fair test of an entry point. Give it the rest of modelled memory.
	cpu.r.dl = 0x5fffff | 3;
	cpu.r.dst = 0x600000;
	cpu.r.put(sf::M, 1);
	// MASK bits 3-5 carry the MPB channel number, which PMPB pushes. The
	// boot code halts any CPU that is not channel 1, so a probe leaving
	// this zero makes every path look like a dead end.
	cpu.r.mask = ers32::set(0, 3, 3, 1);
	// The module reads a parameter block the loader leaves in the stack
	// above Q -- offsets +64, +88 and +92 are copied down to Q-144, Q-120
	// and Q-136 before use. Seed them so the code has something to work
	// with; the values are a hypothesis under test, not documented.
	mem.write32(cpu.r.q + 64 - 3, param_q64);
	mem.write32(cpu.r.q + 88 - 3, param_q64);
	mem.write32(cpu.r.q + 92 - 3, param_q64);
	if (push_tos)
		cpu.push(tos);

	std::fill(visits.begin(), visits.end(), 0);
	result r;
	unsigned distinct = 0;
	for (r.steps = 0; r.steps < budget; r.steps++)
	{
		const u32 idx = (cpu.r.p - base) / 2;
		if (idx < visits.size())
		{
			if (visits[idx] == 0)
				distinct++;
			if (visits[idx] < 0xffff)
				visits[idx]++;
			// A tight self-loop means it is spinning, not progressing.
			// The counter is 16 bits because this test needs it to be: as
			// a saturating byte it stopped at 255 and the comparison below
			// could never be true, so a spinning probe ran its whole
			// budget instead of stopping.
			if (visits[idx] > 20000)
				break;
		}
		r.fault_pc = cpu.r.p;
		if (trail)
		{
			trail->push_back(cpu.r.p);
			if (trail->size() > 24)
				trail->erase(trail->begin());
		}
		r.tos = cpu.stk(0); r.tos1 = cpu.stk(1); r.tos2 = cpu.stk(2);
		const exec_trap t = cpu.step();
		if (t != exec_trap::none)
		{
			r.t = t;
			r.cir = cpu.cir();
			r.at = cpu.last_addr_trap();
			break;
		}
	}
	r.distinct = distinct;
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

// "address" on its own does not say what the code was reaching for. The
// specific addressing trap does, and it is the difference between a wrong
// entry point and a missing piece of the machine.
const char *addr_trap_name(focus::trap t)
{
	switch (t)
	{
	case focus::trap::none:                     return "-";
	case focus::trap::bounds_violation:         return "outside PB..PL / SB..S / DB..DL";
	case focus::trap::eds_pointer_violation:    return "external segment number zero";
	case focus::trap::dst_violation:            return "segment beyond the DST";
	case focus::trap::eds_bounds_violation:     return "offset outside the segment";
	case focus::trap::data_seg_write_violation: return "store to a read-only segment";
	case focus::trap::priv_mode_data_violation: return "unprivileged access to an M segment";
	case focus::trap::absent_segment:           return "absent segment";
	case focus::trap::absent_page_table:        return "absent page table";
	case focus::trap::absent_page:              return "absent page";
	case focus::trap::page_table_violation:     return "page beyond the page table";
	case focus::trap::unexpected_eds_type:      return "unexpected segment type";
	}
	return "?";
}

} // anonymous namespace


int main(int argc, char **argv)
{
	if (focus::knob::handle_help())
		return 0;
	iop_channel = unsigned(knob::number("FOCUS_IOP_CHANNEL", iop_channel)) & 7;
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
	// --run PB ENTRY TOS: one verbose trial instead of a sweep.
	if (argc >= 7 && std::strcmp(argv[3], "--run") == 0)
	{
		const u32 pb = u32(strtoul(argv[4], nullptr, 0));
		const u32 entry = u32(strtoul(argv[5], nullptr, 0));
		const u32 tos = u32(strtoul(argv[6], nullptr, 0));
		if (argc >= 8)
			param_q64 = u32(strtoul(argv[7], nullptr, 0));
		std::vector<u16> visits(img.size() / 2 + 2, 0);
		std::vector<u32> trail;
		flat_memory mem(0x600000);
		const result r = run_from(mem, img, base, base + pb, base + entry,
				20000000, visits, true, tos, &trail);
		std::printf("PB %#x  entry %#x  TOS %d\n", pb, entry, int(tos));
		std::printf("  %u steps, %u distinct, stopped by %s at %08x cir=%08x\n",
				r.steps, r.distinct, trap_name(r.t), r.fault_pc, r.cir);
		if (r.t == exec_trap::address_error)
			std::printf("  addressing trap: %s\n", addr_trap_name(r.at));
		std::printf("  stack at the fault: (S)=%08x (S-1)=%08x (S-2)=%08x\n",
				r.tos, r.tos1, r.tos2);
		std::printf("  last addresses:");
		for (u32 a : trail)
			std::printf(" %x", a - base);
		std::printf("\n");
		return 0;
	}

	std::printf("sweeping PB x entry over the first 1 KB...\n\n");
	std::printf("%8s %8s %8s %8s  %-14s %s\n",
			"PB", "entry", "steps", "distinct", "stopped by", "at");
	std::printf("%s\n", std::string(70, '-').c_str());

	std::vector<u16> visits(img.size() / 2 + 2, 0);
	flat_memory mem(0x600000);
	struct cand { unsigned distinct; u32 pb, entry; };
	std::vector<cand> best;

	for (u32 pboff = 0; pboff < 0x400; pboff += 4)
	{
		for (u32 eoff = pboff; eoff < pboff + 0x600 && eoff + 2 <= img.size();
				eoff += 2)
		{
			const result r = run_from(mem, img, base, base + pboff,
					base + eoff, 4000, visits);
			if (r.distinct >= 60)
				best.push_back({r.distinct, pboff, eoff});
		}
	}

	std::sort(best.begin(), best.end(), [](const cand &a, const cand &b)
			{ return a.distinct > b.distinct; });

	for (size_t i = 0; i < best.size() && i < 15; i++)
	{
		const result r = run_from(mem, img, base, base + best[i].pb,
				base + best[i].entry, 4000, visits);
		std::printf("%8x %8x %8u %8u  %-14s %08x cir=%08x\n",
				best[i].pb, best[i].entry, r.steps, r.distinct,
				trap_name(r.t), r.fault_pc, r.cir);
	}
	if (best.empty())
		std::printf("  no entry point ran for 40 instructions\n");

	return 0;
}
