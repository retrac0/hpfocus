#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Joel Heikkila
"""Rebuild the 520's graphics raster from a FOCUS_GFXDUMP log.

FOCUS_GFXDUMP=<n> makes the emulator print the graphics plane every n
frames, the way FOCUS_DIMDUMP prints the alphanumeric one. A frame is a
header and then one line per raster line that has anything on it:

    --- gfx frame 2400 planes 3 stride 36 origin 0000
    |g454 7777777777777777...|

The digit is the pixel's three plane bits -- 0 is unlit and 7 is all three
planes, which the screen shows as white. The "g" is what tells these lines
apart from the alphanumeric dump's, which tools/screen.py reads out of the
same stream.

    tools/gfxscreen.py < run.log              the last frame, as text
    tools/gfxscreen.py --all < run.log        every frame
    tools/gfxscreen.py --plane 1 < run.log    one plane only
    tools/gfxscreen.py --ppm out.ppm < run.log
    tools/gfxscreen.py --crop < run.log       only the lines with pixels

Blank lines are filled back in, so what comes out is the raster the screen
draws rather than the compressed dump.
"""
import argparse
import re
import sys

WIDTH, HEIGHT = 560, 455
HEAD = re.compile(r"^--- gfx frame (\d+) planes (\d+) stride (\d+) origin ([0-9a-f]+)")
LINE = re.compile(r"^\|g\s*(\d+) (.*)\|$")


def frames(text):
    """Yield (header dict, {y: row}) for each dump in the log."""
    cur = None
    for ln in text.splitlines():
        m = HEAD.match(ln)
        if m:
            if cur:
                yield cur
            cur = ({"frame": int(m.group(1)), "planes": int(m.group(2)),
                    "stride": int(m.group(3)), "origin": int(m.group(4), 16)}, {})
            continue
        m = LINE.match(ln)
        if m and cur:
            cur[1][int(m.group(1))] = m.group(2)
    if cur:
        yield cur


def render(hdr, rows, plane, crop):
    out = []
    ys = sorted(rows) if crop else range(HEIGHT)
    for y in ys:
        row = rows.get(y, "0" * WIDTH)
        if plane is not None:
            row = "".join("#" if (int(c, 8) >> plane) & 1 else "." for c in row)
        else:
            row = row.replace("0", ".")
        out.append(f"{y:3d} {row}")
    return out


def ppm(path, rows):
    with open(path, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (WIDTH, HEIGHT))
        for y in range(HEIGHT):
            row = rows.get(y, "0" * WIDTH)
            buf = bytearray()
            for c in row.ljust(WIDTH, "0")[:WIDTH]:
                v = int(c, 8)
                buf += bytes((255 if v & 1 else 0, 255 if v & 2 else 0,
                              255 if v & 4 else 0))
            f.write(bytes(buf))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log", nargs="?", help="default: stdin")
    ap.add_argument("--all", action="store_true", help="every frame, not just the last")
    ap.add_argument("--plane", type=int, help="show one plane as #/. instead of the pixel value")
    ap.add_argument("--crop", action="store_true", help="only the lines that have pixels")
    ap.add_argument("--ppm", help="write the frame as a PPM image")
    a = ap.parse_args()

    text = open(a.log).read() if a.log else sys.stdin.read()
    got = list(frames(text))
    if not got:
        print("no FOCUS_GFXDUMP frames in the log", file=sys.stderr)
        return 1

    for hdr, rows in (got if a.all else got[-1:]):
        print(f"--- frame {hdr['frame']}  {hdr['planes']} planes, "
              f"stride {hdr['stride']}, origin {hdr['origin']:#06x}, "
              f"{len(rows)} lines with pixels")
        for ln in render(hdr, rows, a.plane, a.crop):
            print(ln)
    if a.ppm:
        ppm(a.ppm, got[-1][1])
    return 0


if __name__ == "__main__":
    # A 455-line raster is nearly always piped into head or less, and a
    # closed pipe is not an error here.
    import signal
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    raise SystemExit(main())
