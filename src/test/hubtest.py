#!/usr/bin/env python3
"""Tests for the terminal relay, tools/hpuxterm.py hub.

    src/test/hubtest.py [path to hpuxterm.py]

No emulator: fake machines connect to the hub's private ports the way MAME's
bitbanger does, fake users to its public ones. Checked:

  relay      bytes pass both ways on a port
  backlog    what the machine printed with nobody attached is shown to the
             next user
  takeover   a second user on a port takes it over; the first is told
  stalled    a user who stops reading does not hold up the other ports, or
             the SIGTERM that ends the run. The hub used to send to users
             with a blocking sendall(): one suspended client with a few
             megabytes owed to it froze every port, and the hub then
             ignored SIGTERM too.

Exit status 0 only if every check passed.
"""
import os, socket, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
HUB = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, '..', '..', 'tools', 'hpuxterm.py')
fails = 0


def check(what, ok):
    global fails
    print('  %s %s' % ('pass' if ok else 'FAIL', what), flush=True)
    fails += 0 if ok else 1


def got(sock, want, seconds=3.0):
    """True once `want` has arrived on sock."""
    sock.settimeout(0.2)
    buf, end = b'', time.time() + seconds
    while want not in buf and time.time() < end:
        try:
            data = sock.recv(65536)
        except socket.timeout:
            continue
        if not data:
            break
        buf += data
    return want in buf


def main():
    work = tempfile.mkdtemp(prefix='hubtest.')
    ready = os.path.join(work, 'ready')
    base = 20000 + os.getpid() % 20000
    hub = subprocess.Popen([sys.executable, HUB, 'hub', '--ports', '1,2',
                            '--base', str(base), '--ready', ready])
    try:
        t0 = time.time()
        while not os.path.exists(ready) and time.time() - t0 < 10:
            time.sleep(0.05)
        ports = {l.split()[0]: (int(l.split()[1]), int(l.split()[2])) for l in open(ready)}
        mach = {n: socket.create_connection(('127.0.0.1', ports[n][0])) for n in ports}
        time.sleep(0.2)

        print('hub: relay, backlog and takeover', flush=True)
        mach['1'].sendall(b'login: ')
        time.sleep(0.2)
        a = socket.create_connection(('127.0.0.1', ports['1'][1]))
        check('a prompt printed before the user came is replayed', got(a, b'login: '))
        a.sendall(b'root\r')
        check('typing reaches the machine', got(mach['1'], b'root\r'))
        mach['1'].sendall(b'Password:')
        check('output reaches the user', got(a, b'Password:'))
        a2 = socket.create_connection(('127.0.0.1', ports['1'][1]))
        check('the first user is told of a takeover', got(a, b'took this port over'))
        mach['1'].sendall(b'# ')
        check('the second user has the port', got(a2, b'# '))

        print('hub: a user who stops reading', flush=True)
        # a2 stops reading; its machine goes on printing.
        a2.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
        b = socket.create_connection(('127.0.0.1', ports['2'][1]))
        time.sleep(0.2)
        mach['1'].setblocking(False)
        sent, chunk, end = 0, b'x' * 65536, time.time() + 6
        while sent < (12 << 20) and time.time() < end:
            try:
                sent += mach['1'].send(chunk)
            except BlockingIOError:
                time.sleep(0.02)
        check('the machine is never held up by it (%d KB taken)' % (sent >> 10),
              sent >= (12 << 20))
        mach['2'].sendall(b'hello from port 2\r\n')
        check('another port still hears its machine', got(b, b'hello from port 2'))
        b.sendall(b'who\r')
        check('another port can still type', got(mach['2'], b'who\r'))
        # The stalled user was let go; the port takes a new one, with the
        # tail of what it missed.
        mach['1'].settimeout(3)
        try:
            mach['1'].sendall(b'\r\nTAIL-MARK\r\n')
        except socket.timeout:
            pass
        time.sleep(0.3)
        a3 = socket.create_connection(('127.0.0.1', ports['1'][1]))
        check('the port takes a new user, with the tail of the output', got(a3, b'TAIL-MARK'))
        hub.terminate()
        try:
            hub.wait(3)
            check('SIGTERM ends the hub', True)
        except subprocess.TimeoutExpired:
            check('SIGTERM ends the hub', False)
    finally:
        if hub.poll() is None:
            hub.kill()
        for f in os.listdir(work):
            os.remove(os.path.join(work, f))
        os.rmdir(work)
    print('hubtest: %s' % ('every check passed' if not fails else '%d FAILED' % fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
