#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Joel Heikkila
"""Differential test: MAME's FOCUS disassembler against tools/focusdis.py.

Two independent implementations of the same opcode map, compared
instruction by instruction over real FOCUS code: the HP-UX 5.01 kernel,
the BASIC boot and utility discs, and a control text that is not code at
all. Any divergence is a bug in one of them.

    tests/dasm_difftest.py [--unidasm mame/unidasm]

unidasm comes from a TOOLS=1 build.
"""

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOLS = ROOT / 'tools'


def sh(*args):
	subprocess.run([sys.executable, *map(str, args)], check=True,
			stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def focusdis(path, start, end):
	out = subprocess.run([sys.executable, str(TOOLS / 'focusdis.py'), str(path),
			'--start', hex(start), '--end', hex(end), '--count', '0'],
			capture_output=True, text=True).stdout
	lines = []
	for l in out.splitlines():
		l = re.sub(r'^([0-9a-f]{8})  [0-9a-f]+ +', r'\1  ', l)
		l = re.sub(r'  <<< misaligned$', '', l)
		lines.append(l)
	return lines


def unidasm(tool, path, start, end):
	out = subprocess.run([tool, str(path), '-arch', 'focus', '-norawbytes',
			'-basepc', '0', '-skip', str(start), '-count', str(end - start)],
			capture_output=True, text=True).stdout
	lines = []
	for l in out.splitlines():
		m = re.match(r'^([0-9A-Fa-f]+): (.*)$', l)
		if m:
			addr = int(m.group(1), 16)
			if start <= addr < end:
				lines.append(f'{addr + 0:08x}  {m.group(2).rstrip()}')
	return lines


def main():
	ap = argparse.ArgumentParser()
	ap.add_argument('--unidasm', default=str(ROOT / 'mame' / 'unidasm'))
	args = ap.parse_args()

	with tempfile.TemporaryDirectory() as w:
		w = Path(w)
		tape = ROOT / 'software' / '97089C.hpi'
		sh(TOOLS / 'lifdir.py', tape, '--extract', 'S97080C', '--out', w / 'S97080C')
		sh(TOOLS / 'cpiolist.py', w / 'S97080C', '--extract', 'system/97080C/os.req', '--out', w / 'os.req')
		sh(TOOLS / 'lifdir.py', tape, '--extract', 'SHPUX_MAN', '--out', w / 'SHPUX_MAN')
		sh(TOOLS / 'td0.py', ROOT / 'software' / 'basic' / '500BT1.TD0', '--raw', w / 'BT1.img')
		sh(TOOLS / 'td0.py', ROOT / 'software' / 'basic' / '500UT3.TD0', '--raw', w / 'UT3.img')

		status = 0
		for name, f, start, end in [
				('HP-UX kernel', w / 'os.req', 0, 0x58000),
				('BASIC boot', w / 'BT1.img', 0, 0x46000),
				('BASIC util', w / 'UT3.img', 0, 0x40000),
				('control text', w / 'SHPUX_MAN', 0, 0x40000)]:
			py = focusdis(f, start, end)
			cc = unidasm(args.unidasm, f, start, end)
			if py == cc:
				print(f'  {name:<16} {len(py):>9} instructions  identical')
			else:
				status = 1
				print(f'  {name:<16} {len(py):>9} instructions  MISMATCH')
				shown = 0
				for a, b in zip(py, cc):
					if a != b and shown < 10:
						print(f'    py: {a}\n    cc: {b}')
						shown += 1
				if len(py) != len(cc):
					print(f'    {len(py)} lines against {len(cc)}')
		print('PASS' if status == 0 else 'FAIL')
		return status


if __name__ == '__main__':
	sys.exit(main())
