#!/usr/bin/env python3
"""Generate the instruction-set edge-case checks run under HP-UX 5.30.

Every case is one instruction sequence with its operands, the STATUS bits
it starts from, and what the ERS says the stack, STATUS and memory hold
afterwards. The expectations come from the ERS text, computed here; they
are not taken from the emulator.

Out come three files, typed into the emulated machine by
tests/run_tests.py hpux-cc-isa (or tools/hpuxrun.py):

  isaops*.s one routine per distinct instruction sequence, in HP's
           assembler syntax, all with the same frame:
             t(r, and, or, x, a, b, c, d, m)
           X := x; push three zero sentinels and the operands a..d;
           STATUS := (STATUS & and) | or (SETR, so unprivileged code may
           only change what the ERS lets it); run the sequence; then
           r[0] := STATUS and r[1..3] := the top three stack words.
           m points at four words of memory the sequence may use.
  isa.dat  one line per case: which routine, its arguments, the masked
           expectations, memory before and after.
  isa.files the sources to build, in order
  isa.c    reads isa.dat, runs each case under signal catchers (a trap
           the kernel turns into a signal is an outcome, not a crash),
           and prints every mismatch and a summary line.

    python3 tests/hpux/gen_isa.py        writes the three files beside it
"""
import math
import struct
from fractions import Fraction
from pathlib import Path

HERE = Path(__file__).resolve().parent
M32 = 0xffffffff

# STATUS fields as masks (ERS sheet 12, bit 0 the most significant).
IR, C, O, T = 1 << 26, 1 << 25, 1 << 24, 1 << 23
M = 1 << 17
RMF, RMS = 0x3 << 19, 19
CCF, CCS = 0x3 << 14, 14
CCG, CCL, CCE, CCU = 0, 1, 2, 3


def u(v):
	return v & M32


def s(v):
	v &= M32
	return v - (1 << 32) if v & 0x80000000 else v


def cca(v):
	v = s(v)
	return CCL if v < 0 else CCE if v == 0 else CCG


def ccc(a, b):
	return CCL if a < b else CCE if a == b else CCG


# ---------------------------------------------------------------------------
#  cases
# ---------------------------------------------------------------------------

CASES = []


def case(name, code, args=(), x=0, pre=None, top=None, st=None, sig=False,
		mem=None, memx=None):
	"""top: expected words from the top of stack down (None = unchecked);
	st: {'CC','C','O','IR','M','RM'} expected after; pre: their values
	before (C, O, IR, RM, T and CC default to zero, i.e. CCG). HP-UX runs
	user code with T set, which turns an overflow into cause 031 and
	clears Overflow, so the cases clear it to see the flag itself."""
	p = dict(C=0, O=0, IR=0, RM=0, CC=CCG, T=0)
	p.update(pre or {})
	CASES.append(dict(name=name, code=tuple(code), args=tuple(args), x=x,
			pre=p, top=list(top or []), st=dict(st or {}), sig=sig,
			mem=list(mem or [0, 0, 0, 0]), memx=memx))


def status_preset(p):
	clear = IR | C | O | T | RMF | CCF
	val = ((p['IR'] and IR) | (p['C'] and C) | (p['O'] and O) | (p['T'] and T)
			| (p['RM'] << RMS) | (p['CC'] << CCS))
	return u(~clear), val


SAND = u(~(IR | C | O | T | RMF | CCF))


def status_check(st):
	mask = val = 0
	for k, bit in (('IR', IR), ('C', C), ('O', O), ('M', M)):
		if k in st:
			mask |= bit
			val |= bit if st[k] else 0
	if 'CC' in st:
		mask |= CCF
		val |= st['CC'] << CCS
	if 'RM' in st:
		mask |= RMF
		val |= st['RM'] << RMS
	return mask, val


EDGE = [0, 1, 2, u(-1), u(-2), 0x7fffffff, 0x80000000, 0x80000001, 0x7ffffffe,
		0x55555555, 0xaaaaaaaa, 0x10000]
SMALL = [0, 1, u(-1), 0x7fffffff, 0x80000000, 0x55555555, 3]


# ---- integer add/subtract ---------------------------------------------------
# ERS sheets 74-76. Overflow is sticky (sheet 12: "cleared only by the
# Branch on Overflow, Branch on No Overflow, and Set Registers
# instructions"); Carry is not among ADD's or SUB's indicators, so it is
# not checked for them.

def add_cases():
	for a in SMALL:
		for b in SMALL:
			r = u(a + b)
			ov = s(a) + s(b) != s(r)
			case(f'add {a:x}+{b:x}', ['add'], (a, b), top=[r], st=dict(CC=cca(r), O=ov))
			r = u(a - b)
			ov = s(a) - s(b) != s(r)
			case(f'sub {a:x}-{b:x}', ['sub'], (a, b), top=[r], st=dict(CC=cca(r), O=ov))
	# Overflow stays set across an operation that does not overflow.
	case('add keeps O', ['add'], (1, 2), pre=dict(O=1), top=[3], st=dict(O=1))
	case('sub keeps O', ['sub'], (5, 2), pre=dict(O=1), top=[3], st=dict(O=1))
	# The b forms: carry in, and a zero result leaves CC alone.
	for cin in (0, 1):
		for a, b in ((0, 0), (u(-1), 0), (5, 3), (3, 5), (0x7fffffff, 0)):
			r = u(a + b + cin)
			cc = cca(r) if r else CCL
			case(f'add,c c={cin} {a:x}+{b:x}', ['add,c'], (a, b), pre=dict(C=cin, CC=CCL),
					top=[r], st=dict(CC=cc, O=s(a) + s(b) + cin != s(r)))
			r = u(a - b - (1 - cin))
			cc = cca(r) if r else CCL
			case(f'sub,c c={cin} {a:x}-{b:x}', ['sub,c'], (a, b), pre=dict(C=cin, CC=CCL),
					top=[r], st=dict(CC=cc, O=s(a) - s(b) - (1 - cin) != s(r)))
	# Logical add and subtract: unsigned, Carry out, CCG/CCE (sheet 76).
	for a in SMALL:
		for b in (0, 1, u(-1), 0x80000000):
			r = a + b
			case(f'ladd {a:x}+{b:x}', ['ladd'], (a, b), top=[u(r)],
					st=dict(CC=CCG if u(r) else CCE, C=r >> 32))
			r = a - b
			case(f'lsub {a:x}-{b:x}', ['lsub'], (a, b), top=[u(r)],
					st=dict(CC=CCG if u(r) else CCE, C=0 if r < 0 else 1))
	case('ladd,c zero keeps CCL', ['ladd,c'], (0, 0), pre=dict(C=0, CC=CCL), top=[0], st=dict(CC=CCL))
	case('lsub,c 0-0-(1-1)', ['lsub,c'], (0, 0), pre=dict(C=1, CC=CCL), top=[0], st=dict(CC=CCL, C=1))
	case('lsub,c 0-0-(1-0)', ['lsub,c'], (0, 0), pre=dict(C=0), top=[u(-1)], st=dict(CC=CCG, C=0))


# ---- multiply, divide, negate ----------------------------------------------
# ERS sheets 74-75.

def mul_cases():
	for a in SMALL + [0x10000, 0x8000]:
		for b in (0, 1, u(-1), 2, 0x10000, 0x7fffffff):
			p = s(a) * s(b)
			lo = u(p)
			ov = p != s(lo)
			case(f'mpy {a:x}*{b:x}', ['mpy'], (a, b), top=[lo], st=dict(CC=cca(lo), O=ov))
			case(f'mpy,d {a:x}*{b:x}', ['mpy,d'], (a, b), top=[lo, u(p >> 32)])
			pl = a * b
			case(f'lmpy {a:x}*{b:x}', ['lmpy'], (a, b), top=[u(pl), u(pl >> 32)])
	case('mpy keeps O', ['mpy'], (2, 3), pre=dict(O=1), top=[6], st=dict(O=1))


def divide(a, d, positive):
	if positive:
		r = a % abs(d)
		q = (a - r) // d
	else:
		q = abs(a) // abs(d)
		if (a < 0) != (d < 0):
			q = -q
		r = a - q * d
	return q, r


def div_cases():
	for a in (7, -7, 6, -6, 0, 1, -1, 0x7fffffff, -0x80000000, 100):
		for d in (2, -2, 3, -3, 1, -1, 7):
			for pos in (False, True):
				if a == -0x80000000 and d == -1:
					continue
				q, r = divide(a, d, pos)
				op = 'divs,e' if pos else 'divs'
				case(f'{op} {a}/{d}', [op], (u(a), u(d)), top=[u(r), u(q)],
						st=dict(CC=cca(q), O=0))
				op = 'div,e' if pos else 'div'
				q, r = divide(a, d, pos)
				case(f'{op} {a}/{d}', [op], (u(a >> 32), u(a), u(d)),
						top=[u(r), u(q), u(q >> 32)], st=dict(O=0))
	# A doubleword dividend wider than a word.
	for hi, lo, d in ((1, 0, 2), (u(-1), 0, 2), (0x12345678, 0x9abcdef0, 0x10000)):
		a = s(hi) * (1 << 32) + lo
		q, r = divide(a, d, False)
		case(f'div {hi:x}:{lo:x}/{d:x}', ['div'], (hi, lo, d), top=[u(r), u(q), u(q >> 32)])
	# The single quotient that does not fit, and divide by zero with the
	# user trap disabled: "the overflow bit ... is set and no trap is
	# taken" (sheet 48).
	case('divs INT_MIN/-1', ['divs'], (0x80000000, u(-1)), st=dict(O=1))
	case('divs n/0 (T clear)', ['divs'], (5, 0), st=dict(O=1))
	case('div n/0 (T clear)', ['div'], (0, 5, 0), st=dict(O=1))
	case('divi n/0 (T clear)', ['divi 0'], (5,), st=dict(O=1))
	for a in SMALL:
		r = u(-s(a))
		case(f'neg {a:x}', ['neg'], (a,), top=[r], st=dict(CC=cca(r), O=a == 0x80000000))
	case('neg keeps O', ['neg'], (5,), pre=dict(O=1), top=[u(-5)], st=dict(O=1))


# ---- immediates -------------------------------------------------------------
# ERS sheets 77-80: the 8-bit operand is a positive quantity; ANDI, ORI and
# XORI expand it with leading zeros.

def imm_cases():
	for a in SMALL:
		for k in (0, 1, 255):
			r = u(a + k)
			case(f'addi {a:x}+{k}', [f'addi {k}'], (a,), top=[r],
					st=dict(CC=cca(r), O=s(a) + k != s(r)))
			r = u(a - k)
			case(f'subi {a:x}-{k}', [f'subi {k}'], (a,), top=[r],
					st=dict(CC=cca(r), O=s(a) - k != s(r)))
			p = s(a) * k
			case(f'mpyi {a:x}*{k}', [f'mpyi {k}'], (a,), top=[u(p)],
					st=dict(CC=cca(u(p)), O=p != s(u(p))))
			if k:
				q = abs(s(a)) // k * (1 if s(a) >= 0 else -1)
				case(f'divi {a:x}/{k}', [f'divi {k}'], (a,), top=[u(q)], st=dict(CC=cca(q)))
			case(f'andi {a:x}&{k}', [f'andi {k}'], (a,), top=[a & k], st=dict(CC=cca(a & k)))
			case(f'ori {a:x}|{k}', [f'ori {k}'], (a,), top=[a | k], st=dict(CC=cca(a | k)))
			case(f'xori {a:x}^{k}', [f'xori {k}'], (a,), top=[a ^ k], st=dict(CC=cca(a ^ k)))
			case(f'cmpi {a:x}?{k}', [f'cmpi {k}'], (a,), top=[0], st=dict(CC=ccc(s(a), k)))
			case(f'cmpn {a:x}?-{k}', [f'cmpn {k}'], (a,), top=[0], st=dict(CC=ccc(s(a), -k)))
	for k in (0, 1, 127, 255):
		case(f'ldis {k}', [f'ldis {k}'], top=[k], st=dict(CC=cca(k)))
		case(f'lnis {k}', [f'lnis {k}'], top=[u(-k)], st=dict(CC=cca(u(-k))))
		case(f'ldxi {k}', [f'ldxi {k}', 'ldxa'], top=[k])
		case(f'ldxn {k}', [f'ldxn {k}', 'ldxa'], top=[u(-k)])
	for k in (0, 1, 0x7fffff, 0x800000, 0xffffff):
		case(f'ldi {k:x}', [f'ldi {k}'], top=[k], st=dict(CC=cca(k)))
		case(f'lni {k:x}', [f'lni {k}'], top=[u(-k)], st=dict(CC=cca(u(-k))))


# ---- compares ---------------------------------------------------------------

def cmp_cases():
	for a in SMALL:
		for b in SMALL:
			case(f'cmp {a:x}:{b:x}', ['cmp'], (a, b), top=[a, 0], st=dict(CC=ccc(s(a), s(b))))
			case(f'lcmp {a:x}:{b:x}', ['lcmp'], (a, b), top=[a, 0], st=dict(CC=ccc(a, b)))
	case('cmp,t deletes both', ['cmp,t'], (1, 2), top=[0, 0, 0], st=dict(CC=CCL))
	case('lcmp,t deletes both', ['lcmp,t'], (2, 1), top=[0, 0, 0], st=dict(CC=CCG))
	for a, mv in ((5, 5), (5, 6), (u(-1), 0), (0x80000000, 0x7fffffff)):
		case(f'cmpm {a:x}:{mv:x}', ['cmpm Q-63,i'], (a,), mem=[mv, 0, 0, 0],
				top=[0], st=dict(CC=ccc(s(a), s(mv))))


# ---- the index register -----------------------------------------------------

def x_cases():
	for xv in (0, 5, 0x7fffffff, u(-1), 0x80000000):
		for k in (0, 1, 255):
			r = u(xv + k)
			case(f'adxi x={xv:x}+{k}', [f'adxi {k}', 'ldxa'], x=xv, top=[r],
					st=dict(O=s(xv) + k != s(r)))
			r = u(xv - k)
			case(f'sbxi x={xv:x}-{k}', [f'sbxi {k}', 'ldxa'], x=xv, top=[r],
					st=dict(O=s(xv) - k != s(r)))
		for a in (1, u(-1), 0x7fffffff):
			r = u(xv + a)
			case(f'adax x={xv:x}+{a:x}', ['adax', 'ldxa'], (a,), x=xv, top=[r, a],
					st=dict(O=s(xv) + s(a) != s(r)))
			case(f'adax,p x={xv:x}+{a:x}', ['adax,p', 'ldxa'], (a,), x=xv, top=[r, 0],
					st=dict(O=s(xv) + s(a) != s(r)))
			case(f'adxa x={xv:x}+{a:x}', ['adxa'], (a,), x=xv, top=[r],
					st=dict(CC=cca(r), O=s(xv) + s(a) != s(r)))
		case(f'xax x={xv:x}', ['xax', 'ldxa'], (7,), x=xv, top=[7, xv])
		case(f'stax x={xv:x}', ['stax', 'ldxa'], (9,), x=xv, top=[9, 9])
		case(f'stax,p x={xv:x}', ['stax,p', 'ldxa'], (9,), x=xv, top=[9, 0])
		case(f'ldxa x={xv:x}', ['ldxa'], x=xv, top=[xv], st=dict(CC=cca(xv)))


# ---- memory increment and decrement -------------------------------------------

def mem_cases():
	for mv in (0, 0x7fffffff, u(-1), 0x80000000, u(-4), 0x7ffffffc):
		for op, k in (('incm', 1), ('decm', -1), ('incf', 4), ('decf', -4)):
			r = u(mv + k)
			case(f'{op} {mv:x}', [f'{op} Q-63,i'], mem=[mv, 0x11, 0x22, 0x33],
					memx=[r, 0x11, 0x22, 0x33], st=dict(CC=cca(r), O=s(mv) + k != s(r)))
	case('incm keeps O', ['incm Q-63,i'], pre=dict(O=1), mem=[1, 0, 0, 0], memx=[2, 0, 0, 0], st=dict(O=1))


# ---- loads and stores of every width ----------------------------------------
# LDH sign extends (sheet 15). Byte addresses: m+1 is the second byte.

def ld_cases():
	w = [0x8182c3c4, 0x7f00ff80, 0x12345678, 0xdeadbeef]
	case('ld', ['ld Q-63,i'], mem=w, top=[w[0]], st=dict(CC=cca(w[0])))
	case('ld,x 4', ['ldxi 4', 'ld Q-63,i,x'], mem=w, top=[w[1]])
	case('ldd', ['ldd Q-63,i'], mem=w, top=[w[1], w[0]])
	case('ldh', ['ldh Q-63,i'], mem=w, top=[0xffff8182], st=dict(CC=CCL))
	case('ldh,x 2', ['ldxi 2', 'ldh Q-63,i,x'], mem=w, top=[0xffffc3c4])
	case('ldh,x 4', ['ldxi 4', 'ldh Q-63,i,x'], mem=w, top=[0x7f00])
	case('ldb', ['ldb Q-63,i'], mem=w, top=[0x81])
	case('ldb,x 3', ['ldxi 3', 'ldb Q-63,i,x'], mem=w, top=[0xc4])
	case('ldb,x 7', ['ldxi 7', 'ldb Q-63,i,x'], mem=w, top=[0x80])
	case('st', ['st Q-63,i'], (0xcafef00d,), mem=w, memx=[0xcafef00d] + w[1:], top=[0])
	case('std', ['std Q-63,i'], (1, 2), mem=w, memx=[1, 2] + w[2:], top=[0])
	case('sth', ['sth Q-63,i'], (0x1234abcd,), mem=w, memx=[0xabcdc3c4] + w[1:])
	case('sth,x 2', ['ldxi 2', 'sth Q-63,i,x'], (0x1234abcd,), mem=w, memx=[0x8182abcd] + w[1:])
	case('stb,x 1', ['ldxi 1', 'stb Q-63,i,x'], (0x123456ee,), mem=w, memx=[0x81eec3c4] + w[1:])
	case('stb,x 6', ['ldxi 6', 'stb Q-63,i,x'], (0x55,), mem=w, memx=[w[0], 0x7f005580] + w[2:])


# ---- shifts -------------------------------------------------------------------
# ERS sheets 72-73. CNT is the six low bits of the operand, plus X when
# indexed; LSL/LSR/ASL by 32 or more leave zero. ASR negates a negative
# TOS, shifts in zeros and negates back. ASL sets Overflow if the sign
# changes during the shift.

def shift(op, v, n):
	if op == 'lsl':
		return u(v << n) if n < 32 else 0, False
	if op == 'lsr':
		return v >> n if n < 32 else 0, False
	if op == 'csl':
		n %= 32
		return u((v << n) | (v >> (32 - n))) if n else v, False
	if op == 'csr':
		n %= 32
		return u((v >> n) | (v << (32 - n))) if n else v, False
	if op == 'asl':
		ov, r = False, v
		for _ in range(n):
			nr = u(r << 1)
			ov |= (nr ^ r) >> 31 == 1
			r = nr
		return r, ov
	if op == 'asr':
		neg = v & 0x80000000
		m = u(-s(v)) if neg else v
		m = m >> n if n < 32 else 0
		return (u(-m) if neg else m), False


def shift_cases():
	vals = [1, 0x80000000, 0xf0000001, u(-1), 0x40000000, 0x12345678, u(-3)]
	for op in ('lsl', 'lsr', 'csl', 'csr', 'asl', 'asr'):
		for v in vals:
			for n in (0, 1, 4, 31, 32, 33, 63):
				r, ov = shift(op, v, n)
				st = dict(CC=cca(r))
				if op == 'asl':
					st['O'] = ov
				case(f'{op} {v:x} {n}', [f'{op} {n}'], (v,), top=[r], st=st)
		# Indexed: CNT is the six low bits of operand + X.
		for n, xv in ((1, 3), (1, 31), (1, 63), (60, 8), (0, 0x41)):
			r, ov = shift(op, 0x12345678, (n + xv) & 63)
			case(f'{op} 12345678 {n}+x{xv}', [f'{op} {n},x'], (0x12345678,), x=xv, top=[r])
	for xv in (1, 0x80000001):
		for n in (0, 1, 31, 32):
			r = u(xv << n) if n < 32 else 0
			case(f'lsxl x={xv:x} {n}', [f'lsxl {n}', 'ldxa'], x=xv, top=[r])


# ---- fields -------------------------------------------------------------------
# ERS sheets 72-73. Bits are numbered from the most significant (0); bits
# beyond 31 read as zero and are lost on deposit. Indexing adds X to the
# 10-bit (K, J) pair, K = 32 - width above J.

def bits_of(v, j, l):
	out = 0
	for i in range(l):
		b = j + i
		bit = (v >> (31 - b)) & 1 if b <= 31 else 0
		out = (out << 1) | bit
	return out


def exf(v, j, l, sign):
	f = bits_of(v, j, l)
	if sign and l and f >> (l - 1):
		f -= 1 << l
	return u(f)


def dpf(dst, src, j, l):
	for i in range(l):
		b = j + i
		if b > 31:
			break
		bit = (src >> (l - 1 - i)) & 1
		dst = (dst & ~(1 << (31 - b))) | (bit << (31 - b))
	return u(dst)


def fx(j, l, xv):
	"""The (J, L) an indexed field instruction really uses."""
	k = (32 - l) & 31
	both = ((k << 5) | j) + xv
	j2, k2 = both & 31, (both >> 5) & 31
	return j2, 32 - k2


def field_cases():
	v = 0xc3a55a96
	for j, l in ((0, 32), (31, 1), (0, 1), (3, 5), (28, 8), (16, 16), (24, 8), (30, 4)):
		for sgn in (False, True):
			r = exf(v, j, l, sgn)
			op = f'exf {j}:{l}' + (',s' if sgn else '')
			case(op, [op], (v,), top=[r], st=dict(CC=cca(r)))
		r = dpf(0x0f0f0f0f, v, j, l)
		case(f'dpf {j}:{l}', [f'dpf {j}:{l}'], (0x0f0f0f0f, v), top=[r, 0], st=dict(CC=cca(r)))
	for xv in (1, 31, 32, 33, 2 << 5):
		j, l = fx(3, 5, xv)
		r = exf(v, j, l, False)
		case(f'exf 3:5,x x={xv}', ['exf 3:5,x'], (v,), x=xv, top=[r])
		r = dpf(0x0f0f0f0f, v, j, l)
		case(f'dpf 3:5,x x={xv}', ['dpf 3:5,x'], (0x0f0f0f0f, v), x=xv, top=[r])


# ---- logical and stack operations --------------------------------------------

def logic_stack_cases():
	for a, b in ((0xf0f0f0f0, 0xff00ff00), (0, 0), (u(-1), 1), (0x80000000, 0x80000000)):
		for op, f in (('and', a & b), ('or', a | b), ('xor', a ^ b)):
			case(f'{op} {a:x},{b:x}', [op], (a, b), top=[f, 0], st=dict(CC=cca(f)))
		case(f'not {a:x}', ['not'], (a,), top=[u(~a), 0], st=dict(CC=cca(u(~a))))
	for a in (0, 1, u(-1)):
		case(f'test {a:x}', ['test'], (a,), top=[a], st=dict(CC=cca(a)))
		case(f'test,p {a:x}', ['test,p'], (a,), top=[0], st=dict(CC=cca(a)))
		case(f'dup {a:x}', ['dup'], (a,), top=[a, a, 0])
	for hi, lo in ((0, 0), (0, 1), (u(-1), 0), (1, 0), (0x80000000, 0), (0, 0x80000000)):
		d = s(hi) * (1 << 32) + lo
		cc = CCL if d < 0 else CCE if d == 0 else CCG
		case(f'tstd {hi:x}:{lo:x}', ['tstd'], (hi, lo), top=[lo, hi], st=dict(CC=cc))
		case(f'tstd,p {hi:x}:{lo:x}', ['tstd,p'], (hi, lo), top=[0, 0], st=dict(CC=cc))
		case(f'ddup {hi:x}:{lo:x}', ['ddup'], (hi, lo), top=[lo, hi, lo], st=dict(CC=cc))
	for b in (0, 0x100, 0x1ff, 0xffffff00, 0x80):
		case(f'tstb {b:x}', ['tstb'], (b,), top=[b], st=dict(CC=CCG if b & 0xff else CCE))
	case('xch', ['xch'], (1, 2), top=[1, 2], st=dict(CC=CCG))
	case('xch neg', ['xch'], (u(-1), 0), top=[u(-1), 0], st=dict(CC=CCL))
	case('dxch', ['dxch'], (1, 2, 3, 4), top=[2, 1, 4])
	case('cab', ['cab'], (1, 2, 3), top=[1, 3, 2], st=dict(CC=CCG))
	case('cab neg', ['cab'], (u(-5), 2, 3), top=[u(-5), 3, 2], st=dict(CC=CCL))
	# Indicators unaffected (sheets 19-21).
	for op, args, top in (('del', (1, 2), [1, 0]), ('ddel', (1, 2, 3), [1, 0]),
			('delb', (1, 2), [2, 0])):
		for cc in (CCL, CCE):
			case(f'{op} keeps cc={cc}', [op], args, pre=dict(CC=cc), top=top, st=dict(CC=cc))
	case('zero', ['zero'], (7,), top=[0, 7])
	case('dzro', ['dzro'], (7,), top=[0, 0, 7])


# ---- ISC, PSHR/SETR ---------------------------------------------------------

def misc_cases():
	for v in (0, 0x7fff, 0x8000, u(-0x8000), u(-0x8001), 0x10000, u(-1)):
		ov = not (-0x8000 <= s(v) <= 0x7fff)
		case(f'isc {v:x}', ['isc'], (v,), top=[v], st=dict(O=ov, CC=cca(v)))
	case('isc keeps O', ['isc'], (5,), pre=dict(O=1), top=[5], st=dict(O=1))
	# "In unprivileged mode ... only STATUS(0:9), STATUS(10:3), and
	# STATUS(16:2) are modified" (sheet 71): the mode bit stays clear.
	case('setr cannot set M', ['pshr 4', 'lds Q-47', 'or', 'setr 4'], (M,), st=dict(M=0))
	case('setr sets CC=11', [], pre=dict(CC=CCU), st=dict(CC=CCU))


# ---- branches -----------------------------------------------------------------
# A branch case leaves 2 on the stack when taken, 1 when not.

def br(insn):
	return [f'{insn} ~L.2', 'ldis 1', 'brs ~L.3', '~L.2:\tldis 2', '~L.3:\tnop']


def branch_cases():
	for cc, name in ((CCG, 'G'), (CCL, 'L'), (CCE, 'E'), (CCU, 'U')):
		for insn, takes in (('blts', {CCL, CCU}), ('bgts', {CCG}), ('beqs', {CCE}),
				('bles', {CCL, CCE, CCU}), ('bges', {CCG, CCE}), ('bnes', {CCG, CCL, CCU}),
				('bun', {CCU})):
			case(f'{insn} cc={name}', br(insn), pre=dict(CC=cc), top=[2 if cc in takes else 1])
	for o in (0, 1):
		case(f'bov o={o}', br('bov'), pre=dict(O=o), top=[2 if o else 1], st=dict(O=0))
		case(f'bno o={o}', br('bno'), pre=dict(O=o), top=[1 if o else 2], st=dict(O=0))
	for c in (0, 1):
		case(f'bcy c={c}', br('bcy'), pre=dict(C=c), top=[2 if c else 1], st=dict(C=0))
		case(f'bnc c={c}', br('bnc'), pre=dict(C=c), top=[1 if c else 2], st=dict(C=0))
	for i in (0, 1):
		case(f'bir ir={i}', br('bir'), pre=dict(IR=i), top=[2 if i else 1], st=dict(IR=0))
	# A zero offset with IR set is a Floating Point Result Trap (sheet 94).
	case('bir 0 traps', ['~L.4:\tbir ~L.4'], pre=dict(IR=1), sig=True)

# ---- decimal ------------------------------------------------------------------

def bcd_add(a, b, cin):
	out, c = 0, cin
	for i in range(8):
		d = ((a >> 4 * i) & 15) + ((b >> 4 * i) & 15) + c
		c = 1 if d > 9 else 0
		out |= (d - 10 if c else d) << 4 * i
	return out, c


def bcd_sub(a, b, bin_):
	out, br_ = 0, bin_
	for i in range(8):
		d = ((a >> 4 * i) & 15) - ((b >> 4 * i) & 15) - br_
		br_ = 1 if d < 0 else 0
		out |= (d + 10 if br_ else d) << 4 * i
	return out, br_


def bcd_cases():
	for a, b in ((0x99999999, 1), (0x12345678, 0x87654321), (0, 0), (0x5, 0x5)):
		r, c = bcd_add(a, b, 0)
		case(f'dlad {a:x}+{b:x}', ['dlad'], (a, b), top=[r], st=dict(C=c, CC=CCG if r else CCE))
		r, br_ = bcd_sub(a, b, 0)
		case(f'dlsb {a:x}-{b:x}', ['dlsb'], (a, b), top=[r], st=dict(C=1 - br_, CC=CCG if r else CCE))
	case('dlad,c zero keeps CCL', ['dlad,c'], (0, 0), pre=dict(C=0, CC=CCL), top=[0], st=dict(CC=CCL, C=0))
	# A digit above nine is an Illegal Decimal trap (sheet 93.8).
	case('dlad bad digit', ['dlad'], (0xa, 1), sig=True)
	case('dlsb bad digit', ['dlsb'], (1, 0xf0000000), sig=True)


# ---- binary floating point ----------------------------------------------------
# Appendix F: IEEE single and double, all four rounding modes, inexact
# results set STATUS(5:1) and never trap, while infinities, NaNs,
# denormals and division by zero are Floating Point Operand Traps and
# overflow and underflow Floating Point Result Traps.

def d2w(f):
	b = struct.unpack('>Q', struct.pack('>d', f))[0]
	return b >> 32, b & M32


def f2w(f):
	return struct.unpack('>I', struct.pack('>f', f))[0]


def round_frac(x, mode, mbits, emin):
	"""Round an exact Fraction to the binary format with `mbits` of
	significand; returns (Fraction, inexact)."""
	if x == 0:
		return Fraction(0), False
	sign = -1 if x < 0 else 1
	a = abs(x)
	e = math.floor(math.log2(a))
	while Fraction(2) ** e > a:
		e -= 1
	while Fraction(2) ** (e + 1) <= a:
		e += 1
	e = max(e, emin)
	q = a / Fraction(2) ** (e - mbits + 1)
	n = math.floor(q)
	rem = q - n
	if rem:
		up = {0: rem > Fraction(1, 2) or (rem == Fraction(1, 2) and n % 2),
				1: sign > 0, 2: False, 3: sign < 0}[mode]
		if up:
			n += 1
	return sign * n * Fraction(2) ** (e - mbits + 1), rem != 0


def dbl(x, mode=0):
	r, inexact = round_frac(Fraction(x), mode, 53, -1022)
	return float(r), inexact


def sgl(x, mode=0):
	r, inexact = round_frac(Fraction(x), mode, 24, -126)
	return float(r), inexact


def fp_cases():
	third = Fraction(1, 3)
	for mode in range(4):
		# Exact sums leave IR alone; inexact ones set it, rounded per RM.
		for a, b in ((1.0, 2.0 ** -53), (1.0, 3 * 2.0 ** -54), (-1.0, -(2.0 ** -53)),
				(1.0, 2.0), (0.1, 0.2)):
			r, ix = dbl(Fraction(a) + Fraction(b), mode)
			case(f'fadd {a!r}+{b!r} rm{mode}', ['fadd'], (*d2w(a), *d2w(b)), pre=dict(RM=mode),
					top=[d2w(r)[1], d2w(r)[0]], st=dict(IR=ix, CC=CCL if r < 0 else CCE if r == 0 else CCG))
		r, ix = dbl(third, mode)
		case(f'fdiv 1/3 rm{mode}', ['fdiv'], (*d2w(1.0), *d2w(3.0)), pre=dict(RM=mode),
				top=[d2w(r)[1], d2w(r)[0]], st=dict(IR=ix))
		r, ix = dbl(Fraction(-1) / 3, mode)
		case(f'fdiv -1/3 rm{mode}', ['fdiv'], (*d2w(-1.0), *d2w(3.0)), pre=dict(RM=mode),
				top=[d2w(r)[1], d2w(r)[0]], st=dict(IR=ix))
		r, ix = sgl(third, mode)
		case(f'fdvs 1/3 rm{mode}', ['fdvs'], (f2w(1.0), f2w(3.0)), pre=dict(RM=mode),
				top=[f2w(r)], st=dict(IR=ix))
		r, ix = sgl(16777217, mode)
		case(f'flts 16777217 rm{mode}', ['flts'], (16777217,), pre=dict(RM=mode),
				top=[f2w(r)], st=dict(IR=ix))
		r, ix = sgl(Fraction(1, 10), mode)
		case(f'cdsf 0.1 rm{mode}', ['cdsf'], d2w(0.1), pre=dict(RM=mode), top=[f2w(sgl(Fraction(0.1), mode)[0])],
				st=dict(IR=True))
	case('fadd exact keeps IR', ['fadd'], (*d2w(1.0), *d2w(2.0)), pre=dict(IR=1),
			top=[d2w(3.0)[1], d2w(3.0)[0]], st=dict(IR=1))
	# Negation is subtraction from +0 (sheet 86): -(+0) is +0.
	case('fneg +0', ['fneg'], d2w(0.0), top=[0, 0], st=dict(CC=CCE))
	case('fneg -0', ['fneg'], d2w(-0.0), top=[0, 0], st=dict(CC=CCE))
	case('fneg 1.5', ['fneg'], d2w(1.5), top=[d2w(-1.5)[1], d2w(-1.5)[0]], st=dict(CC=CCL))
	case('fngs +0', ['fngs'], (0,), top=[0], st=dict(CC=CCE))
	case('ftst -0', ['ftst'], d2w(-0.0), st=dict(CC=CCE))
	case('ftst,p -2', ['ftst,p'], d2w(-2.0), top=[0, 0], st=dict(CC=CCL))
	case('ftss -0', ['ftss'], (0x80000000,), st=dict(CC=CCE))
	case('fcmp -0:+0', ['fcmp'], (*d2w(-0.0), *d2w(0.0)), st=dict(CC=CCE))
	case('fcmp 1:2', ['fcmp'], (*d2w(1.0), *d2w(2.0)), top=[d2w(1.0)[1], d2w(1.0)[0]], st=dict(CC=CCL))
	case('fcmp,t 2:1', ['fcmp,t'], (*d2w(2.0), *d2w(1.0)), top=[0, 0, 0], st=dict(CC=CCG))
	case('fcps 1:-1', ['fcps'], (f2w(1.0), f2w(-1.0)), top=[f2w(1.0)], st=dict(CC=CCG))
	# Fix: truncate, or round by adding 0.5 to the magnitude (sheet 98).
	for v in (2.5, -2.5, 2.4999, 0.5, -0.5, 1e9, -2147483648.0, 2147483647.5, 2147483648.0, -2147483649.0):
		t = int(v)
		rr = int(abs(v) + 0.5) * (1 if v >= 0 else -1)
		for rnd, want in ((False, t), (True, rr)):
			ov = not (-2 ** 31 <= want < 2 ** 31)
			op = 'fixd,r' if rnd else 'fixd'
			case(f'{op} {v!r}', [op], d2w(v), top=[None if ov else u(want)], st=dict(O=ov))
	case('fixd keeps O', ['fixd'], d2w(3.0), pre=dict(O=1), top=[3], st=dict(O=1))
	case('fixs,r -2.5', ['fixs,r'], (f2w(-2.5),), top=[u(-3)])
	case('fltd INT_MIN', ['fltd'], (0x80000000,), top=[d2w(-2147483648.0)[1], d2w(-2147483648.0)[0]], st=dict(CC=CCL))
	case('csdf 1.5', ['csdf'], (f2w(1.5),), top=[d2w(1.5)[1], d2w(1.5)[0]], st=dict(CC=CCG))
	# Traps.
	inf, nan = float('inf'), float('nan')
	case('fadd inf traps', ['fadd'], (*d2w(inf), *d2w(1.0)), sig=True)
	case('fadd nan traps', ['fadd'], (*d2w(nan), *d2w(1.0)), sig=True)
	case('fadd denormal traps', ['fadd'], (*d2w(5e-324), *d2w(1.0)), sig=True)
	case('fdiv by 0 traps', ['fdiv'], (*d2w(1.0), *d2w(0.0)), sig=True)
	case('fmpy overflow traps', ['fmpy'], (*d2w(1e308), *d2w(10.0)), sig=True)
	case('fmpy underflow traps', ['fmpy'], (*d2w(1e-308), *d2w(1e-10)), sig=True)
	case('cdsf too big traps', ['cdsf'], d2w(1e300), sig=True)
	case('fads inf traps', ['fads'], (f2w(inf), f2w(1.0)), sig=True)


# ---- byte strings ---------------------------------------------------------------
# ERS sheets 101-106. The operands are pointers into m, built as m + k.
# Moves are byte by byte where they overlap unaligned; the count left on
# the stack is (S) - D*X.

def mbytes(words):
	return bytearray(b''.join(struct.pack('>I', w) for w in words))


def mwords(b):
	return [struct.unpack('>I', bytes(b[i:i + 4]))[0] for i in range(0, 16, 4)]


def ptr(k):
	return ['lds Q-63'] + ([f'addi {k}'] if k else [])


def string_cases():
	w = [0x01020304, 0x05060708, 0x090a0b0c, 0x0d0e0f10]
	def mvb(name, src, dst, n, back=False):
		b = mbytes(w)
		if n > 0:
			d = -1 if back else 1
			for i in range(n):
				b[dst + d * i] = b[src + d * i]
		code = ptr(src) + ptr(dst) + [f'ldi {n}' if n >= 0 else f'lni {-n}',
				'mvb 0,b' if back else 'mvb 0']
		case(name, code, mem=w, memx=mwords(b), top=[0 if n > 0 else u(n)])
	mvb('mvb 4 fwd', 0, 8, 4)
	mvb('mvb overlap fwd +1', 0, 1, 5)
	mvb('mvb 4 back', 3, 11, 4, back=True)
	mvb('mvb overlap back +1', 4, 5, 5, back=True)
	mvb('mvb count 0', 0, 8, 0)
	mvb('mvb count -3', 0, 8, -3)
	for name, src, dst, n, cc, left in (('cmpb equal', 0, 0, 4, CCE, 0),
			('cmpb count 0', 0, 8, 0, CCE, 0)):
		case(name, ptr(src) + ptr(dst) + [f'ldi {n}', 'cmpb 0'], mem=w, top=[left], st=dict(CC=cc))
	# Source byte 3 (04) against target byte 11 (0c): source below.
	case('cmpb less', ptr(0) + ptr(8) + ['ldi 4', 'cmpb 0'], mem=w, top=[4], st=dict(CC=CCL))
	case('cmpb greater', ptr(8) + ptr(0) + ['ldi 4', 'cmpb 0'], mem=w, top=[4], st=dict(CC=CCG))
	w2 = [0x41424344, 0x41424345, 0, 0]
	case('cmpb differ at 3', ptr(0) + ptr(4) + ['ldi 4', 'cmpb 0'], mem=w2, top=[1], st=dict(CC=CCL))
	for n in (6, 0, -2):
		b = mbytes(w)
		for i in range(max(n, 0)):
			b[4 + i] = 0xab
		code = ['ldi 171'] + ptr(4) + [f'ldi {n}' if n >= 0 else f'lni {-n}', 'filb 0']
		case(f'filb {n}', code, mem=w, memx=mwords(b), top=[0 if n > 0 else u(n)])


# ---- condition-code tests, range check, bits, rounding mode ------------------------

def misc2_cases():
	for cc, name in ((CCG, 'G'), (CCL, 'L'), (CCE, 'E')):
		for f in range(8):
			want = ((f & 4) and cc == CCG) or ((f & 2) and cc == CCE) or ((f & 1) and cc == CCL)
			case(f'tcc {f} cc={name}', [f'tcc {f}'], pre=dict(CC=cc), top=[1 if want else 0], st=dict(CC=cc))
	for v, lo, hi in ((5, 1, 9), (0, 1, 9), (10, 1, 9), (u(-5), u(-9), u(-1)), (u(-10), u(-9), u(-1))):
		inside = s(lo) < s(v) < s(hi)
		case(f'crb {s(v)} in {s(lo)}..{s(hi)}', br('crb'), (v, lo, hi), top=[1 if inside else 2, v])
	w = [0x80000001, 0x40c0ffee, 0, 0]
	for k in (0, 1, 7, 31, 32, 33, 63):
		bit = (w[k // 32] >> (31 - k % 32)) & 1
		case(f'lbit {k}', ptr(0) + [f'ldi {k}', 'lbit'], mem=w, top=[bit])
	for k, v in ((0, 0), (1, 1), (33, 1), (62, 1)):
		x = list(w)
		word = k // 32
		m = 1 << (31 - k % 32)
		x[word] = (x[word] | m) if v else (x[word] & ~m)
		case(f'sbit {k}={v}', ptr(0) + [f'ldi {k}', f'ldi {v}', 'sbit'], mem=w, memx=x)
	for rm in range(4):
		case(f'srm {rm}', [f'srm {rm}'], st=dict(RM=rm))
	case('div INT64_MIN/-1', ['div'], (0x80000000, 0, u(-1)), st=dict(O=1))
	inf = float('inf')
	case('fcmp inf traps', ['fcmp'], (*d2w(inf), *d2w(1.0)), sig=True)
	case('ftst nan traps', ['ftst'], d2w(float('nan')), sig=True)
	case('fixd inf traps', ['fixd'], d2w(inf), sig=True)
	case('csdf inf traps', ['csdf'], (f2w(inf),), sig=True)
	case('fneg nan traps', ['fneg'], d2w(float('nan')), sig=True)


GROUPS = [add_cases, mul_cases, div_cases, imm_cases, cmp_cases, x_cases, mem_cases,
		ld_cases, shift_cases, field_cases, logic_stack_cases, misc_cases, branch_cases,
		bcd_cases, fp_cases, string_cases, misc2_cases]


# ---------------------------------------------------------------------------
#  output
# ---------------------------------------------------------------------------

PER_FILE = 150
ARG = {'and': 'Q-35', 'or': 'Q-39', 'x': 'Q-43'}
OPND = ['Q-47', 'Q-51', 'Q-55', 'Q-59']


def routine(n, code, nargs):
	out = [f'\tent\t_t{n}', f'_t{n}:\tproc', '\tldx\t~L.1', '\tpshn',
			f'\tlds\t{ARG["x"]}', '\tstax,p', '\tzero', '\tzero', '\tzero']
	out += [f'\tlds\t{OPND[i]}' for i in range(nargs)]
	out += ['\tpshr\t4', f'\tlds\t{ARG["and"]}', '\tand', f'\tlds\t{ARG["or"]}', '\tor', '\tsetr\t4']
	for line in code:
		out.append(line if '\t' in line else '\t' + line.replace(' ', '\t', 1))
	out += ['\tpshr\t4']
	for k in range(4):
		out += [f'\tldxi\t{4 * k}', '\tsts\tQ-31,i,x']
	out += ['\texit\t0', '~L.1:\tword\t0']
	return out


def main():
	for g in GROUPS:
		g()
	routines = {}
	lines = []
	for c in CASES:
		key = (c['code'], len(c['args']))
		if key not in routines:
			routines[key] = len(routines)
		fn = routines[key]
		sand, sor = status_preset(c['pre'])
		assert sand == SAND
		args = list(c['args']) + [0] * (4 - len(c['args']))
		smask, sval = status_check(c['st'])
		tmask, tw = 0, []
		for i in range(3):
			v = c['top'][i] if i < len(c['top']) else None
			if v is not None:
				tmask |= 1 << i
			tw.append(0 if v is None else u(v))
		memchk = int(c['memx'] is not None)
		memx = c['memx'] or [0] * 4
		nums = [fn, int(c['sig']), sor, u(c['x']), *map(u, args), smask, sval, tmask, *tw,
				*map(u, c['mem']), memchk, *map(u, memx)]
		name = c['name'].replace(' ', '_')
		line = ' '.join(f'{v:x}' for v in nums) + ' ' + name
		assert len(line) < 230, line
		lines.append(line)
	# as keeps a fixed-size table of procedures per file ("proctable
	# overflow" somewhere under 300), so the routines are spread out.
	for old in HERE.glob('isaops*.s'):
		old.unlink()
	items = list(routines.items())
	names = []
	for k in range(0, len(items), PER_FILE):
		asm = ['\tddata', '\tidata', '\ttext']
		for (code, nargs), n in items[k:k + PER_FILE]:
			asm += routine(n, code, nargs)
		asm += ['\tidata', '\ttext']
		name = f'isaops{k // PER_FILE}.s'
		(HERE / name).write_text('\n'.join(asm) + '\n')
		names.append(name)
	(HERE / 'isa.files').write_text(' '.join(['isa.c'] + names) + '\n')
	(HERE / 'isa.dat').write_text('\n'.join(lines) + '\n')
	decl = ', '.join(f't{n}()' for n in range(len(routines)))
	table = ', '.join(f't{n}' for n in range(len(routines)))
	csrc = C_DRIVER.replace('@SAND@', f'0x{SAND:x}L').replace('@DECL@', wrap(decl)).replace('@TABLE@', wrap(table))
	(HERE / 'isa.c').write_text(csrc)
	print(f'{len(CASES)} cases, {len(routines)} routines')


def wrap(s, width=70):
	out, line = [], ''
	for part in s.split(', '):
		if len(line) + len(part) > width:
			out.append(line + ',')
			line = ''
		line += (' ' if line else '') + part if not line else ', ' + part
	out.append(line)
	return '\n\t'.join(x.lstrip(', ') for x in out)


C_DRIVER = r'''/*
 * Instruction-set edge cases, generated by tests/hpux/gen_isa.py from the
 * ERS: reads isa.dat, runs each case, prints every mismatch and a summary.
 */
#include <stdio.h>
#include <signal.h>
#include <setjmp.h>

long @DECL@;
long (*fn[])() = {
	@TABLE@
};

#define SAND @SAND@
#define NV 23

jmp_buf jb;
int gotsig;

catch(n)
int n;
{
	gotsig = n;
	longjmp(jb, 1);
}

arm()
{
	signal(SIGILL, catch);
	signal(SIGTRAP, catch);
	signal(SIGIOT, catch);
	signal(SIGEMT, catch);
	signal(SIGFPE, catch);
	signal(SIGBUS, catch);
	signal(SIGSEGV, catch);
	signal(SIGSYS, catch);
}

long v[NV], r[4], m[4];
char name[128];

main()
{
	FILE *f;
	int i, bad, pass, fail;

	f = fopen("isa.dat", "r");
	if (f == NULL) {
		printf("no isa.dat\n");
		exit(1);
	}
	pass = fail = 0;
	for (;;) {
		for (i = 0; i < NV; i++)
			if (fscanf(f, "%lx", &v[i]) != 1)
				goto done;
		if (fscanf(f, "%s", name) != 1)
			goto done;
		for (i = 0; i < 4; i++) {
			m[i] = v[14 + i];
			r[i] = 0x5a5a5a5aL;
		}
		gotsig = 0;
		arm();
		if (setjmp(jb) == 0)
			(*fn[v[0]])(r, SAND, v[2], v[3], v[4], v[5], v[6], v[7], m);
		bad = 0;
		if (v[1]) {
			if (!gotsig)
				bad = 1;
		} else {
			if (gotsig)
				bad = 1;
			if ((r[0] & v[8]) != v[9])
				bad = 1;
			for (i = 0; i < 3; i++)
				if ((v[10] >> i & 1) && r[1 + i] != v[11 + i])
					bad = 1;
			for (i = 0; i < 4; i++)
				if (v[18] && m[i] != v[19 + i])
					bad = 1;
		}
		if (!bad) {
			pass++;
			continue;
		}
		fail++;
		printf("FAIL %s: sig %d/%ld st %lx want %lx/%lx", name, gotsig, v[1],
				r[0] & v[8], v[9], v[8]);
		printf(" top %lx %lx %lx want(%lx) %lx %lx %lx", r[1], r[2], r[3],
				v[10], v[11], v[12], v[13]);
		if (v[18])
			printf(" mem %lx %lx %lx %lx want %lx %lx %lx %lx", m[0], m[1],
					m[2], m[3], v[19], v[20], v[21], v[22]);
		printf("\n");
	}
done:
	printf("isatest %d passed %d failed\n", pass, fail);
	exit(0);
}
'''


if __name__ == '__main__':
	main()
