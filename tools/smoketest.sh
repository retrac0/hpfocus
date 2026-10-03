#!/usr/bin/env bash
# Does HP-UX come up and take a login? PASS or FAIL in about a minute.
#
#   tools/smoketest.sh              one multiuser boot of a fresh copy of
#                                   the pristine machine, a root login on
#                                   the console, `who` and `date`
#   tools/smoketest.sh --twice      then boot again on what the first boot
#                                   left (disc and battery): what a user's
#                                   second `tools/runhpux.sh --multiuser` sees
#   tools/smoketest.sh --seconds N  run each boot for N emulated seconds
#                                   (default 175; the login is typed at 90)
#   tools/smoketest.sh --keep       keep the machine and the logs
#
# Nothing of yours is touched: the machine is a fresh copy of the pristine
# one in var/smoke.XXXXXX (128 MB, deleted afterwards unless --keep), made
# and booted by tools/runhpux.sh --state, headless and unthrottled. Each
# boot passes when MAME exits 0, /etc/rc finishes, seven login banners
# appear (the console and the six ports of the multiplexer), root's `who`
# on the console answers, and nothing halts, panics, faults or fails to
# open a terminal. This is the matrix the alpha was released against
# (scratch/alpha/hpuxmatrix.py), cut down to one case.
#
# Exit status 0 only if every boot passed. Put the output in a bug report.

set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/lib/focus.sh"
source "$(dirname "${BASH_SOURCE[0]}")/lib/hpux.sh"
focus_repo
hpux_paths

SECS=175
TWICE=0
KEEP=0
while [ $# -gt 0 ]; do
	case "$1" in
		--seconds) SECS=$2; shift ;;
		--twice) TWICE=1 ;;
		--keep) KEEP=1 ;;
		-h|--help) sed -n '2,/^# Exit status/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
		*) echo "unknown option '$1' (try --help)" >&2; exit 2 ;;
	esac
	shift
done
[ "$SECS" -ge 160 ] 2>/dev/null || { echo "--seconds must be at least 160: the session is typed from 90 s on" >&2; exit 2; }
focus_built

mkdir -p "$VAR"
WORK=$(mktemp -d "$VAR/smoke.XXXXXX")
cleanup() {
	if [ "$KEEP" = 1 ]; then
		echo "kept: $WORK"
	else
		rm -f "$WORK"/disc.bin "$WORK"/scm.nv "$WORK"/ORIGIN "$WORK"/boot*.log \
			"$WORK"/run.pid "$WORK"/stop
		rmdir "$WORK" 2>/dev/null || true
	fi
}
trap cleanup EXIT

echo "hpfocus smoke test: $(git rev-parse --short HEAD 2>/dev/null || echo 'not a git checkout')," \
	"$(basename "$FOCUS_BIN") built $(date -r "$FOCUS_BIN" '+%Y-%m-%d %H:%M' 2>/dev/null || echo '?')," \
	"$(uname -sm)"

# The console session, as the matrix types it: root, two Returns for the
# TERM prompts, then who and date.
T=90
export FOCUS_TAP=all FOCUS_TYPERATE=60
export FOCUS_TYPE='@console=root\n;@console=\n;@console=\n;@console=who\n;@console=date\n'
export FOCUS_TYPEAT="$T,$((T + 12)),$((T + 22)),$((T + 32)),$((T + 42))"
# The tap writes its raw stream into a file (the terminals lane's line
# mode is for a terminal), which is what the scorer below parses.
export FOCUS_TAPRAW=1

status=0
boot() {
	local n=$1 log="$WORK/boot$1.log" rc=0 t0 t1
	t0=$(date +%s)
	tools/runhpux.sh --multiuser --state "$WORK" --ports none --seconds "$SECS" \
		> "$log" 2>&1 || rc=$?
	t1=$(date +%s)
	echo "boot $n: $SECS emulated seconds in $((t1 - t0)) s, battery" \
		"$( [ "$n" = 1 ] && echo pristine || echo "from boot $((n - 1))")"
	python3 - "$log" "$rc" <<'EOF' || status=1
import collections, re, sys
log, rc = sys.argv[1], int(sys.argv[2])
t = open(log, 'rb').read().decode('latin1')
BANNER = "HP-UX 5.30 (unknown)login: "
def banners_after_rc(t):
    # Login banners printed after /etc/rc, counted by characters because
    # seven terminals tapped at once interleave on stdout.
    i = t.find('Nodename: unknown.')
    if i < 0:
        return 0
    seg = t[i + len('Nodename: unknown.'):]
    j = seg.find('[')
    seg = seg[:j] if j >= 0 else seg
    c = collections.Counter(seg.replace('\r', '').replace('\n', '').replace('.', ''))
    b = collections.Counter(BANNER.replace('.', ''))
    return min(c[ch] // b[ch] for ch in b)
checks = []
def check(ok, what):
    checks.append(ok)
    print('  %-4s %s' % ('ok' if ok else 'FAIL', what))
check(rc == 0, 'MAME exited %d' % rc)
check('Hewlett-Packard Company' in t, 'the kernel printed its banner')
check('Nodename: unknown.' in t, '/etc/rc finished')
n = banners_after_rc(t)
check(n >= 7, 'login banners: %d of 7 (console + six multiplexer ports)' % n)
who = sorted(set(re.findall(r'root +(tty0\d|console) +\w{3} +\d+ \d\d:\d\d', t)))
check('console' in who, "root's who on the console: %s" % (', '.join(who) or 'no answer'))
dates = re.findall(r'\w{3} \w{3} +\d+ \d\d:\d\d:\d\d \w{3} \d{4}', t)
print('       date: %s' % (dates[-1] if dates else 'none printed'))
for pat, what in ((r'System halted|panic|TRAP:', 'halts or panics'),
                  (r'memory fault', 'memory faults'),
                  (r'cannot open', "gettys that cannot open a port"),
                  (r'core dumped', 'core dumps')):
    k = len(re.findall(pat, t))
    check(k == 0, '%s: %d' % (what, k))
sys.exit(0 if all(checks) else 1)
EOF
}

boot 1
[ "$TWICE" = 1 ] && boot 2
if [ "$status" = 0 ]; then
	echo "PASS"
else
	echo "FAIL (the logs: run again with --keep)"
fi
exit "$status"
