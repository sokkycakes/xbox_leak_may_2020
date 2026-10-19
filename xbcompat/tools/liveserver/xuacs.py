"""The user account service (/xuacs/*.ashx) behind the security gateway: the
dashboard's account creation.  Request and reply layouts are the client's in
private/online/accounts.cpp (binary, little-endian, strings as WORD length +
UTF-8)."""
import struct

USER_REALM = 'passport.net'     # the client hard-codes it (accounts.cpp:1054)
USER_DOMAIN = 'passport.net'
TAGS = ['Sion', 'Classic', 'Arcade', 'Halo', 'Duke']   # "kingdoms", up to 12 characters


def handle(gw, sa, data):
    """The full HTTP reply once `data` holds a whole request, else None."""
    end = data.find(b'\r\n\r\n')
    if end < 0:
        return None
    head = data[:end].decode('latin-1').split('\r\n')
    length = 0
    for line in head[1:]:
        k, _, v = line.partition(':')
        if k.strip().lower() == 'content-length':
            length = int(v)
    body = data[end + 4:]
    if len(body) < length:
        return None
    body = body[:length]
    parts = head[0].split()
    path = parts[1] if len(parts) > 1 else ''
    gw.log(f'xuacs: {head[0]} ({length} bytes)')
    handler = ROUTES.get(path.lower())
    if handler is None:
        return http(404, b'', err=0x80004005)
    try:
        return http(200, handler(gw, sa, body))
    except Exception as e:
        gw.log(f'xuacs: {path} failed: {e!r}')
        return http(500, b'', err=0x80004005)


def http(code, body, err=None):
    head = f'HTTP/1.0 {code} {"OK" if code == 200 else "Error"}\r\nContent-Length: {len(body)}\r\n'
    if err is not None:
        head += f'X-Err: {err:x}\r\n'
    return (head + '\r\n').encode() + body


def wstr(s):
    b = s.encode()
    return struct.pack('<H', len(b)) + b


def read_strs(body, pos, n):
    out = []
    for _ in range(n):
        ln = struct.unpack_from('<H', body, pos)[0]
        out.append(body[pos + 2:pos + 2 + ln].decode('utf-8', 'replace'))
        pos += 2 + ln
    return out, pos


def gettags(gw, sa, body):
    max_tags = struct.unpack_from('<H', body, 4)[0] if len(body) >= 6 else 5
    tags = TAGS[:max_tags]
    return struct.pack('<H', len(tags)) + b''.join(wstr(t) for t in tags)


def reservename(gw, sa, body):
    machine, country, max_names = struct.unpack_from('<QHH', body)
    (nick, kingdom), _ = read_strs(body, 12, 2)
    gw.log(f'xuacs: reserve {nick}.{kingdom} for machine {machine:#x}')
    if not gw.db.name_taken(nick, kingdom):
        return struct.pack('<H', 0)
    alts = []
    n = 1
    while len(alts) < max_names:
        cand = f'{nick[:14]}{n}'
        if not gw.db.name_taken(cand, kingdom):
            alts.append(cand)
        n += 1
    return struct.pack('<H', len(alts)) + b''.join(wstr(a) + wstr(kingdom) for a in alts)


def createacct(gw, sa, body):
    (nick, kingdom), _ = read_strs(body, 10, 2)
    if gw.db.name_taken(nick, kingdom):
        raise ValueError(f'{nick}.{kingdom} is taken')
    acct = gw.db.new_user(nick, kingdom, USER_DOMAIN, USER_REALM)
    gw.log(f'xuacs: created {nick}.{kingdom}@{USER_DOMAIN}, xuid {acct["xuid"]:#x}')
    return struct.pack('<Q16s20s24s', acct['xuid'], bytes.fromhex(acct['key']), USER_DOMAIN.encode(),
                       USER_REALM.encode())


ROUTES = {
    '/xuacs/gettags.ashx': gettags,
    '/xuacs/reservename.ashx': reservename,
    '/xuacs/createacct.ashx': createacct,
}
