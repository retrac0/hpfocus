#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Joel Heikkila
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
import gzip
import os
import shutil
import subprocess
import sys
import tempfile
import threading
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
		s = value.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n')
		return '"' + ''.join(f'\\{ord(c):03d}' if ord(c) < 32 or ord(c) == 127 else c for c in s) + '"'
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


# The pristine HP-UX 5.30 machine tools/runhpux.sh --prepare assembles from
# the unit-save tapes (tools/lib/hpux.sh): the disc, and the SCM battery one
# throwaway boot has initialised. Every case boots its own copy of both.
PRISTINE = Path(os.environ.get('HPFOCUS_VAR', ROOT / 'var')) / 'pristine'
PRISTINE_DISC = PRISTINE / 'hpux530.bin'
SCM_NV = PRISTINE / 'nvram' / 'hp9k540' / 'scm_rtc'


TAPES = [SW / 'cct' / t for t in ('7914_unitSave_hpux4.0.bin.gz', '7914_unitSave_hpux4.0_tape2.bin.gz')]
_prepare_lock = threading.Lock()


def prepare_hpux():
	"""Make the pristine machine once, whichever case gets here first:
	cases run in parallel, and two preparations at once would each delete
	the other's work. Without the tapes it is media that is not here; a
	preparation that fails with them is a failure."""
	with _prepare_lock:
		if PRISTINE_DISC.exists() and SCM_NV.exists():
			return
		missing = [t for t in TAPES if not t.exists()]
		if missing and not PRISTINE_DISC.exists():
			raise FileNotFoundError(', '.join(t.name for t in missing))
		subprocess.run([str(ROOT / 'tools' / 'runhpux.sh'), '--prepare'], check=True,
				stdout=subprocess.DEVNULL)


def hpux_volume(work, name, multiuser=False, card='27140', sc=2):
	"""A copy of the pristine disc, set to boot to run level 2 with a login
	on every port of the given MUX, or to the single-user shell."""
	prepare_hpux()
	path = hd(work, PRISTINE_DISC, name)
	level = ['--runlevel', '2', '--card', card, '--sc', str(sc)] if multiuser else ['--runlevel', 's']
	subprocess.run([sys.executable, str(ROOT / 'tools' / 'hpuxconfig.py'), path, *level],
			check=True, stdout=subprocess.DEVNULL)
	return path


def raw_volume(work, name):
	"""A shipped floppy as a raw image, decoded from its Teledisk file
	into the work directory (software/basic/img holds the same, if
	tools/setup.sh has made them)."""
	made = SW / 'basic' / 'img' / (name + '.img')
	if made.exists():
		return made
	raw = Path(work) / (name + '.img')
	if not raw.exists():
		td0 = SW / 'basic' / (name + '.TD0')
		if not td0.exists():
			raise FileNotFoundError(td0.name)
		subprocess.run([sys.executable, str(ROOT / 'tools' / 'td0.py'), '--raw', str(raw), str(td0)],
				check=True, stdout=subprocess.DEVNULL)
	return raw


def hpux501_kernel():
	"""The 5.01 boot image, built from the release tape as
	tools/runhpux.sh --release 5.01 builds it."""
	kernel = SW / 'cct' / 'hpux501.bin'
	if not kernel.exists():
		tape = SW / '97089C.hpi'
		if not tape.exists():
			raise FileNotFoundError(tape.name)
		subprocess.run([sys.executable, str(ROOT / 'tools' / 'hpuxboot.py'), str(tape), '-o', str(kernel),
				'--option', 'HPIB_FLEX', '--option', 'SERIAL', '--option', 'MEMORY_VOL'],
				check=True, stdout=subprocess.DEVNULL)
	return kernel


def battery(work, system, tag, src):
	"""Preload a battery-backed part's contents."""
	prepare_hpux()
	d = Path(work) / 'nvram' / system
	d.mkdir(parents=True, exist_ok=True)
	shutil.copyfile(src, d / tag)


HPUX_TESTS = ROOT / 'tests' / 'hpux'


def hpux_login(port):
	"""Log in as root on a multiuser HP-UX 5.30 terminal and wait for the
	shell. /etc/profile asks for the terminal type twice."""
	return [{'wait': 'login:', 'on': port}, {'sleep': 5}, {'post': 'root\n', 'to': port},
			{'wait': 'TERM =', 'on': port, 'within': 120}, {'post': '\n', 'to': port},
			{'wait': 'Remember to verify', 'on': port, 'within': 60}, {'sleep': 2},
			{'post': '\n', 'to': port},
			{'wait': 'SUPERUSER', 'on': port, 'within': 60}, {'sleep': 3},
			{'post': 'cd /tmp\n', 'to': port}, {'posted': True}]


# A library no test links against, big enough to carry the test sources.
HPUX_CARRIER = '/lib/libUPLIB.a'


def hpux_carry(image, files):
	"""Put files on a copy of the 5.30 volume, as a shell archive written
	over HPUX_CARRIER: `sh /lib/libUPLIB.a` in the running system unpacks
	them into the current directory. The keyboard posts twenty characters
	a second, which is hours for a few hundred kilobytes; the disc is
	instant. The volume is a per-test copy, so the library is only lost
	from that copy. `files` is a list of (name, bytes)."""
	sys.path.insert(0, str(ROOT / 'tools'))
	import hpuxconfig
	text = b''
	for name, data in files:
		if not data.endswith(b'\n'):
			data += b'\n'
		text += b"cat > %s <<'@EOF@'\n" % name.encode() + data + b'@EOF@\n'
	vol = hpuxconfig.Volume(Path(image))
	vol.write_file(vol.lookup(HPUX_CARRIER), text)
	vol.commit()


def hpux_type_files(port, src, names):
	"""Type files into the current directory with here-documents. The
	tty's canonical input holds 256 characters, so no line may be
	longer; the quoted delimiter keeps the shell's hands off $ and `."""
	steps = []
	for name in names:
		text = (src / name).read_text()
		if any(len(l) > 200 for l in text.splitlines()):
			raise ValueError(f'{name}: a line too long to type')
		steps += [{'post': f"cat > {name} <<'@EOF@'\n{text}@EOF@\n", 'to': port},
				{'posted': True}]
	return steps


#--------------------------------------------------------------------------
#  the cases
#--------------------------------------------------------------------------

BASIC_FLOPS = ['-flop1', str(SW / 'basic' / '500BT1.TD0'), '-flop2', str(SW / 'basic' / '500BT2.TD0')]
READY = 'BASIC Ready for use.'

# (name, keystrokes, the line the screen should show). Keystrokes are
# posted once BASIC is ready; '\n' is EXECUTE, and STORE enters a program
# line.
STORE = {'press': 'STORE'}
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
	('less-than', 'DISP 2<1\n', '0'),
	('greater-than', 'DISP 1>2\n', '0'),
	('disp-angle', 'DISP "a<b>c,d.e"\n', 'a<b>c,d.e'),
	('run-compare', ['10 IF 1<2 THEN DISP 5', STORE, '20 IF 2>1 THEN DISP 6', STORE, 'RUN\n'], '6'),
	('disp-bar', 'DISP "a|b"\n', 'a|b'),
	('cursor-left', ['DISP 19', {'press': 'Cursor left'}, '2\n'], '12'),
	('cursor-right', ['DISP 19', {'press': 'Cursor left'}, {'press': 'Cursor right'}, '2\n'], '192'),
	('concat', 'DISP "A"&"B"\n', 'AB'),
	('concat-three', 'DISP "AB"&"CD"&"EF"\n', 'ABCDEF'),
	('concat-long', 'DISP "ABCDEFGHIJ"&"KLMNOPQRST"\n', 'ABCDEFGHIJKLMNOPQRST'),
	('concat-var', 'A$="XY"&"Z"\nDISP A$&A$\n', 'XYZXYZ'),
	('concat-chr', 'DISP "gam"&CHR$(101)&"s"\n', 'games'),
	('list', 'LIST\n', 'AVAILABLE MEMORY = 487888'),
	('list-twice', ['LIST\n', 'LIST\n'], 'AVAILABLE MEMORY = 487888'),
	('frac-div', 'DISP 9/2\n', '4.5'),
	('recip', 'DISP 1/3\n', '.333333333333'),
	('big-div', 'DISP 100/8\n', '12.5'),
	('sqrt2', 'DISP SQR(2)\n', '1.41421356237'),
	('real-add', 'DISP 4.5+1\n', '5.5'),
	('run-typed', ['10 DISP 2+3', STORE, 'RUN\n'], '5'),
	('run-sub', ['10 CALL Xyz', STORE, '20 END', STORE, '30 SUB Xyz', STORE,
			'40 DISP 6*7', STORE, '50 SUBEND', STORE, 'RUN\n'], '42'),
]
KNOWN_FAILING = set()


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


# The 09020 test discs: (disc, label, the programs on it). TEST, the
# Test Supervisor, is on all three.
TEST_DISCS = [
	('500MFTST', '09020-11011R0300', ['(6)STATUS', '(12)DISPLAY', '(8)AGRAPHICS', '(13)BGRAPHICS',
			'(18)CGRAPHICS', '(17)PRINTER', '(10)FLOPPY', '(22)LTPEN', '(7)KEYBOARD', '(15)WINCHESTER']),
	('500MSTST', '09020-10014R0300', ['(6)CS80', '(11)SS80', '(7)TAPE', '(18)9895', '(8)8290X',
			'(13)9885', '(23)9121']),
	('500EDTST', '09020-11013R0300', ['(8)SRM', '(6)EXPRINTER', '(11)PLOTTER', '(16)DIGITIZER',
			'(7)97060', '(12)97062', '(17)RGB_ALIGN', '(9)HPIB', '(14)HPIO', '(19)GPIO', '(24)ASI']),
]
WELCOME = 'Welcome to the HP 9000 Test Supervisor'


def diag_cases(add, work):
	"""The test discs, under the BASIC on diag.img. Typing is a character
	at a time: the supervisor reads its keyboard slower than the natural
	keyboard posts."""
	test = SW / 'test'
	add('diag-env-floppy', 'hp9k520',
			['-ram', '2m', *(a for k in (1, 2, 3) for a in (f'-flop{k}', str(test / f'500ENV{k}.TD0')))],
			[{'wait': READY, 'on': 'screen'}], 60)

	def booted(tag):
		return lambda: ['-ram', '2m', '-hard', hd(work, SW / 'hd' / 'diag.img', tag)]

	# HP's procedure: the environment boots from the Winchester and the
	# test disc goes into the flexible drive afterwards, which the drive
	# reports as a media change. With a Winchester fitted the flexible
	# drive is :CS80,7,1, where the supervisor looks for its programs.
	def ready(disc):
		return [{'wait': READY, 'on': 'screen'}, {'sleep': 3},
				{'insert': str(test / (disc + '.TD0')), 'into': ':disc:floppy1'}, {'sleep': 1},
				{'type': 'MASS STORAGE IS ":CS80,7,1"\n', 'rate': 0.12}, {'sleep': 3}]

	def answer(text):
		return [{'press': 'CLR LINE'}, {'sleep': 1}, {'type': text, 'rate': 0.2}, {'sleep': 1}, STORE]

	for disc, label, programs in TEST_DISCS:
		short = disc[3:].lower()
		add('diag-cat-' + short, 'hp9k520', booted('cat-' + short),
				[*ready(disc), {'type': 'CAT\n', 'rate': 0.15},
				 {'wait': 'LABEL:  ' + label, 'on': 'screen', 'within': 30},
				 *({'wait': p, 'on': 'screen', 'within': 10} for p in ['TEST', *programs])], 120)
		# LOAD prints nothing when it works, and the next command
		# overwrites an error, so each one is checked before the next.
		loads = []
		for p in programs:
			loads += [{'type': f'LOAD "{p}"\n', 'rate': 0.12}, {'sleep': 8},
					{'absent': 'ERROR', 'on': 'screen'}]
		add('diag-load-' + short, 'hp9k520', booted('load-' + short),
				[*ready(disc), *loads], 60 + 12 * len(programs))
	supervisor = [*ready('500MFTST'), {'type': 'LOAD "TEST"\n', 'rate': 0.15}, {'sleep': 15},
			{'type': 'RUN\n', 'rate': 0.15}, {'wait': WELCOME, 'on': 'screen', 'within': 60}]
	add('diag-supervisor', 'hp9k520', booted('supervisor'), supervisor, 150)
	# The first question: where results go. INPUT is answered with ENTER,
	# which on this keyboard is STORE; EXECUTE runs the line as a live
	# keyboard command instead.
	menu = [*supervisor, {'sleep': 10}, *answer('DISPLAY'),
			{'wait': 'PROGRAM MENU', 'on': 'screen', 'within': 40}]
	add('diag-supervisor-display', 'hp9k520', booted('supervisor-display'), menu, 200)
	# HP's KEYBOARD test on its own: the clock, the configuration memory,
	# the keyboard ID and the beeper, register by register.
	add('diag-keyboard', 'hp9k520', booted('keyboard'),
			[*menu, {'sleep': 5}, *answer('KEYBOARD'),
			 {'wait': 'press RETURN to run', 'on': 'screen', 'within': 60}, {'sleep': 5}, *answer(''),
			 {'wait': 'number of times', 'on': 'screen', 'within': 60}, {'sleep': 5}, *answer(''),
			 {'wait': 'Program sequence complete', 'on': 'screen', 'within': 120},
			 {'absent': 'ERROR', 'on': 'screen'}, {'absent': 'FAIL', 'on': 'screen'}], 400)


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
	diag_cases(add, work)
	for name, keys, want in BASIC_TABLE:
		typing = []
		prev = None
		for chunk in [keys] if isinstance(keys, str) else keys:
			if isinstance(chunk, dict):
				typing += [chunk, {'sleep': 1}]
			else:
				if isinstance(prev, str):
					typing.append({'sleep': 6})
				typing += [{'post': chunk}, {'posted': True}]
			prev = chunk
		add('basic-' + name, 'hp9k520', lambda n=name: ['-hard', hd(work, SW / 'hd' / 'basic.img', 'basic-' + n)],
				[{'wait': READY, 'on': 'screen'}, {'sleep': 3}, *typing,
				 {'sleep': 4}, {'line': want, 'on': 'screen'}], 80)
	# The 27112A GPIO through BASIC's own HP27112 driver: a word written
	# with OUTPUT comes back through the loopback plug with ENTER. "AB"
	# is 0x4142.
	gpio_disc = Path(work) / 'gpio.img'

	def setup_gpio():
		if gpio_disc.exists():
			return
		opts = Path(work) / 'opt1'
		subprocess.run([sys.executable, str(ROOT / 'tools' / 'sdfread.py'),
				str(raw_volume(work, '500OPT1')), '--extract', str(opts)],
				check=True, stdout=subprocess.DEVNULL)
		subprocess.run([sys.executable, str(ROOT / 'tools' / 'mkharddisc.py'), str(gpio_disc)],
				check=True, stdout=subprocess.DEVNULL)
		subprocess.run([sys.executable, str(ROOT / 'tools' / 'installbasic.py'), str(gpio_disc),
				'--basic', str(raw_volume(work, '500BT1')), str(raw_volume(work, '500BT2')),
				'--option', *(str(opts / o) for o in ('IO', 'HP27112', 'ERRORS_ENG'))],
				check=True, stdout=subprocess.DEVNULL)
	add('gpio-loopback', 'hp9k520',
			lambda: ['-hard', hd(work, gpio_disc, 'gpio-loopback'), '-cio3', 'gpio'],
			[{'wait': READY, 'on': 'screen'}, {'sleep': 3},
			 {'post': 'OUTPUT 3;"AB"\n'}, {'posted': True}, {'sleep': 3},
			 {'post': 'ENTER 3 USING "#,W";W\n'}, {'posted': True}, {'sleep': 3},
			 {'post': 'DISP W\n'}, {'posted': True}, {'sleep': 3},
			 {'line': '16706', 'on': 'screen'}], 80, setup=setup_gpio)
	hpux501 = Path(work) / 'hpux501.img'

	def setup501():
		if not hpux501.exists():
			subprocess.run([sys.executable, str(ROOT / 'tools' / 'mkharddisc.py'), str(hpux501)],
					check=True, stdout=subprocess.DEVNULL)
			subprocess.run([sys.executable, str(ROOT / 'tools' / 'installbasic.py'), str(hpux501),
					'--boot-image', str(hpux501_kernel())], check=True, stdout=subprocess.DEVNULL)
	add('hpux501-banner', 'hp9k520',
			lambda: ['-ram', '2m', '-hard1', hd(work, hpux501, 'hpux501-boot'),
					'-cio5', 'hpib', '-hard2', hpux_volume(work, 'hpux501-root')],
			[{'wait': 'The Regents of the University of California', 'on': 'screen'}], 150,
			setup=setup501)

	# --- the 540
	console = ':cio0:asi:rs232:terminal'
	add('hpux-single', 'hp9k540',
			lambda: ['-ram', '2m', '-hard', hpux_volume(work, 'hpux-single')],
			[{'wait': 'TERM =', 'on': console}], 400,
			setup=lambda: battery(work, 'hp9k540', 'scm_rtc', SCM_NV))
	def hdd_list():
		d = Path(work) / 'roms' / 'hp9k5xx_hdd' / 'hpux530'
		chd = d / 'hpux530.chd'
		if not chd.exists():
			# MAME's CHD tool comes with a TOOLS=1 build only.
			if not (ROOT / 'mame' / 'chdman').exists():
				raise FileNotFoundError('mame/chdman, which a MAME build with TOOLS=1 makes')
			d.mkdir(parents=True, exist_ok=True)
			vol = Path(work) / 'hpux530-rebuilt.bin'
			tapes = []
			for t in ('7914_unitSave_hpux4.0.bin', '7914_unitSave_hpux4.0_tape2.bin'):
				raw = Path(work) / t
				with gzip.open(SW / 'cct' / (t + '.gz')) as src, open(raw, 'wb') as dst:
					shutil.copyfileobj(src, dst)
				tapes.append(str(raw))
			subprocess.run([sys.executable, str(ROOT / 'tools' / 'mkhpuxdisc.py'), *tapes, str(vol)],
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
			setup=lambda: battery(work, 'hp9k540', 'scm_rtc', SCM_NV))
	# The console card runs the tty's line discipline HP-UX hands it. In
	# canonical mode -- root's sh on a multiuser console -- the card echoes
	# what is typed and edits it, and ^C reaches HP-UX as an event; and
	# because a write waits for the 9600-baud line, it stops output that is
	# still streaming. Without the line discipline nothing typed echoed and
	# ^C reached the shell as text after the command finished; without the
	# pacing ^C showed after minutes of queued output.
	add('hpux-console-tty', 'hp9k540',
			lambda: ['-ram', '2m', '-hard', hpux_volume(work, 'hpux-console-tty', multiuser=True),
					'-cio2', 'mux'],
			[*hpux_login(console),
			 {'post': 'echo TY""PED\n', 'to': console}, {'posted': True},
			 {'wait': 'echo TY""PED', 'on': console, 'within': 5},
			 {'post': 'ecko\b\bho E""DIT\n', 'to': console}, {'posted': True},
			 {'line': 'EDIT', 'on': console, 'within': 5},
			 {'post': 'od -c /bin/csh\n', 'to': console}, {'posted': True}, {'sleep': 10},
			 {'post': '\x03', 'to': console}, {'posted': True}, {'sleep': 3},
			 {'post': 'echo MA""RK\n', 'to': console}, {'posted': True},
			 {'line': 'MARK', 'on': console, 'within': 10}], 900,
			setup=lambda: battery(work, 'hp9k540', 'scm_rtc', SCM_NV))
	# The instruction-set edge cases (tests/hpux/gen_isa.py): the ERS's
	# expectations for some 1700 cases, built by HP's own assembler and
	# run under HP-UX. The sources travel on the disc.
	isa_files = (HPUX_TESTS / 'isa.files').read_text().split()

	def isa_disc():
		d = hpux_volume(work, 'hpux-cc-isa', multiuser=True)
		hpux_carry(d, [(n, (HPUX_TESTS / n).read_bytes()) for n in ['isa.dat', *isa_files]])
		return d
	add('hpux-cc-isa', 'hp9k540',
			lambda: ['-ram', '2m', '-hard', isa_disc(), '-cio2', 'mux', '-cio2:mux:port0', 'terminal'],
			[*hpux_login(mux0),
			 {'post': f'clear; sh {HPUX_CARRIER}; cc -o isa {" ".join(isa_files)} && ./isa\n', 'to': mux0},
			 {'posted': True}, {'wait': ' passed 0 failed', 'on': mux0, 'within': 600}], 900,
			setup=lambda: battery(work, 'hp9k540', 'scm_rtc', SCM_NV))
	# ...and with 4 MB, where HP-UX moves 16 KB per disc transfer: long
	# enough for the mux to interrupt in the middle of one.
	add('hpux-multi-4m', 'hp9k540',
			lambda: ['-ram', '4m', '-hard', hpux_volume(work, 'hpux-multi-4m', multiuser=True),
					'-cio2', 'mux', '-cio2:mux:port0', 'terminal'],
			[{'wait': 'login:', 'on': mux0}, {'sleep': 5}, {'post': 'root\n', 'to': mux0},
			 {'wait': 'Welcome to Hewlett-Packard System 9000', 'on': mux0, 'within': 120},
			 {'wait': 'TERM =', 'on': mux0, 'within': 60}], 900,
			setup=lambda: battery(work, 'hp9k540', 'scm_rtc', SCM_NV))
	# HP's own C toolchain as the test bench: the sources in tests/hpux
	# are typed into /tmp, built with cc, as and ld, and run.
	add('hpux-cc-cpu', 'hp9k540',
			lambda: ['-ram', '2m', '-hard', hpux_volume(work, 'hpux-cc-cpu', multiuser=True),
					'-cio2', 'mux', '-cio2:mux:port0', 'terminal'],
			[*hpux_login(mux0),
			 *hpux_type_files(mux0, HPUX_TESTS, ['cpu.c', 'carry.s', 'dword.c', 'ldst.s', 'run.sh']),
			 {'post': 'clear; sh run.sh\n', 'to': mux0}, {'posted': True},
			 {'line': 'hpux cc tests: yes', 'on': mux0, 'within': 400}], 900,
			setup=lambda: battery(work, 'hp9k540', 'scm_rtc', SCM_NV))
	# The machine the disc came from: a 27130A at select code 0, its port
	# 0 /dev/console (c 31 0x000000).
	mux8con = ':cio0:mux8:port0:terminal'
	add('hpux-mux8-console', 'hp9k540',
			lambda: ['-ram', '2m', '-hard', hpux_volume(work, 'hpux-mux8-console'),
					'-cio0', 'mux8', '-cio0:mux8:port0', 'terminal'],
			[{'wait': 'TERM =', 'on': mux8con}], 400,
			setup=lambda: battery(work, 'hp9k540', 'scm_rtc', SCM_NV))
	# ...and a getty on one of its ports, the card at select code 2 with
	# tty0N on port N, the disc's own numbering.
	mux8 = ':cio2:mux8:port1:terminal'
	add('hpux-mux8-multi', 'hp9k540',
			lambda: ['-ram', '2m', '-hard', hpux_volume(work, 'hpux-mux8-multi', multiuser=True, card='27130'),
					'-cio2', 'mux8', '-cio2:mux8:port1', 'terminal'],
			[{'wait': 'login:', 'on': mux8}, {'sleep': 5}, {'post': 'root\n', 'to': mux8},
			 {'wait': 'Welcome to Hewlett-Packard System 9000', 'on': mux8, 'within': 120},
			 {'wait': 'TERM =', 'on': mux8, 'within': 60}], 900,
			setup=lambda: battery(work, 'hp9k540', 'scm_rtc', SCM_NV))
	# The whole of that machine: one 27130A at select code 0 carrying the
	# console on port 0 and the terminals on the rest, two of them used.
	sc0 = [f':cio0:mux8:port{p}:terminal' for p in range(4)]
	add('hpux-mux8-sc0', 'hp9k540',
			lambda: ['-ram', '2m', '-hard', hpux_volume(work, 'hpux-mux8-sc0', multiuser=True, card='27130', sc=0),
					'-cio0', 'mux8', '-cio0:mux8:port0', 'terminal',
					'-cio0:mux8:port1', 'terminal', '-cio0:mux8:port3', 'terminal'],
			[{'wait': 'login:', 'on': sc0[1]}, {'wait': 'login:', 'on': sc0[3]},
			 {'wait': 'login:', 'on': sc0[0]},
			 {'sleep': 5}, {'post': 'root\n', 'to': sc0[1]}, {'posted': True},
			 {'wait': 'TERM =', 'on': sc0[1], 'within': 120},
			 {'post': 'root\n', 'to': sc0[3]}, {'posted': True},
			 {'wait': 'TERM =', 'on': sc0[3], 'within': 120}], 900,
			setup=lambda: battery(work, 'hp9k540', 'scm_rtc', SCM_NV))
	return c


#--------------------------------------------------------------------------
#  running
#--------------------------------------------------------------------------

def run_case(case, mame, work):
	cwork = Path(work) / case['name']
	cwork.mkdir(parents=True, exist_ok=True)
	# Hewlett-Packard's media is fetched, not carried (MEDIA.txt), and the
	# 520's comes by hand: a case whose media is not here is skipped, not
	# failed.
	try:
		if case['setup']:
			case['setup']()
		args = case['args']() if callable(case['args']) else case['args']
	except FileNotFoundError as e:
		return case['name'], None, f'not here: {e}'
	except subprocess.CalledProcessError as e:
		return case['name'], False, f'setup failed: {e}'
	# ...and so is one whose steps put a disc that is not here in a drive.
	inserted = [s['insert'] for s in case['steps'] if 'insert' in s]
	missing = [a for a in [*args, *inserted] if isinstance(a, str) and a.startswith(str(SW)) and not Path(a).exists()]
	if missing:
		return case['name'], None, 'media not here: ' + ', '.join(Path(m).name for m in missing)
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
	ap.add_argument('--strict', action='store_true',
			help='a case skipped for want of media counts as a failure')
	args = ap.parse_args()
	# MAME runs from its own directory, so a relative path would not.
	args.mame = str(Path(args.mame).resolve())
	if args.work:
		args.work = str(Path(args.work).resolve())

	work = args.work or tempfile.mkdtemp(prefix='hp9k5xx-tests-')
	table = cases(work)
	if args.names:
		table = [c for c in table if any(c['name'].startswith(n) for n in args.names)]
	if args.list:
		for c in table:
			print(f"{c['name']:<20} {c['system']}")
		return 0

	print(f'{len(table)} cases, work directory {work}')
	failed = unexpected = skipped = 0
	with concurrent.futures.ThreadPoolExecutor(max_workers=args.j) as pool:
		for name, ok, why in pool.map(lambda c: run_case(c, args.mame, work), table):
			known = name in KNOWN_FAILING
			if ok is None:
				skipped += 1
				print(f'  SKIP  {name}  {why}')
			elif ok:
				print(f'  PASS  {name}' + ('  (was known failing)' if known else ''))
			else:
				failed += 1
				unexpected += not known
				print(f'  FAIL  {name}' + ('  (known)' if known else '') + f'  {why}')
	print(f'{len(table) - failed - skipped} passed, {failed} failed, {unexpected} unexpected'
			+ (f', {skipped} skipped' if skipped else ''))
	return 1 if unexpected or (args.strict and skipped) else 0


if __name__ == '__main__':
	sys.exit(main())
