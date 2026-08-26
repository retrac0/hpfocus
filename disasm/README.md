# `disasm/` — listings of the boot image's own code

Everything here is **generated**, and one command rebuilds all of it from the
shipped `500ENV1-3` discs:

```sh
tools/mkdisasm.sh
```

so only this file is tracked. The rest is gitignored.

## What is in it

`env.bin` is the assembled diagnostics boot image — the same 680,668-byte,
29-segment image `tools/mkdiagdisc.sh` installs, kept because
[`tools/segdump.py`](../tools/segdump.py) and [`tools/stt.py`](../tools/stt.py)
want the whole image rather than one extracted segment:

```sh
python3 tools/segdump.py disasm/env.bin              # the segment chain
python3 tools/stt.py     disasm/env.bin              # every segment's STT
python3 tools/stt.py     disasm/env.bin --segment 13 # one table in full
python3 tools/stt.py     disasm/env.bin --callers 13 # who calls into it
```

Then, per module:

| file | what |
|---|---|
| `<NAME>.bin` | the segment's code, header and 16-byte name stripped |
| `<NAME>.raw.dis` | [`tools/focusdis.py`](../tools/focusdis.py)'s plain listing, for diffing |
| `<NAME>.dis` | [`tools/structdis.py`](../tools/structdis.py)'s annotated listing |

`mkdisasm.sh` **will not regenerate a `.dis` that already exists** — extraction
is mechanical and reproducible, but an annotated listing may carry structural
comments and hand corrections worth keeping. Delete one yourself to get a
fresh one.

## Which modules, and why

Five are the diagnostics-only binaries that appear on no shipped option disc —
`GRAPHICS`, `SRM`, `HP98770`, `HP97060`, `HP97062` — and are the reason the
9020 System Functional Test driver needs this environment rather than plain
BASIC 2.0 (DIAGNOSTICS.md §1).

Two more are here because they are the machine's own specification of hardware
no manual in the collection documents:

- **`PWP SELF TEST`** (segment 13, 3,712 bytes) — the smallest segment in the
  image and the most useful. Its local entries 17–32 are HP's power-up self
  test for the display, and they name every graphics command, its parameter
  count and both display memory sizes. The whole command table in
  [`src/iop/focusgfx.h`](../src/iop/focusgfx.h) and HARDWARE.md §2.7.4 was read
  off this listing.
- **`SUN OS KERNEL`** (segment 2) — the register and DMA primitives every
  driver above it calls, so a command in a driver's listing only becomes a
  register access by way of this one.

## Finding your way around a listing

A segment's local entries are named by its STT, which gives `PB+X` offsets.
The extracted `.bin` starts 36 bytes past `PB`, so:

    file offset = X - 0x24

For example `PWP SELF TEST` local 21 is `PB+0x764`, at `0x740` in
`disasm/PWP SELF TEST.raw.dis`. `tools/focusdis.py --start`/`--count` will
disassemble a window of `env.bin` directly if you would rather not extract.
