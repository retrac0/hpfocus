#!/usr/bin/env python3
"""HP-UX regression tests: the matrix the alpha was released against.

    src/test/hpuxtest.py matrix [--only a,b] [--skip a,b] [--jobs N] [--list]
    src/test/hpuxtest.py stress [--runs N] [--jobs N]
    src/test/hpuxtest.py soak   [--ram MB] [--poll US]

matrix  every case below, PASS or FAIL each: about ten minutes on a quiet
        8-core machine (measured: 9 min 13 s at --jobs 2)
stress  seven simultaneous logins on 2 MB, over a grid of login schedules
        and IOP poll periods (the case that found the swap corruption)
soak    a multi-user workload -- compile and run a C program, copy and cmp
        binaries, checksum a large file, find/ls -lR/sort on two ports at
        once -- then a reboot of the disc it wrote, and every check again

Every case boots its OWN machine, made from the pristine one by
tools/runhpux.sh --state (var/test/<command>.<pid>/<case>); nothing of yours
is touched, and the machines are deleted afterwards unless --keep. Needs
what tools/setup.sh makes: mame/focus, the pristine HP-UX machine, and for
the 520 cases software/hd/basic.img, software/hd/diag.img and the 5.01
kernel (built on demand).
HPFOCUS_BIN and HPFOCUS_VAR name another build and another var/, as they do
for the run scripts.

Matrix cases:
  boot-keep, boot-fresh     200 s boots, primed / no battery: seven login
                            banners (the console and six multiplexer ports)
  sess-console, sess-mux0, sess-mux3, sess-mux5
                            a root session: who, ls -l /, ps -ef, date
  seven, seven-s1..s5       seven staggered logins at once, the schedule
                            shifted by 0..5 s (memory pressure on 2 MB)
  seven-p350..p450          the same at other IOP poll periods
  busy                      four logins with three background jobs each: 44
                            processes on 2 MB, so stacks and data segments
                            are swapped out under tasks that are still live
  long600                   two sessions early, commands at 560-585 s
  ram3, ram4                a console session with 3 / 4 MB fitted
  poll300..poll700          boot and a console session at other poll periods
  second-boot               a session on the disc a full earlier run wrote
  basic-mul, basic-prog     BASIC on the 520's fixed disc: DISP 3*4, a loop
  diag                      HP's 9020 diagnostics: LOAD "TEST" and RUN typed
                            at four different moments, each on a boot of its
                            own, must each reach the Test Supervisor
  ux501                     HP-UX 5.01 on the 520 reaches its banner

Exit status 0 only if everything that ran passed.
"""
import argparse
import collections
import concurrent.futures as cf
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))
RUNHPUX = os.path.join(ROOT, 'tools', 'runhpux.sh')
RUNBASIC = os.path.join(ROOT, 'tools', 'runbasic-hd.sh')
SCREEN = os.path.join(ROOT, 'tools', 'screen.py')
BASICIMG = os.path.join(ROOT, 'software', 'hd', 'basic.img')
DIAGIMG = os.path.join(ROOT, 'software', 'hd', 'diag.img')
DIAGFLOP = os.path.join(ROOT, 'software', 'test', '500MFTST.TD0')
TAPE501 = os.path.join(ROOT, 'software', '97089C.hpi')
KERNEL501 = os.path.join(ROOT, 'software', 'cct', 'hpux501.bin')

# The 520's software is Hewlett-Packard's and comes from the HP Computer
# Museum by hand (MEDIA.txt). A case that needs it and does not have it is
# SKIPPED, which is neither a pass nor a failure.
MUSEUM = ("the 520's software is not here: tools/fetch-media.sh --list, then "
          "tools/fetch-media.sh --from <what you downloaded> and tools/setup.sh --no-build")


def skipped(name, what):
    return {'case': name, 'ok': True, 'skip': True,
            'why': 'no %s -- %s' % (os.path.relpath(what, ROOT), MUSEUM)}
BANNER = "HP-UX 5.30 (unknown)login: "
RATE = 0.06                       # FOCUS_TYPERATE=60: seconds per keystroke


# ------------------------------------------------------------- the scripts
def session(term):
    return ('@%s=root\\n;@%s=\\n;@%s=\\n;@%s=who\\n;@%s=ls -l /\\n;@%s=ps -ef\\n;@%s=date\\n'
            % ((term,) * 7))


def times(t):
    return [t, t + 12, t + 22, t + 32, t + 42, t + 54, t + 70]


def quick(term):
    return '@%s=root\\n;@%s=\\n;@%s=\\n;@%s=who\\n;@%s=date\\n' % ((term,) * 5)


def qtimes(t):
    return [t, t + 40, t + 70, t + 100, t + 120]


TERMS7 = ['console'] + ['2:%d' % i for i in range(6)]


def seven(offset=0.0, **kw):
    return dict(secs=280, nv='keep', tap='all',
                type=';'.join(quick(t) for t in TERMS7),
                at=[round(x, 1) for x in sum((qtimes(90 + offset + i) for i in range(7)), [])],
                who_all=TERMS7, **kw)


def busy():
    """Four logins, three background jobs each -- find | sort, ls -lR, od,
    cat | sum -- which is about 44 processes on 2 MB. The memory manager
    swaps out stacks and global data segments their tasks have stored into
    only through the registers; a build that loses those halts with
    TRAP:00000012 some 35 s after the jobs start (the exact script matters:
    the same jobs without their closing echo happen to survive). Every
    terminal is asked a question 90 s into the load, and the console's
    answer is the one checked."""
    jobs = ("ls -lR /usr > /tmp/l%s 2>&1 && echo L%s''DONE &",
            "find / -print 2>/dev/null | sort > /tmp/f%s && echo F%s''DONE &",
            "od -x /bin/csh | tail -1 > /tmp/o%s && echo O%s''DONE &",
            "cat /bin/* 2>/dev/null | sum > /tmp/s%s && echo S%s''DONE &")
    k = Script()
    terms = TERMS7[:4]
    tags = ['c'] + ['m%d' % i for i in range(3)]
    for n, term in enumerate(terms):
        k.login(term, 90.0 + n)
    for n, term in enumerate(terms):
        for j in range(3):
            tag = '%s%d' % (tags[n], j)
            k.type(term, jobs[(n + j) % 4] % (tag, tag), 125.0 + n, run=3)
        k.type(term, 'ps -ef | wc -l', None, run=5)
    for n, term in enumerate(terms):
        k.type(term, "who | wc -l && echo LA''TE%s" % tags[n], 235.0 + n, run=5)
    # The clock is pinned: the run is then the same run every time, on any
    # host and day, which is what lets a broken build fail it reliably.
    return k.spec(46, nv='keep', tap='console', want=['LATEc'],
                  env={'FOCUS_RTCTIME': '2026-10-01T12:00:00'})


def cases():
    c = collections.OrderedDict()
    c['boot-keep'] = dict(secs=200, nv='keep', tap='all', expect_banners=7)
    c['boot-fresh'] = dict(secs=200, nv='fresh', tap='all', expect_banners=7)
    for term, nv, t0 in (('console', 'keep', 90), ('2:0', 'fresh', 90), ('2:3', 'keep', 95),
                         ('2:5', 'fresh', 137)):
        c['sess-' + term.replace('2:', 'mux')] = dict(
            secs=t0 + 85, nv=nv, tap=term, type=session(term), at=times(t0), sessions=[term])
    c['seven'] = seven()
    for shift in (1, 2, 3, 4, 5):
        c['seven-s%d' % shift] = seven(shift)
    for p in (350, 380, 420, 450):
        c['seven-p%d' % p] = seven(env={'FOCUS_IOPPOLL': str(p)})
    c['busy'] = busy()
    c['long600'] = dict(secs=600, nv='keep', tap='console,2:3',
                        type=session('console') + ';' + session('2:3')
                        + ';@console=date\\n;@2:3=date\\n;@console=who\\n;@2:3=ps -ef\\n',
                        at=times(100) + times(300) + [560, 570, 580, 585],
                        sessions=['console', '2:3'], late_dates=2)
    for name, mb in (('ram3', 3), ('ram4', 4)):
        c[name] = dict(secs=175, nv='keep', tap='console', ram=mb,
                       type=session('console'), at=times(90), sessions=['console'])
    for p in (300, 450, 500, 600, 700):
        c['poll%d' % p] = dict(secs=175, nv='fresh' if p % 200 else 'keep', tap='console',
                               env={'FOCUS_IOPPOLL': str(p)}, type=session('console'),
                               at=times(90), sessions=['console'])
    c['second-boot'] = dict(secs=175, nv='keep', tap='2:1', type=session('2:1'), at=times(90),
                            sessions=['2:1'], prior=dict(secs=150, nv='fresh', tap='all'))
    c['basic-mul'] = dict(kind='basic', keys='DISP 3*4\\n', want=['12'])
    # Program lines are stored with ENTER (\\r), not EXECUTE (\\n), and a
    # program needs its END; one script per line, because BASIC's keyboard
    # buffer does not hold a line typed while it is taking the one before.
    c['basic-prog'] = dict(kind='basic',
                           keys='10 FOR I=1 TO 3\\r;20 PRINT I*I\\r;30 NEXT I\\r;40 END\\r;RUN\\n',
                           at='20,26,32,38,44', stop=56, want=['1', '4', '9'])
    # HP's test driver on the diagnostics disc. A build that halted here
    # (System halted ... TRAP:00000055 or 00000020, after a disc read lost
    # during the LOAD) did it at some typing times and not at others: when
    # the commands are typed sets the phase between the instruction stream
    # and the I/O processor's poll. With no timing tried at all, the fault
    # went unseen through three rounds of validation. These four are ones
    # it halted at; the first pair is docs/DIAGNOSTICS.md's own recipe.
    c['diag'] = dict(kind='diag', at=['20,36', '26,48', '28,52', '30,60'],
                     want='Welcome to the HP 9000 Test Supervisor')
    c['ux501'] = dict(kind='ux501', want=['Hewlett-Packard Company'])
    return c


# ------------------------------------------------------------- running
def run(cmd, env, log, cwd=ROOT, timeout=None):
    e = dict(os.environ)
    for k in ('FOCUS_TYPE', 'FOCUS_TYPEAT', 'FOCUS_TAP', 'FOCUS_IOPPOLL', 'FOCUS_SCMNV'):
        e.pop(k, None)
    e.update(env)
    e['SDL_VIDEODRIVER'] = 'dummy'
    with open(log, 'wb') as f:
        try:
            return subprocess.run(cmd, cwd=cwd, env=e, stdout=f, stderr=subprocess.STDOUT,
                                  stdin=subprocess.DEVNULL, timeout=timeout).returncode
        except subprocess.TimeoutExpired:
            return 124


def boot(state, spec, log):
    """One multiuser boot of the machine in `state` (made from the pristine
    machine if it is not one yet), through tools/runhpux.sh."""
    os.makedirs(state, exist_ok=True)
    env = {'FOCUS_TAP': spec.get('tap', 'all'), 'FOCUS_TAPRAW': '1', 'FOCUS_TYPERATE': '60'}
    env.update(spec.get('env', {}))
    if spec.get('type'):
        env['FOCUS_TYPE'] = spec['type']
        env['FOCUS_TYPEAT'] = ','.join(str(t) for t in spec['at'])
    if spec.get('nv') == 'fresh':
        env['FOCUS_SCMNV'] = os.path.join(state, 'no-battery.nv')
        try:
            os.remove(env['FOCUS_SCMNV'])
        except FileNotFoundError:
            pass
    cmd = [RUNHPUX, '--multiuser', '--state', state, '--ports', 'none',
           '--seconds', str(int(spec['secs'])), '--ram', str(spec.get('ram', 2))]
    return run(cmd, env, log, timeout=int(spec['secs']) * 10 + 300)


def text(log):
    return open(log, 'rb').read().decode('latin1')


def banners_after_rc(t):
    """Login banners printed after /etc/rc, robust to terminals interleaving."""
    i = t.find('Nodename: unknown.')
    if i < 0:
        return 0
    seg = t[i + len('Nodename: unknown.'):]
    j = seg.find('[')
    seg = seg[:j] if j >= 0 else seg
    c = collections.Counter(seg.replace('\r', '').replace('\n', '').replace('.', ''))
    b = collections.Counter(BANNER.replace('.', ''))
    return min(c[ch] // b[ch] for ch in b)


def dev(term):
    return 'console' if term == 'console' else 'tty0%d' % (int(term.split(':')[1]) + 1)


BAD = (('halted', r'System halted|panic|TRAP:'), ('memfault', r'(?i)memory fault'),
       ('cannot_open', r'cannot open'), ('core', r'core dumped'))


def score(name, spec, t, rc):
    r = {'case': name, 'rc': rc}
    fails = []
    if rc != 0:
        fails.append('exit %d' % rc)
    if 'Nodename: unknown.' not in t:
        fails.append('/etc/rc never finished')
    for k, pat in BAD:
        n = len(re.findall(pat, t))
        if n:
            fails.append('%s=%d' % (k, n))
    if 'expect_banners' in spec:
        n = banners_after_rc(t)
        if n < spec['expect_banners']:
            fails.append('banners %d/%d' % (n, spec['expect_banners']))
    who = set(re.findall(r'root +(tty0\d|console) +\w{3} +\d+ \d\d:\d\d', t))
    dates = re.findall(r'\w{3} \w{3} +\d+ \d\d:\d\d:\d\d \w{3} \d{4}', t)
    for term in spec.get('sessions', []):
        if dev(term) not in who:
            fails.append('no session on %s' % dev(term))
    if spec.get('sessions'):
        if 'unix' not in t and 'new.kernel' not in t:
            fails.append('ls -l / output missing')
        if 'init' not in t:
            fails.append('ps -ef output missing')
    if spec.get('who_all'):
        missing = {dev(x) for x in spec['who_all']} - who
        if len(missing) > 1:          # one late login lost to typing timing is tolerated
            fails.append('logins missing: %s' % ','.join(sorted(missing)))
    if spec.get('late_dates') and len(dates) < 2 + spec['late_dates']:
        fails.append('late commands unanswered (dates=%d)' % len(dates))
    for w in spec.get('want', []):
        if w not in t:
            fails.append('%s never printed' % w)
    r['ok'] = not fails
    r['why'] = '; '.join(fails)
    return r


def run_hpux(name, spec, out, keep):
    state = os.path.join(out, name)
    if spec.get('prior'):
        boot(state, spec['prior'], os.path.join(out, name + '.prior.log'))
    log = os.path.join(out, name + '.log')
    rc = boot(state, spec, log)
    r = score(name, spec, text(log), rc)
    if not keep:
        shutil.rmtree(state, ignore_errors=True)
    return r


def screen_of(log):
    with open(log, 'rb') as f:
        p = subprocess.run([sys.executable, SCREEN], stdin=f, stdout=subprocess.PIPE,
                           stderr=subprocess.DEVNULL)
    return p.stdout.decode('latin1')


def run_diag(name, spec, out, keep):
    """LOAD "TEST" then RUN on the diagnostics disc, once per typing time,
    each on its own boot with the test disc in the flexible drive. The
    driver's first screen is up within 30 s of RUN."""
    for f in (DIAGIMG, DIAGFLOP):
        if not os.path.exists(f):
            return skipped(name, f)
    fails = []
    for at in spec['at']:
        log = os.path.join(out, '%s-%s.log' % (name, at.replace(',', '-')))
        env = {'FOCUS_HDNOSAVE': '1', 'FOCUS_DIMDUMP': '60',
               'FOCUS_TYPE': 'LOAD "TEST"\\n;RUN\\n', 'FOCUS_TYPEAT': at}
        stop = int(at.split(',')[-1]) + 30
        rc = run([RUNBASIC, DIAGIMG, '--', '-mpb3', '1m', '-mpb4', '1m', '-flop1', DIAGFLOP,
                  '-str', str(stop), '-video', 'none'], env, log, timeout=900)
        scr = screen_of(log)
        if rc != 0:
            fails.append('%s: exit %d' % (at, rc))
        elif spec['want'] not in scr:
            m = re.search(r'TRAP:[0-9A-F]+', scr)
            last = [l.strip() for l in scr.splitlines() if l.strip()]
            fails.append('%s: %s' % (at, 'halted, ' + m.group(0) if m else
                                     'no Test Supervisor (screen ends %r)' % (last[-1:] or [''])[0]))
    return {'case': name, 'ok': not fails, 'why': '; '.join(fails)}


def run_520(name, spec, out, keep):
    if spec['kind'] == 'diag':
        return run_diag(name, spec, out, keep)
    log = os.path.join(out, name + '.log')
    if spec['kind'] == 'basic':
        if not os.path.exists(BASICIMG):
            return skipped(name, BASICIMG)
        env = {'FOCUS_HDNOSAVE': '1', 'FOCUS_DIMDUMP': '1', 'FOCUS_TYPE': spec['keys'],
               'FOCUS_TYPEAT': spec.get('at', '20')}
        rc = run([RUNBASIC, BASICIMG, '--', '-str', str(spec.get('stop', 30)), '-video', 'none'],
                 env, log, timeout=600)
    else:
        if not os.path.exists(KERNEL501) and not os.path.exists(TAPE501):
            return skipped(name, TAPE501)
        env = {'FOCUS_KBDNV': os.path.join(out, name + '-kbd.nv')}
        rc = run([RUNHPUX, '--release', '5.01', '--headless', '--seconds', '100'], env, log,
                 timeout=1200)
    scr = screen_of(log)
    lines = [l.strip() for l in scr.splitlines()]
    missing = [w for w in spec['want'] if not any(w == l or w in l and len(w) > 3 for l in lines)]
    fails = []
    if rc != 0:
        fails.append('exit %d' % rc)
    if missing:
        fails.append('screen lacks %s' % ', '.join(repr(m) for m in missing))
    return {'case': name, 'ok': not fails, 'why': '; '.join(fails)}


def report(results, out):
    ran = [r for r in results if not r.get('skip')]
    npass = sum(r['ok'] for r in ran)
    nskip = len(results) - len(ran)
    print('%d/%d passed%s  (logs: %s)' % (npass, len(ran),
          ', %d skipped' % nskip if nskip else '', out))
    return 0 if npass == len(ran) else 1


def parallel(jobs, tasks):
    results = []
    with cf.ThreadPoolExecutor(jobs) as ex:
        futs = {ex.submit(fn, *a): n for n, fn, a in tasks}
        for f in cf.as_completed(futs):
            try:
                r = f.result()
            except Exception as e:                 # a broken case is a failed case
                r = {'case': futs[f], 'ok': False, 'why': 'harness error: %r' % e}
            results.append(r)
            print('%-14s %s %s' % (r['case'], 'SKIP' if r.get('skip') else 'PASS' if r['ok'] else 'FAIL',
                                   r.get('why', '')),
                  flush=True)
    return results


def workdir(kind, given):
    var = os.environ.get('HPFOCUS_VAR') or os.path.join(ROOT, 'var')
    out = os.path.abspath(given or os.path.join(var, 'test', '%s.%d' % (kind, os.getpid())))
    os.makedirs(out, exist_ok=True)
    return out


# ------------------------------------------------------------- matrix
def cmd_matrix(a):
    cs = cases()
    if a.list:
        for k, v in cs.items():
            print(k)
        return 0
    names = a.only.split(',') if a.only else list(cs)
    if a.skip:
        names = [n for n in names if n not in a.skip.split(',')]
    for n in names:
        if n not in cs:
            sys.exit('unknown case %s (--list)' % n)
    out = workdir('matrix', a.out)
    tasks = []
    for n in names:
        s = cs[n]
        fn = run_520 if s.get('kind') else run_hpux
        tasks.append((n, fn, (n, s, out, a.keep)))
    results = parallel(a.jobs, tasks)
    results.sort(key=lambda r: names.index(r['case']))
    return report(results, out)


# ------------------------------------------------------------- stress
def cmd_stress(a):
    out = workdir('stress', a.out)
    tasks = []
    for k in range(a.runs):
        off = 0.5 * k
        poll = (400, 350, 450, 500, 300)[k % 5]
        spec = seven(off, env={'FOCUS_IOPPOLL': str(poll)})
        spec['nv'] = ('keep', 'fresh')[k % 2]
        name = 's7-%02d-o%.1f-p%d' % (k, off, poll)
        tasks.append((name, run_hpux, (name, spec, out, a.keep)))
    return report(parallel(a.jobs, tasks), out)


# ------------------------------------------------------------- soak
SOAK_EXPECT = 'H=3.597739657144 P=181004'
# '@' stands for ';' (FOCUS_TYPE splits scripts on ';'); tr puts them back.
# \012 is the newline: FOCUS_TYPE turns a typed backslash-n into Return.
SOAK_PROG = ('main(){int i@double s=0.0@long p=1@'
             'for(i=1@i<=20@i++){s+=1.0/i@p=(p*31+i)%1000003@}'
             'printf("H=%.12f P=%ld\\012",s,p)@}')


class Script:
    """FOCUS_TYPE scripts with times: each line waits for the one before it
    on the same terminal to be typed, plus a margin for it to run."""
    def __init__(self):
        self.s, self.t, self.free = [], [], {}

    def type(self, term, line, at=None, run=4.0):
        when = max(at or 0, self.free.get(term, 0))
        self.s.append('@%s=%s\\n' % (term, line))
        self.t.append(round(when, 1))
        self.free[term] = when + RATE * (len(line) + 1) + run

    def login(self, term, at):
        self.type(term, 'root', at, 10)
        self.type(term, '', None, 8)
        self.type(term, '', None, 8)

    def spec(self, margin, **kw):
        return dict(type=';'.join(self.s), at=self.t, secs=int(max(self.free.values()) + margin), **kw)


def mark(word):
    """Printed, but never echoed as typed: echo CC''OK prints CCOK."""
    return "echo %s''%s" % (word[:2], word[2:])


def soak_boot1(**kw):
    k = Script()
    for n, term in enumerate(('console', '2:0', '2:1')):
        k.login(term, 90 + n)
    c = 'console'
    k.type(c, 'mkdir /usr/soak', 130)
    k.type(c, 'cd /usr/soak')
    k.type(c, "tr @ '\\073' > p.c <<EOF")
    k.type(c, SOAK_PROG, run=2)
    k.type(c, 'EOF')
    k.type(c, 'cc -o p p.c && ' + mark('CCOK'), run=60)
    k.type(c, './p', run=6)
    k.type(c, 'cp /bin/sh x1 && cmp /bin/sh x1 && ' + mark('CMP1OK'), run=10)
    k.type(c, 'cp /bin/csh x2 && cmp /bin/csh x2 && ' + mark('CMP2OK'), run=10)
    k.type(c, 'cat /bin/* > big 2>/dev/null', run=60)
    k.type(c, 'sum big > big.sum && cat big.sum', run=30)
    k.type(c, 'sync && sync && ' + mark('SYNCOK'), run=20)
    k.type('2:0', 'find / -print | wc -l', 160, run=90)
    k.type('2:0', 'ps -ef | wc -l && ' + mark('PSOK'), run=8)
    k.type('2:1', 'ls -lR /usr > /tmp/lsr 2>/dev/null', 165, run=90)
    # Not `sort | wc -l`: a pipeline's status is its last command's, and a
    # sort that dies leaves wc to print 0 and succeed. /tmp/lsr is 400 KB,
    # which takes sort's data past 512 KB and into a second paged segment.
    k.type('2:1', 'sort /tmp/lsr > /tmp/srt && sort -c /tmp/srt && ' + mark('SORTOK'), run=40)
    # The margin is for the console: with find and ls -lR running on the
    # ports, its cp and cmp of /bin/csh alone take over 100 s, more on 3 MB
    # than on 2 (measured: every check passes on both given 800 s, and the
    # 3 MB run missed its sum and sync with the 60 s this used to allow).
    return k.spec(200, tap='console,2:0,2:1', **kw)


def soak_boot2(**kw):
    k = Script()
    k.login('console', 90)
    c = 'console'
    k.type(c, 'cd /usr/soak', 125)
    k.type(c, 'cmp /bin/sh x1 && ' + mark('CMP3OK'), run=8)
    k.type(c, 'cmp /bin/csh x2 && ' + mark('CMP4OK'), run=8)
    k.type(c, 'sum big > big.sum2 && cmp big.sum big.sum2 && ' + mark('SUMOK'), run=30)
    k.type(c, './p', run=6)
    k.type(c, 'sync && ' + mark('SYNC2OK'), run=6)
    return k.spec(20, tap='console', **kw)


def cmd_soak(a):
    out = workdir('soak', a.out)
    state = os.path.join(out, 'machine')
    kw = {'nv': 'keep', 'ram': a.ram}
    if a.poll:
        kw['env'] = {'FOCUS_IOPPOLL': str(a.poll)}
    rc1 = boot(state, soak_boot1(**kw), os.path.join(out, 'boot1.log'))
    rc2 = boot(state, soak_boot2(**kw), os.path.join(out, 'boot2.log'))
    x1, x2 = text(os.path.join(out, 'boot1.log')), text(os.path.join(out, 'boot2.log'))
    checks = collections.OrderedDict([
        ('boot 1 exited 0', rc1 == 0), ('boot 2 exited 0', rc2 == 0),
        ('cc compiled the program', 'CCOK' in x1),
        ('the program printed %s' % SOAK_EXPECT, SOAK_EXPECT in x1),
        ('cp+cmp /bin/sh', 'CMP1OK' in x1), ('cp+cmp /bin/csh', 'CMP2OK' in x1),
        ('sync', 'SYNCOK' in x1), ('ps on tty01 under load', 'PSOK' in x1),
        ('sort on tty02 under load', 'SORTOK' in x1),
        ('after the reboot: /bin/sh copy intact', 'CMP3OK' in x2),
        ('after the reboot: /bin/csh copy intact', 'CMP4OK' in x2),
        ('after the reboot: big file checksum unchanged', 'SUMOK' in x2),
        ('after the reboot: the compiled program runs', SOAK_EXPECT in x2),
        ('after the reboot: sync', 'SYNC2OK' in x2),
    ])
    for k, pat in BAD:
        for n, x in (('boot 1', x1), ('boot 2', x2)):
            checks['%s: no %s' % (n, k)] = not re.search(pat, x)
    for k, v in checks.items():
        print('  %-4s %s' % ('ok' if v else 'FAIL', k))
    if not a.keep:
        shutil.rmtree(state, ignore_errors=True)
    ok = all(checks.values())
    print('PASS' if ok else 'FAIL  (logs: %s)' % out)
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0],
                                 formatter_class=argparse.RawDescriptionHelpFormatter,
                                 epilog='\n'.join(__doc__.split('\n')[1:]))
    sub = ap.add_subparsers(dest='cmd', required=True)
    for name in ('matrix', 'stress', 'soak'):
        p = sub.add_parser(name)
        p.add_argument('--out', help='where the logs go (default var/test/<command>.<pid>)')
        p.add_argument('--keep', action='store_true', help='keep the machines')
        if name != 'soak':
            p.add_argument('--jobs', type=int, default=3)
    m = sub.choices['matrix']
    m.add_argument('--only'); m.add_argument('--skip'); m.add_argument('--list', action='store_true')
    sub.choices['stress'].add_argument('--runs', type=int, default=30)
    s = sub.choices['soak']
    s.add_argument('--ram', type=int, default=2); s.add_argument('--poll', type=int)
    a = ap.parse_args()
    # The run scripts take another build from HPFOCUS_BIN (tools/lib/focus.sh).
    binary = os.environ.get('HPFOCUS_BIN') or os.path.join(ROOT, 'mame', 'focus')
    if not os.access(binary, os.X_OK):
        sys.exit('%s is not built: run tools/setup.sh' % os.path.relpath(binary, ROOT))
    return {'matrix': cmd_matrix, 'stress': cmd_stress, 'soak': cmd_soak}[a.cmd](a)


if __name__ == '__main__':
    sys.exit(main())
