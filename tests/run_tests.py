#!/usr/bin/env python3
"""Regression tests for the hp9k520 / hp9k540 MAME drivers.

Each case boots MAME headless and unthrottled with an -autoboot_script that
drives it through tests/harness.lua: wait for text on the 520's screen or a
serial terminal, type through the natural keyboard, check what comes back.

    tests/run_tests.py                 every case
    tests/run_tests.py --list          the case table
    tests/run_tests.py basic-ready add the cases named (prefix match)
    tests/run_tests.py -j 4            at most four MAMEs at once

The MAME binary is mame/focus (SUBTARGET=focus) unless --mame says
otherwise. Media come from software/; fixed discs are copied into the work
directory first, because the machine writes to them.
"""

import argparse
import concurrent.futures
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SW = ROOT / 'software'
HARNESS = ROOT / 'tests' / 'harness.lua'


def lua(value):
	"""Python data as a Lua constructor."""
	if isinstance(value, bool):
		return 'true' if value else 'false'
	if isinstance(value, (int, float)):
		return repr(value)
	if isinstance(value, str):
		return '"' + value.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n') + '"'
	if isinstance(value, dict):
		return '{ ' + ', '.join(
				(f'[{k}] = {lua(v)}' if isinstance(k, int) else f'{k} = {lua(v)}')
				for k, v in value.items()) + ' }'
	if isinstance(value, (list, tuple)):
		return '{ ' + ', '.join(lua(v) for v in value) + ' }'
	raise TypeError(value)


#--------------------------------------------------------------------------
#  media
#--------------------------------------------------------------------------

def hd(work, src, name):
	"""A writable copy of a fixed-disc image, with the extension MAME's
	hard disk device takes raw images by."""
	dst = Path(work) / (name + '.hd')
	if not dst.exists():
		shutil.copyfile(src, dst)
	return str(dst)


def hpux_volume(work, name, multiuser=False):
	vol = SW / 'cct' / '7914_hpux_volume.bin'
	if not vol.exists():
		subprocess.run([str(ROOT / 'tools' / 'runhpux.sh'), '--prepare'], check=True)
	path = hd(work, vol, name)
	if multiuser:
		subprocess.run([sys.executable, str(ROOT / 'tools' / 'hpuxconfig.py'), path],
				check=True, stdout=subprocess.DEVNULL)
	return path


def battery(work, system, tag, src):
	"""Preload a battery-backed part's contents."""
	d = Path(work) / 'nvram' / system
	d.mkdir(parents=True, exist_ok=True)
	shutil.copyfile(src, d / tag)


#--------------------------------------------------------------------------
#  the cases
#--------------------------------------------------------------------------

BASIC_FLOPS = ['-flop1', str(SW / 'basic' / '500BT1.TD0'), '-flop2', str(SW / 'basic' / '500BT2.TD0')]
READY = 'BASIC Ready for use.'

# (name, keystrokes, the line the screen should show). Keystrokes are
# posted once BASIC is ready; '\n' is EXECUTE. 22 of these pass on the
# pre-MAME emulator; concat and real-add are the work list.
BASIC_TABLE = [
	('disp-int', 'DISP 7\n', '7'),
	('disp-string', 'DISP "HI"\n', 'HI'),
	('disp-paren', 'DISP (7)\n', '7'),
	('disp-neg-lit', 'DISP -7\n', '-7'),
	('add', 'DISP 1+1\n', '2'),
	('add-zero', 'DISP 1+0\n', '1'),
	('add-big', 'DISP 100+200\n', '300'),
	('sub', 'DISP 9-8\n', '1'),
	('mul', 'DISP 3*4\n', '12'),
	('div', 'DISP 8/2\n', '4'),
	('sqr', 'DISP SQR(4)\n', '2'),
	('let', 'LET A=6\nDISP A\n', '6'),
	('new', 'NEW\nDISP 7\n', '7'),
	('two-stmt', 'DISP 1\nDISP 2\nDISP 3\n', '3'),
	('two-item', 'DISP 4,5\n', '4                   5'),
	('compare', 'DISP 1=1\n', '1'),
	('concat', 'DISP "A"&"B"\n', 'AB'),
	('list', 'LIST\n', 'AVAILABLE MEMORY = 487696'),
	('list-twice', ['LIST\n', 'LIST\n'], 'AVAILABLE MEMORY = 487696'),
	('frac-div', 'DISP 9/2\n', '4.5'),
	('recip', 'DISP 1/3\n', '.333333333333'),
	('big-div', 'DISP 100/8\n', '12.5'),
	('sqrt2', 'DISP SQR(2)\n', '1.41421356237'),
	('real-add', 'DISP 4.5+1\n', '5.5'),
]
KNOWN_FAILING = {'basic-concat', 'basic-real-add'}


def factorial_case():
	"""ERS section 7's recursive factorial (sheet 129), the first program
	the core ever ran: procedure calls, stack markers, twelve-deep
	recursion and a doubleword multiply."""
	def f1(op1, i, x, base, off): return (op1 << 13) | (i << 12) | (x << 11) | (base << 8) | off
	def f3(op3, b3=False): return (0x1000 if b3 else 0) | op3
	def f5(op2, operand): return 0x2000 | (op2 << 8) | operand
	def bccs(ccf, back, hw): return (6 << 13) | (ccf << 9) | (0x100 if back else 0) | hw

	CALLER, PROC, CODE_BASE, CODE_END = 0o10000, 0o20000, 0o4000, 0o30000 - 1
	SB, DB = 0o100000, 0o200000
	PL = CODE_END & ~3
	half = {}

	def emit(at, insns):
		for k, w in enumerate(insns):
			half[at + 2 * k] = w

	# ...then a branch to itself, where the CPU waits for the next case.
	emit(CALLER, [f3(0o6), f1(2, 0, 0, 2, 8), f5(0o22, 2), f1(5, 0, 0, 2, 10), bccs(7, False, 0)])
	emit(PROC, [f1(2, 0, 0, 5, 4), f5(0o3, 0), bccs(5, False, 3), f5(0o14, 1),
			bccs(7, False, 0o10), f3(0o6), f1(2, 0, 0, 5, 4), f5(0o5, 1), f5(0o22, 2),
			f1(2, 0, 0, 5, 4), f3(0o22, True), f3(0o1), f1(5, 0, 0, 5, 5), f5(0o23, 1)])
	words = {}
	for at, w in half.items():
		cell = at & ~3
		words[cell] = words.get(cell, 0) | (w << (0 if at & 2 else 16))
	words[PL] = 8                           # the STT's length: two entries
	words[PL - 8] = PROC - CODE_BASE        # STT entry 2: the procedure

	steps = [{'sleep': 0.1}]
	for k, (n, want) in enumerate([(0, 1), (1, 1), (2, 2), (3, 6), (5, 120), (8, 40320), (12, 479001600)]):
		load = dict(words)
		load[DB + 32] = n
		load[DB + 40] = 0
		# The CPU stops at power-on with nothing to run, so the first case
		# starts it with a reset; later ones find it in its loop.
		steps.append({('reset' if k == 0 else 'setup'): {
			'load': load,
			'regs': {'PB': CODE_BASE, 'PL': PL, 'P': CALLER, 'SB': SB, 'S': SB + 3,
					'Q': SB + 3, 'SL': 0o140000 - 1, 'DB': DB, 'DL': 0o210000 - 1,
					'STATUS': 0}}})
		steps.append({'sleep': 0.1})
		steps.append({'expect': {DB + 40: want}, 'what': f'{n}!'})
	return steps


def cases(work):
	c = []

	def add(name, system, args, steps, limit, setup=None):
		c.append(dict(name=name, system=system, args=args, steps=steps, limit=limit, setup=setup))

	# --- the CPU on its own
	add('isa-factorial', 'hp9k520', [], factorial_case(), 10)

	# --- the 520
	add('basic-floppy', 'hp9k520', BASIC_FLOPS,
			[{'wait': READY, 'on': 'screen'}], 60)
	def swlist():
		d = Path(work) / 'roms' / 'hp9k5xx_flop' / 'basic20'
		d.mkdir(parents=True, exist_ok=True)
		for v in ('500BT1', '500BT2'):
			shutil.copyfile(SW / 'basic' / (v + '.TD0'), d / (v.lower() + '.td0'))
		return ['basic20', '-rompath', str(Path(work) / 'roms')]
	add('basic-softlist', 'hp9k520', swlist,
			[{'wait': READY, 'on': 'screen'}], 60)
	add('basic-hd', 'hp9k520', lambda: ['-hard', hd(work, SW / 'hd' / 'basic.img', 'basic')],
			[{'wait': READY, 'on': 'screen'}], 60)
	add('diag-hd', 'hp9k520', lambda: ['-ram', '2m', '-hard', hd(work, SW / 'hd' / 'diag.img', 'diag'),
			'-flop1', str(SW / 'test' / '500MFTST.TD0')],
			[{'wait': READY, 'on': 'screen'}], 60)
	for name, keys, want in BASIC_TABLE:
		typing = []
		for k, chunk in enumerate([keys] if isinstance(keys, str) else keys):
			if k:
				typing.append({'sleep': 6})
			typing += [{'post': chunk}, {'posted': True}]
		add('basic-' + name, 'hp9k520', lambda n=name: ['-hard', hd(work, SW / 'hd' / 'basic.img', 'basic-' + n)],
				[{'wait': READY, 'on': 'screen'}, {'sleep': 3}, *typing,
				 {'sleep': 4}, {'line': want, 'on': 'screen'}], 80)
	hpux501 = Path(work) / 'hpux501.img'

	def setup501():
		if not hpux501.exists():
			subprocess.run([sys.executable, str(ROOT / 'tools' / 'mkharddisc.py'), str(hpux501)],
					check=True, stdout=subprocess.DEVNULL)
			subprocess.run([sys.executable, str(ROOT / 'tools' / 'installbasic.py'), str(hpux501),
					'--boot-image', str(SW / 'cct' / 'hpux501.bin')], check=True, stdout=subprocess.DEVNULL)
	add('hpux501-banner', 'hp9k520',
			lambda: ['-ram', '2m', '-hard1', hd(work, hpux501, 'hpux501-boot'),
					'-cio5', 'hpib', '-hard2', hpux_volume(work, 'hpux501-root')],
			[{'wait': 'The Regents of the University of California', 'on': 'screen'}], 150,
			setup=setup501)

	# --- the 540
	console = ':cio0:asi:rs232:terminal'
	add('hpux-single', 'hp9k540',
			lambda: ['-ram', '2m', '-hard', hpux_volume(work, 'hpux-single')],
			[{'wait': '/etc/rc', 'on': console}], 400,
			setup=lambda: battery(work, 'hp9k540', 'scm_rtc', SW / 'cct' / 'scm.nv'))
	def hdd_list():
		d = Path(work) / 'roms' / 'hp9k5xx_hdd' / 'hpux530'
		chd = d / 'hpux530.chd'
		if not chd.exists():
			d.mkdir(parents=True, exist_ok=True)
			vol = Path(work) / 'hpux530-rebuilt.bin'
			subprocess.run([sys.executable, str(ROOT / 'tools' / 'mkhpuxdisc.py'),
					str(SW / 'cct' / '7914_unitSave_hpux4.0.bin'),
					str(SW / 'cct' / '7914_unitSave_hpux4.0_tape2.bin'), str(vol)],
					check=True, stdout=subprocess.DEVNULL)
			subprocess.run([str(ROOT / 'mame' / 'chdman'), 'createhd', '-i', str(vol), '-o', str(chd),
					'-chs', '1168,7,64', '-ss', '256'], check=True,
					stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
		return ['hpux530', '-rompath', str(Path(work) / 'roms')]
	# As imaged, the system asks for its run level; single user logs in as
	# root on the console.
	add('hpux-softlist', 'hp9k540', hdd_list,
			[{'wait': 'ENTER RUN LEVEL', 'on': console}, {'sleep': 2}, {'post': 's\n', 'to': console},
			 {'wait': 'TERM =', 'on': console, 'within': 200}, {'sleep': 2}, {'post': '\n', 'to': console},
			 {'sleep': 5}, {'post': 'expr 6 \\* 7\n', 'to': console}, {'posted': True},
			 {'line': '42', 'on': console, 'within': 30}], 600)
	mux0 = ':cio2:mux:port0:terminal'
	add('hpux-multi', 'hp9k540',
			lambda: ['-ram', '2m', '-hard', hpux_volume(work, 'hpux-multi', multiuser=True),
					'-cio2', 'mux', '-cio2:mux:port0', 'terminal'],
			[{'wait': 'login:', 'on': mux0}, {'sleep': 5}, {'post': 'root\n', 'to': mux0},
			 {'wait': 'Welcome to Hewlett-Packard System 9000', 'on': mux0, 'within': 120},
			 {'wait': 'TERM =', 'on': mux0, 'within': 60}], 900,
			setup=lambda: battery(work, 'hp9k540', 'scm_rtc', SW / 'cct' / 'scm.nv'))
	return c


#--------------------------------------------------------------------------
#  running
#--------------------------------------------------------------------------

def run_case(case, mame, work):
	cwork = Path(work) / case['name']
	cwork.mkdir(parents=True, exist_ok=True)
	if case['setup']:
		case['setup']()
	args = case['args']() if callable(case['args']) else case['args']
	script = cwork / 'test.lua'
	# MAME runs the autoboot script again after every soft reset; the
	# test is already running by then.
	script.write_text('if HARNESS_STARTED then return end\nHARNESS_STARTED = true\n'
			f'local H = dofile({lua(str(HARNESS))})\n'
			f'H.run({lua(case["name"])}, {lua(case["steps"])}, {case["limit"]})\n')
	nv = Path(work) / 'nvram'
	case_nv = cwork / 'nvram'
	if nv.exists() and not case_nv.exists():
		shutil.copytree(nv, case_nv)
	if '-rompath' not in args:
		args = [*args, '-rompath', str(Path(mame).parent / 'roms')]
	cmd = [mame, case['system'], *args,
			'-hashpath', str(Path(mame).parent / 'hash'),
			'-nvram_directory', str(case_nv),
			'-cfg_directory', str(cwork / 'cfg'),
			'-diff_directory', str(cwork / 'diff'),
			'-autoboot_script', str(script),
			'-nothrottle', '-video', 'none', '-sound', 'none', '-skip_gameinfo',
			'-seconds_to_run', str(case['limit'] + 30)]
	env = dict(os.environ, SDL_VIDEODRIVER='dummy')
	p = subprocess.run(cmd, cwd=Path(mame).parent, capture_output=True, text=True, env=env,
			timeout=case['limit'] * 20 + 120)
	(cwork / 'output.txt').write_text(p.stdout + p.stderr)
	out = p.stdout + p.stderr
	for line in out.splitlines():
		if line.startswith('HARNESS PASS'):
			return case['name'], True, ''
		if line.startswith('HARNESS FAIL'):
			detail = '\n'.join(l for l in out.splitlines() if l.startswith('  | '))
			return case['name'], False, line.split(':', 1)[1].strip() + ('\n' + detail if detail else '')
	tail = '\n'.join(out.strip().splitlines()[-6:])
	return case['name'], False, f'no result (exit {p.returncode})\n{tail}'


def main():
	ap = argparse.ArgumentParser()
	ap.add_argument('names', nargs='*')
	ap.add_argument('--list', action='store_true')
	ap.add_argument('--mame', default=str(ROOT / 'mame' / 'focus'))
	ap.add_argument('--work', default=None)
	ap.add_argument('-j', type=int, default=max(1, (os.cpu_count() or 2) - 1))
	args = ap.parse_args()

	work = args.work or tempfile.mkdtemp(prefix='hp9k5xx-tests-')
	table = cases(work)
	if args.names:
		table = [c for c in table if any(c['name'].startswith(n) for n in args.names)]
	if args.list:
		for c in table:
			print(f"{c['name']:<20} {c['system']}")
		return 0

	print(f'{len(table)} cases, work directory {work}')
	failed = unexpected = 0
	with concurrent.futures.ThreadPoolExecutor(max_workers=args.j) as pool:
		for name, ok, why in pool.map(lambda c: run_case(c, args.mame, work), table):
			known = name in KNOWN_FAILING
			if ok:
				print(f'  PASS  {name}' + ('  (was known failing)' if known else ''))
			else:
				failed += 1
				unexpected += not known
				print(f'  FAIL  {name}' + ('  (known)' if known else '') + f'  {why}')
	print(f'{len(table) - failed} passed, {failed} failed, {unexpected} unexpected')
	return 1 if unexpected else 0


if __name__ == '__main__':
	sys.exit(main())
