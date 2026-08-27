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

#include <fstream>
#include "cpu/focus/focus.h"
#include "iop/focusiop.h"
#include "iop/focusscm.h"
#include "iop/focuscards.h"
#include "iop/focusdim.h"
#include "iop/focusgfx.h"
#include "iop/focusprint.h"
#include "iop/focusdisc.h"
#include "iop/focusasi.h"
#include "iop/focushpib.h"
#include "imagedev/snapquik.h"
#include "machine/keyboard.h"
#include "machine/terminal.h"
#include "sound/beep.h"  // terminal.cpp needs it linked; the scanner follows driver includes
#include "emupal.h"

#include <map>
#include <tuple>
#include "imagedev/printer.h"
#include "screen.h"

// The run-time knobs: one catalogue, printed by FOCUS_HELP=1.
// See src/cpu/focus/focusknobs.h.
namespace knob = focus::knob;

// Which of the four ways of saying something this file uses, and when:
//
//   knob::channel, knob::device_log  a diagnostic behind a knob, with a
//                                    budget. Everything gated belongs here.
//   osd_printf_error / _info         user-facing reporting -- a bad knob
//                                    value, a file that will not open, what
//                                    was fitted where. MAME's own voice.
//   std::printf                      the debug probes' raw stdout. golden.sh
//                                    byte-compares this, so the text of an
//                                    existing report is frozen: changing one
//                                    is a deliberate act with a re-capture,
//                                    never a tidy-up.
//   logerror                         MAME's -log stream, for the machine's
//                                    own faults rather than the emulator's.

// Every device log is armed the same way: it wears the card's short tag
// and turns on with that card's own knob. The knob names stay written
// out at each call, which is what tools/check_knobs.py reads them from.
static void arm_log(knob::device_log &log, const char *tag, const char *knobname)
{
	log.name(tag);
	log.enable(bool(knob::flag(knobname)));
}

// The same, for a card that sits in a numbered slot.
static void arm_log(knob::device_log &log, const char *tag, unsigned unit,
		const char *knobname)
{
	log.name(tag, unit);
	log.enable(bool(knob::flag(knobname)));
}

// Reading a whole file into memory, which two knobs do. False if the
// file would not open. `short_read` reports one that opened but gave up
// fewer bytes than its size promised: the callers word that differently
// and answer it differently too -- FOCUS_FLOPPY drops the medium, while
// FOCUS_HPIBDISC keeps however much arrived -- so it comes back as a
// flag rather than a failure.
static bool read_file(const char *path, std::vector<u8> &out, bool &short_read)
{
	short_read = false;
	FILE *const f = fopen(path, "rb");
	if (!f)
		return false;
	fseek(f, 0, SEEK_END);
	const long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	out.assign(size_t(n), 0);
	short_read = fread(out.data(), 1, size_t(n), f) != size_t(n);
	fclose(f);
	return true;
}

//**************************************************************************
//  MEDIA
//**************************************************************************

// A disc for the Series 500's drives, as a MAME image device: the 520's
// flexible discs mount as -flop1/-flop2/-flop3 (the stack of volumes the
// operator will feed the internal drive, in order), and the 540's CS/80
// system disc as -hard1. Accepts the raw sector images the tools produce
// and Teledisk .TD0 -- the format the BASIC distribution survives in --
// decoded here so the shipped files mount directly.
//
// The Teledisk layout (normal compression only, which is all the Series
// 500 images use): a 12-byte header, an optional comment block, then per
// track a 4-byte header (sector count, cylinder, head, crc; count 0xFF
// ends the image) and per sector a 6-byte header plus, unless flags say
// otherwise, a data block of {length u16le, encoding u8, payload}:
// encoding 0 = literal, 1 = a two-byte pattern repeated, 2 = RLE chunks.
// Sectors are emitted in (cylinder, head, sector) order, matching
// tools/td0.py, whose output these images were validated against.
class hp_focus_media_device : public device_t, public device_image_interface
{
public:
	hp_focus_media_device(const machine_config &mconfig, const char *tag,
			device_t *owner, uint32_t clock = 0);

	virtual std::pair<std::error_condition, std::string> call_load() override;
	virtual void call_unload() override { m_data.clear(); }

	virtual bool is_readable() const noexcept override { return true; }
	virtual bool is_writeable() const noexcept override { return false; }
	virtual bool is_creatable() const noexcept override { return false; }
	virtual bool is_reset_on_load() const noexcept override { return false; }
	virtual const char *file_extensions() const noexcept override { return "img,td0"; }
	virtual const char *image_type_name() const noexcept override
	{ return m_hard ? "harddisk" : "floppydisk"; }
	virtual const char *image_brief_type_name() const noexcept override
	{ return m_hard ? "hard" : "flop"; }

	hp_focus_media_device &set_hard() { m_hard = true; return *this; }
	const std::vector<u8> &data() const { return m_data; }
	bool has_media() const { return !m_data.empty(); }

protected:
	virtual void device_start() override ATTR_COLD { }

private:
	static bool decode_td0(const std::vector<u8> &in, std::vector<u8> &out);

	std::vector<u8> m_data;
	bool m_hard = false;
};

DEFINE_DEVICE_TYPE(HP_FOCUS_MEDIA, hp_focus_media_device, "hpfocus_media",
		"HP 9000 Series 500 disc image")

hp_focus_media_device::hp_focus_media_device(const machine_config &mconfig,
		const char *tag, device_t *owner, uint32_t clock)
	: device_t(mconfig, HP_FOCUS_MEDIA, tag, owner, clock)
	, device_image_interface(mconfig, *this)
{
}

std::pair<std::error_condition, std::string> hp_focus_media_device::call_load()
{
	std::vector<u8> raw(length());
	if (fread(raw.data(), raw.size()) != raw.size())
		return std::make_pair(image_error::UNSPECIFIED, "short read");

	if (raw.size() >= 12 && raw[0] == 'T' && raw[1] == 'D')
	{
		if (!decode_td0(raw, m_data))
			return std::make_pair(image_error::INVALIDIMAGE,
					"Teledisk image did not decode");
	}
	else if (raw.size() >= 12 && raw[0] == 't' && raw[1] == 'd')
		return std::make_pair(image_error::UNSUPPORTED,
				"Teledisk advanced (LZH) compression is not supported");
	else
		m_data = std::move(raw);

	if (m_data.empty() || (m_data.size() % 256) != 0)
		return std::make_pair(image_error::INVALIDIMAGE,
				"not a whole number of 256-byte sectors");
	return std::make_pair(std::error_condition(), std::string());
}

bool hp_focus_media_device::decode_td0(const std::vector<u8> &in,
		std::vector<u8> &out)
{
	static constexpr unsigned SIZES[7] =
			{ 128, 256, 512, 1024, 2048, 4096, 8192 };
	auto u16le = [&in](size_t p) -> unsigned
	{ return in[p] | (unsigned(in[p + 1]) << 8); };

	// (cylinder, head, sector) -> payload; emitted in key order.
	std::map<std::tuple<u8, u8, u8>, std::vector<u8>> sectors;

	size_t p = 12;
	if (in[7] & 0x80)                       // comment block
	{
		if (p + 10 > in.size())
			return false;
		p = 22 + u16le(14);
	}
	while (p + 4 <= in.size())
	{
		const u8 nsec = in[p], cyl = in[p + 1], head = in[p + 2];
		p += 4;
		if (nsec == 0xff)
			break;
		for (unsigned k = 0; k < nsec; k++)
		{
			if (p + 6 > in.size())
				return false;
			const u8 scyl = in[p], shead = in[p + 1], snum = in[p + 2];
			const u8 scode = in[p + 3], flags = in[p + 4];
			p += 6;
			(void)cyl; (void)head;
			const unsigned size = scode < 7 ? SIZES[scode] : 0;
			if (flags & 0x30)               // no data block follows
				continue;
			if (p + 2 > in.size())
				return false;
			const unsigned blen = u16le(p);
			p += 2;
			if (p + blen > in.size() || blen < 1)
				return false;

			std::vector<u8> sec(size, 0);
			const u8 enc = in[p];
			const size_t body = p + 1, bend = p + blen;
			if (enc == 0)
			{
				const size_t n = std::min<size_t>(size, bend - body);
				std::copy(in.begin() + body, in.begin() + body + n, sec.begin());
			}
			else if (enc == 1)
			{
				if (body + 4 <= bend)
					for (size_t k2 = 0; k2 < size; k2++)
						sec[k2] = in[body + 2 + (k2 & 1)];
			}
			else if (enc == 2)
			{
				size_t q = body, o = 0;
				while (q + 1 < bend && o < size)
				{
					const u8 kind = in[q], n = in[q + 1];
					q += 2;
					if (kind == 0)
					{
						for (unsigned k2 = 0; k2 < n && q < bend && o < size; k2++)
							sec[o++] = in[q++];
					}
					else
					{
						const unsigned width = 2 * kind;
						if (q + width > bend)
							break;
						for (unsigned r = 0; r < n && o < size; r++)
							for (unsigned k2 = 0; k2 < width && o < size; k2++)
								sec[o++] = in[q + k2];
						q += width;
					}
				}
			}
			else
				return false;
			p += blen;
			sectors[{ scyl, shead, snum }] = std::move(sec);
		}
	}

	out.clear();
	for (auto &kv : sectors)
		out.insert(out.end(), kv.second.begin(), kv.second.end());
	return !out.empty();
}


class mpb_ram256k_device;
class mpb_ram512k_device;
class mpb_ram1m_device;
class mpb_slot_device;
class hp27128_asi_device;
class hp27110_hpib_device;
class focus_cio_slot_device;
DECLARE_DEVICE_TYPE(MPB_RAM256K, mpb_ram256k_device)
DECLARE_DEVICE_TYPE(MPB_RAM512K, mpb_ram512k_device)
DECLARE_DEVICE_TYPE(MPB_RAM1M, mpb_ram1m_device)
DECLARE_DEVICE_TYPE(MPB_SLOT, mpb_slot_device)
DECLARE_DEVICE_TYPE(HP27128_ASI, hp27128_asi_device)
DECLARE_DEVICE_TYPE(HP27110_HPIB, hp27110_hpib_device)
DECLARE_DEVICE_TYPE(FOCUS_CIO_SLOT, focus_cio_slot_device)

// RAM boards. The service manual is explicit that memory shares the
// processor stack: "CPU, IOP, and memory boards reside in the stack and
// communicate via the common memory processor bus" -- the same twelve
// finstrate slots that hold the CPU (slot 1) and IOP (slot 2). Boards
// came as 256K finstrates, 512K polystrates or 1M polystrates, in "any
// combination of 256K, 512K, and pairs of 1M" (CE Handbook; the 1 MB
// pairing rule is also the service manual's), and third-party
// "commercial" boards of the same capacities existed alongside HP's own.
// So the boards are slot cards here too: -mpb3 .. -mpb12 each take
// "256k", "512k" or "1m", the default being a pair of 1 MB boards -- a
// plain mid-range 520. Sizing this honestly matters beyond appearances:
// the boot is handed the top of memory and the language system sizes
// everything from it, so the 16 MB this driver used to claim -- a
// machine HP never built -- put BASIC's control blocks at addresses no
// real 520 has.
class mpb_ram_device : public device_t
{
public:
	mpb_ram_device(const machine_config &mconfig, device_type type,
			const char *tag, device_t *owner, u32 clock, u32 size)
		: device_t(mconfig, type, tag, owner, clock)
		, m_size(size)
	{
	}

	u32 size() const { return m_size; }
	u8 *base() { return m_ram.data(); }

protected:
	virtual void device_start() override
	{
		m_ram.resize(m_size, 0);
		save_item(NAME(m_ram));
	}

private:
	const u32 m_size;
	std::vector<u8> m_ram;
};

class mpb_ram256k_device : public mpb_ram_device
{
public:
	mpb_ram256k_device(const machine_config &mconfig, const char *tag,
			device_t *owner, u32 clock)
		: mpb_ram_device(mconfig, MPB_RAM256K, tag, owner, clock, 0x40000) {}
};

class mpb_ram512k_device : public mpb_ram_device
{
public:
	mpb_ram512k_device(const machine_config &mconfig, const char *tag,
			device_t *owner, u32 clock)
		: mpb_ram_device(mconfig, MPB_RAM512K, tag, owner, clock, 0x80000) {}
};

class mpb_ram1m_device : public mpb_ram_device
{
public:
	mpb_ram1m_device(const machine_config &mconfig, const char *tag,
			device_t *owner, u32 clock)
		: mpb_ram_device(mconfig, MPB_RAM1M, tag, owner, clock, 0x100000) {}
};

DEFINE_DEVICE_TYPE(MPB_RAM256K, mpb_ram256k_device, "mpb_ram256k",
		"HP 97047 256K RAM board")
DEFINE_DEVICE_TYPE(MPB_RAM512K, mpb_ram512k_device, "mpb_ram512k",
		"HP 97047 512K RAM board")
DEFINE_DEVICE_TYPE(MPB_RAM1M, mpb_ram1m_device, "mpb_ram1m",
		"HP 1M RAM board")

class mpb_slot_device : public device_t, public device_slot_interface
{
public:
	template <typename T>
	mpb_slot_device(const machine_config &mconfig, const char *tag,
			device_t *owner, T &&opts, const char *dflt)
		: mpb_slot_device(mconfig, tag, owner, u32(0))
	{
		option_reset();
		opts(*this);
		set_default_option(dflt);
		set_fixed(false);
	}

	mpb_slot_device(const machine_config &mconfig, const char *tag,
			device_t *owner, u32 clock)
		: device_t(mconfig, MPB_SLOT, tag, owner, clock)
		, device_slot_interface(mconfig, *this)
	{
	}

	mpb_ram_device *card() { return dynamic_cast<mpb_ram_device *>(get_card_device()); }

protected:
	virtual void device_start() override {}
};

DEFINE_DEVICE_TYPE(MPB_SLOT, mpb_slot_device, "mpb_slot", "MPB stack slot")

static void mpb_ram_cards(device_slot_interface &device)
{
	device.option_add("256k", MPB_RAM256K);
	device.option_add("512k", MPB_RAM512K);
	device.option_add("1m", MPB_RAM1M);
}

//**************************************************************************
//  SHARED CIO BACKPLANE SLOT
//**************************************************************************

// The 27128A ASI and 27110A HP-IB cards are the same physical CIO card
// family on both machines -- the 530/540 CE Handbook fits the ASI as the
// console at slot 0 and the HP-IB at slot 5, and the 520 service manual
// gives its own four backplane slots (SC 2-5) to "the only four available
// to the user" -- and this is that one slot: -cio2 through -cio5 on the
// 520, in either machine sharing the same card devices and the same
// option list, so a card proven correct in one machine's slot is the
// identical device in the other's.
//
// focus::asi_card and focus::hpib_card already implement iop_interface
// on their own account (they were written before there was a slot to plug
// them into); the wrappers here are just enough MAME device to make them
// selectable -findslots/-cio2 options, exposing the interface pointer the
// driver attaches to the IOP channel.
class device_focus_cio_interface : public device_interface
{
public:
	device_focus_cio_interface(const machine_config &mconfig, device_t &device)
		: device_interface(device, "focuscio")
	{
	}
	virtual focus::iop_interface &focus_iface() = 0;
	// Cards whose firmware runs on its own -- the serial card assembling
	// a record, the HP-IB card working through a transfer -- get a call
	// per IOP tick. A card that is purely register-driven ignores it.
	virtual void focus_tick() { }
	// Called once from machine_start with the select code the card was
	// plugged into: the card reads its own knobs, names its own log and
	// picks up its own medium. What genuinely needs the machine around it
	// -- binding a serial card to the console -- stays with the driver.
	virtual void focus_configure(focus::iop_core &iop, unsigned sc) { }
};

class hp27128_asi_device : public device_t, public device_focus_cio_interface
{
public:
	hp27128_asi_device(const machine_config &mconfig, const char *tag,
			device_t *owner, u32 clock)
		: device_t(mconfig, HP27128_ASI, tag, owner, clock)
		, device_focus_cio_interface(mconfig, *this)
	{
	}

	focus::asi_card card;

protected:
	virtual void device_start() override {}

public:
	virtual focus::iop_interface &focus_iface() override { return card; }
	virtual void focus_tick() override { card.tick(); }
	virtual void focus_configure(focus::iop_core &iop, unsigned sc) override
	{
		card.iop = &iop;
		card.pa = sc;
		arm_log(card.log, "asi", sc, "FOCUS_ASILOG");
		// FOCUS_ASILOGMAX raises the line cap: a driver polling
		// status outruns 2000 lines long before the interesting
		// part of a dialogue.
		card.log.set_cap(unsigned(
				knob::number("FOCUS_ASILOGMAX", card.log.cap())));
		// FOCUS_ASIID answers a different card identity -- 5 is the
		// 27130A eight-channel multiplexer, which HP-UX drives with
		// the same module (HP27128/30).
		card.card_id = u8(knob::number("FOCUS_ASIID", card.card_id));
		card.fr1_force = knob::number("FOCUS_ASIFR1", card.fr1_force);
		card.identity = u16(knob::number("FOCUS_ASIIDENT", card.identity));
		card.event_done =
				u8(knob::number("FOCUS_ASIEVENT", card.event_done));
		// FOCUS_ASICFGEVENT sweeps the answer to an FR9 write; the
		// console driver only tests its high nibble.
		card.event_configured =
				u8(knob::number("FOCUS_ASICFGEVENT", card.event_configured));
		// FOCUS_ASISWITCHES sets the card's configuration switch
		// byte, which the driver reads with subfunction 253.
		card.switches =
				u8(knob::number("FOCUS_ASISWITCHES", card.switches));
		// FOCUS_ASIRTS / FOCUS_ASIEVENT2 sweep what the card reports
		// through Read Transparent Status -- the opcode, and the
		// event code it carries under AES.
		// FOCUS_ASIEVDELAY sets how many milliseconds of card time
		// pass before a completion event is announced.
		card.event_delay_ms =
				unsigned(knob::number("FOCUS_ASIEVDELAY", card.event_delay_ms));
		// FOCUS_ASIPAUSE sets how long a Pause order pauses, which
		// is what paces the driver's polling loop.
		card.pause_ms =
				unsigned(knob::number("FOCUS_ASIPAUSE", card.pause_ms));
		card.clc_report =
				u8(knob::number("FOCUS_ASICLCRTS", card.clc_report));
		card.lcn_force = u16(knob::number("FOCUS_ASILCN", card.lcn_force));
		card.rts_opcode = u8(knob::number("FOCUS_ASIRTS", card.rts_opcode));
		card.rts_event = u8(knob::number("FOCUS_ASIEVENT2", card.rts_event));
		// FOCUS_ASINOASYNC silences the asynchronous events the
		// firmware senses -- the transmit-buffer-empty the console
		// drain waits for, and the data-available a typed record
		// raises. The A/B control for docs/HPUX.md 8.
		card.no_async = knob::flag("FOCUS_ASINOASYNC").on();
		// FOCUS_ASIMPX sweeps the multiplexing capability in the
		// identity block: 0 subchannel only, 1 in-channel only,
		// 2 both (27128A 4-67; the ASI ships configured with 1).
		card.mpx = u8(knob::number("FOCUS_ASIMPX", card.mpx));
	}
};

// The 27110A and the drive on its bus. The medium belongs to the card,
// not to the machine: a 540's system disc is a CS/80 drive cabled to this
// controller, and mounting it anywhere else would be describing a machine
// HP did not build. So the card carries the image device, and the disc
// mounts as -hard1 because the card is the only thing in the system that
// has one.
class hp27110_hpib_device : public device_t, public device_focus_cio_interface
{
public:
	hp27110_hpib_device(const machine_config &mconfig, const char *tag,
			device_t *owner, u32 clock)
		: device_t(mconfig, HP27110_HPIB, tag, owner, clock)
		, device_focus_cio_interface(mconfig, *this)
		, m_media(*this, "drive")
	{
	}

	focus::hpib_card card;

	hp_focus_media_device &media() { return *m_media; }

protected:
	virtual void device_start() override {}
	virtual void device_add_mconfig(machine_config &config) override ATTR_COLD
	{
		HP_FOCUS_MEDIA(config, m_media).set_hard();
	}

private:
	required_device<hp_focus_media_device> m_media;

public:
	virtual focus::iop_interface &focus_iface() override { return card; }
	virtual void focus_tick() override { card.tick(); }
	virtual void focus_configure(focus::iop_core &iop, unsigned sc) override
	{
		card.iop = &iop;
		card.pa = sc;
		arm_log(card.log, "hpib", sc, "FOCUS_HPIBLOG");
		card.identity = u16(knob::number("FOCUS_HPIBIDENT", card.identity));
		card.device_address =
				u8(knob::number("FOCUS_HPIBADDR", card.device_address));
		// The drive on this card's bus. Its medium is the card's own
		// image device; FOCUS_HPIBDISC remains as a way to hand a raw
		// file to a card without going through MAME's media system.
		if (media().has_media())
			card.disc.image = media().data();
		if (const char *v = knob::text("FOCUS_HPIBDISC").get())
		{
			bool short_read = false;
			if (!read_file(v, card.disc.image, short_read))
				osd_printf_error("FOCUS_HPIBDISC: cannot open %s\n", v);
			else if (short_read)
				osd_printf_error("FOCUS_HPIBDISC: short read on %s\n", v);
		}
		if (card.disc.loaded())
			osd_printf_info("cio%u: CS/80 drive at HP-IB address %u, "
					"%u blocks\n", sc, card.device_address,
					card.disc.blocks());
		iop.enable_interrupt(sc);
	}
};

DEFINE_DEVICE_TYPE(HP27128_ASI, hp27128_asi_device, "hp27128_asi",
		"HP 27128A Async Serial Interface")
DEFINE_DEVICE_TYPE(HP27110_HPIB, hp27110_hpib_device, "hp27110_hpib",
		"HP 27110A HP-IB Interface")

class focus_cio_slot_device : public device_t, public device_slot_interface
{
public:
	template <typename T>
	focus_cio_slot_device(const machine_config &mconfig, const char *tag,
			device_t *owner, T &&opts, const char *dflt)
		: focus_cio_slot_device(mconfig, tag, owner, u32(0))
	{
		option_reset();
		opts(*this);
		set_default_option(dflt);
		set_fixed(false);
	}

	focus_cio_slot_device(const machine_config &mconfig, const char *tag,
			device_t *owner, u32 clock)
		: device_t(mconfig, FOCUS_CIO_SLOT, tag, owner, clock)
		, device_slot_interface(mconfig, *this)
	{
	}

	// null when the slot is empty -- an unpopulated backplane slot, which
	// the 520 manual's "only four available to the user" makes a normal,
	// expected state, not an error.
	focus::iop_interface *iface()
	{
		device_focus_cio_interface *const c =
				dynamic_cast<device_focus_cio_interface *>(get_card_device());
		return c ? &c->focus_iface() : nullptr;
	}

	// Hand the card its select code and let it read its own knobs. An
	// empty slot has nothing to configure.
	void focus_configure(focus::iop_core &iop, unsigned sc)
	{
		if (device_focus_cio_interface *const c =
				dynamic_cast<device_focus_cio_interface *>(get_card_device()))
			c->focus_configure(iop, sc);
	}

protected:
	virtual void device_start() override {}
};

DEFINE_DEVICE_TYPE(FOCUS_CIO_SLOT, focus_cio_slot_device, "focus_cio_slot",
		"CIO backplane slot")

// Shared between both machines' slot lists, so the option a 520 -cio3 and
// a 540 console slot offer is the same function, not a parallel copy of
// one. GPIO (27112A) and the multiplexer (27130A) are documented in
// HARDWARE.md but have no card model yet, so they are not listed --
// adding one here is what plugs it into both machines at once.
static void focus_cio_cards(device_slot_interface &device)
{
	device.option_add("asi", HP27128_ASI);
	device.option_add("hpib", HP27110_HPIB);
}



namespace {

class hp9000_5xx_state : public driver_device, public focus::iop_core::host
{
public:
	hp9000_5xx_state(const machine_config &mconfig, device_type type,
			const char *tag)
		: driver_device(mconfig, type, tag)
		, m_cpu(*this, "cpu")
		, m_terminal(*this, "terminal")
		, m_keyboard(*this, "keyboard")
		, m_screen(*this, "screen")
		, m_prn(*this, "printer")
		, m_floppy(*this, "floppy%u", 1U)
		, m_hard(*this, "harddisc")
		, m_mpb(*this, "mpb%u", 3U)
		, m_cio(*this, "cio%u", 0U)
		, m_iop(*this, 2)   // the channel the boot handoff's inventory announces
	{
	}

	void add_processor_stack(machine_config &config) ATTR_COLD;
	void hp9020(machine_config &config) ATTR_COLD;
	void hp9040(machine_config &config) ATTR_COLD;

protected:
	virtual void machine_start() override ATTR_COLD;
	virtual void machine_reset() override ATTR_COLD;
	virtual void machine_stop() ATTR_COLD;

private:
	void mem_map(address_map &map) ATTR_COLD;
	void kbd_put(u8 data);
	void attach_probes();
	// The one fit rule: an image boots if it, the loader-stack window and
	// a floor for the relocated world all fit in installed RAM. The floor
	// is a sanity bound, not a promise -- an image whose relocated world
	// outgrows [0, load) still dies in relocation, as it would on real
	// hardware short of memory.
	static constexpr u32 BOOT_STACK_MIN = 0x3000;
	bool boot_image_fits(u32 size) const
	{
		return u64(size) + BOOT_STACK_MIN + 0x40000 <= m_ram_total;
	}
	void install_boot_image(const std::vector<u8> &img, const char *whence);
	bool boot_from_disc();
	bool boot_from_disc_source(bool from_fixed);
	bool boot_from_cs80(focus::hpib_card &card);
	// The HP-IB card carrying the system disc, or null when no slot has
	// one -- a 540 with nothing to boot from.
	focus::hpib_card *system_disc();
	void write_back(const std::vector<u8> &now, hp_focus_media_device &media,
			const char *what);
	u32 screen_update(screen_device &screen, bitmap_rgb32 &bitmap,
			const rectangle &cliprect);
	void gfx_dump(screen_device &screen);

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
	TIMER_CALLBACK_MEMBER(iop_poll_tick);
	void deliver_iop_interrupts();
	TIMER_CALLBACK_MEMBER(rtc_tick);
	TIMER_CALLBACK_MEMBER(type_tick);
	TIMER_CALLBACK_MEMBER(type_start);

	QUICKLOAD_LOAD_MEMBER(quickload_cb);

	required_device<focus_device> m_cpu;
	optional_device<generic_terminal_device> m_terminal;
	optional_device<generic_keyboard_device> m_keyboard;
	optional_device<screen_device> m_screen;
	optional_device<printer_image_device> m_prn;
	optional_device_array<hp_focus_media_device, 3> m_floppy;
	optional_device<hp_focus_media_device> m_hard;
	optional_device_array<mpb_slot_device, 10> m_mpb;
	u32 m_ram_total = 0;
	std::array<u64, 0x4000> m_pcprof{};
	u64 m_pcprof_n = 0;
	// The I/O backplane, indexed BY SELECT CODE -- the slots are
	// physically labelled with their numbers (530/540 Service 2-27), so
	// -cio5 is select code 5 on either machine. A 520 has four of them,
	// 2-5, "the only four available to the user" (520 Service 2-29),
	// because its display, keyboard and disc occupy the rest; a 540 has
	// no built-in peripherals at all and offers 0-6. Absent entries are
	// null, which is what an unpopulated slot is.
	optional_device_array<focus_cio_slot_device, 7> m_cio;
	// The console serial card, once a slot on a 530/540 turns out to
	// hold one: what the terminal window types into and what prints.
	focus::asi_card *m_console_asi = nullptr;

	focus::iop_core m_iop;
	focus::scm_device m_scm;
	focus::dim_card m_dim;            // select code 0: the display
	focus::gfx_card m_gfx;            // select code 1: display graphics
	focus::printer_device m_printer;   // the optional internal thermal printer
	focus::console_card m_console6;   // select code 6: keyboard board
	focus::disc_card m_disc;          // select code 7: the internal disc
	focus::probe_card m_probes[8];    // logging stand-ins for empty slots
	// Everything the driver keeps for its own instruments and none of it
	// the machine: the trails a report is written from, the addresses and
	// moments the knobs are waiting for, and whether any of them asked to
	// run at all. debug_pre_step() is what fills it; report_state() and
	// trip_watch() are what read it.
	struct focus_debug
	{
		bool armed = false;
		std::string trip, trip_seen;
		bool tripped = false;
		bool qcheck_hit = false;
		std::array<std::pair<u32, u32>, 10> qtrail{};
		size_t qtrail_at = 0;
		u32 qtrail_last = 0, qtrail_prev_p = 0;
		// The instructions just executed, with the stack pointer each one
		// left behind. A trap reports the instruction it stopped BEFORE --
		// the end-of-instruction conditions are tested between instructions
		// -- so the one that actually caused it is the entry above the last.
		std::array<std::pair<u32, u32>, 12> pctrail{};
		size_t pctrail_at = 0;
		u32 break_at = 0;
		unsigned break_hits = 0, break_max = 1;
		bool traced = false;
		u32 trace_left = 0;
		u32 wpoint = 0, wpoint_last = 0;
		unsigned traps_seen = 0, escapes = 0;
		u32 ea_at = 0;
		unsigned ea_want = 6;
		// What FOCUS_EATRACE's count is refilled to before every
		// instruction, which is why the trace does not stop after n
		// addresses. See where it is used: the refill is a bug this pass
		// preserved rather than fixed, because fixing it changes what the
		// knob prints.
		unsigned ea_refill = 0;
		bool ea_armed = false;
		u32 watch_lo = 0, watch_hi = 0;
		double watch_from = 0.0;
		double break_from = 0.0;
		double wp_from = 0.0;
		bool runaway = false, ran_away = false;
		std::array<std::pair<u32, u32>, 48> trail{};
		size_t trail_at = 0;
		// FOCUS_POKE requests, fired by a timer rather than the hook.
		std::vector<std::pair<u32, u32>> pokes;
	};
	focus_debug m_dbg;
	// Which routine touched the card: segment and offset, for the device
	// logs. For an interface no manual describes this is most of the
	// answer -- the FR number says what register, the caller says what
	// the register is FOR. Four cards ask for it, all the same way.
	std::string where_now() const
	{
		const focus::cpu_core &c = m_cpu->core();
		char buf[48];
		std::snprintf(buf, sizeof buf, "  seg%u+%04x",
				c.r.get(focus::sf::CSN), c.r.p - c.r.pb);
		return std::string(buf);
	}
	void trip_watch(u8 c);
	void close_print_text() { if (m_print_text) std::fclose(m_print_text); }
	std::FILE *m_print_text = nullptr;
	void report_state(const char *why);
	// machine_start, in the order it does them.
	void install_ram_boards();
	void load_batteries();
	void configure_display();
	void configure_peripherals();
	void configure_backplane();
	void parse_debug_knobs();
	void install_debug_hooks();
	void start_timers();
	void schedule_scripts();
	static bool debug_armed();
	void debug_pre_step(focus::cpu_core &core);
	// Whether any instrument asked to run, settled once at machine_start.
	void memory_find();
	TIMER_CALLBACK_MEMBER(find_tick);
	TIMER_CALLBACK_MEMBER(poke_tick);
	TIMER_CALLBACK_MEMBER(state_tick);
	emu_timer *m_iop_timer = nullptr;
	emu_timer *m_iop_poll_timer = nullptr;
	emu_timer *m_rtc_timer = nullptr;
	emu_timer *m_type_timer = nullptr;
	std::string m_type_text;
	size_t m_type_pos = 0;
	// FOCUS_TYPE may hold several scripts, each with its own start time.
	std::vector<std::string> m_type_scripts;
	attotime m_type_rate;
	u32 m_prev_rq = 0;
	// The discs in the drive, in the order the loader asks for them: a
	// boot image is a set, and the operator feeds the volumes one by one.
	std::vector<std::vector<u8>> m_media;
	unsigned m_boot_volumes = 0;   // how many of them the image needed
	// The 530/540 have no integrated display: select code 0 carries a
	// serial interface card and the console is a terminal on the end of
	// it. The 520 puts its display alphanumeric there instead.
	bool m_console_is_asi = false;
	void term_put(u8 data);
	void save_nv();
	void seed_clocks();
	void load_battery(focus::rtc_nvm &part, const char *knobname, unsigned sc);
	void save_battery(focus::rtc_nvm &part, const char *knobname, unsigned sc,
			const char *why, bool loud);
	knob::device_log m_nv_log;
	unsigned m_nv_save_period = 0;   // seconds between write-backs
	unsigned m_nv_save_phase = 0;
};



// The character cell the display's CRTC asks for: 15 scan lines tall, and
// eight pixels wide for the font this build draws with.
// FOCUS_TYPE's marker for "the next byte is a keycode, not a character".
// 0x01 cannot appear in ordinary script text.
static constexpr u8 RAW_KEY = 0x01;

static constexpr unsigned CELL_WIDTH = 8;
static constexpr unsigned CELL_HEIGHT = 15;
static constexpr unsigned SCREEN_ROWS = 26;

// The screen holds both planes at once. The alphanumeric one is 80 cells
// of 8 x 15 -- 640 x 390, and the 8-wide cell is this emulator's, not the
// machine's, because the character ROM is not dumped (focusfont.h); the
// graphics raster is the 560 x 455 HP98770's own bind plants as its
// maximum X and Y. So the screen is as wide as the alpha plane and as
// tall as the graphics one, with the narrower graphics raster centred
// across it.
//
// Where the two line up vertically is a PRESENTATION CHOICE and not a
// measurement: 26 rows of 15 scan lines is 390 and the graphics raster is
// 455, and nothing in the collection says how a real display registers
// one against the other -- that is in 98770-90031, which nobody has. Both
// start at the top.
static constexpr unsigned SCREEN_WIDTH = focus::dim_card::COLUMNS * CELL_WIDTH;
static constexpr unsigned SCREEN_HEIGHT = focus::gfx_card::HEIGHT;
static constexpr unsigned GFX_LEFT = (SCREEN_WIDTH - focus::gfx_card::WIDTH) / 2;

void hp9000_5xx_state::mem_map(address_map &map)
{
	// RAM comes from the boards in the MPB slots, installed at
	// machine_start; the map itself carries nothing.
	map.unmap_value_high();
}


// Watch the character stream going to the screen for the trip text; when
// it arrives, report where the processor is and who called it. The stack
// marker at Q holds delta-Q, STATUS, the PB-relative return address and
// X in the four words from Q downward (Instruction Set ERS sheet 121),
// so the chain walks back a frame at a time.
void hp9000_5xx_state::trip_watch(u8 c)
{
	if (m_dbg.tripped || m_dbg.trip.empty())
		return;
	m_dbg.trip_seen.push_back(char(c));
	if (m_dbg.trip_seen.size() > m_dbg.trip.size())
		m_dbg.trip_seen.erase(0, m_dbg.trip_seen.size() - m_dbg.trip.size());
	if (m_dbg.trip_seen != m_dbg.trip)
		return;

	m_dbg.tripped = true;
	report_state(m_dbg.trip.c_str());
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
	if (const char *e = knob::text("FOCUS_FIND").get())
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
		for (u32 a = 0; a + want.size() <= m_ram_total && found < 12; a++)
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
	{
		// The message state, for asking why a raised message never traps:
		// delivery wants STATUS.I, an unmasked bit, and a zero PSDB count
		// in the ICS marker.
		const u32 qi = core.read_mem32(16 * focus::ers32::get(core.r.mask, 3, 3) + 8);
		std::printf("  MSG=%08x MASK=%08x I=%d QI=%08x psdb=%08x\n",
				core.r.msg, core.r.mask, core.r.test(focus::sf::I) ? 1 : 0,
				qi, qi ? core.read_mem32(qi - 3 - 16) : 0);
	}
	// Where the processor has just been, oldest first, with the stack
	// pointer as each instruction found it. S jumping between two entries
	// names the earlier one as the instruction that moved it, which is
	// what a trap cannot say for itself: the end-of-instruction conditions
	// are tested before the NEXT instruction, so the trap reports that one.
	std::printf("  last instructions (P, S on entry to it):");
	for (size_t i = 0; i < m_dbg.pctrail.size(); i++)
	{
		const auto &e = m_dbg.pctrail[(m_dbg.pctrail_at + i) % m_dbg.pctrail.size()];
		if (e.first)
			std::printf("%s    %08x S=%08x", (i % 3) ? "" : "\n",
					e.first, e.second);
	}
	std::printf("\n");
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
	if (const char *e = knob::text("FOCUS_DUMP").get())
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

	if (knob::flag("FOCUS_FIND") && !knob::flag("FOCUS_FINDAT"))
		memory_find();

	// The code segment table says which module each frame is in: entry
	// zero's first word is sixteen times the entry count, and every entry
	// carries its length at +4 and its base at +8 (ERS sheets 25-27).
	const u32 cst = core.read_mem32(0);
	if (cst)
	{
		const u32 count = core.read_mem32(cst) / 16;
		// The cap used to be a silent 128, and BASIC's CST has 300
		// entries -- both of the scaffold's MAIN segments live above the
		// cut, so the table simply did not show the thing being hunted.
		// FOCUS_CSTMAX raises it, and the count of what was dropped is
		// printed either way so nothing is hidden without saying so.
		static const u32 cst_max = u32(knob::number("FOCUS_CSTMAX", 128));
		std::printf("  CST at %08x, %u entries\n", cst, count);
		for (u32 seg = 1; seg < count && seg < cst_max; seg++)
		{
			const u32 ea = cst + 16 * seg;
			const u32 len = core.read_mem32(ea + 4);
			const u32 base = core.read_mem32(ea + 8);
			if (!base && !len)
				continue;
			// A code segment carries its own name: the module header the
			// linker wrote is still in front of the code, and CST base
			// points at it (base+20 is the 16-byte space-padded name,
			// base+36 the first instruction). Printing it turns a table
			// of numbers into the machine's list of loaded modules --
			// which is the question being asked whenever this table is
			// read at all. Data segments have no such header, so the
			// name is only printed when it really is one.
			char name[17] = {};
			for (unsigned k = 0; k < 16; k++)
				name[k] = char(core.read_mem8(base + 20 + k));
			bool named = name[0] > ' ';
			for (unsigned k = 0; k < 16; k++)
				if (u8(name[k]) < ' ' || u8(name[k]) > '~')
					named = false;
			while (named && name[0] && name[std::strlen(name) - 1] == ' ')
				name[std::strlen(name) - 1] = 0;
			std::printf("    seg %3u base=%08x len=%6u%s%s\n", seg, base, len,
					named ? "  " : "", named ? name : "");
		}
		if (count > cst_max)
		{
			unsigned dropped = 0;
			for (u32 seg = cst_max; seg < count; seg++)
			{
				const u32 ea = cst + 16 * seg;
				if (core.read_mem32(ea + 4) || core.read_mem32(ea + 8))
					dropped++;
			}
			if (dropped)
				std::printf("    (%u further live entries above %u not "
						"shown; FOCUS_CSTMAX to raise)\n", dropped, cst_max);
		}
	}

	u32 q = core.r.q;
	for (int frame = 0; frame < 24 && q >= core.r.sb && q <= core.r.sl; frame++)
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
	const char *e = knob::text("FOCUS_PROBE").get();
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
		arm_log(m_probes[sc].log, "probe", sc, "FOCUS_PROBELOG");
		m_probes[sc].fr0_answer = knob::number("FOCUS_PROBEFR0", m_probes[sc].fr0_answer);
		m_probes[sc].fr1_mode = knob::number("FOCUS_PROBEFR1", m_probes[sc].fr1_mode);
		m_probes[sc].where = [this]() { return where_now(); };
		m_iop.attach(sc, &m_probes[sc]);
		if (!comma)
			break;
		p = comma + 1;
	}
}


// Is any instrument asking to run? A knob belongs in this list when the
// hook below does work for it every instruction, OR when it can reach
// report_state(), whose "last instructions" section is read straight out
// of the trail the hook keeps -- arm one without the other and the report
// comes out blank.
//
// Deliberately absent, because their handlers print registers or memory
// and never touch the trail: FOCUS_EAWATCH and FOCUS_SEMLOG (hooks the
// core calls directly), FOCUS_RWATCH and FOCUS_WWATCH (memory hooks, and
// they live in focus.cpp), FOCUS_FIND and FOCUS_FINDAT. Nothing reports
// at exit either -- machine_stop only writes the discs back -- so a run
// with none of these set never reads what the hook would have recorded.
bool hp9000_5xx_state::debug_armed()
{
	return knob::flag("FOCUS_PCPROF")        // the bucket counts
		|| knob::flag("FOCUS_QCHECK")           // and its report
		|| knob::flag("FOCUS_RUNAWAY")          // keeps the P/PB trail
		|| bool(knob::text("FOCUS_WATCH"))      // tests P every instruction
		|| bool(knob::text("FOCUS_WPOINT"))     // re-reads the word
		|| bool(knob::text("FOCUS_EATRACE"))    // arms, and refills the count
		|| bool(knob::text("FOCUS_POKEPC"))     // waits for its instruction
		|| bool(knob::text("FOCUS_BREAK"))      // and its report
		|| bool(knob::text("FOCUS_TRACEN"))     // prints after the break
		|| bool(knob::text("FOCUS_TRIP"))       // trip_watch -> report_state
		|| bool(knob::text("FOCUS_STATEAT"))    // state_tick -> report_state
		|| bool(knob::text("FOCUS_TRAPLOG"))    // on_trap -> report_state
		|| knob::flag("FOCUS_ESCLOG");          // on_escape -> report_state
}


// What the driver does before every instruction, when an instrument has
// asked for it: the trails a report is written from, and the knobs that
// watch for a moment to arrive.
void hp9000_5xx_state::debug_pre_step(focus::cpu_core &core)
{
	// FOCUS_PCPROF buckets the program counter by 4 KB and prints the
	// busiest buckets every 40M instructions, so "where is the
	// emulated machine spending its time" is answered by measurement
	// rather than by reading code. It found the cycle-accounting bug
	// this comment sits above the fix for: the counts were far larger
	// than any real workload, which is what pointed at emulated time
	// running too fast rather than at any one routine.
	static const knob::flag pcprof("FOCUS_PCPROF");
	if (pcprof)
	{
		m_pcprof[(core.r.p >> 12) & 0x3fff]++;
		if (++m_pcprof_n == 40000000u)
		{
			std::printf("[pcprof: top buckets by instruction count]\n");
			std::vector<std::pair<u64, unsigned>> v;
			for (unsigned k = 0; k < 0x4000; k++)
				if (m_pcprof[k])
					v.push_back({m_pcprof[k], k});
			std::sort(v.rbegin(), v.rend());
			for (unsigned k = 0; k < 12 && k < v.size(); k++)
				std::printf("  P=%08x..%08x  %llu\n", v[k].second << 12,
						(v[k].second << 12) + 0xfff,
						(unsigned long long)v[k].first);
			std::fflush(stdout);
			m_pcprof_n = 0;
			for (auto &c : m_pcprof) c = 0;
		}
	}
	// A short trail of Q changes: which instruction moved Q last is
	// the question every corrupted-marker hunt ends at.
	if (core.r.q != m_dbg.qtrail_last)
	{
		m_dbg.qtrail[m_dbg.qtrail_at] = std::make_pair(m_dbg.qtrail_prev_p, core.r.q);
		m_dbg.qtrail_at = (m_dbg.qtrail_at + 1) % m_dbg.qtrail.size();
		m_dbg.qtrail_last = core.r.q;
	}
	m_dbg.qtrail_prev_p = core.r.p;
	m_dbg.pctrail[m_dbg.pctrail_at] = std::make_pair(core.r.p, core.r.s);
	m_dbg.pctrail_at = (m_dbg.pctrail_at + 1) % m_dbg.pctrail.size();
	// FOCUS_QCHECK reports the first instruction that runs with a
	// misaligned Q or a STATUS whose CSN field is impossible -- the
	// wreckage of a corrupted stack marker, caught at the moment of
	// restoration rather than at the crash it causes later.
	static const knob::flag qcheck("FOCUS_QCHECK");
	if (qcheck && !m_dbg.qcheck_hit && core.r.q != 0
			&& ((core.r.q & 3) != 3 || core.r.status == 0xfffffffcu))
	{
		m_dbg.qcheck_hit = true;
		std::printf("[qcheck: Q=%08x STATUS=%08x]\n",
				core.r.q, core.r.status);
		report_state("qcheck");
	}
	if (m_dbg.runaway)
	{
		if (!m_dbg.ran_away && (core.r.p >= m_ram_total || core.r.pb >= m_ram_total))
		{
			m_dbg.ran_away = true;
			std::printf("[runaway at %.3fs: P=%08x PB=%08x]\n",
					machine().time().as_double(), core.r.p, core.r.pb);
			std::printf("  last %u instructions, oldest first:\n",
					unsigned(m_dbg.trail.size()));
			for (size_t i = 0; i < m_dbg.trail.size(); i++)
			{
				const size_t k = (m_dbg.trail_at + i) % m_dbg.trail.size();
				if (m_dbg.trail[k].first)
					std::printf("    P=%08x PB=%08x\n",
							m_dbg.trail[k].first, m_dbg.trail[k].second);
			}
			report_state("runaway");
		}
		m_dbg.trail[m_dbg.trail_at] = std::make_pair(core.r.p, core.r.pb);
		m_dbg.trail_at = (m_dbg.trail_at + 1) % m_dbg.trail.size();
	}
	static knob::channel watch("FOCUS_WATCH", 12, "FOCUS_WATCHN");
	if (m_dbg.watch_hi && core.r.p >= m_dbg.watch_lo && core.r.p <= m_dbg.watch_hi
			&& watch
			&& machine().time().as_double() >= m_dbg.watch_from)
		watch("[%.2fs watch %08x  Q=%08x]\n",
				machine().time().as_double(), core.r.p, core.r.q);
	// FOCUS_EATRACE's count, refilled where the installer that used
	// to write it stood. Keeping it here is what makes hoisting the
	// installers a move rather than a change; see the installer for
	// why the refill is left in place.
	if (m_dbg.ea_refill)
		m_dbg.ea_want = m_dbg.ea_refill;
	// FOCUS_WPOINT=<addr> reports every change of one memory word with
	// the instruction that made it, which is how a table's builder is
	// found when the table is all there is to go on.
	if (m_dbg.wpoint && machine().time().as_double() >= m_dbg.wp_from)
	{
		const u32 now = core.read_mem32(m_dbg.wpoint);
		if (now != m_dbg.wpoint_last)
		{
			static knob::channel wp("FOCUS_WPOINT", 20);
			if (wp)
			{
				wp("[wp %08x: %08x -> %08x at P=%08x PB=%08x]\n",
						m_dbg.wpoint, m_dbg.wpoint_last, now, core.r.p, core.r.pb);
				// FOCUS_WPSTACK adds the call chain, which is how the
				// code that asked for a write gets named when the
				// writer itself is a shared helper.
				if (knob::flag("FOCUS_WPSTACK"))
					report_state("wp");
			}
			m_dbg.wpoint_last = now;
			std::fflush(stdout);
		}
	}
	// FOCUS_EATRACEFROM=<emulated seconds> delays the arming, so a
	// shared routine can be traced on a chosen later visit -- the
	// compile-time call and the run-time call of the same helper are
	// different questions.
	static const double ea_from = knob::seconds("FOCUS_EATRACEFROM", 0.0);
	// FOCUS_POKEPC=<pc>:<addr>:<hex value>[,...] writes a word the
	// first time the processor reaches an instruction, which is the
	// only way to reach a moment two events share -- the compiler's
	// copy and the copied code's first execution land in the same
	// emulated millisecond, so a timed poke cannot sit between them.
	{
		static bool parsed = false;
		static std::vector<std::pair<u32, std::pair<u32, u32>>> pcpokes;
		if (!parsed)
		{
			parsed = true;
			if (const char *e = knob::text("FOCUS_POKEPC").get())
				for (std::string list(e); !list.empty(); )
				{
					const size_t comma = list.find(',');
					const std::string one = list.substr(0, comma);
					list = (comma == std::string::npos)
							? "" : list.substr(comma + 1);
					const char *c1 = strchr(one.c_str(), ':');
					if (!c1) continue;
					const char *c2 = strchr(c1 + 1, ':');
					if (!c2) continue;
					pcpokes.push_back(std::make_pair(
							u32(strtoul(one.c_str(), nullptr, 16)),
							std::make_pair(u32(strtoul(c1 + 1, nullptr, 16)),
									u32(strtoul(c2 + 1, nullptr, 16)))));
				}
		}
		for (auto &pk : pcpokes)
			if (pk.first && core.r.p == pk.first)
			{
				const u32 before = core.read_mem32(pk.second.first);
				m_cpu->memory().write32(pk.second.first, pk.second.second);
				std::printf("[pokepc P=%08x: %08x: %08x -> %08x at %.4fs]\n",
						pk.first, pk.second.first, before, pk.second.second,
						machine().time().as_double());
				std::fflush(stdout);
				pk.first = 0;
			}
	}
	if (m_dbg.ea_at && core.r.p == m_dbg.ea_at
			&& machine().time().as_double() >= ea_from)
		m_dbg.ea_armed = true;
	if (m_dbg.traced && m_dbg.trace_left)
	{
		m_dbg.trace_left--;
		std::printf("[t %08x pb=%08x q=%08x]\n", core.r.p, core.r.pb, core.r.q);
		if (!m_dbg.trace_left)
			std::fflush(stdout);
	}
	if (m_dbg.break_at && core.r.p == m_dbg.break_at && m_dbg.break_hits < m_dbg.break_max
			&& machine().time().as_double() >= m_dbg.break_from)
	{
		m_dbg.traced = true;
		m_dbg.break_hits++;
		std::printf("\n[break %08x hit %u]\n", m_dbg.break_at, m_dbg.break_hits);
		std::printf("  last Q changes (instruction that moved it -> new Q):\n");
		for (size_t i = 0; i < m_dbg.qtrail.size(); i++)
		{
			const auto &e = m_dbg.qtrail[(m_dbg.qtrail_at + i) % m_dbg.qtrail.size()];
			if (e.second)
				std::printf("    P=%08x -> Q=%08x\n", e.first, e.second);
		}
		report_state("break");
		// Stack words sit on the q-3 grid: operand (Q-k) is the word
		// at byte q-k-3, so the window walks word slots from there.
		for (int k = -24; k <= 14; k++)
			std::printf("    Q%+3d (%08x) = %08x\n", 4 * k,
					core.r.q + 4 * k - 3,
					core.read_mem32(core.r.q + 4 * k - 3));
		// The stack top, where an instruction's operands actually
		// sit: (S) is the top word, (S-1) beneath it. A block move
		// takes its count, target and source from exactly here.
		for (int k = 0; k <= 5; k++)
			std::printf("    S-%d (%08x) = %08x\n", k,
					core.r.s - 4 * k - 3,
					core.read_mem32(core.r.s - 4 * k - 3));
		std::fflush(stdout);
	}
}


// The RAM boards, wired into the address space from the bottom of the
// processor stack upward. What is fitted decides where a boot image
// can land, so this runs before anything asks how much there is.
void hp9000_5xx_state::install_ram_boards()
{
	// Wire the RAM boards into the address space, bottom of the stack
	// first, each at the next free address -- and apply the manuals'
	// population rules as warnings, since a wrong stack on the real
	// machine simply fails self-test.
	{
		auto &space = m_cpu->space(AS_PROGRAM);
		unsigned onemeg = 0, boards = 0;
		for (auto &slot : m_mpb)
		{
			mpb_ram_device *card = slot ? slot->card() : nullptr;
			if (!card)
				continue;
			space.install_ram(m_ram_total, m_ram_total + card->size() - 1,
					card->base());
			m_ram_total += card->size();
			boards++;
			if (card->size() == 0x100000)
				onemeg++;
		}
		osd_printf_info("mpb: %u RAM board%s, %u KB\n", boards,
				boards == 1 ? "" : "s", m_ram_total / 1024);
		// FOCUS_RAMFILL=<hex byte> poisons all of RAM before the boot,
		// standing in for the garbage a real machine powers up with --
		// MAME's boards come up zeroed, and zeros silently satisfy any
		// code that reads memory nothing initialised. A boot that
		// behaves differently under poison is depending on exactly such
		// a read, which on hardware would differ run to run.
		if (const char *e = knob::text("FOCUS_RAMFILL").get())
		{
			const u8 fill = u8(strtoul(e, nullptr, 16));
			for (auto &slot : m_mpb)
				if (mpb_ram_device *card = slot ? slot->card() : nullptr)
					std::memset(card->base(), fill, card->size());
			osd_printf_info("mpb: RAM poisoned with 0x%02x\n", fill);
		}
		if (onemeg & 1)
			osd_printf_warning("mpb: 1M RAM boards must be installed in "
					"pairs (service manual 2-1); this stack has %u\n", onemeg);
		// No lower bound is enforced here: install_boot_image() places
		// the image to fit the fitted RAM and says so when it cannot.
	}
}


// The two battery-backed parts -- the SCM's clock and NVM at select
// code 7, and the keyboard board's -- and the knobs that poison or
// preserve them. FOCUS_TRIP is read here because it is a string the
// console watch compares against, not a hook.
void hp9000_5xx_state::load_batteries()
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
	// FOCUS_SCMNV=<file> is the battery behind the SCM's clock and
	// configuration memory, and FOCUS_KBDNV=<file> the same for the 520's
	// keyboard board: loaded here, written back at exit and periodically.
	// Without one every boot is a first boot, and HP-UX treats an unset
	// clock as reason to abandon its device bring-up.
	arm_log(m_nv_log, "nv", "FOCUS_NVLOG");
	m_nv_save_period = knob::number("FOCUS_NVSAVE", 30);
	// The manuals' four-second window on the battery-fail bit, off by
	// default because the emulator has always latched it for the run.
	const bool window = bool(knob::flag("FOCUS_RTCWINDOW"));
	m_scm.nv().battery_window = window;
	m_console6.nv.battery_window = window;
	load_battery(m_scm.nv(), "FOCUS_SCMNV", focus::scm_device::SELECT_CODE);
	load_battery(m_console6.nv, "FOCUS_KBDNV", 6);
	// A no-battery first run raises the fail bit on the SCM, which is what
	// the manual says the OS reads to decide to restore its configuration.
	// On the KEYBOARD board it must not be raised implicitly: the 520's own
	// power-up self test fails on it -- "Self test error 1: IO address
	// 006,0002", escape 4, "Battery low condition detected" (520 Service,
	// table C-1) -- so a first run there reports a good battery and an
	// unset clock, and FOCUS_RTCBATFAIL is how the other case gets tested.
	m_console6.nv.battery_fail = false;
	if (const char *e = knob::text("FOCUS_RTCBATFAIL").get())
	{
		const bool both = (*e == 0) || !std::strcmp(e, "both");
		if (both || !std::strcmp(e, "7")) m_scm.nv().battery_fail = true;
		if (both || !std::strcmp(e, "6")) m_console6.nv.battery_fail = true;
	}
	seed_clocks();
	// Written back at exit and at a reset, as well as periodically. Not
	// conditional on a knob: save_battery asks for itself.
	machine().add_notifier(MACHINE_NOTIFY_EXIT,
			machine_notify_delegate(&hp9000_5xx_state::save_nv, this));
	machine().add_notifier(MACHINE_NOTIFY_RESET,
			machine_notify_delegate(&hp9000_5xx_state::save_nv, this));
	m_dim.id = knob::number("FOCUS_SC0ID", m_dim.id);
	m_dim.log_fr.name("dim");
	m_dim.log_fr.enable(bool(knob::flag("FOCUS_SC0LOG")));
	m_dim.log_cells.name("dim");
	m_dim.log_cells.enable(bool(knob::flag("FOCUS_DIMCELLS")));
	// FOCUS_TRIP=<text> prints the processor state and a walk back up the
	// stack markers the moment that text reaches the screen, which is how
	// the code behind a message -- an error report, say -- gets found.
	if (const char *e = knob::text("FOCUS_TRIP").get())
	{
		m_dbg.trip = e;
		m_dim.on_cell = [this](u16 v) { trip_watch(u8(v)); };
	}
	m_dim.fr1 = knob::number("FOCUS_SC0FR1", m_dim.fr1);
}


// Select code 0 and 1 on a machine that has them: the display
// alphanumeric and Display Graphics.
void hp9000_5xx_state::configure_display()
{
	// Select code 0 is the display alphanumeric on the 520. On the
	// 530/540 it is a backplane slot like any other and the console
	// serial card lives in it, which the loop over the backplane below
	// fits along with every other card.
	if (!m_console_is_asi)
		m_iop.attach(0, &m_dim);

	// Select code 1 is Display Graphics on a machine with the display
	// built in (520 Service, sheet 2-29). FOCUS_SC1FR1 is the status
	// word the whole bring-up keys on -- see focusgfx.h, where every
	// gate that reads it is named; FOCUS_SC1ID is FR3, whose low byte
	// the powerup scan requires to be zero.
	m_gfx.fr1 = knob::number("FOCUS_SC1FR1", m_gfx.fr1);
	m_gfx.id = knob::number("FOCUS_SC1ID", m_gfx.id);
	arm_log(m_gfx.log, "gfx", "FOCUS_SC1LOG");
	m_gfx.where = [this]() { return where_now(); };
	// The memory behind the commands, and the raster its words mean.
	// Both numbers came off the self test's own transfer lengths rather
	// than a manual, so both stay sweepable: 3 planes of 36-word lines
	// is the 98770A colour display, 1 plane of 35-word lines the 98780A
	// monochrome one.
	m_gfx.planes = std::min<unsigned>(
			knob::number("FOCUS_GFXPLANES", m_gfx.planes),
			focus::gfx_card::MAX_PLANES);
	if (!m_gfx.planes)
		m_gfx.planes = 1;
	m_gfx.stride = knob::number("FOCUS_GFXSTRIDE", m_gfx.stride);
	m_gfx.origin = knob::number("FOCUS_GFXORIGIN", m_gfx.origin, 16);
	m_gfx.nomem = bool(knob::flag("FOCUS_GFXNOMEM"));
	m_gfx.iop = &m_iop;
	if (!m_console_is_asi && !knob::flag("FOCUS_NOGFX"))
	{
		m_iop.attach(1, &m_gfx);
		m_iop.enable_interrupt(1);
	}

	m_console6.id = 2;   // keyboard ID register: data bus bit 1
	m_console6.log_fr.name("sc", 6);
	m_console6.log_fr.enable(bool(knob::flag("FOCUS_KBDLOG")));
	// Poison the configuration memory on BOTH boards that carry one: the
	// 520 keeps it on the keyboard board and the 530/540 on the SCM, and
	// the knob has always claimed to reach the SCM's while reaching only
	// the keyboard board's.
	if (const knob::number v{"FOCUS_NVMFILL", 0}; v.set())
	{
		m_console6.fill_nvm(u8(v));
		m_scm.fill_nvm(u8(v));
	}
	m_console6.fr1_base = knob::number("FOCUS_SC6FR1", m_console6.fr1_base);
	m_console6.st = knob::number("FOCUS_KBDST", m_console6.st);
	m_console6.iop = &m_iop;
	m_console6.pa = 6;
}


// The printer, the keyboard board and the internal disc: the
// peripherals built into a 520 rather than plugged into it.
void hp9000_5xx_state::configure_peripherals()
{
	// The internal thermal printer is an option (CE Handbook 1-2), so it
	// is fitted only when asked for. It hangs off the keyboard board's
	// interface rather than a slot of its own -- select code 6 carries
	// both -- and FOCUS_PRINTTEXT takes the lossy text extraction while
	// the -prin image device takes the raw byte stream, which is the
	// artefact that actually represents what a graphics printer printed.
	if (knob::flag("FOCUS_PRINTER"))
	{
		m_printer.status = u8(knob::number("FOCUS_PRINTSTATUS", 0));
		m_printer.present = knob::number("FOCUS_PRINTPRESENT", 1) != 0;
		arm_log(m_printer.log, "prn", "FOCUS_PRINTLOG");
		m_printer.where = [this]() { return where_now(); };
		m_printer.on_byte = [this](u8 b) {
			if (m_prn)
				m_prn->output(b);
		};
		if (const char *e = knob::text("FOCUS_PRINTTEXT").get())
		{
			m_print_text = std::fopen(e, "w");
			if (!m_print_text)
				osd_printf_warning("printer: cannot write %s\n", e);
			else
				machine().add_notifier(MACHINE_NOTIFY_EXIT,
						machine_notify_delegate(
								&hp9000_5xx_state::close_print_text, this));
		}
		m_printer.on_line = [this](const std::string &line) {
			if (m_print_text)
			{
				std::fprintf(m_print_text, "%s\n", line.c_str());
				std::fflush(m_print_text);
			}
		};
		m_console6.printer = &m_printer;
	}
	m_console6.where = [this]() { return where_now(); };
	m_iop.attach(6, &m_console6);
	// SC6 keyboard interrupts are enabled by the hardware itself; the
	// internal disc's select code is armed the same way when present.
	m_iop.enable_interrupt(6);
	// The 520's internal disc is built-in hardware, always present, at
	// select code 7 -- where the 530/540 put the SCM instead.
	if (!m_console_is_asi)
		m_iop.enable_interrupt(7);

	// The 520's internal disc.
	if (!m_console_is_asi)
	{
		m_disc.iop = &m_iop;
		arm_log(m_disc.log, "disc", "FOCUS_DISCLOG");
		// FOCUS_DISCID: the halfword the identify transaction answers;
		// 1..6 map to internal device types, anything else types the
		// slot 29 (the kernel's default).
		m_disc.identity = u16(knob::number("FOCUS_DISCID", m_disc.identity));
		m_disc.id_fr3 = u16(knob::number("FOCUS_DISCFR3", m_disc.id_fr3));
		m_disc.id_fr3_alt = u16(knob::number("FOCUS_DISCFR3B", m_disc.id_fr3_alt));
		m_disc.busy_ms = knob::number("FOCUS_DISCBUSY", m_disc.busy_ms);
		// FOCUS_FLOPPY=<raw image>[,<raw image>...]: the discs in the
		// drive, one byte per byte (35 cyl x 2 heads x 16 sectors of 256
		// bytes = 286,720 for the media the system shipped on). A boot
		// image is a SET -- BASIC 2.0 is two volumes, the diagnostics
		// three -- and the loader works through them in order, which is
		// what an operator feeding discs to the drive does.
		if (const char *e = knob::text("FOCUS_FLOPPY").get())
			for (std::string list(e); !list.empty(); )
			{
				const size_t comma = list.find(',');
				const std::string path = list.substr(0, comma);
				list = (comma == std::string::npos) ? "" : list.substr(comma + 1);
				if (path.empty())
					continue;
				std::vector<u8> medium;
				bool short_read = false;
				if (!read_file(path.c_str(), medium, short_read))
					osd_printf_error("FOCUS_FLOPPY: cannot open %s\n",
							path.c_str());
				else if (short_read)
					osd_printf_error("FOCUS_FLOPPY: short read on %s\n",
							path.c_str());
				else
					m_media.push_back(std::move(medium));
			}
		if (!m_media.empty())
			m_disc.mount(m_media.front());
		// The internal fixed disc, when one is fitted: device 1 behind the
		// same select code, which then answers identity 5, a Winchester,
		// instead of 8, a bare bootstrap loader card. -hard1 mounts it.
		if (m_hard && m_hard->has_media())
		{
			m_disc.mount_fixed(m_hard->data());
			osd_printf_info("disc: fixed disc fitted, %u blocks\n",
					m_disc.fixed_blocks());
		}
		m_disc.reset_self_test();
		m_iop.attach(7, &m_disc);
	}

}


// The I/O backplane. One loop serves both machines because a card is
// a card wherever it is plugged in.
void hp9000_5xx_state::configure_backplane()
{
	// The I/O backplane. One loop serves both machines because a card is
	// one MAME device family in either: the array is indexed by select
	// code, so a 520 finds cards in 2-5 and a 540 in 0-6, and the same
	// FOCUS_ASI*/FOCUS_HPIB* debug knobs reach whichever card is in a
	// slot. On a 540 this is where the console and the system disc come
	// from -- there is no built-in anything to fall back on.
	for (unsigned sc = 0; sc < m_cio.size(); sc++)
	{
		if (!m_cio[sc])
			continue;
		focus::iop_interface *const iface = m_cio[sc]->iface();
		if (!iface)
			continue;
		m_cio[sc]->focus_configure(m_iop, sc);
		// The console is the serial card in slot 0 of a machine that
		// has no display: what it prints goes to the terminal window
		// and to stdout, and what the window types goes back into it.
		// This is the one thing a card cannot do for itself, because
		// it is not about the card -- it is about which card this
		// machine is listening to.
		if (auto *asi = dynamic_cast<hp27128_asi_device *>(
				m_cio[sc]->get_card_device()))
		{
			if (m_console_is_asi && sc == 0)
			{
				m_console_asi = &asi->card;
				asi->card.on_char = [this](u8 c) {
					trip_watch(c);
					if (m_terminal)
						m_terminal->write(c);
					std::printf("%c", (c >= 32 && c < 127) ? char(c) : '.');
					std::fflush(stdout);
				};
			}
			else
				asi->card.on_char = [this](u8 c) { trip_watch(c); };
		}
		m_iop.attach(sc, iface);
		osd_printf_info("cio%u: %s fitted at select code %u\n", sc,
				m_cio[sc]->get_card_device()->name(), sc);
	}
	attach_probes();
	m_cpu->set_io(&m_iop);
	m_iop.trace_pc = [this]() { return m_cpu->core().r.p; };
	m_iop.trace_time = [this]() { return machine().time().as_double(); };


}


// The knobs the pre-step hook tests every instruction, read once into
// the members it tests. Their hooks go in next; debug_armed() decides
// whether any of it runs at all.
void hp9000_5xx_state::parse_debug_knobs()
{
	// FOCUS_BREAK=<absolute address> reports the same state as the trip
	// wire the first time the processor reaches that instruction, with a
	// window of the current stack frame -- enough to read the arguments a
	// routine was called with.
	if (const char *e = knob::text("FOCUS_BREAK").get())
	{
		m_dbg.break_at = strtoul(e, nullptr, 16);
		if (const char *colon = strchr(e, ':'))
			m_dbg.break_max = strtoul(colon + 1, nullptr, 0);
		// FOCUS_BREAKFROM=<emulated seconds>, as for the watch: ignore
		// the boot's own visits to the address.
		m_dbg.break_from = knob::seconds("FOCUS_BREAKFROM", m_dbg.break_from);
	}
	// FOCUS_TRACEN=<n> lists the next n instructions after the breakpoint,
	// which is how a call gets followed into whatever it reaches.
	if (const char *e = knob::text("FOCUS_WPOINT").get())
	{
		m_dbg.wpoint = strtoul(e, nullptr, 16);
		// FOCUS_WPFROM=<emulated seconds> arms the watch late, as for the
		// execution watch: heap memory is recycled, and a write seen
		// before the interesting object exists at the address belongs to
		// some earlier tenant, not to the code being hunted.
		m_dbg.wp_from = knob::seconds("FOCUS_WPFROM", m_dbg.wp_from);
	}
	m_dbg.trace_left = knob::number("FOCUS_TRACEN", m_dbg.trace_left);

	// FOCUS_WATCH=<lo>:<hi> reports the first instructions executed inside
	// an address range, which answers whether a driver is ever entered.
	if (const char *e = knob::text("FOCUS_WATCH").get())
	{
		m_dbg.watch_lo = strtoul(e, nullptr, 16);
		if (const char *colon = strchr(e, ':'))
			m_dbg.watch_hi = strtoul(colon + 1, nullptr, 16);
		// FOCUS_WATCHN=<n> sets how many hits to report; the channel that
		// prints them owns that budget.
		// FOCUS_WATCHFROM=<emulated seconds> ignores everything before that
		// moment, which is how activity from a typed command gets separated
		// from the boot's own.
		m_dbg.watch_from = knob::seconds("FOCUS_WATCHFROM", m_dbg.watch_from);
	}

	// FOCUS_RUNAWAY reports the moment the processor leaves real memory,
	// with the last instructions it executed before it did. A machine
	// that has jumped to nowhere cannot say where it came from, and the
	// stack marker is usually gone too, so the trail has to be kept.
	m_dbg.runaway = knob::flag("FOCUS_RUNAWAY").on();

}


// The hooks the core calls when something happens, and the pre-step
// hook that runs the instruments.
void hp9000_5xx_state::install_debug_hooks()
{
	// The hooks the core calls when something happens, rather than on
	// every instruction: a fault taken, an address computed, a semaphore
	// blocked, an escape raised. Each is installed once, here, from the
	// knob that asks for it.
	//
	// They used to sit inside the pre-step lambda below, one tab short of
	// the code around them, which is how they got there: an edit landed
	// them a level out and the compiler had no reason to object. The
	// machine could not tell -- they build the same closures from the
	// same environment however often they run, and nothing executes
	// between here and the first instruction -- but the processor paid
	// for five std::function allocations and a re-read of the
	// environment before every instruction it ever executed.
	// FOCUS_TRAPLOG reports each fault as it is taken, with the state the
	// processor is in, which names the code that faulted. A number sets
	// how many reports to print (default 6); FOCUS_TRAPFROM=<seconds>
	// skips the boot's own faults, which otherwise eat the whole budget
	// before the interesting one arrives.
	if (const char *e = knob::text("FOCUS_TRAPLOG").get())
	{
		// "1" is the historical on-switch, not a budget of one.
		const unsigned long n = strtoul(e, nullptr, 0);
		const unsigned cap = n > 1 ? unsigned(n) : 6;
		const double from = knob::seconds("FOCUS_TRAPFROM", 0.0);
		m_cpu->core().on_trap =
				[this, cap, from](focus::trap_cause cause, u32 parameter) {
			if (machine().time().as_double() < from)
				return;
			if (m_dbg.traps_seen++ < cap)
			{
				std::printf("\n[trap %u parameter=%08x t=%.3f]\n",
						unsigned(cause), parameter,
						machine().time().as_double());
				report_state("trap");
			}
		};
	}

	// FOCUS_EATRACE=<addr>[:<n>] prints the next n data addresses the
	// processor computes once it reaches that instruction, with what it
	// found there.
	//
	// Except that it does not stop after n. This installer used to run
	// inside the pre-step hook, so the count below was written afresh
	// before every instruction and the trace ran until the machine did.
	// Installing once would have ended it after n addresses -- the
	// documented behaviour, and a different one: FOCUS_EATRACE=x:20
	// prints twenty lines instead of the half million golden.sh has
	// recorded since the knob was written. That is a fix, not a move,
	// so it is not made here; m_dbg.ea_refill keeps the refill exactly
	// where it was and README/knob docs stay true to the code.
	if (const char *e = knob::text("FOCUS_EATRACE").get())
	{
		m_dbg.ea_at = strtoul(e, nullptr, 16);
		if (const char *colon = strchr(e, ':'))
			m_dbg.ea_want = m_dbg.ea_refill = strtoul(colon + 1, nullptr, 0);
		m_cpu->core().on_ea = [this](u32 pc, u32 addr) {
			if (!m_dbg.ea_armed || !m_dbg.ea_want)
				return;
			m_dbg.ea_want--;
			std::printf("[ea pc=%08x addr=%08x = %08x]\n", pc, addr,
					m_cpu->core().read_mem32(addr & ~3u));
			std::fflush(stdout);
		};
	}

	// FOCUS_EAWATCH=<lo>:<hi> logs every data address the processor
	// computes inside a range, with the instruction that did it -- the
	// definitive record of which words of a structure are actually read
	// or written, where a static scan of the code can misjudge.
	if (const char *e = knob::text("FOCUS_EAWATCH").get())
	{
		static u32 lo = strtoul(e, nullptr, 16);
		static u32 hi = strchr(e, ':')
				? strtoul(strchr(e, ':') + 1, nullptr, 16) : lo + 4;
		m_cpu->core().on_ea = [this](u32 pc, u32 addr) {
			static knob::channel ea("FOCUS_EAWATCH", 400);
			if (addr >= lo && addr <= hi && ea)
				ea("[ea p=%08x addr=%08x = %08x]\n", pc, addr,
						m_cpu->core().read_mem32(addr & ~3u));
		};
	}

	// FOCUS_SEMLOG traces the hardware semaphores: which one a task blocks
	// on and which one wakes it. An idle kernel that has stopped short
	// says so here -- the semaphore it parked on names what it is waiting
	// for.
	if (knob::flag("FOCUS_SEMLOG"))
		m_cpu->core().sem_hook = [this](bool block, u32 sem, u32 tcb, u32 pc) {
			std::printf("[%8.3fs sem %s sem=%08x tcb=%08x at P=%08x]\n",
					machine().time().as_double(),
					block ? "DOWN-block" : "UP-wake  ", sem, tcb, pc);
			std::fflush(stdout);
		};

	if (knob::flag("FOCUS_ESCLOG"))
		m_cpu->core().on_escape = [this](u32 p, u32 value) {
			// FOCUS_ESCVAL=<hex> logs only that escape value, uncapped;
			// otherwise the first 400.
			if (const char *e = knob::text("FOCUS_ESCVAL").get())
			{
				if (*e == '*')      // log everything, uncapped
				{
					std::printf("[%.2fs escape at %08x value=%08x q=%08x]\n",
							machine().time().as_double(), p, value,
							m_cpu->core().r.q);
					std::fflush(stdout);
					return;
				}
				if (value == strtoul(e, nullptr, 16) && m_dbg.escapes++ < 40)
				{
					std::printf("[escape at %08x value=%08x q=%08x]\n",
							p, value, m_cpu->core().r.q);
					report_state("escval");
				}
				return;
			}
			if (m_dbg.escapes++ < 400)
			{
				std::printf("[escape at %08x value=%08x q=%08x]\n",
						p, value, m_cpu->core().r.q);
				std::fflush(stdout);
			}
		};

	// The instruments run only when something has asked for them; see
	// debug_armed(). The traps check below is not one of them.
	m_dbg.armed = debug_armed();
	m_cpu->set_pre_step([this](focus::cpu_core &core) {
		if (m_dbg.armed)
			debug_pre_step(core);
		// Traps vector through CST entry 1 once the powerup module has
		// built the tables and pointed dedicated location 0 at them.
		if (!core.traps_enabled() && core.read_mem32(0) != 0)
			core.enable_traps(true);
	});

}


// The timers that keep the IOP polling and the clock running.
void hp9000_5xx_state::start_timers()
{
	// The IOP polls its interfaces continuously and runs the interval
	// timer; one emulated tick per millisecond is enough to get both
	// moving. Real timings are in the I/O ERS section 7.
	m_iop_timer = timer_alloc(FUNC(hp9000_5xx_state::iop_tick), this);
	m_iop_timer->adjust(attotime::from_msec(1), 0, attotime::from_msec(1));
	// FOCUS_IOPPOLL=<us> is how close to "continuously" the interface
	// poll runs, and the default is the middle of a band with a measured
	// edge at each end. Slower than about 700 us and HP-UX's console open
	// starts losing its hand-offs again (2x56 and a 66 return at 900);
	// faster than about 200 us and the 520's boot stops reaching its
	// banner at all -- 150 and 100 both fail, 200 does not, and what it
	// is that the 520 cannot take at that rate is NOT yet known. So 400
	// us, roughly a factor of two clear either way.
	m_iop_poll_timer = timer_alloc(FUNC(hp9000_5xx_state::iop_poll_tick), this);
	const unsigned poll_us = unsigned(knob::number("FOCUS_IOPPOLL", 400));
	m_iop_poll_timer->adjust(attotime::from_usec(poll_us), 0,
			attotime::from_usec(poll_us));
	m_rtc_timer = timer_alloc(FUNC(hp9000_5xx_state::rtc_tick), this);
	m_rtc_timer->adjust(attotime::from_seconds(1), 0, attotime::from_seconds(1));

}


// The scripted knobs: keystrokes to type, words to poke, memory to
// search, state to report -- each one an alarm set for an emulated
// moment rather than something done now.
void hp9000_5xx_state::schedule_scripts()
{
	// FOCUS_TYPE=<text> feeds keystrokes through the keyboard board once
	// the boot has settled -- the headless way to drive the command line.
	// \n becomes EXECUTE. FOCUS_TYPEAT=<emulated seconds> delays the
	// first keystroke (default 15).
	if (const char *e = knob::text("FOCUS_FINDAT").get())
	{
		emu_timer *const t = timer_alloc(FUNC(hp9000_5xx_state::find_tick), this);
		t->adjust(attotime::from_double(strtod(e, nullptr)));
	}
	// FOCUS_POKE=<hex addr>:<hex value>[:<seconds>][,...] writes a word at
	// a chosen moment -- the fake-V experiment: hand a waiting task the
	// event nobody produces and watch whether the machine sails on, which
	// confirms a blocker and shows the downstream in one run.
	if (const char *e = knob::text("FOCUS_POKE").get())
		for (std::string list(e); !list.empty(); )
		{
			const size_t comma = list.find(',');
			const std::string one = list.substr(0, comma);
			list = (comma == std::string::npos) ? "" : list.substr(comma + 1);
			const u32 addr = strtoul(one.c_str(), nullptr, 16);
			const char *colon = strchr(one.c_str(), ':');
			const u32 val = colon ? strtoul(colon + 1, nullptr, 16) : 1;
			const char *colon2 = colon ? strchr(colon + 1, ':') : nullptr;
			const double at = colon2 ? strtod(colon2 + 1, nullptr) : 3.0;
			m_dbg.pokes.push_back(std::make_pair(addr, val));
			emu_timer *const t =
					timer_alloc(FUNC(hp9000_5xx_state::poke_tick), this);
			t->adjust(attotime::from_double(at), int(m_dbg.pokes.size() - 1));
		}
	if (const char *e = knob::text("FOCUS_STATEAT").get())
		for (std::string list(e); !list.empty(); )
		{
			const size_t comma = list.find(',');
			const double at = strtod(list.substr(0, comma).c_str(), nullptr);
			list = (comma == std::string::npos) ? "" : list.substr(comma + 1);
			emu_timer *const t =
					timer_alloc(FUNC(hp9000_5xx_state::state_tick), this);
			t->adjust(attotime::from_double(at));
		}
	// FOCUS_TYPE=<text> feeds keystrokes at FOCUS_TYPEAT seconds, with \n
	// and \r for the two ways a line ends. A ';' starts a further script
	// and FOCUS_TYPEAT then takes one time per script:
	//
	//   FOCUS_TYPE='LOAD \"CAT_BOOT\"\n;RUN\n' FOCUS_TYPEAT=25,45
	//
	// which is what a command that takes seconds to finish needs -- the
	// keyboard buffer does not hold a line typed while the disc is busy,
	// so the next command has to wait for the first to land.
	if (const char *e = knob::text("FOCUS_TYPE").get())
	{
		std::string one;
		for (const char *pc = e; *pc; pc++)
		{
			if (pc[0] == '\\' && pc[1] == 'n') { one.push_back('\n'); pc++; }
			else if (pc[0] == '\\' && pc[1] == 'r') { one.push_back('\r'); pc++; }
			// \xNN presses a KEYCODE rather than typing a character, which
			// is the only way to reach a key that puts nothing on the
			// screen. A demo that pauses with "(CONT = 100)" wants the
			// CONTINUE key and nothing else will do. RAW_KEY never occurs
			// in ordinary script text, so it needs no escaping of its own.
			else if (pc[0] == '\\' && pc[1] == 'x'
					&& std::isxdigit((unsigned char)pc[2])
					&& std::isxdigit((unsigned char)pc[3]))
			{
				one.push_back(char(RAW_KEY));
				one.push_back(char(std::stoi(std::string(pc + 2, 2), nullptr, 16)));
				pc += 3;
			}
			else if (pc[0] == ';') { m_type_scripts.push_back(one); one.clear(); }
			else one.push_back(*pc);
		}
		m_type_scripts.push_back(one);
		m_type_timer = timer_alloc(FUNC(hp9000_5xx_state::type_tick), this);
		// FOCUS_TYPERATE=<ms> paces the injected keystrokes. The default
		// mimics fast human typing; the display driver drops or reorders
		// characters pushed faster than it redraws the input line.
		const unsigned ms = unsigned(knob::number("FOCUS_TYPERATE", 350));
		m_type_rate = attotime::from_msec(ms);
		const knob::text typeat("FOCUS_TYPEAT");
		std::string list(typeat ? typeat.get() : "15");
		double at = 15.0;
		for (size_t i = 0; i < m_type_scripts.size(); i++)
		{
			// A script with no time of its own follows the one before it
			// by a wide margin -- far enough that a run which needs a
			// tighter schedule says so rather than getting one by luck.
			if (!list.empty())
			{
				const size_t comma = list.find(',');
				at = strtod(list.substr(0, comma).c_str(), nullptr);
				list = (comma == std::string::npos) ? "" : list.substr(comma + 1);
			}
			else
				at += 20.0;
			emu_timer *const t =
					timer_alloc(FUNC(hp9000_5xx_state::type_start), this);
			t->adjust(attotime::from_double(at), int(i));
		}
	}
}


void hp9000_5xx_state::machine_start()
{
	// FOCUS_HELP=1 lists every knob and what it does. Printed here
	// rather than at exit so it lands before the machine's own output.
	knob::handle_help();
	machine().add_notifier(MACHINE_NOTIFY_EXIT,
			machine_notify_delegate(&hp9000_5xx_state::machine_stop, this));

	install_ram_boards();
	load_batteries();
	configure_display();
	configure_peripherals();
	configure_backplane();
	parse_debug_knobs();
	install_debug_hooks();
	start_timers();
	schedule_scripts();
}

TIMER_CALLBACK_MEMBER(hp9000_5xx_state::find_tick)
{
	memory_find();
}


TIMER_CALLBACK_MEMBER(hp9000_5xx_state::poke_tick)
{
	const auto &req = m_dbg.pokes[size_t(param)];
	const u32 before = m_cpu->core().read_mem32(req.first);
	m_cpu->memory().write32(req.first, req.second);
	std::printf("[poke %08x: %08x -> %08x at %.2fs]\n",
			req.first, before, req.second, machine().time().as_double());
	std::fflush(stdout);
}


// FOCUS_STATEAT=<emulated seconds>[,<more>...] reports the processor and
// its stack at chosen moments -- where a wedged machine is sitting, which
// a breakpoint cannot ask because the code in question is not known yet.
TIMER_CALLBACK_MEMBER(hp9000_5xx_state::state_tick)
{
	report_state("stateat");
}


TIMER_CALLBACK_MEMBER(hp9000_5xx_state::type_start)
{
	m_type_text = m_type_scripts[size_t(param)];
	m_type_pos = 0;
	m_type_timer->adjust(attotime::zero, 0, m_type_rate);
}


TIMER_CALLBACK_MEMBER(hp9000_5xx_state::type_tick)
{
	if (m_type_pos == 0)
		std::printf("[typing starts, disc blocks %u, sc6 writes %u]\n",
				m_disc.m_seq, m_console6.writes);
	if (m_type_pos < m_type_text.size())
	{
		const u8 c = u8(m_type_text[m_type_pos++]);
		// A \xNN escape: the byte after the marker is a keycode, handed
		// to the keyboard board as it stands.
		if (c == RAW_KEY && m_type_pos < m_type_text.size())
		{
			const u8 code = u8(m_type_text[m_type_pos++]);
			if (m_console_asi)
				m_console_asi->type(code);
			else
				m_console6.type(code);
			return;
		}
		// The 540's console is a terminal on the serial card, and a
		// terminal sends ASCII -- no keycode translation.
		if (m_console_asi)
			m_console_asi->type(c == '\n' ? u8('\r') : c);
		else
			kbd_put(c);
	}
	else
		m_type_timer->adjust(attotime::never);
}


// The IOP is a processor of its own and it polls the interfaces
// CONTINUOUSLY (I/O ERS figure 2.1). Running that poll on the same 1 ms
// beat as the cards' own timekeeping made it a visitor instead: the CPU
// got a whole millisecond -- tens of thousands of instructions -- between
// one command-list entry and the next, and every protocol in which the
// two processors hand something to each other broke on it.
//
// Measured, all three from the console open: the driver arms its
// transaction by writing a branch target into its command list and the
// interrupt handler overwrote it again before the IOP ever read it; the
// list parked one entry short of the Read Transparent Status that
// refreshes the report buffer, so the handler dispatched a stale report
// against a channel its own completion had just closed; and a status
// block's completion interrupt arrived before the status block's own
// list had finished. None of those is a race on real hardware, where the
// IOP answers in microseconds.
//
// So the poll -- and the interrupts it produces -- run on their own fast
// timer, and the millisecond tick keeps what is genuinely millisecond
// business: the cards' internal clocks, the interval timer, the pacing
// of a Pause order.
TIMER_CALLBACK_MEMBER(hp9000_5xx_state::iop_poll_tick)
{
	m_iop.poll_and_service();
	deliver_iop_interrupts();
}


// Every accepted device raise is one message at 16 + its level (the
	// microcode handler maps it back and pulls the device from the
	// level's list). The attention poll reaches the CPU as message 14
	// (I/O ERS section 2.1).
// is one message at 16 + its level (the microcode handler maps it back and
// pulls the device from the level's list). The attention poll reaches the
// CPU as message 14 (I/O ERS section 2.1).
void hp9000_5xx_state::deliver_iop_interrupts()
{
	const u32 raised = m_iop.take_raise_events();
	for (unsigned pa = 0; pa < 8; pa++)
		if (raised & (1u << pa))
			m_cpu->core().raise_message(16 + m_iop.interrupt_level(pa));
	if (m_iop.take_attention_event())
		m_cpu->core().raise_message(14);
}


TIMER_CALLBACK_MEMBER(hp9000_5xx_state::iop_tick)
{
	// The transfers the fast poll leaves alone, at the rate the devices
	// driving them expect.
	m_iop.pace_uncounted_dma();
	deliver_iop_interrupts();
	m_disc.tick();
	for (unsigned sc = 0; sc < m_cio.size(); sc++)
		if (m_cio[sc])
			if (auto *c = dynamic_cast<device_focus_cio_interface *>(
					m_cio[sc]->get_card_device()))
				c->focus_tick();
	// One timer count is 512 clock periods = 28.4 us; a 1 ms tick is
	// about 35 of them.
	m_iop.timer_tick(35);
	if (m_iop.take_timer_event())
		m_cpu->core().raise_message(13);
	// FOCUS_FORCETICK=<period ms>[:<start s>] raises the interval-timer
	// message on a fixed beat whether or not the OS armed the timer -- the
	// experiment for a kernel whose clock service waits on a tick it never
	// asked the IOP for. bootrun's --tick taught that injecting during
	// powerup panics a machine that is not ready, hence the start delay.
	if (const char *e = knob::text("FOCUS_FORCETICK").get())
	{
		static const unsigned period = std::max(1L, strtol(e, nullptr, 0));
		static const double from = strchr(e, ':')
				? strtod(strchr(e, ':') + 1, nullptr) : 2.0;
		static unsigned phase = 0;
		if (machine().time().as_double() >= from && ++phase >= period)
		{
			phase = 0;
			m_cpu->core().raise_message(13);
		}
	}

}


TIMER_CALLBACK_MEMBER(hp9000_5xx_state::rtc_tick)
{
	m_console6.rtc_second_tick();
	m_scm.rtc_second_tick();
	// Write the batteries back every so often. Saving only at exit meant a
	// run that was killed -- which is how most of them end here, with -str
	// or a ^C -- lost everything the guest had put in the clock and the
	// configuration memory.
	if (m_nv_save_period && ++m_nv_save_phase >= m_nv_save_period)
	{
		m_nv_save_phase = 0;
		save_battery(m_scm.nv(), "FOCUS_SCMNV",
				focus::scm_device::SELECT_CODE, "periodic", false);
		save_battery(m_console6.nv, "FOCUS_KBDNV", 6, "periodic", false);
	}
}


// Write a battery back if it is configured and something has changed it.
// `why` names the occasion for the log; a periodic save is silent unless
// FOCUS_NVLOG asks, so a long run does not narrate itself every 30 seconds.
void hp9000_5xx_state::save_battery(focus::rtc_nvm &part, const char *knobname,
		unsigned sc, const char *why, bool loud)
{
	const char *const path = knob::text(knobname).get();
	if (!path || !part.dirty)
		return;
	if (part.save(path, sc))
	{
		if (loud)
			std::printf("[select code %u RTC/NVM saved to %s]\n", sc, path);
		m_nv_log("[nv sc%u saved to %s (%s)]\n", sc, path, why);
	}
	else
		osd_printf_error("nv: cannot write select code %u battery to %s\n",
				sc, path);
}


// FOCUS_RTCTIME seeds the clock the way a real machine powers up holding
// the time. The default is none: all-zero, "clock never set", which is the
// path HP-UX's first boot takes (boot error 941) and what BASIC reports as
// "Clock and date not set" -- and it is the only setting under which two
// runs of the same image produce the same output, which is what golden.sh
// needs. "now" takes the host's wall clock; a fixed moment is what a
// reproducible run wants.
//
// WHAT THIS DOES NOT YET DO. Seeding puts the moment in both parts -- the
// clock and the NVM's copy of the same map -- and HP-UX 5.30 then clears
// the century and the year again during its bring-up and sets its own. So
// this seeds the HARDWARE, and the OS does not yet accept what it finds.
// The likely gate is map offset 1, the manuals' "Clock Check Byte", whose
// value is not known: a battery a real run wrote holds 1 in the clock and
// 14 in the NVM there, and nothing here writes either. Until that is read
// off a run, do not expect a seeded date to reach the OS's own clock.
void hp9000_5xx_state::seed_clocks()
{
	const char *const e = knob::text("FOCUS_RTCTIME").get();
	if (!e || !*e || !std::strcmp(e, "none"))
		return;
	focus::calendar c;
	if (!std::strcmp(e, "now"))
	{
		const std::time_t t = std::time(nullptr);
		if (knob::flag("FOCUS_RTCUTC"))
			c = focus::calendar::from_epoch((long long)t);
		else
		{
			// Local time, via the host's own broken-down conversion: the
			// offset is the platform's business, not this file's.
			const std::tm *lt = std::localtime(&t);
			if (!lt)
				return;
			c.year = lt->tm_year + 1900; c.month = u8(lt->tm_mon + 1);
			c.day = u8(lt->tm_mday); c.hour = u8(lt->tm_hour);
			c.minute = u8(lt->tm_min);
			c.second = u8(lt->tm_sec > 59 ? 59 : lt->tm_sec);
		}
	}
	else if (std::strchr(e, '-'))
	{
		// ISO 8601, as much of it as a clock with one-second resolution
		// can hold: YYYY-MM-DD[Thh:mm:ss], the T interchangeable with a
		// space so a shell need not quote it.
		int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
		const int n = std::sscanf(e, "%d-%d-%d%*[T ]%d:%d:%d",
				&y, &mo, &d, &h, &mi, &se);
		if (n < 3)
		{
			osd_printf_error("FOCUS_RTCTIME=%s is not a date\n", e);
			return;
		}
		c.year = y; c.month = u8(mo); c.day = u8(d);
		c.hour = u8(h); c.minute = u8(mi); c.second = u8(se);
	}
	else
		c = focus::calendar::from_epoch(strtoll(e, nullptr, 0));

	if (!c.valid())
	{
		osd_printf_error("FOCUS_RTCTIME=%s is not a moment a clock can "
				"hold\n", e);
		return;
	}
	m_scm.nv().set_time(c);
	m_scm.nv().set_time_mirror(c);
	m_console6.nv.set_time(c);
	m_console6.nv.set_time_mirror(c);
	osd_printf_info("clock seeded to %04d-%02d-%02d %02d:%02d:%02d\n",
			c.year, c.month, c.day, c.hour, c.minute, c.second);
}


void hp9000_5xx_state::save_nv()
{
	save_battery(m_scm.nv(), "FOCUS_SCMNV", focus::scm_device::SELECT_CODE,
			"exit", true);
	save_battery(m_console6.nv, "FOCUS_KBDNV", 6, "exit", true);
}


// Read a battery in, say what happened, and decide what the machine should
// believe about it. A load that fails is a flat battery: the manuals make
// that a status bit (R2 bit 5 on the SCM), and say the OS restores its
// configuration when it sees one. It used to fail in silence.
void hp9000_5xx_state::load_battery(focus::rtc_nvm &part, const char *knobname,
		unsigned sc)
{
	const char *const path = knob::text(knobname).get();
	if (!path)
		return;
	const focus::nv_load_result r = part.load(path, sc);
	if (r.ok)
	{
		// The battery kept the clock running while the machine was off --
		// off by default, because it makes a run depend on when it started.
		if (const char *b = knob::text("FOCUS_RTCBATTERY").get())
			if (std::strcmp(b, "off") != 0)
			{
				const long long days = (std::strcmp(b, "on") == 0)
						? focus::rtc_nvm::BATTERY_DAYS
						: strtoll(b, nullptr, 0);
				focus::nv_load_result rr = r;
				part.run_battery(rr, days);
				if (rr.battery_expired)
					osd_printf_info("select code %u battery was flat: %lld "
							"days off, past the %lld it holds for\n",
							sc, rr.off_seconds / 86400, days);
				else if (rr.off_seconds > 0)
					osd_printf_info("select code %u clock advanced %lld s "
							"over the time the machine was off\n",
							sc, rr.off_seconds);
			}
		osd_printf_info("select code %u RTC/NVM restored from %s%s\n",
				sc, path, r.legacy ? " (headerless, pre-format)" : "");
	}
	else if (r.error == focus::nv_error::no_file)
	{
		// The ordinary first run. Not an error, but the machine must know:
		// no battery is a flat battery.
		part.battery_fail = true;
		osd_printf_info("select code %u has no battery file yet (%s); "
				"the clock reads as never set\n", sc, path);
	}
	else
	{
		part.battery_fail = true;
		osd_printf_error("nv: %s: %s -- select code %u keeps the clock it "
				"had and reports a flat battery\n",
				path, focus::nv_error_text(r.error), sc);
	}
	m_nv_log("[nv sc%u load %s: ok=%d legacy=%d saved_at=%lld board=%u]\n",
			sc, path, int(r.ok), int(r.legacy), r.saved_at, r.board);
}


// A key typed at the 540's console terminal. The serial card's firmware
// assembles characters into records and hands the host a whole one, so a
// keystroke goes into its receive buffer and the card decides when the
// record is complete.
void hp9000_5xx_state::term_put(u8 data)
{
	if (m_console_asi)
		m_console_asi->type(data);
}


// A fixed disc is not a shipped medium: it is a disc this machine owns and
// writes to, so what the running system does to it has to survive the run.
// The card holds the medium, so the image goes back to the file it came
// from at the end -- which is what makes INITIALIZE, STORE and the rest
// mean anything, and what lets HP's own utilities build the file system
// rather than this emulator pretending to.
void hp9000_5xx_state::machine_stop()
{
	if (knob::flag("FOCUS_HDNOSAVE"))
		return;
	// The 520's internal Winchester, behind its own controller.
	if (m_hard && m_hard->has_media() && m_disc.fixed_loaded())
	{
		m_disc.select_device(1);
		write_back(m_disc.medium(), *m_hard, "fixed disc");
	}
	// Every CS/80 drive on a card in the backplane -- the 540's system
	// disc among them, which HP-UX writes to from its first boot.
	for (unsigned sc = 0; sc < m_cio.size(); sc++)
		if (m_cio[sc])
			if (auto *hpib = dynamic_cast<hp27110_hpib_device *>(
					m_cio[sc]->get_card_device()))
				if (hpib->media().has_media())
					write_back(hpib->card.disc.image, hpib->media(),
							"system disc");
}


// A disc this machine owns and writes to: what the running system did to
// it has to survive the run, so the medium goes back to the file it came
// from. Unchanged media are left alone, which keeps a read-only run from
// touching the file's timestamp.
void hp9000_5xx_state::write_back(const std::vector<u8> &now,
		hp_focus_media_device &media, const char *what)
{
	if (now.empty() || now == media.data())
		return;
	const std::string path = media.filename() ? media.filename() : "";
	if (path.empty())
		return;
	if (FILE *f = fopen(path.c_str(), "r+b"))
	{
		fwrite(now.data(), 1, now.size(), f);
		fclose(f);
		osd_printf_info("disc: %s written back to %s\n", what, path.c_str());
	}
	else
		osd_printf_error("disc: cannot write %s back\n", path.c_str());
}


void hp9000_5xx_state::machine_reset()
{
	// iop_core::reset() rebuilds every channel_state but carries the
	// interface pointers across (focusiop.h): what is in a slot stays
	// there. So the attaches below are not the repair this comment used
	// to claim they were -- they are stores of the pointers already in
	// place, kept because they cost nothing and because a slot list that
	// exists in one place only is a slot list that gets forgotten in the
	// other.
	//
	// What reset DOES clear is every channel's interrupt enable, which is
	// why the enables are here at all. They are deliberately not the same
	// set machine_start arms: this path enables select code 6 and every
	// populated backplane slot, and machine_start also enables 1 and 7.
	// MAME runs reset after start, so this is the set the running machine
	// actually has. Which of the two is right for the hardware is an
	// emulation question and not a tidying one; the difference is left
	// standing and written down rather than unified by hand.
	//
	// The order matters and is shared with machine_start: the SCM claims
	// select code 7 and the internal disc claims it back, so on a 520 the
	// disc is what answers there. A 530/540 has no internal disc, does not
	// reach that line, and leaves the SCM in place.
	m_iop.reset();
	m_scm.reset();
	m_console6.reset();
	m_iop.attach(focus::scm_device::SELECT_CODE, &m_scm);
	if (!m_console_is_asi)
	{
		m_iop.attach(0, &m_dim);
		m_iop.attach(7, &m_disc);
	}
	m_iop.attach(6, &m_console6);
	for (unsigned sc = 0; sc < m_cio.size(); sc++)
		if (m_cio[sc])
			if (focus::iop_interface *const iface = m_cio[sc]->iface())
			{
				m_iop.attach(sc, iface);
				m_iop.enable_interrupt(sc);
			}
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

	// The volumes in the drive come from the mounted image devices, in
	// slot order; FOCUS_FLOPPY's list (loaded at start) stands only when
	// nothing is mounted, as compatibility for old scripts. Re-reading
	// them each reset means a disc mounted through the UI boots on the
	// next soft reset.
	{
		std::vector<std::vector<u8>> media;
		for (auto &f : m_floppy)
			if (f && f->has_media())
				media.push_back(f->data());
		if (!media.empty())
			m_media = std::move(media);
	}

	if (m_console_is_asi)
	{
		// The 540 boots from the CS/80 system disc on its HP-IB card --
		// whichever slot holds a card with a drive in it, which is slot
		// 5 unless someone re-cabled the machine.
		if (focus::hpib_card *const card = system_disc())
			boot_from_cs80(*card);
		else
			osd_printf_error("boot: no CS/80 system disc -- fit an HP-IB "
					"card and mount a disc on it (-cio5 hpib -hard1 <image>)\n");
	}
	else if (!m_media.empty() || m_disc.fixed_loaded())
		boot_from_disc();   // the fixed disc boots on its own
}


// Boot the 540 from the CS/80 disc on its HP-IB card, the way the loader
// ROM does ("CS80 DEVICE: SELECT CODE NN" is its own error vocabulary).
// A hard disc carries the boot descriptor at block 0 and the image from
// byte 0x400 -- the layout the 7914 unit save uses -- and every byte is
// read through the device's Locate-and-Read path, bounds checks and all.
focus::hpib_card *hp9000_5xx_state::system_disc()
{
	for (unsigned sc = 0; sc < m_cio.size(); sc++)
		if (m_cio[sc])
			if (auto *hpib = dynamic_cast<hp27110_hpib_device *>(
					m_cio[sc]->get_card_device()))
				if (hpib->card.disc.loaded())
					return &hpib->card;
	return nullptr;
}


bool hp9000_5xx_state::boot_from_cs80(focus::hpib_card &card)
{
	constexpr u32 MAGIC = 0xe9c28206;
	constexpr u32 SECTOR = focus::cs80_device::BLOCK;

	std::vector<u8> desc;
	if (!card.disc.read_blocks(0, SECTOR, desc))
	{
		osd_printf_error("boot: cannot read the system disc\n");
		return false;
	}
	auto beword = [&desc](u32 off) {
		return (u32(desc[off]) << 24) | (u32(desc[off + 1]) << 16)
				| (u32(desc[off + 2]) << 8) | desc[off + 3];
	};
	if (beword(0) != MAGIC)
	{
		osd_printf_error("boot: the system disc is not bootable -- block 0 "
				"begins %08x, not the boot magic %08x\n", beword(0), MAGIC);
		return false;
	}
	const u32 total = beword(4);
	if (!boot_image_fits(total))
	{
		osd_printf_error("boot: a %u byte image will not fit in %u KB of "
				"RAM; fit more boards (-mpb5 512k ...)\n",
				total, m_ram_total / 1024);
		return false;
	}

	std::vector<u8> image;
	if (!card.disc.read_blocks(0x400 / SECTOR, total, image))
	{
		osd_printf_error("boot: the system disc is short\n");
		return false;
	}
	logerror("boot: %u bytes from the CS/80 system disc\n", total);
	install_boot_image(image, "the system disc");
	return true;
}


// Boot a Series 500 image the way the (undumped) SCM loader ROM hands
// one over -- the contract bootrun reverse-engineered from SYSTEM
// POWERUP itself. The image loads high, the powerup module executes in
// place with PB = 0, and the loader's parameter block sits at Q: the
// MPB inventory doubling as the final IXIT's frame, the load address at
// Q+52, the top-of-memory word at Q+64.
void hp9000_5xx_state::install_boot_image(const std::vector<u8> &base,
		const char *whence)
{
	// FOCUS_BOOTLOAD=<hex> moves the image, testing the load address the
	// (undumped) loader ROM chose. Nothing downstream may care: powerup
	// reads it back out of Q+52, relocates every segment into [0, load)
	// and never touches the image region again -- measured with RWATCH
	// over both the image and the loader stack after t=1.3. A behaviour
	// difference under this knob would mean a hidden dependency on the
	// placement, which is exactly what it exists to find.
	//
	// The default placement is 1 MB when the machine can take it -- the
	// address every measured result was taken at -- and otherwise high,
	// leaving a small loader-stack window under top of memory, so the
	// base 1 MB machine (two 512 KB boards) boots the images that fit.
	// The remaining constraint is real: the relocated world must fit
	// below the image, and when it does not, that is the machine
	// genuinely out of memory.
	u32 LOAD = 0x100000;

	// FOCUS_BOOTOPT=<file>[,<file>...] appends option binaries to the
	// boot image, which is what a configured machine actually runs.
	// HP's BUILD_BOOT utility (on the option disc) copies the boot to a
	// destination disc and then "allows any number of option binaries to
	// be added to the boot area"; the option files are chain modules in
	// exactly the boot image's format -- length at +4, name at +28, and
	// the byte-sum checksum at length+8, verified against every binary on
	// the disc -- so appending one through its checksum word extends the
	// chain the powerup walk follows. This stands in for BUILD_BOOT while
	// the emulated media is read-only.
	std::vector<u8> extended;
	if (const char *e = knob::text("FOCUS_BOOTOPT").get())
	{
		// The modules go INTO the chain, not after it. The powerup walk
		// stops at the terminator that follows the last segment, so an
		// option appended past it is loaded into memory and then ignored
		// -- the code segment table comes back unchanged, which is
		// exactly what happened the first time this was tried. Walk the
		// chain the way tools/bootimage.py verifies it (length at +4,
		// checksum at length+8 for every segment after the first) to
		// find where it ends, and splice the options in there.
		auto word = [&base](size_t off) {
			return (u32(base[off]) << 24) | (u32(base[off + 1]) << 16)
					| (u32(base[off + 2]) << 8) | base[off + 3];
		};
		size_t hdr = 4, seg = 0;
		while (hdr + 8 <= base.size())
		{
			const u32 ln = word(hdr + 4);
			const size_t ckpos = (seg == 0) ? size_t(ln) : hdr + ln + 8;
			if (ckpos + 4 > base.size())
				break;
			hdr = ckpos + 4;
			seg++;
			if (hdr + 4 > base.size() || s32(word(hdr)) < 0
					|| (base[hdr] == 0 && base[hdr + 1] == 0))
				break;
		}
		osd_printf_info("bootopt: chain of %u segments ends at %#x\n",
				unsigned(seg), unsigned(hdr));
		extended.assign(base.begin(), base.begin() + hdr);
		for (std::string list(e); !list.empty(); )
		{
			const size_t comma = list.find(',');
			const std::string path = list.substr(0, comma);
			list = (comma == std::string::npos) ? "" : list.substr(comma + 1);
			std::ifstream f(path, std::ios::binary);
			if (!f)
			{
				osd_printf_error("bootopt: cannot open %s\n", path.c_str());
				continue;
			}
			const std::vector<u8> mod((std::istreambuf_iterator<char>(f)),
					std::istreambuf_iterator<char>());
			if (mod.size() < 12)
			{
				osd_printf_error("bootopt: %s is too short\n", path.c_str());
				continue;
			}
			const u32 ln = (u32(mod[4]) << 24) | (u32(mod[5]) << 16)
					| (u32(mod[6]) << 8) | mod[7];
			const size_t take = size_t(ln) + 12;   // through the checksum
			if (take > mod.size())
			{
				osd_printf_error("bootopt: %s claims %u bytes, has %u\n",
						path.c_str(), unsigned(take), unsigned(mod.size()));
				continue;
			}
			const std::string name(
					reinterpret_cast<const char *>(&mod[28]), 16);
			osd_printf_info("bootopt: adding %s (%u bytes) from %s\n",
					name.c_str(), unsigned(take), path.c_str());
			extended.insert(extended.end(), mod.begin(), mod.begin() + take);
		}
		// then the terminator and whatever else trailed the chain
		extended.insert(extended.end(), base.begin() + hdr, base.end());
	}
	const std::vector<u8> &img = extended.empty() ? base : extended;
	const u32 size = u32(img.size());

	const knob::number bootload("FOCUS_BOOTLOAD", LOAD, 16);
	if (bootload.set())
		LOAD = u32(bootload);
	else if (LOAD + size + BOOT_STACK_MIN > m_ram_total)
	{
		if (!boot_image_fits(size))
		{
			osd_printf_error("boot: a %u KB image cannot fit in %u KB of "
					"RAM; fit more boards (-mpb5 512k ...)\n",
					size / 1024, m_ram_total / 1024);
			return;
		}
		LOAD = (m_ram_total - size - BOOT_STACK_MIN) & ~0xfffu;
		osd_printf_info("boot: %u KB fitted, the %u KB image loads at %#x "
				"instead of 1 MB\n", m_ram_total / 1024, size / 1024, LOAD);
	}

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

	// FOCUS_BOOTPB=<base> overrides the code base the module runs with,
	// for testing what the loader really hands over.
	core.r.pb = u32(knob::number("FOCUS_BOOTPB", 0, 16));
	core.r.pl = LOAD + beword(8) + 3;
	core.r.p  = LOAD + entry_off;

	// The loader's own stack, which the system replaces with per-task
	// stacks in low memory as soon as it is up. These were absolute
	// addresses up at 14-15 MB, which only existed because this driver
	// claimed 16 MB of RAM; on a real machine they have to be inside the
	// memory actually fitted, so the stack hangs off the top of installed
	// memory instead -- a base below the marker, the rest of memory above
	// it.
	//
	// It also has to clear the image it has just loaded. A fixed base of
	// top - 0x80000 is only above the image while the image is smaller
	// than half a megabyte: the BASIC 2.0 boot is 373 KB and fits, but
	// the 9020 diagnostics environment is 680 KB, and its Q at 0x180203
	// landed at image offset 0x80203 -- inside the HP97062 segment, which
	// powerup then relocated with the loader's stack marker written
	// through it and the self test reported as "CHECKSUM for segment0014".
	// Anything the loader writes below the end of the image corrupts a
	// segment that has not been relocated yet, so the base goes above
	// whichever is higher.
	const u32 top = m_ram_total;
	const u32 above_image = (LOAD + size + 0xfff) & ~0xfffu;
	core.r.sb = std::max(top - 0x80000, above_image);
	core.r.sl = (top - 4) | 3;
	core.r.q  = core.r.sb + 0x203;
	core.r.s  = core.r.q + 0x200;
	// The handoff was written around a half-megabyte window, so say so
	// when the image has eaten into it -- the boot may still be fine,
	// since powerup uses only a few hundred bytes of this stack, but a
	// machine this tight is worth another RAM board (-mpb5 1m and up).
	if (LOAD == 0x100000 && core.r.sb + 0x80000 > top)
		osd_printf_warning("boot: the %u KB image leaves the loader only "
				"%u KB of stack under the %u KB fitted, not the half "
				"megabyte the handoff assumes\n",
				size / 1024, (top - core.r.sb) / 1024, top / 1024);
	core.r.db = 0;
	core.r.dl = 0xffffff | 3;
	core.r.dst = 1152;
	core.r.put(focus::sf::M, 1);
	core.r.mask = focus::ers32::set(0, 3, 3, 1);

	const u32 q = core.r.q;

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
	mem.write32(q + 64 - 3, m_ram_total);    // top of memory
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
// A fitted fixed disc is tried first, because that is where a machine
// with one keeps its system -- but a fixed disc is not always a system
// disc. A data disc has no boot area at all, and the machine should then
// come up off the flexible drive exactly as it did before there was
// anywhere else to look.
bool hp9000_5xx_state::boot_from_disc()
{
	if (m_disc.fixed_loaded() && boot_from_disc_source(true))
		return true;
	if (!m_media.empty())
		return boot_from_disc_source(false);
	return false;
}


bool hp9000_5xx_state::boot_from_disc_source(bool from_fixed)
{
	constexpr u32 SECTOR = focus::disc_card::BLOCK;
	constexpr u32 MAGIC = 0xe9c28206;

	// Selecting device 1 points every read below at the fixed medium;
	// the flexible drive is still there for the running system.
	m_disc.select_device(from_fixed ? 1 : 0);
	if (!from_fixed && m_media.empty())
		return false;

	auto beword = [](const std::vector<u8> &v, u32 off) {
		return (u32(v[off]) << 24) | (u32(v[off + 1]) << 16)
				| (u32(v[off + 2]) << 8) | v[off + 3];
	};

	std::vector<u8> image;
	u32 total = 0, volumes = 0, placed = 0;
	const unsigned sources = from_fixed ? 1 : unsigned(m_media.size());
	for (unsigned v = 0; v < sources; v++)
	{
		if (!from_fixed)
			m_disc.mount(m_media[v]);

		std::vector<u8> header;
		if (!m_disc.read_blocks(0, SECTOR, header))
		{
			osd_printf_error("boot: cannot read the volume header\n");
			return false;
		}
		// Two layouts, told apart by what block 0 begins with. A disc
		// carries the boot descriptor at offset 0 and its image at
		// 0x400 -- that is how the 7914 unit save holds HP-UX and how
		// the BASIC disc backup holds BASIC. A flexible volume puts an
		// SDF volume header there instead, and the descriptor lives in
		// block 1 with the image from block 2.
		std::vector<u8> desc;
		u32 slice_start = 0, boot_area = 0;
		const char *volume_name = reinterpret_cast<const char *>(
				header.data()) + 4;
		if (beword(header, 0) == MAGIC)
		{
			desc = header;
			slice_start = 0x400;
			boot_area = u32(m_media[v].size()) - slice_start;
			volume_name = reinterpret_cast<const char *>(header.data()) + 0x30;
		}
		else
		{
			const u32 blocksize = beword(header, 0x1c);
			const u32 first_free = beword(header, 0x28);
			if (blocksize < SECTOR || (blocksize % SECTOR) != 0
					|| first_free < 3)
			{
				osd_printf_error("boot: %.16s has no usable boot area "
						"(block size %u, first free block %u)\n",
						volume_name, blocksize, first_free);
				m_disc.select_device(0);
				return false;
			}
			if (!m_disc.read_blocks(blocksize / SECTOR, SECTOR, desc))
				return false;
			slice_start = 2 * blocksize;
			boot_area = (first_free - 2) * blocksize;
		}
		if (beword(desc, 0) != MAGIC)
		{
			osd_printf_error("boot: %.16s is not a boot volume -- it begins "
					"%08x, not the boot magic %08x\n",
					volume_name, beword(desc, 0), MAGIC);
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
			if (!boot_image_fits(total))
			{
				osd_printf_error("boot: a %u byte image will not fit in "
						"%u KB of RAM; fit more boards (-mpb5 512k ...)\n",
						total, m_ram_total / 1024);
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

		// The slice holds either the rest of the image or as much of it
		// as the boot area has room for.
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
			if (v + 1 < sources)
				m_boot_volumes = v + 1;
			break;
		}
		if (v + 1 >= sources)
		{
			osd_printf_error("boot: the image needs %u volumes and %u "
					"%s mounted; %u of %u bytes loaded\n", volumes,
					sources, sources == 1 ? "is" : "are", placed, total);
			return false;
		}
	}

	install_boot_image(image, from_fixed ? "the fixed disc"
			: "the internal disc");
	if (from_fixed)
		m_disc.select_device(0);   // leave the flexible drive selected

	// Put a disc back in the drive for the running system: the one after
	// the boot set if the operator mounted a spare, else volume 1. A
	// machine booted from its fixed disc may have no flexible volume at
	// all, and there is then nothing to put back.
	if (!m_media.empty())
	{
		const unsigned after = (m_boot_volumes && m_boot_volumes < m_media.size())
				? m_boot_volumes : 0;
		logerror("boot: leaving volume %u in the drive\n", after + 1);
		m_disc.mount(m_media[after]);
	}
	m_disc.reset_self_test();
	return true;
}


QUICKLOAD_LOAD_MEMBER(hp9000_5xx_state::quickload_cb)
{
	constexpr u32 LOAD = 0x100000;
	const u64 size = image.length();
	if (size < 0x100 || LOAD + size > m_ram_total)
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
	0xa4, 0x7c, 0x4e, 0x4d, 0x5c, 0x6c, 0x65, 0x5d,   // @ABCDEFG
	0x66, 0x76, 0x5e, 0x67, 0x5f, 0x4f, 0x57, 0x6f,   // HIJKLMNO
	0x77, 0x78, 0x74, 0x54, 0x6d, 0x6e, 0x56, 0x64,   // PQRSTUVW
	0x55, 0x75, 0x4c, 0x69, 0x68, 0x71, 0x23, 0xa7,   // XYZ[\]^_
	0x28, 0x7c, 0x4e, 0x4d, 0x5c, 0x6c, 0x65, 0x5d,   // `abcdefg
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
	// The whole alphabet reaches the machine, and getting there took
	// two corrections. The first: bit 7 of the keycode is Shift, and on
	// this machine the SHIFTED letter is the lower-case one -- the
	// opposite of a modern keyboard -- so the host's lower-case letters
	// are the ones that carry the bit.
	//
	// The second was `e`, which typed `{` for a long time and was
	// written up as a key this model had no way to press. It was simply
	// the wrong keycode. `E` was mapped to 0x2a because a sweep of the
	// unshifted half found 0x2a echoing `E` and stopped there; 0x2a's
	// shifted partner 0xaa is `{`, and no amount of shift makes it an
	// `e`. The E key is 0x6c: 0x6c echoes `E` and 0xec echoes `e`, which
	// a two-keystroke experiment shows directly. The lesson generalises
	// to the rest of this table -- a sweep that validates one half of a
	// shifted pair has validated half a key.
	//
	// It mattered: most demo-disc file names are lower case, and
	// `/DEMO/DIR/others/QIX` could not be typed at all until this was
	// right.
	//
	// Keys with no character of their own are not in the table at all and
	// are reached with FOCUS_TYPE's \xNN. The ones identified so far:
	// 0x48 EXECUTE (what `\n` sends), 0x60 STORE (`\r`), 0x70 clears the
	// input line, 0x7f is the space bar, and **0x1d is CONTINUE** -- the
	// key an INPUT that offers a default is asking for, found by
	// bisecting 0x00-0x7f against a demo program's next question.
	//
	static const knob::flag rawkeys("FOCUS_RAWKEYS");
	u8 code = rawkeys ? data : KEYCODES[data & 0x7f];
	if (!rawkeys && data >= 'a' && data <= 'z')
		code |= 0x80;
	if (rawkeys)
	{
		// FOCUS_KEYOR adds flag bits above the 8-bit host byte, for
		// probing the holding register's shift/control encoding.
		const u16 flags = u16(knob::number("FOCUS_KEYOR", 0, 16));
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

	static const knob::number dimdump("FOCUS_DIMDUMP", 0);
	if (dimdump.set() && (screen.frame_number() % dimdump.get()) == 0)
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

	gfx_dump(screen);

	// The graphics plane first, because the alphanumeric one goes over
	// it: "alpha pixels are dominant over graphics -- wherever an alpha
	// pixel is present, the graphics pixel at the same location is
	// suppressed" (HARDWARE.md 2.7.2). Three memory planes drive the
	// guns directly, which is what the 9020C's "8 pure" colours are;
	// a display with one plane lights the same phosphor the text does.
	bitmap.fill(rgb_t::black(), cliprect);
	if (!m_console_is_asi)
		for (unsigned y = 0; y < focus::gfx_card::HEIGHT; y++)
		{
			u32 *p = &bitmap.pix(y, GFX_LEFT);
			for (unsigned x = 0; x < focus::gfx_card::WIDTH; x++)
			{
				const unsigned v = m_gfx.pixel(x, y);
				*p++ = v ? (m_gfx.planes > 1
						? rgb_t((v & 1) ? 0xff : 0, (v & 2) ? 0xff : 0,
								(v & 4) ? 0xff : 0)
						: phosphor)
						: rgb_t::black();
			}
		}

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
				// An alpha pixel is present where the font bit is set,
				// and over the whole cell where it is inverse video --
				// an inverse cell's lit background is alpha too. Where
				// neither, the graphics plane already painted below and
				// must be left alone.
				for (unsigned bit = 0; bit < CELL_WIDTH; bit++, p++)
				{
					const bool on = BIT(gfx, 7 - bit) != invert;
					if (on)
						*p = phosphor;
					else if (invert)
						*p = rgb_t::black();
				}
			}
		}
	return 0;
}


// The graphics plane as text, the way FOCUS_DIMDUMP gives the
// alphanumeric one: a digit per pixel holding its three plane bits, and
// only the lines with something on them, so an empty raster costs a line
// and a drawn one reads at a glance. tools/gfxscreen.py turns it back
// into a picture. FOCUS_GFXPGM writes the same frame as a PPM.
void hp9000_5xx_state::gfx_dump(screen_device &screen)
{
	static const knob::number gfxdump("FOCUS_GFXDUMP", 0);
	static const knob::text gfxpgm("FOCUS_GFXPGM");
	if (!gfxdump.set() && !bool(gfxpgm))
		return;
	const unsigned every = gfxdump.set() ? gfxdump.get() : 60;
	if (!every || (screen.frame_number() % every))
		return;

	if (gfxdump.set())
	{
		std::printf("--- gfx frame %d planes %u stride %u origin %04x\n",
				int(screen.frame_number()), m_gfx.planes, m_gfx.stride,
				m_gfx.origin);
		for (unsigned y = 0; y < focus::gfx_card::HEIGHT; y++)
		{
			std::string line;
			bool any = false;
			for (unsigned x = 0; x < focus::gfx_card::WIDTH; x++)
			{
				const unsigned v = m_gfx.pixel(x, y);
				any |= v != 0;
				line.push_back(char('0' + v));
			}
			// The "g" is load-bearing: tools/screen.py reads the
			// alphanumeric dump's |<row> <text>| lines out of the same
			// stream, and an unmarked raster line looks exactly like one.
			if (any)
				std::printf("|g%3u %s|\n", y, line.c_str());
		}
		std::fflush(stdout);
	}

	if (bool(gfxpgm))
		if (FILE *f = std::fopen(gfxpgm.get(), "wb"))
		{
			std::fprintf(f, "P6\n%u %u\n255\n",
					focus::gfx_card::WIDTH, focus::gfx_card::HEIGHT);
			for (unsigned y = 0; y < focus::gfx_card::HEIGHT; y++)
				for (unsigned x = 0; x < focus::gfx_card::WIDTH; x++)
				{
					const unsigned v = m_gfx.pixel(x, y);
					const unsigned char px[3] = {
						(unsigned char)((v & 1) ? 0xff : 0),
						(unsigned char)((v & 2) ? 0xff : 0),
						(unsigned char)((v & 4) ? 0xff : 0) };
					std::fwrite(px, 1, 3, f);
				}
			std::fclose(f);
		}
}


// The processor stack both machines are built on: the same 18 MHz CPU
// (55 ns microinstruction time, Hardware Technical Data) and the same
// twelve-slot MPB backplane. A 520 and a 540 differ in what hangs off
// it, not in this.
void hp9000_5xx_state::add_processor_stack(machine_config &config)
{
	FOCUS(config, m_cpu, 18_MHz_XTAL);
	m_cpu->set_addrmap(AS_PROGRAM, &hp9000_5xx_state::mem_map);
	m_cpu->set_mpb_channel(1);

	// The MPB stack's RAM slots. Slot 1 is the CPU and slot 2 the IOP;
	// boards go in the ten remaining slots as -mpb3 .. -mpb12, each
	// "256k", "512k" or "1m" (1 MB boards in pairs). The default is the
	// base machine HP shipped: 1 MB as two 512 KB boards (HARDWARE.md
	// §1.2). Fit more with e.g. -mpb3 1m -mpb4 1m; the boot loads the
	// image adaptively, so small machines boot whatever fits.
	MPB_SLOT(config, m_mpb[0], mpb_ram_cards, "512k");
	MPB_SLOT(config, m_mpb[1], mpb_ram_cards, "512k");
	for (unsigned k = 2; k < 10; k++)
		MPB_SLOT(config, m_mpb[k], mpb_ram_cards, nullptr);
}


void hp9000_5xx_state::hp9020(machine_config &config)
{
	add_processor_stack(config);

	// The 520's integrated display, as its own CRTC programs it: 80
	// columns by 26 rows of 15-scanline characters.
	// The paper. MAME's image device writes the bytes straight through to
	// a file (-printer out.bin), which is the right artefact for this
	// device: it does overprinting, underline, inverse, 150% tall and
	// pixel-by-pixel graphics dumps, and its font ROM is not dumped, so
	// a text file cannot represent what it printed. FOCUS_PRINTTEXT takes
	// the lossy readable version alongside.
	PRINTER(config, m_prn, 0);

	SCREEN(config, m_screen);
	m_screen->set_refresh_hz(60);
	m_screen->set_size(SCREEN_WIDTH, SCREEN_HEIGHT);
	m_screen->set_visarea_full();
	m_screen->set_screen_update(FUNC(hp9000_5xx_state::screen_update));

	// The keyswitch matrix: a keystroke becomes a keycode in the keyboard
	// board's holding register.
	GENERIC_KEYBOARD(config, m_keyboard, 0);
	m_keyboard->set_keyboard_callback(FUNC(hp9000_5xx_state::kbd_put));

	// The internal flexible drive's media: a boot image is a SET of
	// volumes (BASIC is two, the diagnostics three), so three mounts
	// stand for the stack of discs the operator will feed the one drive,
	// in order. Raw sector images or Teledisk .TD0 both mount.
	HP_FOCUS_MEDIA(config, m_floppy[0]);
	HP_FOCUS_MEDIA(config, m_floppy[1]);
	HP_FOCUS_MEDIA(config, m_floppy[2]);
	// The internal Winchester was an option on a real 520, and the
	// system reaches it as device 1 of select code 7; -hard1 fits one.
	HP_FOCUS_MEDIA(config, m_hard).set_hard();

	// The user-expansion backplane: select codes 2-5, empty by default
	// (520 Service 2-29). -cio2 through -cio5 take "asi" or "hpib" -- the
	// same card devices, and the same option list, the 530/540 use for
	// its console and system-disc slots below.
	for (unsigned sc = 2; sc <= 5; sc++)
		FOCUS_CIO_SLOT(config, m_cio[sc], focus_cio_cards, nullptr);

	// The quickload remains as a debug side door for images that exist
	// only as files; the supported path is booting from the discs.
	QUICKLOAD(config, "quickload", "bin").set_load_callback(
			FUNC(hp9000_5xx_state::quickload_cb));
}


void hp9000_5xx_state::hp9040(machine_config &config)
{
	// The 540 is the same processor stack in a free-standing cabinet with
	// no integrated display. Its console is an RS-232 terminal on a
	// serial interface card in slot 0 -- the CE handbook's installation
	// procedure names the card and sets the terminal to 9600 baud, 8
	// bits, no parity, ENQ/ACK and XON/XOFF (9030/40 CE Handbook, 2-3).
	add_processor_stack(config);
	m_console_is_asi = true;

	GENERIC_TERMINAL(config, m_terminal, 0);
	m_terminal->set_keyboard_callback(FUNC(hp9000_5xx_state::term_put));

	// The whole machine's I/O is the backplane: select codes 0-6, seven
	// labelled slots, with the SCM at 7 (530/540 Service 2-27). Nothing
	// is built in, so the two cards that make the machine usable are
	// fitted by default exactly where the installation procedure puts
	// them -- the System Terminal interface in slot 0 and the System
	// Disc HP-IB in slot 5 (9030/40 CE Handbook 2-3). Both are ordinary
	// slot options: -cio0 and -cio5 can empty or re-card either one, and
	// the remaining five slots start empty.
	for (unsigned sc = 0; sc <= 6; sc++)
		FOCUS_CIO_SLOT(config, m_cio[sc], focus_cio_cards,
				sc == 0 ? "asi" : sc == 5 ? "hpib" : nullptr);

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
