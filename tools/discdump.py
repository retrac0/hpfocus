#!/usr/bin/env python3
"""Recover what can be recovered from a Series 500 disc unit save.

The 7914 unit saves are raw disc images whose filesystem is SDF, HP's own
Structured Directory Format. The on-disc form of SDF is **not** decoded --
see the note at the bottom of this file for what has been ruled out. What
this tool does recover, without needing that:

  * the boot image, located by its descriptor;
  * every embedded cpio archive member, both the ASCII (odc) and the
    big-endian binary forms, each header validated rather than pattern
    matched;
  * an inventory of loadable modules, by their e307fffc signature.

    discdump.py <image> --list
    discdump.py <image> --extract DIR
"""
import argparse
import struct
from pathlib import Path

SECTOR = 256
MODULE_SIG = bytes([0xe3, 0x07, 0xff, 0xfc])
BOOT_MAGIC = 0xe9c28206


def boot_image(d: bytes):
    """The descriptor is a 48-byte record; find it and return the image."""
    for off in (0, 0x400):
        if off + 32 > len(d):
            continue
        w = struct.unpack(">8I", d[off:off + 32])
        if w[0] == BOOT_MAGIC and 0 < w[1] < len(d):
            label = d[off + 0x30:off + 0x80].decode("ascii", "replace").rstrip()
            # The image itself begins at the first module header, which is
            # more reliable than the descriptor's sector field.
            i = d.find(MODULE_SIG)
            start = (i - 12) if i >= 12 else off + 0x400
            return {"desc": off, "start": start, "size": w[1], "label": label}
    return None


def ascii_cpio(d: bytes):
    """ASCII cpio (odc): '070707' then octal fields. Validated, not guessed."""
    i = 0
    while True:
        i = d.find(b"070707", i)
        if i < 0:
            return
        h = d[i:i + 76]
        if len(h) == 76 and all(0x30 <= c <= 0x37 for c in h[6:76]):
            ns = int(h[59:65], 8)
            sz = int(h[65:76], 8)
            if 1 <= ns <= 256:
                name = d[i + 76:i + 76 + ns - 1].decode("ascii", "replace")
                body = i + 76 + ns
                yield {"kind": "odc", "at": i, "name": name,
                       "size": sz, "body": body}
        i += 6


def binary_cpio(d: bytes):
    """Old binary cpio with big-endian fields, as FOCUS writes them."""
    i = 0
    magic = bytes([0x71, 0xc7])
    while True:
        i = d.find(magic, i)
        if i < 0:
            return
        if i + 26 <= len(d):
            f = struct.unpack(">13H", d[i:i + 26])
            ns = f[10]
            sz = (f[11] << 16) | f[12]
            mode = f[3]
            # Reject the many chance matches: a real header has a sane name
            # length, a plausible mode, and a NUL-terminated ASCII name.
            sane_mode = (mode & 0o170000) in (
                    0o100000, 0o040000, 0o120000, 0o020000, 0o060000)
            if 2 <= ns <= 256 and sane_mode:
                raw = d[i + 26:i + 26 + ns]
                if raw.endswith(b"\x00") and all(
                        32 <= c < 127 for c in raw[:-1]):
                    name = raw[:-1].decode("ascii")
                    body = i + 26 + ns + (ns & 1)
                    yield {"kind": "bin", "at": i, "name": name,
                           "size": sz, "body": body}
        i += 2


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("image", type=Path)
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--extract", type=Path)
    args = ap.parse_args()

    d = args.image.read_bytes()
    print(f"{args.image.name}: {len(d):,} bytes, {len(d)//SECTOR:,} sectors\n")

    b = boot_image(d)
    if b:
        print(f"boot image  {b['start']:#010x}  {b['size']:,} bytes")
        print(f"            {b['label']}\n")

    members = sorted(list(ascii_cpio(d)) + list(binary_cpio(d)),
                     key=lambda m: m["at"])
    print(f"cpio members: {len(members)} "
          f"({sum(1 for m in members if m['kind']=='odc')} ASCII, "
          f"{sum(1 for m in members if m['kind']=='bin')} binary)")

    mods = d.count(MODULE_SIG)
    print(f"module signatures: {mods}\n")

    if args.list:
        for m in members[:200]:
            print(f"  {m['at']:#010x} {m['kind']} {m['size']:>9,}  {m['name']}")
        if len(members) > 200:
            print(f"  ... and {len(members)-200} more")

    if args.extract:
        root = args.extract
        root.mkdir(parents=True, exist_ok=True)
        if b:
            (root / "boot_image.bin").write_bytes(
                    d[b["start"]:b["start"] + b["size"]])
        n = 0
        for m in members:
            if m["name"] in ("TRAILER!!!", "") or m["size"] == 0:
                continue
            # Names come from the image; keep them inside the output tree.
            rel = Path(m["name"].lstrip("/"))
            if ".." in rel.parts:
                continue
            dest = root / "cpio" / rel
            # The same name can appear as both a file and a directory across
            # different archives on the disc; keep both rather than crashing.
            try:
                dest.parent.mkdir(parents=True, exist_ok=True)
                if dest.is_dir():
                    dest = dest.with_name(dest.name + ".file")
                dest.write_bytes(d[m["body"]:m["body"] + m["size"]])
                n += 1
            except (FileExistsError, NotADirectoryError, OSError):
                continue
        print(f"extracted {n} files to {root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())


# WHAT SDF'S ON-DISC FORM IS NOT
#
# Ruled out while looking for the directory structure:
#
#   * the floppy layout -- a 48-byte volume header with magic 07000000 and a
#     printable part number, then 36-byte directory entries. No sector on
#     either tape carries a header with a printable label.
#   * a run of 36-byte directory entries anywhere on the disc. The longest
#     such run, 302 entries, turned out to be process accounting records in
#     /usr/adm/pacct -- 8-character command names padded to a fixed record,
#     with sh, date, cc, ld, cpp and ccom repeating in the pattern of a
#     compile session. A nice find, but not a directory.
#
# The boot image's own module list names SDF and HP-UX DIRECTORY modules, so
# the filesystem code is present in the image and could be disassembled to
# recover the format. That is the remaining route.
