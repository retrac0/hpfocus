#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Joel Heikkila
"""Reach the emulated machine's terminals from the host.

    tools/hpuxterm.py 1                 log in on /dev/tty01 (MUX port 0)
    tools/hpuxterm.py console           the console, when runhpux put it on TCP
    tools/hpuxterm.py 127.0.0.1:4003    any TCP port
    tools/hpuxterm.py /dev/pts/7        a pseudo terminal (--ports pty)

    tools/hpuxterm.py hub ...           the relay tools/runhpux.sh runs (below)

CONNECT puts this terminal in raw mode and joins it to the port: what you
type goes to HP-UX a key at a time (Return is CR, as on a real terminal),
and HP-UX does the echoing. Ctrl-] leaves; the session on the port carries
on, logged in, and you can come back to it. Terminal numbers N = 1..6 are
TCP ports 4000+N on 127.0.0.1 (--base, or HPFOCUS_PORT_BASE, changes
that); 'console' is 4000.

THE HUB exists because MAME's bitbanger takes one TCP connection per run.
Given -bitb socket.HOST:PORT it listens, accepts once and closes the
listener, so a user who quits nc can never get back in (and on the alpha's
base build the next write to the vanished client raised SIGPIPE, which
ended the emulator without saving the disc). The hub turns that round: it
listens first, on a private port per terminal, and MAME -- finding a
listener there -- CONNECTS to it instead of listening itself (MAME's image
open tries a plain open, which for a socket is connect(), before it
creates). That one connection then lasts the whole run, and the hub
accepts users on the public port as often as they like:

    tools/hpuxterm.py hub --ports console,1,2,3,4,5,6 --base 4000 --ready FILE

binds 127.0.0.1:<ephemeral> for MAME and 127.0.0.1:4000+N for users, writes
"name internal-port public-port" lines to FILE once every socket is bound,
and runs until it is sent SIGTERM or the run that started it is gone.
A user connecting to a port that already has one takes it over (the old
connection is told and closed): a dead client never locks anyone out.
While nobody is attached, the last few kilobytes the machine printed are
kept and replayed to the next user, so a login: prompt printed before you
connected is shown.

Standard library only; Python 3.8+; Linux and macOS.
"""
import argparse
import os
import select
import selectors
import signal
import socket
import sys

DEFAULT_BASE = 4000
BACKLOG = 4096          # bytes of output kept for a user who is not there yet
# How far a peer may fall behind before the hub stops waiting for it. The
# hub is one thread serving every port, so it never blocks on any socket:
# what a peer cannot take yet is queued, and a queue has an end. A user
# this far behind is not reading (a suspended client, a dead link) and is
# disconnected -- the session on the port carries on and the last BACKLOG
# bytes are there when they come back. Keystrokes for a machine that is not
# reading (paused, stopped in the debugger) are dropped past the limit.
USER_BEHIND = 1 << 18
MACHINE_BEHIND = 1 << 16


def parse_target(text, base):
    """'1'..'6' -> port base+N; 'console' -> base; 'host:port'; a path."""
    if text == 'console':
        return ('127.0.0.1', base)
    if text.isdigit() and int(text) < 100:
        return ('127.0.0.1', base + int(text))
    if text.startswith('/'):
        return text
    if ':' in text:
        host, port = text.rsplit(':', 1)
        return (host or '127.0.0.1', int(port))
    if text.isdigit():
        return ('127.0.0.1', int(text))
    raise SystemExit("hpuxterm: don't know how to reach '%s' (try 1..6, console, "
                     "host:port or /dev/pts/N)" % text)


# --------------------------------------------------------------- connect
def connect(target, base):
    import termios
    import tty
    where = parse_target(target, base)
    if isinstance(where, str):
        try:
            fd = os.open(where, os.O_RDWR | os.O_NOCTTY)
        except OSError as e:
            raise SystemExit('hpuxterm: cannot open %s: %s' % (where, e.strerror))
        # The pty's line discipline would otherwise echo and cook.
        try:
            tty.setraw(fd)
        except termios.error:
            pass
        rd = lambda: os.read(fd, 4096)                       # noqa: E731
        wr = lambda b: os.write(fd, b)                       # noqa: E731
        name = where
    else:
        try:
            s = socket.create_connection(where, timeout=5)
        except OSError as e:
            raise SystemExit('hpuxterm: cannot connect to %s:%d: %s\n'
                             'Is tools/runhpux.sh --multiuser running?'
                             % (where[0], where[1], e.strerror or e))
        s.settimeout(None)
        fd = s.fileno()
        rd = lambda: s.recv(4096)                            # noqa: E731
        wr = lambda b: s.sendall(b)                          # noqa: E731
        name = '%s:%d' % where
    sys.stderr.write('[connected to %s -- Ctrl-] leaves; press Return for a prompt]\r\n'
                     % name)
    sys.stderr.flush()
    stdin = sys.stdin.fileno()
    interactive = os.isatty(stdin)
    old = termios.tcgetattr(stdin) if interactive else None
    try:
        if interactive:
            tty.setraw(stdin)
        while True:
            r, _, _ = select.select([stdin, fd], [], [])
            if fd in r:
                try:
                    data = rd()
                except OSError:
                    data = b''
                if not data:
                    sys.stderr.write('\r\n[the machine closed the connection]\r\n')
                    break
                os.write(sys.stdout.fileno(), data)
            if stdin in r:
                data = os.read(stdin, 1024)
                if not data:
                    break
                if b'\x1d' in data:                          # Ctrl-]
                    data = data[:data.index(b'\x1d')]
                    if data:
                        wr(data)
                    sys.stderr.write('\r\n[left; the session goes on]\r\n')
                    break
                if not interactive:
                    data = data.replace(b'\n', b'\r')
                wr(data)
    finally:
        if old is not None:
            termios.tcsetattr(stdin, termios.TCSADRAIN, old)


# --------------------------------------------------------------- hub
class Port:
    def __init__(self, name, public):
        self.name = name
        self.public = public
        self.inner_listener = None   # MAME connects here
        self.outer_listener = None   # users connect here
        self.machine = None          # MAME's connection
        self.user = None             # the attached user's connection
        self.backlog = bytearray()   # output while nobody is attached
        self.to_user = bytearray()   # output the user has not taken yet
        self.to_machine = bytearray()  # typing the machine has not taken yet


def listener(port):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(('127.0.0.1', port))
    s.listen(4)
    s.setblocking(False)
    return s


def hub(names, base, ready):
    ports = []
    for n in names:
        public = base if n == 'console' else base + int(n)
        p = Port(n, public)
        try:
            p.outer_listener = listener(public)
        except OSError as e:
            raise SystemExit('hpuxterm hub: cannot listen on 127.0.0.1:%d (%s); '
                             'another run, or pass --port-base to tools/runhpux.sh'
                             % (public, e.strerror))
        p.inner_listener = listener(0)
        ports.append(p)
    tmp = ready + '.tmp'
    with open(tmp, 'w') as f:
        for p in ports:
            f.write('%s %d %d\n' % (p.name, p.inner_listener.getsockname()[1], p.public))
    os.replace(tmp, ready)

    stop = []
    # Only SIGTERM ends the hub. A ^C at the terminal reaches the whole
    # process group -- runhpux.sh, MAME and this -- and the hub must outlive
    # MAME's clean exit: a peer that vanished first would turn MAME's next
    # write into SIGPIPE (the driver ignores it now; an older build died of
    # it before saving the disc).
    signal.signal(signal.SIGTERM, lambda *a: stop.append(1))
    signal.signal(signal.SIGINT, signal.SIG_IGN)
    signal.signal(signal.SIGHUP, signal.SIG_IGN)
    sel = selectors.DefaultSelector()
    for p in ports:
        sel.register(p.inner_listener, selectors.EVENT_READ, (p, 'inner'))
        sel.register(p.outer_listener, selectors.EVENT_READ, (p, 'outer'))

    def drop(p, which, note=None):
        s = getattr(p, which)
        if s is None:
            return
        if note:
            # Best effort, after whatever was still queued for it.
            try:
                s.sendall(bytes(p.to_user) + note)
                p.to_user.clear()
            except OSError:
                pass
        sel.unregister(s)
        s.close()
        setattr(p, which, None)
        if which == 'user':
            # What it never took is the start of the next user's backlog.
            p.backlog += p.to_user
            del p.backlog[:-BACKLOG]
            p.to_user.clear()
        else:
            p.to_machine.clear()

    # Send what a peer will take now and leave the rest queued; the
    # selector says when it will take more. Never blocks: a user who stops
    # reading used to hold the hub in sendall(), and with it every other
    # port and the SIGTERM that ends the run.
    def flush(p, which):
        s = getattr(p, which)
        q = p.to_user if which == 'user' else p.to_machine
        if s is None:
            return
        try:
            if q:
                del q[:s.send(q)]
        except (BlockingIOError, InterruptedError):
            pass
        except OSError:
            drop(p, which)
            return
        sel.modify(s, selectors.EVENT_READ | (selectors.EVENT_WRITE if q else 0),
                   (p, which))

    # The hub belongs to the run that started it. If that run is killed
    # outright (kill -9 skips its cleanup), the hub is re-parented, and it
    # must go too: otherwise it holds the ports and the next run cannot
    # listen on them.
    parent = os.getppid()
    while not stop:
        if os.getppid() != parent:
            break
        try:
            events = sel.select(timeout=0.5)
        except InterruptedError:
            continue
        for key, mask in events:
            p, kind = key.data
            if kind in ('user', 'machine'):
                if getattr(p, kind) is not key.fileobj:
                    continue        # dropped earlier in this round
                if mask & selectors.EVENT_WRITE:
                    flush(p, kind)
                if not mask & selectors.EVENT_READ or getattr(p, kind) is None:
                    continue
            if kind == 'inner':
                conn, _ = p.inner_listener.accept()
                conn.setblocking(False)
                drop(p, 'machine')
                p.machine = conn
                sel.register(conn, selectors.EVENT_READ, (p, 'machine'))
            elif kind == 'outer':
                conn, _ = p.outer_listener.accept()
                conn.setblocking(False)
                drop(p, 'user', b'\r\n[hpuxterm: another connection took this port over]\r\n')
                p.user = conn
                sel.register(conn, selectors.EVENT_READ, (p, 'user'))
                if p.machine is None:
                    p.to_user += b'[hpuxterm: the machine is not connected to this port yet]\r\n'
                p.to_user += p.backlog
                p.backlog.clear()
                flush(p, 'user')
            elif kind == 'machine':
                try:
                    data = p.machine.recv(4096)
                except (BlockingIOError, InterruptedError):
                    continue
                except OSError:
                    data = b''
                if not data:
                    drop(p, 'machine')
                    drop(p, 'user', b'\r\n[hpuxterm: the machine has stopped]\r\n')
                    continue
                if p.user is not None:
                    p.to_user += data
                    if len(p.to_user) > USER_BEHIND:
                        drop(p, 'user')
                    else:
                        flush(p, 'user')
                else:
                    p.backlog += data
                    del p.backlog[:-BACKLOG]
            elif kind == 'user':
                try:
                    data = p.user.recv(4096)
                except (BlockingIOError, InterruptedError):
                    continue
                except OSError:
                    data = b''
                if not data:
                    drop(p, 'user')
                    continue
                if p.machine is not None:
                    p.to_machine += data
                    del p.to_machine[MACHINE_BEHIND:]
                    flush(p, 'machine')
    for p in ports:
        for which in ('user', 'machine'):
            drop(p, which)
        p.inner_listener.close()
        p.outer_listener.close()


def main():
    if len(sys.argv) > 1 and sys.argv[1] == 'hub':
        ap = argparse.ArgumentParser(prog='hpuxterm.py hub')
        ap.add_argument('--ports', default='1,2,3,4,5,6',
                        help="comma list: 'console' and/or 1..6")
        ap.add_argument('--base', type=int, default=DEFAULT_BASE)
        ap.add_argument('--ready', required=True,
                        help='file to write the port map to once listening')
        a = ap.parse_args(sys.argv[2:])
        hub([x for x in a.ports.split(',') if x], a.base, a.ready)
        return 0
    ap = argparse.ArgumentParser(
        description='Connect this terminal to a terminal of the emulated machine.',
        epilog='Ctrl-] leaves without logging out.')
    ap.add_argument('target', help="1..6 (tty01..tty06), console, host:port or /dev/pts/N")
    ap.add_argument('--base', type=int, default=int(os.environ.get('HPFOCUS_PORT_BASE',
                                                                    DEFAULT_BASE)),
                    help='TCP port of the console; tty0N is base+N (default %(default)s)')
    a = ap.parse_args()
    connect(a.target, a.base)
    return 0


if __name__ == '__main__':
    sys.exit(main())
