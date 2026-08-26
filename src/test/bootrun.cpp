// license:BSD-3-Clause
// Run a boot image through the loader handoff the SYSTEM POWERUP module
// actually expects, as reverse-engineered from the module itself (see
// docs/IMPLEMENTATION.md).
//
// The contract, derived by reading the module end to end:
//
//   * DB maps physical memory from absolute 0. The module builds the
//     dedicated tables there: location 0 (System CST pointer, set to 1152 =
//     octal 2200), the CST/DST at 1152, the DRT, the per-CPU blocks.
//   * The word at Q+52 is the byte offset in DB where the loader placed the
//     boot image -- the module zero-fills [0, that offset) first, then walks
//     the segment chain there, verifying each segment's byte-sum checksum
//     and terminating on a negative word (the image ends with ffffffff).
//   * A halfword table starting at the halfword ending at Q-3 inventories
//     the MPB: bit 0 (ERS numbering) set marks a valid entry, bits 1-3 are
//     the channel, the low byte is the type -- 4 = CPU, 2 = IOP, 1 = other.
//     A zero halfword terminates. The module assigns each type-2 entry its
//     channel with WMCN, resets it with RIOP/RIOB, and scans its devices.
//   * Q+64 holds 2060 + G for a region the module indexes at DB[G];
//     Q+65..Q+84 is a 20-byte record copied to DB+172 (boot device record);
//     Q+88 and Q+92 are saved for later boot stages.
//   * The stack top holds -2 for a cold start; MASK bits 3-5 must identify
//     the executing CPU's channel (1).
//
//   bootrun <image.bin> [--p64 N] [--steps N] [--load ADDR] [--trace]

#include "../cpu/focus/focusexec.h"
#include "../iop/focusiop.h"
#include "../iop/focusscm.h"
#include "../iop/focuscards.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
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
	void write16(u32 a, u16 v)
	{
		if (a + 1 >= m_d.size()) return;
		m_d[a] = u8(v >> 8); m_d[a+1] = u8(v);
	}
	void load(u32 at, const std::vector<u8> &img)
	{
		for (size_t i = 0; i < img.size() && at + i < m_d.size(); i++)
			m_d[at + i] = img[i];
	}
	std::vector<u8> m_d;
};

struct run_host : public iop_core::host
{
	flat_memory &mem;
	bool int_line = false;
	explicit run_host(flat_memory &m) : mem(m) { }
	memory_interface &iop_memory() override { return mem; }
	void iop_interrupt(bool state) override { int_line = state; }
};


// Wraps the IOP to log the I/O the software attempts, refused or not.
struct logging_io : public io_interface
{
	io_interface &inner;
	bool log = false;
	unsigned logged = 0;
	explicit logging_io(io_interface &i) : inner(i) { }
	void note(const char *kind, const target &t, u32 data, bool ok)
	{
		if (!log || logged >= 40000) return;
		logged++;
		std::printf("io: %-7s op=%02x ch=%u pa=%u ic=%u data=%08x %s\n",
				kind, t.opcode, t.channel, t.pa, t.ic, data,
				ok ? "" : "REFUSED");
	}
	bool io_write(const target &t, u32 data) override
	{
		const bool ok = inner.io_write(t, data);
		note("write", t, data, ok);
		return ok;
	}
	bool io_read(const target &t, u32 &data) override
	{
		const bool ok = inner.io_read(t, data);
		note("read", t, data, ok);
		return ok;
	}
	bool io_control(const target &t) override
	{
		const bool ok = inner.io_control(t);
		note("control", t, 0, ok);
		return ok;
	}
};

const char *trap_name(exec_trap t)
{
	switch (t)
	{
	case exec_trap::none:               return "step budget exhausted";
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
	if (knob::handle_help())
		return 0;
	const char *path = nullptr;
	u32 p64 = 2060 + 8000;         // Q+64: 2060 + G, G unknown; a knob
	u32 load = 0x100000;           // where the "loader" places the image
	unsigned long long budget = 50000000;
	bool trace = false;
	s32 catch_off = -1;            // dump history when P reaches this offset
	const char *dump_path = nullptr;
	u32 dump_lo = 0, dump_hi = 0;  // memory range to dump afterwards
	bool far_trace = false;        // log far control transfers
	u32 watch = 0xffffffff;        // print P when this word changes
	u32 boot_ret = 0x5c84;         // (Q-2): where the final IXIT returns
	bool io_trace = false;         // log every I/O the software attempts
	const char *type_text = nullptr;  // keystrokes to feed the console
	unsigned long long trace_after = ~0ull;  // start tracing at this step
	unsigned probe_pa = 99;        // attach a probe card at this select code
	u32 probe_id = 8;              // the identity it answers
	bool probe_log = false;
	u32 cpu_date = 0x19820319;     // what RCS location 6 reports
	// What the console at select code 6 answers to RIF3. The powerup
	// scan registers a card only when this reads zero, and resets an
	// identity of 9 to get there (bootrun.cpp console_card). At the
	// default 4 nothing is registered, the device table stays empty and
	// the machine idles in the dispatcher. At 9 the console registers
	// and the driver drives it for real -- which currently ends in a
	// repeating instruction-sequencing trap out of an SXIT, because the
	// card model does not implement the completion queues the driver
	// then expects (absolute 6096; see the I/O protocol notes).
	u32 console_id = 4;   // 9 in MAME; bootrun still lacks the IOP-side
	                      // plumbing the registered console then needs
	bool kbd_log = false;
	bool sem_log = false;
	// --poke STEP ADDR VAL: write a word mid-run, for probing what a
	// missing producer would have delivered (e.g. faking a semaphore V).
	unsigned long long poke_step = 0;
	u32 poke_addr = 0, poke_val = 0;
	// --tick N: raise MSG bit 13 -- the I/O ERS's "13 for timer
	// interrupt" -- every N steps, once the boot has settled.
	unsigned long long tick_period = 0;
	// --rtcsteps N: how many instructions stand for one second of the
	// real-time clock. This harness has no clock of its own, so the
	// heartbeat the boot's clock-sync waits on is counted in steps; the
	// MAME driver runs the same tick off an emulated-time timer instead.
	unsigned long long rtc_steps = 2000000;

	for (int i = 1; i < argc; i++)
	{
		if (!std::strcmp(argv[i], "--p64") && i + 1 < argc)
			p64 = u32(strtoul(argv[++i], nullptr, 0));
		else if (!std::strcmp(argv[i], "--steps") && i + 1 < argc)
			budget = strtoull(argv[++i], nullptr, 0);
		else if (!std::strcmp(argv[i], "--load") && i + 1 < argc)
			load = u32(strtoul(argv[++i], nullptr, 0));
		else if (!std::strcmp(argv[i], "--trace"))
			trace = true;
		else if (!std::strcmp(argv[i], "--catch") && i + 1 < argc)
			catch_off = s32(strtoul(argv[++i], nullptr, 0));  // absolute
		else if (!std::strcmp(argv[i], "--dump") && i + 3 < argc)
		{
			dump_path = argv[++i];
			dump_lo = u32(strtoul(argv[++i], nullptr, 0));
			dump_hi = u32(strtoul(argv[++i], nullptr, 0));
		}
		else if (!std::strcmp(argv[i], "--far"))
			far_trace = true;
		else if (!std::strcmp(argv[i], "--watch") && i + 1 < argc)
			watch = u32(strtoul(argv[++i], nullptr, 0));
		else if (!std::strcmp(argv[i], "--ret") && i + 1 < argc)
			boot_ret = u32(strtoul(argv[++i], nullptr, 0));
		else if (!std::strcmp(argv[i], "--io"))
			io_trace = true;
		else if (!std::strcmp(argv[i], "--type") && i + 1 < argc)
			type_text = argv[++i];
		else if (!std::strcmp(argv[i], "--traceafter") && i + 1 < argc)
			trace_after = strtoull(argv[++i], nullptr, 0);
		else if (!std::strcmp(argv[i], "--probe") && i + 2 < argc)
		{
			probe_pa = unsigned(strtoul(argv[++i], nullptr, 0));
			probe_id = u32(strtoul(argv[++i], nullptr, 0));
		}
		else if (!std::strcmp(argv[i], "--probelog"))
			probe_log = true;
		else if (!std::strcmp(argv[i], "--cpudate") && i + 1 < argc)
			cpu_date = u32(strtoul(argv[++i], nullptr, 16));
		else if (!std::strcmp(argv[i], "--consoleid") && i + 1 < argc)
			console_id = u32(strtoul(argv[++i], nullptr, 0));
		else if (!std::strcmp(argv[i], "--kbdlog"))
			kbd_log = true;
		else if (!std::strcmp(argv[i], "--semlog"))
			sem_log = true;
		else if (!std::strcmp(argv[i], "--resp9") && i + 1 < argc)
			++i;                       // retired: FR9 is the RTC now
		else if (!std::strcmp(argv[i], "--tick") && i + 1 < argc)
			tick_period = strtoull(argv[++i], nullptr, 0);
		else if (!std::strcmp(argv[i], "--rtcsteps") && i + 1 < argc)
			rtc_steps = strtoull(argv[++i], nullptr, 0);
		else if (!std::strcmp(argv[i], "--poke") && i + 3 < argc)
		{
			poke_step = strtoull(argv[++i], nullptr, 0);
			poke_addr = u32(strtoul(argv[++i], nullptr, 0));
			poke_val = u32(strtoul(argv[++i], nullptr, 0));
		}
		else
			path = argv[i];
	}
	if (!path)
	{
		std::fprintf(stderr, "usage: bootrun <image.bin> [--p64 N] "
				"[--steps N] [--load ADDR] [--trace] "
				"[--rtcsteps N]\n");
		return 2;
	}

	std::FILE *f = std::fopen(path, "rb");
	if (!f) { std::perror(path); return 2; }
	std::vector<u8> img;
	{
		u8 buf[65536];
		size_t n;
		while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
			img.insert(img.end(), buf, buf + n);
	}
	std::fclose(f);

	flat_memory mem(0x1000000);    // 16 MB of physical memory
	mem.load(load, img);

	run_host host(mem);
	iop_core iop(host, 2);         // the IOP announced in the inventory
	iop.reset();
	scm_device scm;
	iop.attach(scm_device::SELECT_CODE, &scm);
	probe_card probe;
	console_card console, console6;
	console.iop = &iop;
	// Select code 0 is where the error reporter writes rather than a CIO
	// interface, so it keeps the identity the scan does not know and
	// stays out of the device bitmap whatever the console is set to.
	console.id = 4;
	console6.id = console_id;
	console6.log_fr.name("sc", 6);
	console6.log_fr.enable(kbd_log);
	iop.attach(0, &console);       // select code 0: the error reporter's
	console6.iop = &iop;
	console6.pa = 6;
	iop.attach(6, &console6);      // select code 6: the console driver's
	if (probe_pa < 8)
	{
		probe.pa = probe_pa;
		probe.id = probe_id;
		probe.log.name("probe", probe_pa);
		probe.log.enable(probe_log);
		iop.attach(probe_pa, &probe);
	}

	cpu_core cpu(mem);
	cpu.set_control_store_date(cpu_date);
	logging_io iolog(iop);
	iolog.log = io_trace;
	cpu.set_io(&iolog);
	if (sem_log)
		cpu.sem_hook = [](bool block, u32 sem, u32 who, u32 p) {
			std::printf(block ? "[DOWN blocks: sem=%08x ctcbp=%08x p=%08x]\n"
			                  : "[UP wakes:   sem=%08x tcb=%08x p=%08x]\n",
					sem, who, p);
		};

	// Code: the powerup module executes in place within the loaded image,
	// with PB = 0. That is not a guess: the module copies its relocation
	// routine to the absolute address in its header word (0xc4) and
	// branches there with BRSI through that same word as a "local"
	// pointer, and LRAS P+8 continuations only return correctly through
	// SXIT's PB+offset if PB-relative means absolute. PL is the module's
	// own limit -- its checksum word, at the length in header word +8 --
	// which with PB = 0 also keeps the low-memory copy executable.
	//
	// Both are read out of the image: the length from header word +8, and
	// the entry as terminator + 0x1e, the terminator being the 00c4 low
	// halfword of the first 000000c4 word after the module name (its
	// position varies with the SCCS string: +0x36 for BASIC and the
	// diagnostics, +0x3e for HP-UX).
	auto beword = [&img](u32 off) {
		return (u32(img[off]) << 24) | (u32(img[off+1]) << 16)
				| (u32(img[off+2]) << 8) | img[off+3];
	};
	u32 entry_off = 0;
	for (u32 off = 0x30; off + 4 <= 0x80; off += 4)
		if (beword(off) == 0xc4) { entry_off = off + 0x20; break; }
	if (!entry_off)
	{
		std::fprintf(stderr, "no 000000c4 terminator in the header\n");
		return 2;
	}
	cpu.r.pb = 0;
	cpu.r.pl = load + beword(8) + 3;
	cpu.r.p  = load + entry_off;

	// The stack lives in high memory, clear of the region the module
	// zero-fills ([0, load)) and of the image itself. SB sits well below
	// Q because the final IXIT unwinds a marker whose delta-Q is the low
	// 31 bits of the inventory word at (Q) -- see the frame layout below.
	cpu.r.sb = 0xe00000;
	cpu.r.sl = 0xf7ffff | 3;
	cpu.r.q  = 0xf00203;
	cpu.r.s  = cpu.r.q + 0x200;

	// The global data segment is physical memory, from 0.
	cpu.r.db = 0;
	cpu.r.dl = 0xffffff | 3;
	cpu.r.dst = 1152;             // octal 2200, where the module builds it
	cpu.r.put(sf::M, 1);
	cpu.r.mask = ers32::set(0, 3, 3, 1);   // this CPU is MPB channel 1

	const u32 q = cpu.r.q;

	// --- the loader's parameter block -----------------------------------
	// Word operands address their last byte, so the word "Q+n" occupies
	// [q+n-3, q+n]. Byte operands address their first byte.

	// MPB inventory: the module reads halfword k at q-3+2k (LDH Q-3,X with
	// X stepping by 2). Bit 0 = valid, bits 1-3 = channel, low byte = type
	// (4 = CPU, 2 = IOP); a halfword that is not a known type is skipped,
	// and a zero halfword terminates.
	//
	// These same words are the IXIT frame of ERS sheet 97: the module's
	// last instruction assumes a stack marker at (Q)..(Q-3), a global
	// data segment pointer at (Q+1), the previous interrupt mask at (Q+2)
	// and a device number at (Q+3). The two roles coexist exactly one
	// way, and it pins the layout:
	//
	//   (Q)   = [CPU entry, IOP entry] -- bit 0 doubles as the marker's
	//           dispatcher flag, and the low 31 bits as its delta-Q, so
	//           the CPU entry's channel field must be zero to keep
	//           Q - delta above SB
	//   (Q+1) = 8000 0000 -- a global pointer with offset zero ("DB is
	//           current"); its top halfword is skipped by the inventory
	//           scan (type 0) and its zero low halfword terminates it
	//
	// The marker below Q returns into the dispatcher -- the trap-handler
	// segment's STT entry 7, at trap-base + 0x34 -- with a privileged
	// CSN 0 status, which is where the powerup module's own launch frame
	// at (QI) also points. --ret overrides the target.
	mem.write16(q - 3 + 0, 0x9004);   // CPU, channel 1
	mem.write16(q - 3 + 2, 0xa002);   // IOP, channel 2
	mem.write16(q - 3 + 4, 0x8000);   // (Q+1) hi: global DS pointer...
	mem.write16(q - 3 + 6, 0x0000);   // ...and the inventory terminator
	mem.write32(q - 3 - 4, 0x00020000);  // (Q-1): privileged, CSN 0
	mem.write32(q - 3 - 8, boot_ret);    // (Q-2): the dispatcher
	mem.write32(q - 3 - 12, 0);          // (Q-3): X
	mem.write32(q + 2 * 4 - 3, 0);       // (Q+2): previous mask
	mem.write32(q + 3 * 4 - 3, 0);       // (Q+3): device number

	mem.write32(q + 52 - 3, load);    // where the image was loaded
	mem.write32(q + 64 - 3, p64);     // 2060 + G
	for (u32 i = 0; i < 20; i++)      // 20-byte boot device record
		mem.m_d[q + 65 + i] = 0;
	mem.write32(q + 88 - 3, 0);
	mem.write32(q + 92 - 3, 0);

	cpu.push(u32(-2));                // cold start

	std::printf("image %s, %zu bytes at %#x; entry %#x, Q %#x, S %#x\n\n",
			path, img.size(), load, cpu.r.p, cpu.r.q, cpu.r.s);

	// --------------------------------------------------------------------
	std::vector<u32> trail(32, 0);
	size_t tri = 0;
	// Ring of recent (pc, S, TOS) for --catch.
	struct hist { u32 pc, s, tos, tos1; };
	std::vector<hist> ring(96);
	size_t ri = 0;
	std::vector<u8> visits(img.size() / 2 + 2, 0);
	unsigned distinct = 0;
	exec_trap t = exec_trap::none;
	unsigned long long steps = 0;
	u32 fault_pc = 0;
	bool nudged = false;
	u32 prev_rq = 0;               // IOP request bits, for edge detection

	for (; steps < budget; steps++)
	{
		// Traps vector through CST entry 1 once the module has built the
		// tables and pointed dedicated location 0 at them.
		if (!cpu.traps_enabled() && mem.read32(0) != 0)
			cpu.enable_traps(true);
		const u32 idx = (cpu.r.p - load) / 2;
		if (idx < visits.size() && visits[idx] < 255)
		{
			if (visits[idx] == 0)
				distinct++;
			visits[idx]++;
		}
		fault_pc = cpu.r.p;
		trail[tri++ & 31] = cpu.r.p;
		if (catch_off >= 0 && cpu.r.p == u32(catch_off))
		{
			std::printf("caught address %#x at step %llu; history:\n",
					catch_off, steps);
			std::printf("PB=%08x PL=%08x Q=%08x S=%08x SB=%08x SL=%08x "
					"X=%08x STATUS=%08x\n", cpu.r.pb, cpu.r.pl, cpu.r.q,
					cpu.r.s, cpu.r.sb, cpu.r.sl, cpu.r.x, cpu.r.status);
			std::printf("SB words 0..6: ");
			for (int k = 0; k <= 6; k++)
				std::printf("%08x ", mem.read32(cpu.r.sb + 4 * k));
			std::printf("\n");
			for (size_t i = 0; i < ring.size(); i++)
			{
				const hist &h = ring[(ri + i) % ring.size()];
				std::printf("  %6x  S=%08x (S)=%08x (S-1)=%08x\n",
						h.pc - load, h.s, h.tos, h.tos1);
			}
			return 0;
		}
		ring[ri] = { cpu.r.p, cpu.r.s, cpu.stk(0), cpu.stk(1) };
		ri = (ri + 1) % ring.size();
		if (trace || steps >= trace_after)
			std::printf("%08x cir=%08x S=%08x (S)=%08x Q=%08x X=%08x\n",
					cpu.r.p, mem.read32(cpu.r.p), cpu.r.s, cpu.stk(0),
					cpu.r.q, cpu.r.x);
		// Chain-walk top: one line per segment visited.
		if (cpu.r.p == load + 0x2d4)
			std::printf("walk: step %9llu  S=%08x  Q-88=%08x  hdr=%08x %08x\n",
					steps, cpu.r.s,
					mem.read32(cpu.r.q - 88 - 3),
					mem.read32(mem.read32(cpu.r.q - 88 - 3)),
					mem.read32(mem.read32(cpu.r.q - 88 - 3) + 4));
		// SYSTEM POWERUP's final IXIT launches the first task: with (Q)
		// zero it takes the TCB path -- IXIT case 2 -- and exits through
		// the marker the module built at the top of the task's stack
		// segment, landing at trap-segment offset 0x24 as code segment 1.
		// The word at (Q) is the loader's, and the module consumed it as
		// the MPB inventory long before this point; whatever the real
		// loader does to reconcile the two roles is still unknown, so
		// the harness clears the word when the module reaches its IXIT
		// with the handoff stack still current.
		if (!nudged && cpu.r.q == q
				&& (mem.read32(cpu.r.p & ~3u)
						>> ((cpu.r.p & 2) ? 0 : 16) & 0xffff) == 0x007c)
		{
			mem.write32(q - 3, 0);
			std::printf("nudge: (Q) cleared for the final IXIT's "
					"TCB dispatch\n");
			nudged = true;
		}
		if (tick_period && (steps % tick_period) == 0)
			cpu.raise_message(13);
		if (poke_step && steps == poke_step)
		{
			std::printf("poke: [%08x] %08x -> %08x at step %llu\n",
					poke_addr, mem.read32(poke_addr), poke_val, poke_step);
			mem.write32(poke_addr, poke_val);
			cpu.resume();
		}
		// The microcode interrupt handler runs between instructions:
		// when STATUS.I permits and an unmasked I/O message bit is up,
		// pull the first waiting device from its level's list and vector
		// through its DRT entry (ERS section 5.1.3).
		cpu.service_interrupts();
		// The machine's heartbeat: tick the interval timer and run a DMA
		// poll round every few instructions, and deliver the IOP's
		// interrupt line to the CPU between instructions. The device
		// number is the DRT address, 128*channel + 16*PA.
		if ((steps & 15) == 0)
		{
			iop.timer_tick();
			iop.poll_and_service();
			if (iop.take_timer_event())
				cpu.raise_message(13);
		}
		// Diagnostic RTC heartbeat while bootrun is phased out: the boot
		// synchronises to a seconds edge before launching BASIC.
		if (rtc_steps && steps % rtc_steps == 0)
			console6.rtc_second_tick();
		// Feed keystrokes: once the machine has idled (the dispatcher's
		// STOP with nothing ready), type the next character into the
		// console and let the attention wake it.
		if (type_text && *type_text && cpu.halted()
				&& !console6.input_ready())
		{
			console6.type(u8(*type_text++));
			std::printf("[type '%c']\n", type_text[-1] >= 32
					? type_text[-1] : '?');
		}
		// The IOP's interrupt request reaches the CPU as Message
		// register bits 16-31, one per device (ERS sheet 13). The OS
		// polls MSG through PMPB and services the device itself; there
		// is no DRT vectoring in this system, so the raise is all the
		// hardware owes it. A raised bit also wakes a STOPped CPU.
		{
			const u32 rq = iop.interrupt_request();
			const u32 fresh = rq & ~prev_rq;
			prev_rq = rq;
			// The message bit is 16 + the device's interrupt level: the
			// microcode handler maps the bit back to a level and pulls
			// the device from that level's list (ERS sheet 142).
			for (unsigned pa = 0; pa < 8; pa++)
				if (fresh & (1u << pa))
					cpu.raise_message(16 + iop.interrupt_level(pa));
			// The IOP's attention poll reaches the CPU as message 14,
			// "CSG attention requests" (I/O ERS section 2.1) -- this is
			// how a keystroke waiting on a card becomes visible to the
			// dispatcher's PMPB poll.
			if (iop.take_attention_event())
				cpu.raise_message(14);
			// A STOPped CPU resumes on a *new* request, not on the level.
			// CMD_RIRQ's word is a latch that only CIRQ clears, so testing
			// it directly undoes every STOP the moment any device has ever
			// interrupted -- which is what kept the diagnostics' idle loop
			// spinning instead of settling.
			if (cpu.halted() && (fresh != 0 || iop.take_request_event()))
				cpu.resume();
		}
		const u32 prev_cir = mem.read32(fault_pc & ~1u);
		const u32 watch_was = (watch != 0xffffffff) ? mem.read32(watch) : 0;
		t = cpu.step();
		if (watch != 0xffffffff && mem.read32(watch) != watch_was)
			std::printf("watch: step %9llu  P=%08x  [%#x] %08x -> %08x\n",
					steps, fault_pc, watch, watch_was, mem.read32(watch));
		if (far_trace)
		{
			const s32 d = s32(cpu.r.p) - s32(fault_pc);
			if (d < -0x400 || d > 0x400)
				std::printf("far: %9llu  %08x -> %08x  cir=%08x "
						"PB=%08x Q=%08x S=%08x STATUS=%08x\n",
						steps, fault_pc, cpu.r.p,
						(fault_pc & 2) ? prev_cir << 16 : prev_cir,
						cpu.r.pb, cpu.r.q, cpu.r.s, cpu.r.status);
		}
		// A halted CPU is idling for a message or interrupt. The
		// dispatcher STOPs with STATUS.I clear and an armed MASK, so a
		// wake can come from either path; keep the world turning --
		// timer ticks, typed input, attention -- and let raise_message
		// un-halt the core. Halted iterations still consume the step
		// budget, so an idle machine ends the run rather than spinning
		// the harness forever.
		if (t == exec_trap::halted)
		{
			// The for loop's own increment advances the budget; a halted
			// iteration must not double-count or the harness's modulo
			// schedules (ticks) skip.
			if (tick_period || (type_text && *type_text))
				continue;
			break;
		}
		if (t != exec_trap::none)
			break;
	}

	std::printf("%llu steps, %u distinct module addresses\n", steps, distinct);
	std::printf("stopped by %s at %08x (module offset %#x) cir=%08x\n",
			trap_name(t), fault_pc, fault_pc - load, cpu.cir());
	if (t != exec_trap::none)
	{
		std::printf("history before the stop:\n");
		for (size_t i = 0; i < ring.size(); i++)
		{
			const hist &h = ring[(ri + i) % ring.size()];
			std::printf("  %6x  S=%08x (S)=%08x (S-1)=%08x\n",
					h.pc - load, h.s, h.tos, h.tos1);
		}
	}
	if (t == exec_trap::address_error)
		std::printf("addressing trap: %s\n", addr_trap_name(cpu.last_addr_trap()));
	std::printf("stack: S=%08x (S)=%08x (S-1)=%08x (S-2)=%08x\n",
			cpu.r.s, cpu.stk(0), cpu.stk(1), cpu.stk(2));
	std::printf("trail:");
	for (size_t i = 0; i < 32; i++)
		std::printf(" %x", trail[(tri + i) & 31] - load);
	std::printf("\n\n");

	// State the module should have built, for judging progress.
	std::printf("DB[0]   (CST ptr)   = %u\n", mem.read32(0));
	std::printf("DB[4]   (shar CST)  = %u\n", mem.read32(4));
	std::printf("DB[128] (alloc ptr) = %u\n", mem.read32(128));
	std::printf("DB[136]             = %08x\n", mem.read32(136));
	std::printf("DB[140] (reloc tgt) = %u\n", mem.read32(140));
	std::printf("DB[160]             = %08x\n", mem.read32(160));
	std::printf("DB[164] (min addr)  = %u\n", mem.read32(164));
	std::printf("DB[168]             = %08x\n", mem.read32(168));
	std::printf("DB[968] (G)         = %u\n", mem.read32(968));
	std::printf("CST[17] at 1424     = %08x %08x %08x %08x\n",
			mem.read32(1424), mem.read32(1428), mem.read32(1432), mem.read32(1436));
	std::printf("Q-104 (cpu count)   = %u\n", mem.read32(q - 104));
	std::printf("Q-108 (iop count)   = %u\n", mem.read32(q - 108));
	std::printf("cpu chan bytes Q-156..: ");
	for (int i = 0; i < 8; i++) std::printf("%02x ", mem.m_d[q - 156 + i]);
	std::printf("\niop chans Q-180+4k: %08x %08x %08x\n",
			mem.read32(q - 180 + 4), mem.read32(q - 180 + 8), mem.read32(q - 180 + 12));
	std::printf("mailboxes Q-44+4k   : %08x %08x %08x\n",
			mem.read32(q - 44 + 4), mem.read32(q - 44 + 8), mem.read32(q - 44 + 12));
	std::printf("iop channel now     = %u\n", iop.channel_number());
	for (u32 seg : {0u, 1u, 2u, 4u, 6u, 17u, 49u})
		std::printf("CST[%2u] = %08x %08x %08x %08x\n", seg,
				mem.read32(1152 + 16*seg), mem.read32(1152 + 16*seg + 4),
				mem.read32(1152 + 16*seg + 8), mem.read32(1152 + 16*seg + 12));
	std::printf("PB=%08x PL=%08x P=%08x Q=%08x SB=%08x DB=%08x "
			"STATUS=%08x\n", cpu.r.pb, cpu.r.pl, cpu.r.p, cpu.r.q,
			cpu.r.sb, cpu.r.db, cpu.r.status);
	for (unsigned i = 0; i < cpu.m_trap_log_len; i++)
		std::printf("trap %-2u: cause %02o at ret %08x param %08x PB %08x\n",
				i, cpu.m_trap_log[i][0], cpu.m_trap_log[i][1],
				cpu.m_trap_log[i][2], cpu.m_trap_log[i][3]);

	if (dump_path)
	{
		std::FILE *df = std::fopen(dump_path, "wb");
		if (df)
		{
			for (u32 a = dump_lo; a < dump_hi && a < mem.m_d.size(); a++)
				std::fputc(mem.m_d[a], df);
			std::fclose(df);
			std::printf("dumped [%#x,%#x) to %s\n", dump_lo, dump_hi, dump_path);
		}
	}

	return 0;
}
