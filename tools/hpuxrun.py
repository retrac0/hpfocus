#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Joel Heikkila
"""Run a shell command under HP-UX 5.30 on the emulated 540 and bring back
its output.

The files given travel on the disc (tests/run_tests.py hpux_carry: a shell
archive in place of a library nothing uses) and are unpacked into /tmp,
then the command runs with its output
redirected to /tmp/hpuxrun.out. `sync` puts the file on the disc, and when
the machine has stopped the file is read straight off the disc image -- so
the output can be as long as it likes, not one 24-line screen.

    tools/hpuxrun.py -f tests/hpux/isa.s -f tests/hpux/isa.c \\
            'cc -o isa isa.c isa.s && ./isa'

The disc is a fresh multiuser copy of the 5.30 volume (tests/run_tests.py
hpux_volume); the console is port 0 of a 27140A.
"""
import argparse
import os
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
sys.path.insert(0, str(ROOT / 'tools'))
import run_tests as R  # noqa: E402
import sdf7914 as sdf  # noqa: E402

PORT = ':cio2:mux:port0:terminal'
DONE = 'hpuxrun-done'


def read_file(image, path):
	data = Path(image).read_bytes()
	disc = sdf.Disc([(data, 0)])
	fa_ino, _root, _at = sdf.find_fa_root([(data, 0)])[0]
	fs = sdf.SDF(disc, fa_ino)
	ino = fs.inode(1)
	for part in path.strip('/').split('/'):
		ents = dict(fs.readdir(ino))
		if part not in ents:
			return None
		ino = fs.inode(ents[part])
	return fs.read_file(ino)


def main():
	ap = argparse.ArgumentParser()
	ap.add_argument('command')
	ap.add_argument('-f', '--file', action='append', default=[], type=Path)
	ap.add_argument('--ram', default='2m')
	ap.add_argument('--within', type=int, default=600,
			help='emulated seconds to allow the command')
	ap.add_argument('--mame', default=str(ROOT / 'mame' / 'focus'))
	ap.add_argument('--work', default=None)
	args = ap.parse_args()

	work = args.work or tempfile.mkdtemp(prefix='hpuxrun-')
	os.makedirs(work, exist_ok=True)
	disc = R.hpux_volume(work, 'hpuxrun', multiuser=True)
	R.hpux_carry(disc, [(f.name, f.read_bytes()) for f in args.file])
	steps = [*R.hpux_login(PORT)]
	# The marker is printed by echo, so the typed command line itself --
	# which shows 'hpuxrun-''done' -- never matches it.
	cmd = (f"(sh {R.HPUX_CARRIER}; {args.command}) > /tmp/hpuxrun.out 2>&1; sync; sync; "
			f"echo hpuxrun-''done\n")
	steps += [{'post': cmd, 'to': PORT}, {'posted': True},
			{'wait': DONE, 'on': PORT, 'within': args.within}, {'sleep': 3}]
	case = dict(name='hpuxrun', system='hp9k540',
			args=lambda: ['-ram', args.ram, '-hard', disc,
					'-cio2', 'mux', '-cio2:mux:port0', 'terminal'],
			steps=steps, limit=args.within + 300,
			setup=lambda: R.battery(work, 'hp9k540', 'scm_rtc', R.SW / 'cct' / 'scm.nv'))
	name, ok, why = R.run_case(case, args.mame, work)
	out = read_file(Path(work) / 'hpuxrun.hd', '/tmp/hpuxrun.out')
	if out is None:
		print(f'no output file ({why})', file=sys.stderr)
		return 1
	sys.stdout.write(out.decode('latin-1'))
	if not ok:
		print(f'\n[hpuxrun: {why}]', file=sys.stderr)
	if not args.work:
		import shutil
		shutil.rmtree(work, ignore_errors=True)
	return 0 if ok else 1


if __name__ == '__main__':
	sys.exit(main())
