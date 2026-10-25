#!/usr/bin/env python3
"""A stand-in for the 2002 Xbox Live data center, for xbcompat.

  DNS      answers the Live host names (*.xbox.com, and retail's *.xboxlive.com) with the
           server's address and forwards everything else
  UDP 88   MACS, AS and the Xbox KDC (kdc.py)
  UDP 3074 the security gateway and the account service behind it (sg.py)

Local use with xbcompat's NAT (the console sees this host as 10.0.2.2):
  tools/liveserver/liveserver.py &
  XBCOMPAT_DNS=127.0.0.1:5353 tools/xodashbuild/run.sh /tmp/xodash
Start the server before xbcompat so it holds UDP 3074 on this host.

With --insignia it is only a DNS server, as insigniaDNS is: the retail Live names
Insignia serves point at Insignia (insignia.py), everything else is forwarded,
and the console signs in to Insignia instead.
"""
import argparse
import os
import select
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import db as dbmod      # noqa: E402
import kdc as kdcmod    # noqa: E402
import sg as sgmod      # noqa: E402

LIVE_NAMES = ('xbox.com', 'xboxlive.com')   # the 2002 beta's names, then retail's


def log(msg):
    print(time.strftime('%H:%M:%S ') + msg, flush=True)


class Dns:
    def __init__(self, address, upstream, zones=None):
        self.address = address
        self.upstream = upstream
        self.zones = zones      # Insignia's names -> (kind, value); None: every Live name is this server
        self.resolved = {}      # zone name -> (address, when)
        self.pending = {}   # upstream id -> client address
        self.up = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    def live_address(self, name):
        """The address a Live name gets, b'' for one with none yet, None for any other name."""
        if self.zones is None:
            return self.address if any(name == d or name.endswith('.' + d) for d in LIVE_NAMES) else None
        if name not in self.zones:
            return None
        kind, value = self.zones[name]
        if kind == 'a':
            return socket.inet_aton(value)
        address, when = self.resolved.get(name, (b'', 0))
        if time.time() - when > 300:
            try:
                address = socket.inet_aton(socket.gethostbyname(value))
                self.resolved[name] = (address, time.time())
            except OSError as e:
                log(f'dns: cannot resolve {value} for {name}: {e}')
        return address

    def query(self, sock, data, peer):
        if len(data) < 12:
            return
        p = 12
        labels = []
        while p < len(data) and data[p]:
            labels.append(data[p + 1:p + 1 + data[p]].decode('latin-1'))
            p += 1 + data[p]
        name = '.'.join(labels).lower()
        qtype = int.from_bytes(data[p + 1:p + 3], 'big')
        question = data[12:p + 5]
        address = self.live_address(name)
        if address is not None:
            log(f'dns: {name} -> {socket.inet_ntoa(address) if address else "(none)"}')
            answer = b''
            if qtype == 1 and address:
                answer = b'\xc0\x0c' + (1).to_bytes(2, 'big') + (1).to_bytes(2, 'big') + \
                    (60).to_bytes(4, 'big') + (4).to_bytes(2, 'big') + address
            hdr = data[:2] + b'\x85\x80' + (1).to_bytes(2, 'big') + (1 if answer else 0).to_bytes(2, 'big') + \
                bytes(4)
            sock.sendto(hdr + question + answer, peer)
            return
        log(f'dns: {name} (type {qtype}) passed upstream')
        self.pending[data[:2]] = (sock, peer)
        self.up.sendto(data, self.upstream)

    def upstream_reply(self):
        data, _ = self.up.recvfrom(4096)
        sock, peer = self.pending.pop(data[:2], (None, None))
        if sock:
            sock.sendto(data, peer)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--bind', default='127.0.0.1', help='host address to listen on')
    ap.add_argument('--address', default='10.0.2.2',
                    help='address the console reaches this server at (NAT gateway by default)')
    ap.add_argument('--dns-port', type=int, default=5353)
    ap.add_argument('--upstream-dns', default='8.8.8.8')
    ap.add_argument('--db', default=os.path.expanduser('~/.xbcompat-live.json'))
    ap.add_argument('--console-id', action='append', default=[], metavar='FILE',
                    help="an xbcompat hdd's console-id, so that console can sign in (repeatable)")
    ap.add_argument('--insignia', action='store_true',
                    help="only DNS, pointing the console at Insignia (insignia.live) instead of this server")
    args = ap.parse_args()

    if args.insignia:
        import insignia
        zones = insignia.zones()
        dns = Dns(None, (args.upstream_dns, 53), zones)
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind((args.bind, args.dns_port))
        log(f'Insignia DNS on {args.bind}:{args.dns_port}: ' + ', '.join(f'{n} -> {v}' for n, (_, v) in zones.items()))
        while True:
            ready, _, _ = select.select([s, dns.up], [], [], 1.0)
            if dns.up in ready:
                dns.upstream_reply()
            if s in ready:
                data, peer = s.recvfrom(65536)
                dns.query(s, data, peer)

    db = dbmod.Db(args.db)
    for path in args.console_id:
        try:
            log(f'console {db.add_console(path)} from {path}')
        except (OSError, KeyError) as e:
            log(f'no console from {path}: {e!r}')
    site = socket.inet_aton(args.address)
    kdc = kdcmod.Kdc(db, site, sgmod.SERVICE_PORT, log)
    dns = Dns(site, (args.upstream_dns, 53))

    socks = {}
    for port, name in ((args.dns_port, 'dns'), (88, 'kdc'), (sgmod.SG_PORT, 'sg')):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind((args.bind, port))
        socks[s] = name
    sg = sgmod.Gateway(db, site, log, next(s for s, n in socks.items() if n == 'sg'))
    log(f'live server on {args.bind} (console sees {args.address}), DNS on port {args.dns_port}, db {args.db}')

    while True:
        ready, _, _ = select.select(list(socks) + [dns.up], [], [], 1.0)
        for s in ready:
            if s is dns.up:
                dns.upstream_reply()
                continue
            data, peer = s.recvfrom(65536)
            kind = socks[s]
            if kind == 'dns':
                dns.query(s, data, peer)
            elif kind == 'kdc':
                reply = kdc.handle(data)
                if reply:
                    s.sendto(reply, peer)
            else:
                sg.packet(data, peer)
        sg.tick()


if __name__ == '__main__':
    main()
