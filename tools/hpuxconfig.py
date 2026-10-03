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

Since the MVB fix init reads that passwd entry correctly, and the shipped
disc boots instead to init's "ENTER RUN LEVEL (0-6,s, or S):" prompt,
because its only initdefault line is commented out. --runlevel picks what
init does without asking:

  --runlevel 2   (the default) the multiuser edits above, initdefault 2
  --runlevel s   initdefault s and nothing else: init(1M), "the initdefault
                 entry can use s to specify that init start in the SINGLE
                 USER state" -- root's csh on the console, as shipped

Every edit is the same size or shorter than what it replaces, so nothing
is allocated: a file is rewritten into the extents it already has and its
length field (i-node +76) updated; a device node has its major and minor
bytes (i-node +124..127) restamped. Nothing else on the volume moves.

The disc is HP-UX's from the first boot on, so the tool runs on a live
system's volume before every boot and must never undo what that system or
its user has done. It edits root's passwd entry only while it is still the
shipped expired one (a password set with passwd(1), or a chsh or chfn,
is left alone); it turns the gettys on only when the disc is entering run
level 2 and all six are still `off` as shipped (a port its user turned off
stays off); it changes nothing at all on a disc already as asked. It also
puts right the s_maxblk an older tools/mkhpuxdisc.py wrote (see there),
which would otherwise let HP-UX allocate the FA file's own extent map.

Everything is checked before anything is written, and the new volume is
written beside the old one and renamed into place, so an image of an
unexpected shape is refused whole and an interrupted run leaves the old
disc as it was. Undone by deleting the volume, which tools/runhpux.sh
assembles again from the tapes.

    python3 tools/hpuxconfig.py <disc> [--runlevel s|2] [--sc 2]
    python3 tools/hpuxconfig.py <disc> --check [--runlevel s|2]

--check writes nothing. It shows the disc's state; given --runlevel too,
it lists what that run level would change and exits 1 if anything would
(0 if the disc is already so, 2 if the disc cannot be configured).
"""
import argparse
import os
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import sdf7914 as sdf  # noqa: E402
import mkhpuxdisc  # noqa: E402

BLK = sdf.BLK
FA_REC = sdf.FA_REC
PORTS = 6
MAJOR_MUX6 = 29     # "9020 console, HP98700 console, HP27140 terminal interface"
MODE_DIRECT = 0x04

PASSWD_OLD = b"root:,..:0:1:The Super User:/usr/root:/bin/csh"
PASSWD_NEW = b"root::0:1:The Super User:/usr/root:/bin/sh"


def refuse(msg):
    """Stop without writing anything: every edit is planned in memory and
    only Volume.commit touches the disc."""
    print(f"hpuxconfig: {msg}", file=sys.stderr)
    sys.exit(2)


class Volume:
    """An SDF volume open for in-place edits."""

    def __init__(self, path):
        self.path = path
        try:
            self.data = bytearray(path.read_bytes())
        except OSError as e:
            refuse(f"{path}: {e.strerror} (tools/mkhpuxdisc.py assembles "
                   "the disc from the two unit-save tapes)")
        exmap_at = (mkhpuxdisc.S_FA * BLK + mkhpuxdisc.FA_EXMAP * FA_REC)
        self.header_ok, self.header_msg = mkhpuxdisc.header_state(
            self.data[:BLK], self.data[exmap_at:exmap_at + FA_REC])
        if self.header_ok is None:
            refuse(f"{path}: {self.header_msg}")
        self.disc = sdf.Disc([(self.data, 0)])
        cands = [c for c in sdf.find_fa_root([(self.data, 0)])
                 if c[2] == mkhpuxdisc.S_FA]
        if len(cands) != 1:
            refuse(f"{path}: no FA file at block {mkhpuxdisc.S_FA:,}; "
                   "not the HP-UX 5.30 volume this tool knows")
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
        refuse(f"i-node {n} is outside the FA file")

    def lookup(self, path):
        """The i-node dict for an absolute path."""
        ino = self.fs.inode(1)
        for name in path.strip("/").split("/"):
            if not name:
                continue
            ents = dict(self.fs.readdir(ino))
            if name not in ents:
                refuse(f"{path}: no such file on the volume")
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
            refuse(f"{ino['name']}: replacement is longer "
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
            refuse(f"i-node {ino['ino']}: not a character device "
                   f"(mode {mode:06o})")
        self.patch(rec + 124, bytes([major & 0xff, (minor >> 16) & 0xff,
                                     (minor >> 8) & 0xff, minor & 0xff]))

    def device_of(self, ino):
        rec = self.record_offset(ino["ino"])
        r = self.data[rec:rec + FA_REC]
        return r[124], (r[125] << 16) | (r[126] << 8) | r[127]

    def commit(self):
        """Write the edited volume beside the old one, then rename it into
        place: whatever stops this run, the disc is either the old one or
        the new one, never half of each."""
        if not self.writes:
            return 0
        part = self.path.with_name(self.path.name + ".part")
        try:
            with open(part, "wb") as f:
                f.write(self.data)
                f.flush()
                os.fsync(f.fileno())
            os.chmod(part, os.stat(self.path).st_mode & 0o7777)
            os.replace(part, self.path)
        except OSError as e:
            try:
                part.unlink()
            except OSError:
                pass
            refuse(f"{self.path}: {e.strerror}; the disc is unchanged")
        n = len(self.writes)
        self.writes = []
        return n


# ---- the edits --------------------------------------------------------------

def root_line(vol):
    """(i-node, file text, root's line) of /etc/passwd; refuses unless there
    is exactly one root entry."""
    ino = vol.lookup("/etc/passwd")
    text = vol.fs.read_file(ino)
    roots = [ln for ln in text.split(b"\n") if ln.startswith(b"root:")]
    if len(roots) != 1:
        refuse(f"/etc/passwd: {len(roots)} root entries; refusing to guess")
    return ino, text, roots[0]


def passwd_state(line):
    if line == PASSWD_OLD:
        return "shipped: an empty password that expires at once"
    if line == PASSWD_NEW:
        return "set for multiuser by this tool"
    if line.split(b":")[1].startswith(b","):
        return "unknown: an empty password with an aging field"
    return "the running system's own (left as it is)"


def fix_passwd(vol):
    ino, old, line = root_line(vol)
    if line == PASSWD_OLD:
        vol.write_file(ino, old.replace(PASSWD_OLD, PASSWD_NEW, 1))
        return "root entry rewritten"
    # Once HP-UX has run, root's entry is its user's: passwd(1), chsh and
    # chfn all rewrite it, and refusing here would stop every later boot.
    # Only an empty password still carrying an aging field -- one login
    # would stop at, demanding a new password -- is not left alone.
    if line.split(b":")[1].startswith(b","):
        refuse("/etc/passwd: root's entry is not the one this tool knows "
               "and would stop a login at the password-aging prompt: "
               + line.decode("ascii", "replace"))
    return "already fixed" if line == PASSWD_NEW else \
        "root entry is the running system's own; left as it is"


INITDEFAULT = re.compile(rb"^(#?)is:([0-6sS]):initdefault:", re.M)


def initdefault(text):
    """(commented, run level, match) of inittab's one initdefault line."""
    found = list(INITDEFAULT.finditer(text))
    if len(found) != 1 or len(re.findall(rb"initdefault", text)) != 1:
        refuse("/etc/inittab: no single initdefault line this tool knows; "
               "refusing to guess")
    m = found[0]
    return bool(m.group(1)), m.group(2).decode(), m


def getty_actions(text):
    """{port: action} for the tty01..tty06 getty lines that exist."""
    out = {}
    for n in range(1, PORTS + 1):
        m = re.search(rb"^%02d:[^:\n]*:([^:\n]*):/etc/getty tty%02d " % (n, n),
                      text, re.M)
        if m:
            out[n] = m.group(1).strip().decode("ascii", "replace")
    return out


def gettys_on(text):
    for n in range(1, PORTS + 1):
        pat = re.compile(rb"^(%02d:2 :)off    (:/etc/getty tty%02d )[HM]"
                         % (n, n), re.M)
        text, k = pat.subn(lambda m: m.group(1) + b"respawn" + m.group(2) + b"H",
                           text)
        if k != 1:
            refuse("/etc/inittab: no getty line for tty%02d in the shape "
                   "this tool knows; refusing to guess" % n)
    return text


def fix_inittab(vol, level):
    ino = vol.lookup("/etc/inittab")
    old = vol.fs.read_file(ino)
    commented, cur, m = initdefault(old)
    if not commented and cur == level:
        return "already fixed"
    line = b"is:%s:initdefault:" % level.encode()
    new = old[:m.start()] + line + old[m.end():]
    did = "initdefault %s" % level
    # The gettys are turned on as part of making the disc multiuser, and
    # only while all six are still `off` as shipped; any other mix is the
    # running system's own and is left as it is.
    acts = getty_actions(new)
    if level == "2" and len(acts) == PORTS \
            and all(a == "off" for a in acts.values()):
        new = gettys_on(new)
        did += ", gettys tty01..tty%02d respawn (H)" % PORTS
    vol.write_file(ino, new)
    return did


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


def fix_header(vol):
    if vol.header_ok:
        return "already fixed"
    vol.patch(0x30, struct.pack(">I", mkhpuxdisc.MAXBLK))
    return "s_maxblk %s -> %s" % (format(mkhpuxdisc.OLD_MAXBLK, ","),
                                  format(mkhpuxdisc.MAXBLK, ","))


def configure(vol, level, sc):
    """Plan every edit the run level needs; returns the report lines."""
    out = [("volume header", fix_header(vol))]
    if level == "2":
        out.append(("/etc/passwd", fix_passwd(vol)))
    out.append(("/etc/inittab", fix_inittab(vol, level)))
    if level == "2":
        out.append(("/dev/tty0N", fix_devices(vol, sc)))
    return out


def check(vol):
    print(f"== volume header: {vol.header_msg}"
          + ("" if vol.header_ok else " -- would be repaired"))
    _ino, _text, line = root_line(vol)
    print(f"== /etc/passwd: root {passwd_state(line)}")
    print("   " + line.decode("ascii", "replace"))
    ino = vol.lookup("/etc/inittab")
    text = vol.fs.read_file(ino)
    commented, level, _m = initdefault(text)
    print("== /etc/inittab: " + (
        "no initdefault (shipped): init asks ENTER RUN LEVEL" if commented else
        "initdefault %s%s" % (level, {"2": " (multiuser)", "s": " (single user)",
                                       "S": " (single user)"}.get(level, ""))))
    for ln in text.decode("ascii", "replace").splitlines():
        if "getty" in ln or "initdefault" in ln:
            print("   " + ln)
    print("== /dev")
    for n in range(1, PORTS + 2):
        name = "/dev/tty%02d" % n
        major, minor = vol.device_of(vol.lookup(name))
        print(f"   {name}  c {major:2d} 0x{minor:06x}")


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("volume", type=Path, help="the assembled system disc")
    ap.add_argument("--runlevel", choices=("2", "s"),
                    help="what init starts in: 2, multiuser with a login on "
                         "every MUX port (the default), or s, single user")
    ap.add_argument("--sc", type=int, default=2,
                    help="the select code the 27140A sits at (default 2)")
    ap.add_argument("--check", action="store_true",
                    help="show the disc's state (and, with --runlevel, what "
                         "would change); write nothing")
    args = ap.parse_args()
    if not 0 <= args.sc <= 15:
        ap.error("--sc must be a select code, 0..15")
    vol = Volume(args.volume)
    if args.check:
        check(vol)
        if not args.runlevel:
            return 0
        plan = configure(vol, args.runlevel, args.sc)
        changes = [(w, r) for w, r in plan if r != "already fixed"
                   and not r.endswith("left as it is")]
        print(f"== --runlevel {args.runlevel} would change: "
              + ("nothing" if not changes else
                 "; ".join(f"{w}: {r}" for w, r in changes)))
        return 1 if changes else 0
    for what, result in configure(vol, args.runlevel or "2", args.sc):
        print(f"{what + ':':15s}{result}")
    n = vol.commit()
    print(f"{n} edit(s) written to {args.volume}" if n else "volume unchanged")
    return 0


if __name__ == "__main__":
    sys.exit(main())
