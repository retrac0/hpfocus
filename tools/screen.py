#!/usr/bin/env python3
"""Reconstruct the 500's display from a FOCUS_DIMDUMP log.

FOCUS_DIMDUMP prints the display card's frame buffer in *vram* order:

    --- frame 1440 start 0000 rows 26
    | 0 (c) Copyright 1983, 1984 Hewlett-Packard Company...
    |20  7

That is not what the screen shows. The 6845 scrolls by moving its start
address, and screen_update reads cell (start + row*80 + col) & 4095, so once
BASIC has scrolled the dumped row numbers no longer name displayed lines. This
rebuilds the vram array from the dump -- omitted rows are blank -- and then
reads it the way the display does.

    tools/screen.py < log          the final frame's screen
    tools/screen.py --all < log    every frame in the log
    tools/screen.py --raw < log    final frame without the rotation
"""

import re
import sys

COLUMNS = 80
CELLS = 4096

HEAD = re.compile(r'^--- frame (\d+) start ([0-9a-f]{4}) rows (\d+)')
ROW = re.compile(r'^\|\s*(\d+) (.*)\|$')


def frames(lines):
    """Yield (frame, start, rows, {row: text}) for each dump in the log."""
    cur = None
    for line in lines:
        line = line.rstrip('\n')
        m = HEAD.match(line)
        if m:
            if cur:
                yield cur
            cur = (int(m.group(1)), int(m.group(2), 16), int(m.group(3)), {})
            continue
        if cur is None:
            continue
        m = ROW.match(line)
        if m:
            cur[3][int(m.group(1))] = m.group(2)
    if cur:
        yield cur


def screen(frame, rotate=True):
    """The displayed rows of one dump, as a list of strings."""
    _, start, rows, dumped = frame
    if not rotate:
        return [dumped.get(r, '') for r in range(rows)]

    # Rebuild vram. The dump's last row covers cells 4080..4159, which runs
    # off the end of the 4096-cell array, so only its first 16 cells are
    # real; the rest is whatever sits past the buffer and is discarded.
    vram = [' '] * CELLS
    for r, text in dumped.items():
        for c, ch in enumerate(text):
            i = r * COLUMNS + c
            if i < CELLS:
                vram[i] = ch

    return [''.join(vram[(start + r * COLUMNS + c) & (CELLS - 1)]
                    for c in range(COLUMNS))
            for r in range(rows)]


def main():
    every = '--all' in sys.argv
    rotate = '--raw' not in sys.argv

    got = list(frames(sys.stdin))
    if not got:
        print('no FOCUS_DIMDUMP output found', file=sys.stderr)
        return 1

    for f in (got if every else got[-1:]):
        if every:
            print(f'--- frame {f[0]} start {f[1]:04x} rows {f[2]}')
        for line in screen(f, rotate):
            line = line.rstrip()
            if line:
                print(line)
    return 0


if __name__ == '__main__':
    sys.exit(main())
