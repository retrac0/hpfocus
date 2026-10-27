#!/usr/bin/env python3
"""Read the 9020 internal thermal printer's character sets off the CE
Handbook's picture of its self test.

The printer's font ROM is not dumped. Its self test, though, prints every
code 0..255 of both character sets in display functions mode, and the 9020
CE Handbook (09020-90035, page 5-8) reproduces that printout. This reads it
back dot by dot:

    tools/printerfont.py                    the table, as C++, on stdout
    tools/printerfont.py --png font.png     and the font re-printed, to compare

The page is scanned at 400 dpi, 1 bit deep, with the printout reduced to
about 3.1 pixels a dot. Each line of the printout is 80 fields of 7 x 12
dots; CR (0x0d) prints and then ends the first line of each set, so the
lines start at 0x00, 0x0e, 0x5e, 0xae and 0xfe. A dot is on when most of
its pitch-sized block is ink.
"""
import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
HANDBOOK = ROOT / 'docs' / 'bitsavers' / '09020-90035_HP_9020_CE_Handbook_Jul85.pdf'
PAGE = 43

COLS, ROWS = 7, 12
LINE_STARTS = [0x00, 0x0e, 0x5e, 0xae, 0xfe]


def page_image(pdf):
	with tempfile.TemporaryDirectory() as d:
		subprocess.run(['pdfimages', '-f', str(PAGE), '-l', str(PAGE), '-png', str(pdf), f'{d}/p'], check=True)
		return np.array(Image.open(f'{d}/p-000.png').convert('L')) < 128


def ink_runs(profile):
	runs, start = [], None
	for i, v in enumerate(profile):
		if v and start is None:
			start = i
		elif not v and start is not None:
			runs.append((start, i))
			start = None
	return runs


def fit_grid(profile, lo, hi):
	"""Pitch and phase of a dot grid: centres on ink, half-steps off it."""
	best = None
	n = len(profile)
	xs = np.arange(n, dtype=float)
	for p in np.arange(lo, hi, 0.0025):
		for o in np.arange(0, p, 0.05):
			c = np.arange(o, n - 1, p)
			e = c + p / 2
			e = e[e < n - 1]
			s = np.interp(c, xs, profile).sum() - np.interp(e, xs, profile).sum()
			if best is None or s > best[0]:
				best = (s, p, o)
	return best[1], best[2]


def field(img, x0, y0, px, py):
	"""One field's dots, at the sub-dot offset that leaves least doubt:
	blocks tile the field, so each dot is just its block's majority. Only
	the field's own pixels count -- its neighbours' strokes run right up
	to its edges."""
	best = None
	for dy in np.arange(-1.0, 1.01, 0.25):
		for dx in np.arange(-1.0, 1.01, 0.25):
			xs = np.round(x0 + dx + np.arange(COLS + 1) * px).astype(int)
			ys = np.round(y0 + dy + np.arange(ROWS + 1) * py).astype(int)
			mean = np.array([[img[ys[r]:ys[r + 1], xs[c]:xs[c + 1]].mean() for c in range(COLS)] for r in range(ROWS)])
			err = np.minimum(mean, 1 - mean).sum()
			if best is None or err < best[0]:
				best = (err, mean > 0.5)
	return best[1], best[0]


def underline_row(img, x, y, px, py):
	"""How much more '_' looks like row 11 than anything above it."""
	g, _ = field(img, x, y, px, py)
	return g[11].sum() - g[:11].sum()


def extract(img):
	# The printout's text lines, from the firmware banner down: banner,
	# five lines of Roman Extension, five of Katakana. Two lines whose
	# descenders touch come out as one run; split it.
	runs = [(s + 900, e + 900) for s, e in ink_runs(img[900:1420].any(1)) if e - s > 6]
	lines = []
	for s, e in runs[1:]:
		if e - s > 60:
			lines += [(s, s + (e - s) // 2), (s + (e - s) // 2, e)]
		else:
			lines.append((s, e))
	assert len(lines) == 10, lines

	# Horizontal: one grid over the full 80-field lines, with the cell
	# boundary where the ink is thinnest -- the fields are centred.
	full = [(s, e) for s, e in lines if np.ptp(np.where(img[s:e].any(0))[0]) > 1500]
	px, ox = fit_grid(np.concatenate([img[s:e] for s, e in full]).mean(0), 3.0, 3.3)
	cols = np.array([img[s:e].mean(0) for s, e in full]).mean(0)
	ink = [np.interp(ox + (np.arange(k, 560, COLS) + 0.5) * px, np.arange(len(cols)), cols).sum() for k in range(COLS)]
	c0 = (int(np.argmin(np.array(ink) + np.roll(ink, -1))) + 1) % COLS
	x0 = ox + c0 * px
	left = min(np.where(img[s:e].any(0))[0].min() for s, e in full)
	x0 += np.floor((left + px - x0) / (COLS * px)) * COLS * px

	font = np.zeros((512, ROWS, COLS), dtype=bool)
	worst = np.zeros(512)
	for block in range(2):
		blk = lines[block * 5:block * 5 + 5]
		# Vertical: the printer feeds a block's lines exactly twelve rows
		# apart, so one grid per block; which of its rows starts a field
		# is the one that puts '_' (third line, column 1) on row 11.
		lo, hi = blk[0][0] - 12, blk[-1][1] + 12
		py, oy = fit_grid(img[lo:hi].mean(1), 2.95, 3.15)
		origin = lo + oy
		top = max((origin + k * py for k in range(-6, 30) if origin + k * py <= blk[0][0] + py),
				key=lambda t: underline_row(img, x0 + COLS * px, t + 2 * ROWS * py, px, py))
		print(f'block {block}: dot pitch {px:.4f} x {py:.4f} px, first field at ({x0:.1f}, {top:.1f})', file=sys.stderr)
		for k, start in enumerate(LINE_STARTS):
			end = LINE_STARTS[k + 1] if k + 1 < len(LINE_STARTS) else 0x100
			for i in range(end - start):
				code = block * 256 + start + i
				font[code], worst[code] = field(img, x0 + i * COLS * px, top + k * ROWS * py, px, py)
	return font, worst


def render(font, scale=4):
	lines = []
	for block in range(2):
		for k, start in enumerate(LINE_STARTS):
			end = LINE_STARTS[k + 1] if k + 1 < len(LINE_STARTS) else 0x100
			lines.append([block * 256 + c for c in range(start, end)])
	out = np.full((len(lines) * ROWS, 80 * COLS), 255, dtype=np.uint8)
	for n, codes in enumerate(lines):
		for i, c in enumerate(codes):
			out[n * ROWS:(n + 1) * ROWS, i * COLS:(i + 1) * COLS][font[c]] = 0
	return Image.fromarray(out).resize((80 * COLS * scale, len(lines) * ROWS * scale), Image.NEAREST)


def cpp(font):
	out = []
	for c in range(512):
		if c in (0, 256):
			out.append('\t// ' + ('Roman Extension' if c == 0 else 'Katakana'))
		rows = [sum(int(b) << (COLS - 1 - k) for k, b in enumerate(font[c, r])) for r in range(ROWS)]
		out.append('\t{ ' + ', '.join(f'0x{v:02x}' for v in rows) + ' },   // ' + f'{c & 0xff:02x}')
	return '\n'.join(out) + '\n'


def main():
	ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
	ap.add_argument('--pdf', default=str(HANDBOOK))
	ap.add_argument('--png', help='also write the font, re-printed')
	a = ap.parse_args()
	font, worst = extract(page_image(a.pdf))
	order = np.argsort(worst)[::-1][:8]
	print('least certain: ' + ', '.join(f'{c >> 8}:{c & 0xff:02x}' for c in order), file=sys.stderr)
	if a.png:
		render(font).save(a.png)
	sys.stdout.write(cpp(font))


if __name__ == '__main__':
	main()
