#!/usr/bin/env python3
"""Insignia, the Xbox Live revival (insignia.live), for xbcompat consoles.

    insignia.py register CONSOLE-ID [--dry-run]
    insignia.py zones

register does what the Register button of Insignia's Setup Assistant does: it
sends the console's serial number, hard disk key and online key (EEPROM factory
value 0x102) to insignia.live, so Insignia's MACS can give it a machine account
(the MACS key is the online key morphed by the hard disk key).  The Setup
Assistant itself drives the GPU directly and doesn't run under xbcompat;
xbcompat keeps a console's three values in <hdd>/console-id.

zones prints the Live host names Insignia answers, as insigniaDNS does, and as
liveserver.py --insignia answers them for the console.
"""
import argparse
import json
import socket
import sys

API = 'https://insignia.live/api/v1/xbox'
ZONES_URL = 'https://insignia.live/dns_zones.json'
# insignia.live/dns_zones.json (2026-10), for when it can't be fetched: kind 'a'
# answers with the value, 'p' with the value's own current address.
ZONES = [
    {'type': 'p', 'name': 'macs.xboxlive.com', 'value': 'macs.insig.uk'},
    {'type': 'p', 'name': 'as.xboxlive.com', 'value': 'as.insig.uk'},
    {'type': 'p', 'name': 'tgs.xboxlive.com', 'value': 'tgs.insig.uk'},
    {'type': 'p', 'name': 'xds.xboxlive.com', 'value': 'xexds.xboxlive.com'},
]


def read_console_id(path):
    f = sys.stdin if path == '-' else open(path)
    with f:
        return {k: v.strip() for k, v in (l.split(None, 1) for l in f.read().splitlines() if ' ' in l)}


def request_body(ident):
    """The Setup Assistant's JSON, after the same checks it makes."""
    serial = ident['serial']
    if len(serial) != 12 or not serial.isdigit():
        raise ValueError(f'serial {serial!r} is not 12 digits (Insignia: "Your serial number is invalid")')
    hdd_key = bytes.fromhex(ident['hdd-key'])
    online_key = bytes.fromhex(ident['online-key'])
    if len(hdd_key) != 16 or not any(hdd_key):
        raise ValueError('the hard disk key is missing or all zero (Insignia: "All-zero HDD keys are not supported")')
    if len(online_key) != 16:
        raise ValueError('the online key is missing')
    return ('{"serial_number": "%s","hdd_key": "%s","online_key": "%s"}'
            % (serial, hdd_key.hex().upper(), online_key.hex().upper())).encode()


def register(body):
    """POST it as the Setup Assistant does; (HTTP status, message)."""
    import urllib.error
    import urllib.request
    req = urllib.request.Request(API, data=body, method='POST', headers={
        'Accept': 'application/json', 'Content-Type': 'application/json',
        'User-Agent': 'xbcompat-insignia/1'})
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            status, text = r.status, r.read().decode(errors='replace')
    except urllib.error.HTTPError as e:
        status, text = e.code, e.read().decode(errors='replace')
    try:
        reply = json.loads(text)
    except ValueError:
        return status, text.strip()[:300]
    if not isinstance(reply, dict):
        return status, text.strip()[:300]
    message = reply.get('message') or text.strip()[:300]
    if reply.get('errors'):
        message += ' ' + json.dumps(reply['errors'])
    return status, message


def zones(fetch=True):
    """Insignia's DNS zone list: name -> (kind, value)."""
    table = ZONES
    if fetch:
        try:
            # (the Sion's trimmed Python has no http package, hence no urllib.request)
            import urllib.request
            req = urllib.request.Request(ZONES_URL, headers={'User-Agent': 'insigniaDNS/1.0 (xbcompat)'})
            with urllib.request.urlopen(req, timeout=8) as r:
                fetched = json.loads(r.read())
            if fetched and all({'type', 'name', 'value'} <= set(z) for z in fetched):
                table = fetched
        except (ImportError, OSError, ValueError):
            pass
    return {z['name'].lower(): (z['type'], z['value']) for z in table if z['type'] in ('a', 'p')}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('register', help="register a console-id's console with Insignia")
    r.add_argument('console_id', help="the hdd's console-id file, or - for standard input")
    r.add_argument('--dry-run', action='store_true', help='print the request instead of sending it')
    sub.add_parser('zones', help="print Insignia's Live host names and where they point now")
    a = ap.parse_args()

    if a.cmd == 'zones':
        for name, (kind, value) in zones().items():
            try:
                where = value if kind == 'a' else socket.gethostbyname(value)
            except OSError as e:
                where = f'unresolved ({e})'
            print(f'{name:20} {value:22} {where}')
        return
    try:
        body = request_body(read_console_id(a.console_id))
    except (OSError, KeyError, ValueError) as e:
        sys.exit(f'insignia: {e}')
    if a.dry_run:
        print(f'POST {API}\n{body.decode()}')
        return
    status, message = register(body)
    print(f'insignia: {status} {message}')
    # 200 registered; 422 refused (the message says why, e.g. the serial is taken); 429 try later
    sys.exit(0 if status == 200 else 1)


if __name__ == '__main__':
    main()
