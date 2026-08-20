// license:BSD-3-Clause
// copyright-holders:
/***************************************************************************

    HP 9000 Series 500

    The first commercial 32-bit workstations, built on the HP FOCUS chip set
    (1982). Four models share one processor stack and differ in packaging:

      520 (HP 9020)  integrated workstation, built-in display and keyboard.
                     Boots HP BASIC or HP-UX.
      530 (HP 9030)  19 inch rack mount, HP-UX only.
      540 (HP 9040)  free-standing cabinet, HP-UX only.
      550            industrial system, 1984, replaced the 530 and 540.

    The card stack holds twelve slots on the Memory Processor Bus: slot 1 a
    CPU, slot 2 an I/O Processor, further CPUs and IOPs above, then RAM.

    STATUS: early. The CPU, its address space and RAM are present and the
    machine can be single-stepped in the debugger. The IOP, the System
    Control Module with its loader ROM, mass storage and the 520's display
    and keyboard are all still to come, so nothing boots yet. See
    docs/IMPLEMENTATION.md in the hpfocus project for the plan.

    References:
      Focus Machine Instruction Set ERS, A-1FE1-3020-B, 12/06/82
      HP 9000 Models 530/540 Service Manual, 09040-90038, Mar 1985
      HP Journal, August 1983 (the chip set), March and May 1984

***************************************************************************/

#include "emu.h"
#include "cpu/focus/focus.h"
#include "iop/focusiop.h"
#include "iop/focusscm.h"
#include "iop/focuscards.h"
#include "iop/focusdim.h"
#include "iop/focusdisc.h"
#include "imagedev/snapquik.h"
#include "machine/keyboard.h"
#include "machine/terminal.h"
#include "sound/beep.h"  // terminal.cpp needs it linked; the scanner follows driver includes
#include "emupal.h"
#include "screen.h"

namespace {

class hp9000_5xx_state : public driver_device, public focus::iop_core::host
{
public:
	hp9000_5xx_state(const machine_config &mconfig, device_type type,
			const char *tag)
		: driver_device(mconfig, type, tag)
		, m_cpu(*this, "cpu")
		, m_ram(*this, "ram")
		, m_terminal(*this, "terminal")
		, m_keyboard(*this, "keyboard")
		, m_screen(*this, "screen")
		, m_iop(*this, 2)   // the channel the boot handoff's inventory announces
	{
	}

	void hp9020(machine_config &config) ATTR_COLD;
	void hp9040(machine_config &config) ATTR_COLD;

protected:
	virtual void machine_start() override ATTR_COLD;
	virtual void machine_reset() override ATTR_COLD;

private:
	void mem_map(address_map &map) ATTR_COLD;
	void kbd_put(u8 data);
	void attach_probes();
	void install_boot_image(const std::vector<u8> &img, const char *whence);
	bool boot_from_disc();
	u32 screen_update(screen_device &screen, bitmap_rgb32 &bitmap,
			const rectangle &cliprect);

	// focus::iop_core::host -- the IOP reaches memory and the CPU through
	// these. The CPU device already implements focus::memory_interface.
	virtual focus::memory_interface &iop_memory() override
	{ return m_cpu->memory(); }
	virtual void iop_interrupt(bool state) override
	{
		// Message register bit 16 upward are the I/O device interrupts
		// (Instruction Set ERS sheet 13).
		m_cpu->set_input_line(16, state ? ASSERT_LINE : CLEAR_LINE);
	}

	TIMER_CALLBACK_MEMBER(iop_tick);
	TIMER_CALLBACK_MEMBER(rtc_tick);
	TIMER_CALLBACK_MEMBER(type_tick);

	QUICKLOAD_LOAD_MEMBER(quickload_cb);

	required_device<focus_device> m_cpu;
	required_shared_ptr<u32> m_ram;
	optional_device<generic_terminal_device> m_terminal;
	optional_device<generic_keyboard_device> m_keyboard;
	optional_device<screen_device> m_screen;

	focus::iop_core m_iop;
	focus::scm_device m_scm;
	focus::dim_card m_dim;            // select code 0: the display
	focus::console_card m_console0;   // select code 0 on models without one
	focus::console_card m_console6;   // select code 6: keyboard board
	focus::disc_card m_disc;          // select code 7: the internal disc
	focus::probe_card m_probes[8];    // logging stand-ins for empty slots
	std::string m_trip, m_trip_seen;
	bool m_tripped = false;
	u32 m_break_at = 0;
	unsigned m_break_hits = 0, m_break_max = 1;
	bool m_traced = false;
	u32 m_trace_left = 0;
	u32 m_wpoint = 0, m_wpoint_last = 0;
	unsigned m_wpoint_hits = 0;
	unsigned m_traps_seen = 0, m_escapes = 0;
	u32 m_ea_at = 0;
	unsigned m_ea_want = 6;
	bool m_ea_armed = false;
	u32 m_watch_lo = 0, m_watch_hi = 0;
	unsigned m_watch_hits = 0, m_watch_max = 12;
	double m_watch_from = 0.0;
	double m_break_from = 0.0;
	double m_wp_from = 0.0;
	bool m_runaway = false, m_ran_away = false;
	std::array<std::pair<u32, u32>, 48> m_trail{};
	size_t m_trail_at = 0;
	void trip_watch(u8 c);
	void report_state(const char *why);
	void memory_find();
	TIMER_CALLBACK_MEMBER(find_tick);
	TIMER_CALLBACK_MEMBER(state_tick);
	emu_timer *m_iop_timer = nullptr;
	emu_timer *m_rtc_timer = nullptr;
	emu_timer *m_type_timer = nullptr;
	std::string m_type_text;
	size_t m_type_pos = 0;
	u32 m_prev_rq = 0;
	bool m_nudged = false;
	u32 m_handoff_q = 0;
	// The discs in the drive, in the order the loader asks for them: a
	// boot image is a set, and the operator feeds the volumes one by one.
	std::vector<std::vector<u8>> m_media;
	unsigned m_boot_volumes = 0;   // how many of them the image needed
};


// The FOCUS address space is 29 bits of byte address. 16 MB matches the
// boot handoff this driver performs: the powerup module sizes memory from
// the top-of-memory word at Q+64, and the loader's stack lives just below
// the top (SB 0xe00000, Q 0xf00203).
static constexpr u32 RAM_SIZE = 16 * 1024 * 1024;

// The character cell the display's CRTC asks for: 15 scan lines tall, and
// eight pixels wide for the font this build draws with.
static constexpr unsigned CELL_WIDTH = 8;
static constexpr unsigned CELL_HEIGHT = 15;
static constexpr unsigned SCREEN_ROWS = 26;

void hp9000_5xx_state::mem_map(address_map &map)
{
	map.unmap_value_high();
	map(0x0000000, 0x0000000 + RAM_SIZE - 1).ram().share("ram");
}


// Watch the character stream going to the screen for the trip text; when
// it arrives, report where the processor is and who called it. The stack
// marker at Q holds delta-Q, STATUS, the PB-relative return address and
// X in the four words from Q downward (Instruction Set ERS sheet 121),
// so the chain walks back a frame at a time.
void hp9000_5xx_state::trip_watch(u8 c)
{
	if (m_tripped || m_trip.empty())
		return;
	m_trip_seen.push_back(char(c));
	if (m_trip_seen.size() > m_trip.size())
		m_trip_seen.erase(0, m_trip_seen.size() - m_trip.size());
	if (m_trip_seen != m_trip)
		return;

	m_tripped = true;
	report_state(m_trip.c_str());
}



// FOCUS_FIND=<hex bytes> or ="<text>" scans ALL of memory for a byte
// string -- an instruction pair to locate a site in the loaded copy, or a
// piece of text to find out whether the system composed it at all and
// where it put it. (It used to scan only the first 2 MB, which silently
// missed everything the running system builds up near the top of RAM.)
// FOCUS_FINDAT=<emulated seconds> runs the search at a chosen moment
// instead of at a breakpoint, which is how "did the system ever compose
// this text?" gets asked of a machine that is simply sitting there.
void hp9000_5xx_state::memory_find()
{
	focus::cpu_core &core = m_cpu->core();
	if (const char *e = getenv("FOCUS_FIND"))
	{
		std::vector<u8> want;
		if (*e == '"')
			for (const char *p = e + 1; *p && *p != '"'; p++)
				want.push_back(u8(*p));
		else
			for (const char *p = e; p[0] && p[1]; p += 2)
			{
				const char pair[3] = { p[0], p[1], 0 };
				want.push_back(u8(strtoul(pair, nullptr, 16)));
			}
		unsigned found = 0;
		for (u32 a = 0; a + want.size() <= RAM_SIZE && found < 12; a++)
		{
			size_t k = 0;
			while (k < want.size() && core.read_mem8(a + u32(k)) == want[k])
				k++;
			if (k != want.size())
				continue;
			std::printf("  found at %08x:", a);
			for (u32 j = 0; j < want.size() + 8 && j < 24; j++)
			{
				const u8 c = core.read_mem8(a + j);
				std::printf("%c", (c >= 32 && c < 127) ? char(c) : '.');
			}
			std::printf("\n");
			found++;
		}
		if (!found)
			std::printf("  found nothing\n");
	}

}

void hp9000_5xx_state::report_state(const char *why)
{
	focus::cpu_core &core = m_cpu->core();
	std::printf("[%s: P=%08x PB=%08x (P-PB=%08x) Q=%08x S=%08x DB=%08x "
			"SB=%08x X=%08x]\n",
			why, core.r.p, core.r.pb, core.r.p - core.r.pb,
			core.r.q, core.r.s, core.r.db, core.r.sb, core.r.x);
	std::printf("  DL=%08x SL=%08x PL=%08x DST=%08x STATUS=%08x M=%d\n",
			core.r.dl, core.r.sl, core.r.pl, core.r.dst, core.r.status,
			core.r.test(focus::sf::M) ? 1 : 0);
	std::printf("  SB+0..+16: %08x %08x %08x %08x %08x\n",
			core.read_mem32(core.r.sb), core.read_mem32(core.r.sb + 4),
			core.read_mem32(core.r.sb + 8), core.read_mem32(core.r.sb + 12),
			core.read_mem32(core.r.sb + 16));
	std::printf("  DB+208..+228: %08x %08x %08x %08x %08x %08x\n",
			core.read_mem32(core.r.db + 208), core.read_mem32(core.r.db + 212),
			core.read_mem32(core.r.db + 216), core.read_mem32(core.r.db + 220),
			core.read_mem32(core.r.db + 224), core.read_mem32(core.r.db + 228));
	// FOCUS_DUMP=<hex addr>[:<words>][,...] prints memory windows, for
	// looking at tables the system built.
	if (const char *e = getenv("FOCUS_DUMP"))
		for (const char *p = e; *p; )
		{
			const u32 addr = strtoul(p, nullptr, 16) & ~3u;
			const char *colon = strchr(p, ':');
			const char *comma = strchr(p, ',');
			const unsigned n = (colon && (!comma || colon < comma))
					? strtoul(colon + 1, nullptr, 0) : 16;
			for (unsigned i = 0; i < n; i += 4)
			{
				std::printf("  %08x:", addr + 4 * i);
				for (unsigned j = 0; j < 4 && i + j < n; j++)
					std::printf(" %08x", core.read_mem32(addr + 4 * (i + j)));
				std::printf("\n");
			}
			if (!comma)
				break;
			p = comma + 1;
		}

	if (getenv("FOCUS_FIND") && !getenv("FOCUS_FINDAT"))
		memory_find();

	// The code segment table says which module each frame is in: entry
	// zero's first word is sixteen times the entry count, and every entry
	// carries its length at +4 and its base at +8 (ERS sheets 25-27).
	const u32 cst = core.read_mem32(0);
	if (cst)
	{
		const u32 count = core.read_mem32(cst) / 16;
		std::printf("  CST at %08x, %u entries\n", cst, count);
		for (u32 seg = 1; seg < count && seg < 40; seg++)
		{
			const u32 ea = cst + 16 * seg;
			const u32 len = core.read_mem32(ea + 4);
			const u32 base = core.read_mem32(ea + 8);
			if (base || len)
				std::printf("    seg %2u base=%08x len=%6u\n", seg, base, len);
		}
	}

	u32 q = core.r.q;
	for (int frame = 0; frame < 10 && q >= core.r.sb && q <= core.r.sl; frame++)
	{
		const u32 delta  = core.read_mem32(q);
		const u32 status = core.read_mem32(q - 4);
		const u32 ret    = core.read_mem32(q - 8);
		std::printf("  frame %d: Q=%08x delta=%u seg=%u ret=%08x\n",
				frame, q, delta, focus::ers32::get(status, 20, 12), ret);
		if (delta == 0 || delta > 0x100000)
			break;
		q -= delta;
	}
	std::fflush(stdout);
}


// FOCUS_PROBE=<sc>[:<id>][,...] parks a logging card in a slot, which is
// how what a driver expects of a controller we do not have gets read off
// a run. It is applied after the real cards so a probe can stand in for
// one of them -- select code 7 in particular, where the 520's manual puts
// internal mass storage and this driver puts the System Control Module.
void hp9000_5xx_state::attach_probes()
{
	const char *e = getenv("FOCUS_PROBE");
	if (!e)
		return;
	for (const char *p = e; *p; )
	{
		const unsigned sc = strtoul(p, nullptr, 0) & 7;
		const char *colon = strchr(p, ':');
		const char *comma = strchr(p, ',');
		m_probes[sc].id = (colon && (!comma || colon < comma))
				? strtoul(colon + 1, nullptr, 0) : 0;
		m_probes[sc].pa = sc;
		m_probes[sc].log = getenv("FOCUS_PROBELOG") != nullptr;
		if (const char *m = getenv("FOCUS_PROBEFR0"))
			m_probes[sc].fr0_answer = strtoul(m, nullptr, 0);
		if (const char *m = getenv("FOCUS_PROBEFR1"))
			m_probes[sc].fr1_mode = strtoul(m, nullptr, 0);
		m_iop.attach(sc, &m_probes[sc]);
		if (!comma)
			break;
		p = comma + 1;
	}
}


void hp9000_5xx_state::machine_start()
{
	// The System Control Module answers select code 7, which is IOP
	// channel 1, peripheral address 7. The consoles: select code 0 is
	// the display -- the panic reporter writes there, and so does the
	// alphanumeric driver, which binds only when the card answers 1 or
	// 6 and then writes the screen with halfword DMA. 6 is the identity
	// that also drives the frame buffer directly (2400 halfwords of
	// blanks, one per character cell) the way the 520 manual describes,
	// so it is the default. Select code 6 is the keyboard board.
	m_iop.attach(focus::scm_device::SELECT_CODE, &m_scm);
	if (const char *e = getenv("FOCUS_SC0ID"))
		m_dim.id = strtoul(e, nullptr, 0);
	m_dim.log_fr = getenv("FOCUS_SC0LOG") != nullptr;
	m_dim.log_cells = getenv("FOCUS_DIMCELLS") != nullptr;
	// FOCUS_TRIP=<text> prints the processor state and a walk back up the
	// stack markers the moment that text reaches the screen, which is how
	// the code behind a message -- an error report, say -- gets found.
	if (const char *e = getenv("FOCUS_TRIP"))
	{
		m_trip = e;
		m_dim.on_cell = [this](u16 v) { trip_watch(u8(v)); };
	}
	if (const char *e = getenv("FOCUS_SC0FR1"))
		m_dim.fr1 = strtoul(e, nullptr, 0);
	m_iop.attach(0, &m_dim);
	m_console6.id = 2;   // keyboard ID register: data bus bit 1
	m_console6.log_fr = getenv("FOCUS_KBDLOG") != nullptr;
	if (const char *e = getenv("FOCUS_NVMFILL"))
		m_console6.fill_nvm(u8(strtoul(e, nullptr, 0)));
	if (const char *e = getenv("FOCUS_SC6FR1"))
		m_console6.fr1_base = strtoul(e, nullptr, 0);
	if (const char *e = getenv("FOCUS_KBDST"))
		m_console6.st = strtoul(e, nullptr, 0);
	m_console6.iop = &m_iop;
	m_console6.pa = 6;
	m_iop.attach(6, &m_console6);
	// SC6 keyboard interrupts are enabled by the hardware itself; the
	// internal disc's select code is armed the same way when present.
	m_iop.enable_interrupt(6);
	if (getenv("FOCUS_DISC"))
		m_iop.enable_interrupt(7);

	// FOCUS_PROBE=<select code>[:<identity>] parks a logging card in an
	// empty slot, which is how what a driver expects of a controller we
	// do not have gets read off a run.
	// The internal disc sits where the System Control Module answers the
	// boot handoff; FOCUS_DISC puts it in, while what it should say to
	// the driver is still being established.
	if (getenv("FOCUS_DISC"))
	{
		m_disc.iop = &m_iop;
		m_disc.log = getenv("FOCUS_DISCLOG") != nullptr;
		// FOCUS_DISCID: the halfword the identify transaction answers;
		// 1..6 map to internal device types, anything else types the
		// slot 29 (the kernel's default).
		if (const char *e = getenv("FOCUS_DISCID"))
			m_disc.identity = u16(strtoul(e, nullptr, 0));
		if (const char *e = getenv("FOCUS_DISCFR3"))
			m_disc.id_fr3 = u16(strtoul(e, nullptr, 0));
		if (const char *e = getenv("FOCUS_DISCFR3B"))
			m_disc.id_fr3_alt = u16(strtoul(e, nullptr, 0));
		if (const char *e = getenv("FOCUS_DISCBUSY"))
			m_disc.busy_ms = strtoul(e, nullptr, 0);
		// FOCUS_FLOPPY=<raw image>[,<raw image>...]: the discs in the
		// drive, one byte per byte (35 cyl x 2 heads x 16 sectors of 256
		// bytes = 286,720 for the media the system shipped on). A boot
		// image is a SET -- BASIC 2.0 is two volumes, the diagnostics
		// three -- and the loader works through them in order, which is
		// what an operator feeding discs to the drive does.
		if (const char *e = getenv("FOCUS_FLOPPY"))
			for (std::string list(e); !list.empty(); )
			{
				const size_t comma = list.find(',');
				const std::string path = list.substr(0, comma);
				list = (comma == std::string::npos) ? "" : list.substr(comma + 1);
				if (path.empty())
					continue;
				if (FILE *f = fopen(path.c_str(), "rb"))
				{
					fseek(f, 0, SEEK_END);
					const long n = ftell(f);
					fseek(f, 0, SEEK_SET);
					std::vector<u8> medium(size_t(n), 0);
					if (fread(medium.data(), 1, size_t(n), f) == size_t(n))
						m_media.push_back(std::move(medium));
					else
						osd_printf_error("FOCUS_FLOPPY: short read on %s\n",
								path.c_str());
					fclose(f);
				}
				else
					osd_printf_error("FOCUS_FLOPPY: cannot open %s\n",
							path.c_str());
			}
		if (!m_media.empty())
			m_disc.mount(m_media.front());
		m_disc.reset_self_test();
		m_iop.attach(7, &m_disc);
	}
	attach_probes();
	m_cpu->set_io(&m_iop);
	m_iop.trace_pc = [this]() { return m_cpu->core().r.p; };


	// The powerup module's final IXIT unwinds a frame whose delta-Q word
	// is the loader's MPB inventory; the word must be cleared when the
	// module reaches that IXIT with the handoff stack still current --
	// what the real loader does to reconcile the two roles is unknown.
	// Same nudge as the bootrun harness, instruction-precise.
	// FOCUS_BREAK=<absolute address> reports the same state as the trip
	// wire the first time the processor reaches that instruction, with a
	// window of the current stack frame -- enough to read the arguments a
	// routine was called with.
	if (const char *e = getenv("FOCUS_BREAK"))
	{
		m_break_at = strtoul(e, nullptr, 16);
		if (const char *colon = strchr(e, ':'))
			m_break_max = strtoul(colon + 1, nullptr, 0);
		// FOCUS_BREAKFROM=<emulated seconds>, as for the watch: ignore
		// the boot's own visits to the address.
		if (const char *n = getenv("FOCUS_BREAKFROM"))
			m_break_from = strtod(n, nullptr);
	}
	// FOCUS_TRACEN=<n> lists the next n instructions after the breakpoint,
	// which is how a call gets followed into whatever it reaches.
	if (const char *e = getenv("FOCUS_WPOINT"))
		m_wpoint = strtoul(e, nullptr, 16);
	if (const char *e = getenv("FOCUS_TRACEN"))
		m_trace_left = strtoul(e, nullptr, 0);

	// FOCUS_WATCH=<lo>:<hi> reports the first instructions executed inside
	// an address range, which answers whether a driver is ever entered.
	if (const char *e = getenv("FOCUS_WATCH"))
	{
		m_watch_lo = strtoul(e, nullptr, 16);
		if (const char *colon = strchr(e, ':'))
			m_watch_hi = strtoul(colon + 1, nullptr, 16);
		if (const char *n = getenv("FOCUS_WATCHN"))
			m_watch_max = strtoul(n, nullptr, 0);
		// FOCUS_WATCHFROM=<emulated seconds> ignores everything before that
		// moment, which is how activity from a typed command gets separated
		// from the boot's own.
		if (const char *n = getenv("FOCUS_WATCHFROM"))
			m_watch_from = strtod(n, nullptr);
	}

	// FOCUS_RUNAWAY reports the moment the processor leaves real memory,
	// with the last instructions it executed before it did. A machine
	// that has jumped to nowhere cannot say where it came from, and the
	// stack marker is usually gone too, so the trail has to be kept.
	m_runaway = getenv("FOCUS_RUNAWAY") != nullptr;
	m_cpu->set_pre_step([this](focus::cpu_core &core) {
		if (m_runaway)
		{
			if (!m_ran_away && (core.r.p >= RAM_SIZE || core.r.pb >= RAM_SIZE))
			{
				m_ran_away = true;
				std::printf("[runaway at %.3fs: P=%08x PB=%08x]\n",
						machine().time().as_double(), core.r.p, core.r.pb);
				std::printf("  last %u instructions, oldest first:\n",
						unsigned(m_trail.size()));
				for (size_t i = 0; i < m_trail.size(); i++)
				{
					const size_t k = (m_trail_at + i) % m_trail.size();
					if (m_trail[k].first)
						std::printf("    P=%08x PB=%08x\n",
								m_trail[k].first, m_trail[k].second);
				}
				report_state("runaway");
			}
			m_trail[m_trail_at] = std::make_pair(core.r.p, core.r.pb);
			m_trail_at = (m_trail_at + 1) % m_trail.size();
		}
		if (m_watch_hi && core.r.p >= m_watch_lo && core.r.p <= m_watch_hi
				&& m_watch_hits < m_watch_max
				&& machine().time().as_double() >= m_watch_from)
		{
			m_watch_hits++;
			std::printf("[%.2fs watch %08x  Q=%08x]\n",
					machine().time().as_double(), core.r.p, core.r.q);
			std::fflush(stdout);
		}
		// FOCUS_TRAPLOG reports each fault as it is taken, with the state the
	// processor is in, which names the code that faulted.
	if (getenv("FOCUS_TRAPLOG"))
		m_cpu->core().on_trap = [this](focus::trap_cause cause, u32 parameter) {
			if (m_traps_seen++ < 6)
			{
				std::printf("\n[trap %u parameter=%08x]\n",
						unsigned(cause), parameter);
				report_state("trap");
			}
		};

	// FOCUS_EATRACE=<addr>[:<n>] prints the next n data addresses the
	// processor computes once it reaches that instruction, with what it
	// found there.
	if (const char *e = getenv("FOCUS_EATRACE"))
	{
		m_ea_at = strtoul(e, nullptr, 16);
		if (const char *colon = strchr(e, ':'))
			m_ea_want = strtoul(colon + 1, nullptr, 0);
		m_cpu->core().on_ea = [this](u32 pc, u32 addr) {
			if (!m_ea_armed || !m_ea_want)
				return;
			m_ea_want--;
			std::printf("[ea pc=%08x addr=%08x = %08x]\n", pc, addr,
					m_cpu->core().read_mem32(addr & ~3u));
			std::fflush(stdout);
		};
	}

	if (getenv("FOCUS_ESCLOG"))
		m_cpu->core().on_escape = [this](u32 p, u32 value) {
			// FOCUS_ESCVAL=<hex> logs only that escape value, uncapped;
			// otherwise the first 400.
			if (const char *e = getenv("FOCUS_ESCVAL"))
			{
				if (*e == '*')      // log everything, uncapped
				{
					std::printf("[%.2fs escape at %08x value=%08x q=%08x]\n",
							machine().time().as_double(), p, value,
							m_cpu->core().r.q);
					std::fflush(stdout);
					return;
				}
				if (value == strtoul(e, nullptr, 16) && m_escapes++ < 40)
				{
					std::printf("[escape at %08x value=%08x q=%08x]\n",
							p, value, m_cpu->core().r.q);
					report_state("escval");
				}
				return;
			}
			if (m_escapes++ < 400)
			{
				std::printf("[escape at %08x value=%08x q=%08x]\n",
						p, value, m_cpu->core().r.q);
				std::fflush(stdout);
			}
		};

	// FOCUS_WPOINT=<addr> reports every change of one memory word with
		// the instruction that made it, which is how a table's builder is
		// found when the table is all there is to go on.
		if (m_wpoint && machine().time().as_double() >= m_wp_from)
		{
			const u32 now = core.read_mem32(m_wpoint);
			if (now != m_wpoint_last)
			{
				if (m_wpoint_hits++ < 20)
				{
					std::printf("[wp %08x: %08x -> %08x at P=%08x PB=%08x]\n",
							m_wpoint, m_wpoint_last, now, core.r.p, core.r.pb);
					// FOCUS_WPSTACK adds the call chain, which is how the
					// code that asked for a write gets named when the
					// writer itself is a shared helper.
					if (getenv("FOCUS_WPSTACK"))
						report_state("wp");
				}
				m_wpoint_last = now;
				std::fflush(stdout);
			}
		}
		if (m_ea_at && core.r.p == m_ea_at)
			m_ea_armed = true;
		if (m_traced && m_trace_left)
		{
			m_trace_left--;
			std::printf("[t %08x pb=%08x q=%08x]\n", core.r.p, core.r.pb, core.r.q);
			if (!m_trace_left)
				std::fflush(stdout);
		}
		if (m_break_at && core.r.p == m_break_at && m_break_hits < m_break_max
				&& machine().time().as_double() >= m_break_from)
		{
			m_traced = true;
			m_break_hits++;
			std::printf("\n[break %08x hit %u]\n", m_break_at, m_break_hits);
			report_state("break");
			// Stack words sit on the q-3 grid: operand (Q-k) is the word
			// at byte q-k-3, so the window walks word slots from there.
			for (int k = -24; k <= 14; k++)
				std::printf("    Q%+3d (%08x) = %08x\n", 4 * k,
						core.r.q + 4 * k - 3,
						core.read_mem32(core.r.q + 4 * k - 3));
			std::fflush(stdout);
		}
		// Traps vector through CST entry 1 once the powerup module has
		// built the tables and pointed dedicated location 0 at them.
		if (!core.traps_enabled() && core.read_mem32(0) != 0)
			core.enable_traps(true);
		if (m_nudged || core.r.q != m_handoff_q || m_handoff_q == 0)
			return;
		const u32 w = core.read_mem32(core.r.p & ~3u);
		const u16 insn = (core.r.p & 2) ? u16(w) : u16(w >> 16);
		if (insn == 0x007c)
		{
			core.write_mem32(m_handoff_q - 3, 0);
			m_nudged = true;
			logerror("nudge: (Q) cleared for the final IXIT\n");
		}
	});

	// The IOP polls its interfaces continuously and runs the interval
	// timer; one emulated tick per millisecond is enough to get both
	// moving. Real timings are in the I/O ERS section 7.
	m_iop_timer = timer_alloc(FUNC(hp9000_5xx_state::iop_tick), this);
	m_iop_timer->adjust(attotime::from_msec(1), 0, attotime::from_msec(1));
	m_rtc_timer = timer_alloc(FUNC(hp9000_5xx_state::rtc_tick), this);
	m_rtc_timer->adjust(attotime::from_seconds(1), 0, attotime::from_seconds(1));

	// FOCUS_TYPE=<text> feeds keystrokes through the keyboard board once
	// the boot has settled -- the headless way to drive the command line.
	// \n becomes EXECUTE. FOCUS_TYPEAT=<emulated seconds> delays the
	// first keystroke (default 15).
	if (const char *e = getenv("FOCUS_FINDAT"))
	{
		emu_timer *const t = timer_alloc(FUNC(hp9000_5xx_state::find_tick), this);
		t->adjust(attotime::from_double(strtod(e, nullptr)));
	}
	if (const char *e = getenv("FOCUS_STATEAT"))
		for (std::string list(e); !list.empty(); )
		{
			const size_t comma = list.find(',');
			const double at = strtod(list.substr(0, comma).c_str(), nullptr);
			list = (comma == std::string::npos) ? "" : list.substr(comma + 1);
			emu_timer *const t =
					timer_alloc(FUNC(hp9000_5xx_state::state_tick), this);
			t->adjust(attotime::from_double(at));
		}
	if (const char *e = getenv("FOCUS_TYPE"))
	{
		for (const char *pc = e; *pc; pc++)
		{
			if (pc[0] == '\\' && pc[1] == 'n') { m_type_text.push_back('\n'); pc++; }
			else m_type_text.push_back(*pc);
		}
		const double at = getenv("FOCUS_TYPEAT")
				? strtod(getenv("FOCUS_TYPEAT"), nullptr) : 15.0;
		m_type_timer = timer_alloc(FUNC(hp9000_5xx_state::type_tick), this);
		m_type_timer->adjust(attotime::from_double(at), 0,
				attotime::from_msec(150));
	}
}

TIMER_CALLBACK_MEMBER(hp9000_5xx_state::find_tick)
{
	memory_find();
}


// FOCUS_STATEAT=<emulated seconds>[,<more>...] reports the processor and
// its stack at chosen moments -- where a wedged machine is sitting, which
// a breakpoint cannot ask because the code in question is not known yet.
TIMER_CALLBACK_MEMBER(hp9000_5xx_state::state_tick)
{
	report_state("stateat");
}


TIMER_CALLBACK_MEMBER(hp9000_5xx_state::type_tick)
{
	if (m_type_pos == 0)
		std::printf("[typing starts, disc blocks %u, sc6 writes %u]\n",
				m_disc.m_seq, m_console6.writes);
	if (m_type_pos < m_type_text.size())
		kbd_put(u8(m_type_text[m_type_pos++]));
	else
		m_type_timer->adjust(attotime::never);
}


TIMER_CALLBACK_MEMBER(hp9000_5xx_state::iop_tick)
{
	m_iop.poll_and_service();
	m_disc.tick();
	// One timer count is 512 clock periods = 28.4 us; a 1 ms tick is
	// about 35 of them.
	m_iop.timer_tick(35);
	if (m_iop.take_timer_event())
		m_cpu->core().raise_message(13);

	// Every accepted device raise is one message at 16 + its level (the
	// microcode handler maps it back and pulls the device from the
	// level's list). The attention poll reaches the CPU as message 14
	// (I/O ERS section 2.1).
	const u32 raised = m_iop.take_raise_events();
	for (unsigned pa = 0; pa < 8; pa++)
		if (raised & (1u << pa))
			m_cpu->core().raise_message(16 + m_iop.interrupt_level(pa));
	if (m_iop.take_attention_event())
		m_cpu->core().raise_message(14);
}


TIMER_CALLBACK_MEMBER(hp9000_5xx_state::rtc_tick)
{
	m_console6.rtc_second_tick();
}


void hp9000_5xx_state::machine_reset()
{
	// iop_core::reset() rebuilds every channel_state, which drops the
	// interface attachments; hook everything back up afterwards.
	m_iop.reset();
	m_scm.reset();
	m_iop.attach(focus::scm_device::SELECT_CODE, &m_scm);
	m_iop.attach(0, &m_dim);
	m_iop.attach(6, &m_console6);
	if (getenv("FOCUS_DISC"))
		m_iop.attach(7, &m_disc);
	attach_probes();
	m_iop.enable_interrupt(6);
	m_prev_rq = 0;

	// A loader ROM image would be installed here. None is dumped, so the
	// SCM answers the select-code scan but has nothing to hand over; see
	// docs/SOFTWARE.md. What the ROM would do -- find a boot volume, read
	// its descriptor, load the image and jump to it -- is done at a high
	// level instead, from whatever discs are in the drive.
	const u8 *rom = memregion("loader") ? memregion("loader")->base() : nullptr;
	m_scm.set_rom(rom, focus::scm_device::ROM_SIZE);

	if (!m_media.empty())
		boot_from_disc();
}


// Boot a Series 500 image the way the (undumped) SCM loader ROM hands
// one over -- the contract bootrun reverse-engineered from SYSTEM
// POWERUP itself. The image loads high, the powerup module executes in
// place with PB = 0, and the loader's parameter block sits at Q: the
// MPB inventory doubling as the final IXIT's frame, the load address at
// Q+52, the top-of-memory word at Q+64.
void hp9000_5xx_state::install_boot_image(const std::vector<u8> &img,
		const char *whence)
{
	constexpr u32 LOAD = 0x100000;
	const u32 size = u32(img.size());

	auto &core = m_cpu->core();
	focus::memory_interface &mem = m_cpu->memory();
	for (u32 i = 0; i < size; i++)
		mem.write8(LOAD + i, img[i]);

	auto beword = [&img](u32 off) {
		return (u32(img[off]) << 24) | (u32(img[off + 1]) << 16)
				| (u32(img[off + 2]) << 8) | img[off + 3];
	};
	u32 entry_off = 0;
	for (u32 off = 0x30; off + 4 <= 0x80; off += 4)
		if (beword(off) == 0xc4) { entry_off = off + 0x20; break; }
	if (!entry_off)
	{
		osd_printf_error("%s: no 000000c4 terminator in the module header\n",
				whence);
		return;
	}

	core.r.pb = 0;
	core.r.pl = LOAD + beword(8) + 3;
	core.r.p  = LOAD + entry_off;

	core.r.sb = 0xe00000;
	core.r.sl = 0xf7ffff | 3;
	core.r.q  = 0xf00203;
	core.r.s  = core.r.q + 0x200;
	core.r.db = 0;
	core.r.dl = 0xffffff | 3;
	core.r.dst = 1152;
	core.r.put(focus::sf::M, 1);
	core.r.mask = focus::ers32::set(0, 3, 3, 1);

	const u32 q = core.r.q;
	m_handoff_q = q;
	m_nudged = false;

	// The loader's parameter block; see bootrun.cpp for the full
	// derivation of every word.
	mem.write16(q - 3 + 0, 0x9004);   // MPB inventory: CPU, channel 1
	mem.write16(q - 3 + 2, 0xa002);   // IOP, channel 2
	mem.write16(q - 3 + 4, 0x8000);   // (Q+1) high / global DS pointer
	mem.write16(q - 3 + 6, 0x0000);   // inventory terminator
	mem.write32(q - 3 - 4, 0x00020000);   // (Q-1): privileged, CSN 0
	mem.write32(q - 3 - 8, 0x5c84);       // (Q-2): the dispatcher
	mem.write32(q - 3 - 12, 0);           // (Q-3): X
	mem.write32(q + 2 * 4 - 3, 0);        // (Q+2): previous mask
	mem.write32(q + 3 * 4 - 3, 0);        // (Q+3): device number
	mem.write32(q + 52 - 3, LOAD);        // where the image was loaded
	mem.write32(q + 64 - 3, RAM_SIZE);    // top of memory
	for (u32 i = 0; i < 20; i++)          // boot device record
		mem.write8(q + 65 + i, 0);
	mem.write32(q + 88 - 3, 0);
	mem.write32(q + 92 - 3, 0);
	core.push(u32(-2));                   // cold start

	core.enable_traps(false);
	logerror("boot image from %s: %u bytes at %#x, entry %#x\n",
			whence, size, LOAD, core.r.p);
}


// The bootstrap loader's own job, done from the medium instead of from a
// file handed in around the side: read the volume header, read the boot
// descriptor behind it, and copy each volume's slice of the image into
// memory. Every byte comes through the controller's Locate-and-Read path,
// so the loader uses the disc the way the driver later will.
//
// A boot disc is not a filesystem (docs/HARDWARE.md 3.2), and the volume
// header says everything the loader needs:
//
//   +0x1c  the volume's BLOCK SIZE -- 1024 on boot media, 256 on data
//          media. (HARDWARE.md reads this field as a bootable flag
//          because those are the only two values it ever saw. It is the
//          block size: 1024 puts block 1 at byte 0x400, which is where
//          boot media carry their descriptor and data media their file
//          directory. Both kinds keep block 1 for "what is on this
//          volume", and both agree once the field is read this way.)
//   +0x28  the first free block, which is where the boot area ends.
//
// So block 1 holds the descriptor -- image size, how many volumes carry
// it, which one this is, where its slice belongs -- block 2 begins the
// slice, and the first free block ends it. Taking the length from the
// medium rather than from the file's size is what keeps a volume from
// over-reading into whatever follows its boot area.
bool hp9000_5xx_state::boot_from_disc()
{
	constexpr u32 SECTOR = focus::disc_card::BLOCK;
	constexpr u32 MAGIC = 0xe9c28206;

	if (m_media.empty())
		return false;

	auto beword = [](const std::vector<u8> &v, u32 off) {
		return (u32(v[off]) << 24) | (u32(v[off + 1]) << 16)
				| (u32(v[off + 2]) << 8) | v[off + 3];
	};

	std::vector<u8> image;
	u32 total = 0, volumes = 0, placed = 0;
	for (unsigned v = 0; v < m_media.size(); v++)
	{
		m_disc.mount(m_media[v]);

		std::vector<u8> header;
		if (!m_disc.read_blocks(0, SECTOR, header))
		{
			osd_printf_error("boot: cannot read the volume header\n");
			return false;
		}
		const u32 blocksize = beword(header, 0x1c);
		const u32 first_free = beword(header, 0x28);
		if (blocksize < SECTOR || (blocksize % SECTOR) != 0 || first_free < 3)
		{
			osd_printf_error("boot: %.16s has no usable boot area "
					"(block size %u, first free block %u)\n",
					reinterpret_cast<const char *>(header.data()) + 4,
					blocksize, first_free);
			return false;
		}

		std::vector<u8> desc;
		if (!m_disc.read_blocks(blocksize / SECTOR, SECTOR, desc))
			return false;
		if (beword(desc, 0) != MAGIC)
		{
			osd_printf_error("boot: %.16s is not a boot volume -- block 1 "
					"begins %08x, not the boot magic %08x\n",
					reinterpret_cast<const char *>(header.data()) + 4,
					beword(desc, 0), MAGIC);
			return false;
		}

		const u32 this_total = beword(desc, 4);
		const u32 nvols = beword(desc, 8);
		const u32 number = beword(desc, 12);
		const u32 offset = beword(desc, 16);
		// The volumes go in in order, as the operator would feed them.
		if (number != v + 1)
		{
			osd_printf_error("boot: this is volume %u of %u; %s\n",
					number, nvols, v == 0
							? "the loader wants volume 1 first"
							: "that is not the volume it asked for");
			return false;
		}
		if (v == 0)
		{
			total = this_total;
			volumes = nvols;
			if (total > RAM_SIZE - 0x100000)
			{
				osd_printf_error("boot: image of %u bytes will not fit\n", total);
				return false;
			}
			image.assign(total, 0);
		}
		else if (this_total != total)
		{
			osd_printf_error("boot: volume %u belongs to a different set\n",
					number);
			return false;
		}

		// The slice runs from block 2 to the first free block, and holds
		// either the rest of the image or as much of it as the boot area
		// has room for.
		const u32 slice_start = 2 * blocksize;
		const u32 boot_area = (first_free - 2) * blocksize;
		const u32 length = std::min(total - offset, boot_area);
		std::vector<u8> slice;
		if (!m_disc.read_blocks(slice_start / SECTOR, length, slice))
		{
			osd_printf_error("boot: volume %u is short\n", number);
			return false;
		}
		std::copy(slice.begin(), slice.end(), image.begin() + offset);
		placed += length;
		logerror("boot: volume %u of %u, %u bytes at image offset %#x\n",
				number, nvols, length, offset);

		if (placed >= total)
		{
			// Anything mounted past the volumes the image needed is the
			// disc the operator puts in once the machine is up.
			if (v + 1 < m_media.size())
				m_boot_volumes = v + 1;
			break;
		}
		if (v + 1 >= m_media.size())
		{
			osd_printf_error("boot: the image needs %u volumes and %u "
					"%s mounted; %u of %u bytes loaded\n", volumes,
					unsigned(m_media.size()),
					m_media.size() == 1 ? "is" : "are", placed, total);
			return false;
		}
	}

	install_boot_image(image, "the internal disc");

	// Put a disc back in the drive for the running system: the one after
	// the boot set if the operator mounted a spare, else volume 1.
	const unsigned after = (m_boot_volumes && m_boot_volumes < m_media.size())
			? m_boot_volumes : 0;
	logerror("boot: leaving volume %u in the drive\n", after + 1);
	m_disc.mount(m_media[after]);
	m_disc.reset_self_test();
	return true;
}


QUICKLOAD_LOAD_MEMBER(hp9000_5xx_state::quickload_cb)
{
	constexpr u32 LOAD = 0x100000;
	const u64 size = image.length();
	if (size < 0x100 || LOAD + size > RAM_SIZE)
		return std::make_pair(image_error::INVALIDLENGTH,
				"image empty or larger than RAM allows");

	std::vector<u8> img(size);
	image.fread(img.data(), size);
	install_boot_image(img, image.filename());
	return std::make_pair(std::error_condition(), std::string());
}


// A keystroke becomes a keycode in the keyboard board's holding
// register (R10), with the service-request latch set. The keyswitch
// matrix does not send ASCII: the system translates keycodes through a
// 256-byte table, and sweeping every keycode from 0x20 to 0x7f through
// the emulated keyboard -- clearing the input line between each, and
// reading the character each one put in the display's frame buffer --
// agreed with the table at diag.bin 0x23614 wherever both spoke. The
// map below is the sweep's own result, not the table's: the table lists
// characters for keycodes that turn out to be dead keys or softkeys --
// 0x48 echoes nothing and 0x39 answers UNDEFINED KEY, though the table
// calls them X and 1 -- so only the seventy-one keycodes that actually
// put a character on the screen are mapped here. The system folds
// letters to upper case as it echoes them. Four keys are
// not characters: 0x58 executes the line -- typing RUN and 0x58 answers
// LABEL NOT FOUND, where 0x60 answers IMPROPER LINE NUMBER, so 0x60 is
// the one that stores a line into the program and 0x58 is EXECUTE --
// 0x70 clears the line, and 0x7f is the space bar. Anything the table reaches only through a modifier --
// the keycodes below 0x20, and the shifted punctuation -- is unmapped
// until the modifier is identified.
static const u8 KEYCODES[128] =
{
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   // ........
	0x00, 0x00, 0x48, 0x00, 0x00, 0x60, 0x00, 0x00,   // ........
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,   // ........
	0x00, 0x00, 0x00, 0x70, 0x00, 0x00, 0x00, 0x00,   // ........
	0x7f, 0xf9, 0xd9, 0xac, 0xfd, 0xa0, 0xa5, 0x59,   //  !"#$%&'
	0x22, 0x2b, 0x5b, 0x43, 0x42, 0x27, 0x40, 0x50,   // ()*+,-./
	0x2f, 0x52, 0x24, 0x2c, 0x62, 0x5a, 0x2d, 0x25,   // 01234567
	0x2e, 0x26, 0xe1, 0x61, 0x89, 0x29, 0x88, 0x45,   // 89:;<=>?
	0xa4, 0x7c, 0x4e, 0x4d, 0x5c, 0x2a, 0x65, 0x5d,   // @ABCDEFG
	0x66, 0x76, 0x5e, 0x67, 0x5f, 0x4f, 0x57, 0x6f,   // HIJKLMNO
	0x77, 0x78, 0x74, 0x54, 0x6d, 0x6e, 0x56, 0x64,   // PQRSTUVW
	0x55, 0x75, 0x4c, 0x69, 0x68, 0x71, 0x23, 0xa7,   // XYZ[\]^_
	0x28, 0x7c, 0x4e, 0x4d, 0x5c, 0x2a, 0x65, 0x5d,   // `abcdefg
	0x66, 0x76, 0x5e, 0x67, 0x5f, 0x4f, 0x57, 0x6f,   // hijklmno
	0x77, 0x78, 0x74, 0x54, 0x6d, 0x6e, 0x56, 0x64,   // pqrstuvw
	0x55, 0x75, 0x4c, 0xaa, 0xc2, 0xa3, 0xa8, 0x00,   // xyz{|}~.
};

void hp9000_5xx_state::kbd_put(u8 data)
{
	// Shift is bit 7 of the keycode, not a key of its own: the
	// translation table at diag.bin 0x23614 is 256 bytes, its top half
	// the shifted map, and feeding a keycode with 0x80 set produces
	// exactly that half -- 0x59 types an apostrophe, 0xd9 a double
	// quote. (The earlier reading of bit 7 as a make/break release flag
	// was taken before the UI existed to echo anything.) Host bytes that
	// need shift on the real keyboard therefore map to code | 0x80.
	// FOCUS_RAWKEYS makes the host's byte the keycode itself, which is
	// how the table above was read off the machine.
	const u8 code = getenv("FOCUS_RAWKEYS") ? data : KEYCODES[data & 0x7f];
	if (getenv("FOCUS_RAWKEYS"))
	{
		// FOCUS_KEYOR adds flag bits above the 8-bit host byte, for
		// probing the holding register's shift/control encoding.
		u16 flags = 0;
		if (const char *e = getenv("FOCUS_KEYOR"))
			flags = u16(strtoul(e, nullptr, 16));
		m_console6.type(u16(data) | flags);
		return;
	}
	if (code)
		m_console6.type(code);
}


// The screen is the display card's frame buffer drawn through a built-in
// character generator: the CRTC's display start picks the first cell, its
// vertical-displayed register the row count, and its cursor address the
// cell drawn inverted. Each cell carries its attribute in the high byte;
// bit 15 is the one the system sets on inverse-video text.
u32 hp9000_5xx_state::screen_update(screen_device &screen,
		bitmap_rgb32 &bitmap, const rectangle &cliprect)
{
	const rgb_t phosphor(0x00, 0xff, 0x40);
	const unsigned rows = std::min<unsigned>(m_dim.rows(), SCREEN_ROWS);
	const bool blink = (screen.frame_number() & 0x10) != 0;

	if (getenv("FOCUS_DIMDUMP")
			&& (screen.frame_number() % strtoul(getenv("FOCUS_DIMDUMP"), nullptr, 0)) == 0)
	{
		std::printf("--- frame %d start %04x rows %u\n",
				int(screen.frame_number()), m_dim.start(), rows);
		for (unsigned r = 0; r * focus::dim_card::COLUMNS < focus::dim_card::CELLS; r++)
		{
			std::string line;
			for (unsigned c = 0; c < focus::dim_card::COLUMNS; c++)
			{
				const u8 ch = u8(m_dim.vram[r * focus::dim_card::COLUMNS + c]);
				line.push_back((ch >= 32 && ch < 127) ? char(ch) : '.');
			}
			if (line.find_first_not_of(' ') != std::string::npos)
				std::printf("|%2u %s|\n", r, line.c_str());
		}
		// MAME leaves stdout fully buffered, so a run that does not end
		// -- an interactive session -- would hold every dump until it
		// did. Flush each one as it is written.
		std::fflush(stdout);
	}

	bitmap.fill(rgb_t::black(), cliprect);
	for (unsigned row = 0; row < rows; row++)
		for (unsigned col = 0; col < focus::dim_card::COLUMNS; col++)
		{
			const unsigned offset = row * focus::dim_card::COLUMNS + col;
			const u16 v = m_dim.cell(offset);
			bool invert = (v & 0x8000) != 0;
			if (m_dim.cursor_visible() && blink
					&& ((m_dim.start() + offset) & (focus::dim_card::CELLS - 1))
							== m_dim.cursor())
				invert = !invert;

			for (unsigned line = 0; line < CELL_HEIGHT; line++)
			{
				const u8 gfx = focus::font8x16[((v & 0xff) << 4) | line];
				u32 *p = &bitmap.pix(row * CELL_HEIGHT + line, col * CELL_WIDTH);
				for (unsigned bit = 0; bit < CELL_WIDTH; bit++)
					*p++ = (BIT(gfx, 7 - bit) != invert) ? phosphor : rgb_t::black();
			}
		}
	return 0;
}


void hp9000_5xx_state::hp9020(machine_config &config)
{
	// 18 MHz, 55 ns microinstruction time (Hardware Technical Data).
	FOCUS(config, m_cpu, 18_MHz_XTAL);
	m_cpu->set_addrmap(AS_PROGRAM, &hp9000_5xx_state::mem_map);
	m_cpu->set_mpb_channel(1);

	// The 520's integrated display, as its own CRTC programs it: 80
	// columns by 26 rows of 15-scanline characters.
	SCREEN(config, m_screen);
	m_screen->set_refresh_hz(60);
	m_screen->set_size(focus::dim_card::COLUMNS * CELL_WIDTH,
			SCREEN_ROWS * CELL_HEIGHT);
	m_screen->set_visarea_full();
	m_screen->set_screen_update(FUNC(hp9000_5xx_state::screen_update));

	// The keyswitch matrix: a keystroke becomes a keycode in the keyboard
	// board's holding register.
	GENERIC_KEYBOARD(config, m_keyboard, 0);
	m_keyboard->set_keyboard_callback(FUNC(hp9000_5xx_state::kbd_put));

	QUICKLOAD(config, "quickload", "bin").set_load_callback(
			FUNC(hp9000_5xx_state::quickload_cb));
}


void hp9000_5xx_state::hp9040(machine_config &config)
{
	// The 540 is the same processor stack in a free-standing cabinet, with
	// no integrated display; the console is an external terminal.
	FOCUS(config, m_cpu, 18_MHz_XTAL);
	m_cpu->set_addrmap(AS_PROGRAM, &hp9000_5xx_state::mem_map);
	m_cpu->set_mpb_channel(1);

	QUICKLOAD(config, "quickload", "bin").set_load_callback(
			FUNC(hp9000_5xx_state::quickload_cb));
}


// The System Control Module's 8 KB bootstrap loader ROM, 09020-80000 rev A
// or 09020-80001 rev B. No dump of either is known to exist; the museum's
// Series 500 collection does not include one. The documented alternative is
// to emulate the loader's behaviour at a high level, which is what the plan
// plumps for -- the protocol is fully described even though the code is not.
ROM_START(hp9020)
	ROM_REGION(0x2000, "loader", ROMREGION_ERASEFF)
	ROM_LOAD_OPTIONAL("09020-80001.bin", 0x0000, 0x2000, NO_DUMP)
ROM_END

ROM_START(hp9040)
	ROM_REGION(0x2000, "loader", ROMREGION_ERASEFF)
	ROM_LOAD_OPTIONAL("09020-80001.bin", 0x0000, 0x2000, NO_DUMP)
ROM_END

} // anonymous namespace


//   YEAR  NAME    PARENT  COMPAT  MACHINE  INPUT  CLASS             INIT        COMPANY            FULLNAME         FLAGS
COMP(1982, hp9020, 0,      0,      hp9020,  0,     hp9000_5xx_state, empty_init, "Hewlett-Packard", "HP 9000/520", MACHINE_NOT_WORKING | MACHINE_NO_SOUND)
COMP(1982, hp9040, 0,      0,      hp9040,  0,     hp9000_5xx_state, empty_init, "Hewlett-Packard", "HP 9000/540", MACHINE_NOT_WORKING | MACHINE_NO_SOUND)
