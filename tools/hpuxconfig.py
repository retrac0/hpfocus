#!/usr/bin/env python3
"""Turn the assembled HP-UX 5.30 system disc multiuser, in place.

The volume tools/mkhpuxdisc.py assembles boots to a single-user shell, for
one reason init prints itself: "No passwd entry for root / FIX root ENTRY
IN /etc/passwd AND REBOOT". Three things stand between that disc and a
login on every port of an HP 27140A six-channel multiplexer:

  /etc/passwd    root's password field is `,..` -- an empty password with
                 a System V aging string whose maximum age is 0 weeks, so
                 the entry is expired the moment it is read. It becomes
                 `root::0:1:The Super User:/usr/root:/bin/sh`, the shape
                 /etc/newconfig/passwd ships (sh, so /etc/profile runs;
                 the shell is one byte shorter than csh and needs no
                 dot-files).
  /etc/inittab   `#is:2:initdefault:` loses its `#`, and the gettys on
                 tty01..tty06 go from `off` to `respawn` -- the same
                 seven characters -- with gettydefs entry H (9600, CLOCAL,
                 8 bits) on every one, since a MAME terminal is 8N1 and
                 entry M is 7-bit even parity behind carrier detect.
  /dev/tty01..06 the nodes exist, but as `c 31 0x000N00` -- the 27130A
                 eight-channel MUX the disc came off. A 27140A port is
                 major 29, minor `Sc 0N 04`: select code, port, direct
                 (hard-wired) access (mkdev(1M): "Modem MUX ... Hard wire
                 ... c 29 0xScAd04"). tty01 is port 0.

Every edit is the same size or shorter than what it replaces, so nothing
is allocated: a file is rewritten into the extents it already has and its
length field (i-node +76) updated; a device node has its major and minor
bytes (i-node +124..127) restamped. Nothing else on the volume moves.
Idempotent -- running it on a patched disc changes nothing -- and undone
by deleting the volume, which tools/runhpux.sh rebuilds from the tapes.

    python3 tools/hpuxconfig.py software/cct/7914_hpux_volume.bin [--sc 2]
    python3 tools/hpuxconfig.py --check software/cct/7914_hpux_volume.bin
"""
import argparse
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sdf7914 as sdf  # noqa: E402

BLK = sdf.BLK
FA_REC = sdf.FA_REC
PORTS = 6
MAJOR_MUX6 = 29     # "9020 console, HP98700 console, HP27140 terminal interface"
MODE_DIRECT = 0x04

PASSWD_OLD = b"root:,..:0:1:The Super User:/usr/root:/bin/csh"
PASSWD_NEW = b"root::0:1:The Super User:/usr/root:/bin/sh"


class Volume:
    """An SDF volume open for in-place edits."""

    def __init__(self, path):
        self.path = path
        self.data = bytearray(path.read_bytes())
        self.disc = sdf.Disc([(self.data, 0)])
        cands = sdf.find_fa_root([(self.data, 0)])
        if len(cands) != 1:
            raise SystemExit(f"{path}: {len(cands)} FA-file candidates, expected 1")
        fa_ino, _root, at = cands[0]
        self.fs = sdf.SDF(self.disc, fa_ino)
        self.fa_block = at
        self.writes = []    # (offset, bytes)

    # ---- addressing -----------------------------------------------------

    def record_offset(self, n):
        """Image byte offset of FA record n (i-node n)."""
        want, seen = n * FA_REC, 0
        for s, c in self.fs.ext:
            if seen + c * BLK > want:
                return s * BLK + (want - seen)
            seen += c * BLK
        raise SystemExit(f"i-node {n} is outside the FA file")

    def lookup(self, path):
        """The i-node dict for an absolute path."""
        ino = self.fs.inode(1)
        for name in path.strip("/").split("/"):
            if not name:
                continue
            ents = dict(self.fs.readdir(ino))
            if name not in ents:
                raise SystemExit(f"{path}: no such file on the volume")
            ino = self.fs.inode(ents[name])
        return ino

    def capacity(self, ino):
        return sum(c for _, c in self.fs.all_extents(ino)) * BLK

    # ---- edits ------------------------------------------------------------

    def patch(self, off, blob):
        self.data[off:off + len(blob)] = blob
        self.writes.append((off, bytes(blob)))

    def write_file(self, ino, new):
        """Replace a file's content with something no longer than it."""
        old = self.fs.read_file(ino)
        if len(new) > len(old):
            raise SystemExit(f"{ino['name']}: replacement is longer "
                             f"({len(new)} > {len(old)}); only shrinks are safe")
        blob = new + bytes(len(old) - len(new))     # zero the freed tail
        pos = 0
        for s, c in self.fs.all_extents(ino):
            n = min(len(blob) - pos, c * BLK)
            if n <= 0:
                break
            self.patch(s * BLK, blob[pos:pos + n])
            pos += n
        rec = self.record_offset(ino["ino"])
        self.patch(rec + 76, struct.pack(">i", len(new)))

    def set_device(self, ino, major, minor):
        rec = self.record_offset(ino["ino"])
        mode = ino["mode"]
        if (mode & 0o170000) != 0o020000:
            raise SystemExit(f"i-node {ino['ino']}: not a character device "
                             f"(mode {mode:06o})")
        self.patch(rec + 124, bytes([major & 0xff, (minor >> 16) & 0xff,
                                     (minor >> 8) & 0xff, minor & 0xff]))

    def device_of(self, ino):
        rec = self.record_offset(ino["ino"])
        r = self.data[rec:rec + FA_REC]
        return r[124], (r[125] << 16) | (r[126] << 8) | r[127]

    def commit(self):
        if not self.writes:
            return 0
        with open(self.path, "r+b") as f:
            for off, blob in self.writes:
                f.seek(off)
                f.write(blob)
        n = len(self.writes)
        self.writes = []
        return n


# ---- the three edits --------------------------------------------------------

def fix_passwd(vol):
    ino = vol.lookup("/etc/passwd")
    old = vol.fs.read_file(ino)
    if PASSWD_NEW in old:
        return "already fixed"
    if PASSWD_OLD not in old:
        raise SystemExit("/etc/passwd: the root line is not the one this "
                         "tool knows; refusing to guess")
    vol.write_file(ino, old.replace(PASSWD_OLD, PASSWD_NEW, 1))
    return "root entry rewritten"


def inittab_text(old):
    new = old.replace(b"#is:2:initdefault:", b"is:2:initdefault:", 1)
    for n in range(1, PORTS + 1):
        pat = re.compile(rb"^(%02d:2 :)off    (:/etc/getty tty%02d )[HM]"
                         % (n, n), re.M)
        new = pat.sub(lambda m: m.group(1) + b"respawn" + m.group(2) + b"H", new)
    return new


def fix_inittab(vol):
    ino = vol.lookup("/etc/inittab")
    old = vol.fs.read_file(ino)
    new = inittab_text(old)
    if new == old:
        return "already fixed"
    vol.write_file(ino, new)
    return "initdefault 2, gettys tty01..tty%02d respawn (H)" % PORTS


def fix_devices(vol, sc):
    done = []
    for n in range(PORTS):
        name = "/dev/tty%02d" % (n + 1)
        ino = vol.lookup(name)
        want = (MAJOR_MUX6, (sc << 16) | (n << 8) | MODE_DIRECT)
        if vol.device_of(ino) == want:
            continue
        vol.set_device(ino, *want)
        done.append(name)
    return ("restamped " + ", ".join(done)) if done else "already fixed"


def check(vol):
    for f in ("/etc/passwd", "/etc/inittab"):
        ino = vol.lookup(f)
        text = vol.fs.read_file(ino).decode("ascii", "replace")
        print(f"== {f} ({ino['size']} bytes)")
        for line in text.splitlines():
            if f == "/etc/passwd" and not line.startswith("root:"):
                continue
            if f == "/etc/inittab" and "getty" not in line and "initdefault" not in line:
                continue
            print("   " + line)
    print("== /dev")
    for n in range(1, PORTS + 2):
        name = "/dev/tty%02d" % n
        ino = vol.lookup(name)
        major, minor = vol.device_of(ino)
        print(f"   {name}  c {major:2d} 0x{minor:06x}")


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("volume", type=Path, help="the assembled system disc")
    ap.add_argument("--sc", type=int, default=2,
                    help="the select code the 27140A sits at (default 2)")
    ap.add_argument("--check", action="store_true",
                    help="show the files and nodes; change nothing")
    args = ap.parse_args()
    vol = Volume(args.volume)
    if args.check:
        check(vol)
        return 0
    print("/etc/passwd:  " + fix_passwd(vol))
    print("/etc/inittab: " + fix_inittab(vol))
    print("/dev/tty0N:   " + fix_devices(vol, args.sc))
    n = vol.commit()
    print(f"{n} write(s) to {args.volume}" if n else "volume unchanged")
    return 0


if __name__ == "__main__":
    sys.exit(main())
