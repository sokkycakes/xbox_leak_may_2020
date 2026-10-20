"""The retail (5849) dashboard's web services behind the security gateway.
None of it aims to be the real service: it answers just enough for the
dashboard's sign-in and account screens to work.

Content files are plain HTTP GETs on service 11 (XOnlineDownloadToMemory,
private/online/download.cpp: any 200 with a Content-Length will do):
/Country/<region>/CountryMaster2.INX and /<country>/CountryInfo2.INX, the
HeidiDashLocalization string tables, and the terms text.  The .INX files are
UTF-16LE INI with a byte order mark (parser at xonlinedash 0xe03fb): each
section is a country, named as the dashboard puts it in the CountryInfo2 path,
and its keys are read by the setter at 0x59388.  Values must be in double
quotes (Key="value"); a bare value fails the whole file."""
import os
import struct

import xuacs

DEBUG = bool(os.environ.get('LIVE_DEBUG'))

# Country codes are the dwUserFlags country byte (XONLINE_USER_COUNTRY_MASK,
# bits 8-15) the dashboard selects a country by.
US = 103
COUNTRIES = {
    'US': {
        'LocaleCode': US,
        'DayPosition': 2, 'MonthPosition': 1, 'YearPosition': 3, 'DateSeparator': '/',
        'DateOfBirthPresent': 1, 'MinimumAge': 13, 'ParentAge': 18,
    },
}
MASTER_KEYS = ('LocaleCode',)
TERMS = {
    'TermsOfUse.ini': 'Terms of Use\r\n\r\nThis is a private test server. There are no terms.\r\n',
    'CodeOfConduct.ini': 'Code of Conduct\r\n\r\nBe nice.\r\n',
    'PrivacyStatement.ini': 'Privacy Statement\r\n\r\nNothing you send leaves this server.\r\n',
    'RefundInfo.ini': 'Refunds\r\n\r\nNothing here costs anything.\r\n',
}


def inx(keys=None):
    lines = []
    for name, info in COUNTRIES.items():
        lines.append(f'[{name}]')
        lines += [f'{k}="{v}"' for k, v in info.items() if keys is None or k in keys]
        lines.append('')
    return utf16('\r\n'.join(lines))


def utf16(text):
    return b'\xff\xfe' + text.encode('utf-16-le')


def content(path):
    parts = [p for p in path.split('/') if p]
    if len(parts) == 3 and parts[0] == 'Country' and parts[2].lower() == 'countrymaster2.inx':
        return inx(MASTER_KEYS)
    if len(parts) == 2 and parts[1].lower() == 'countryinfo2.inx' and parts[0] in COUNTRIES:
        return inx()
    if parts and parts[-1] in TERMS:
        return utf16(TERMS[parts[-1]])
    return None


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
    method, _, rest = head[0].partition(' ')
    path = rest.rpartition(' ')[0]
    body = body[:length]
    gw.log(f'xweb: {head[0]} ({length} bytes)')
    if DEBUG:
        gw.log(f'xweb: headers {head[1:]} body {body.hex()}')
    if method == 'GET':
        reply = content(path)
        if reply is None:
            gw.log(f'xweb: no content for {path}')
            return http(404, b'')
        return http(200, reply)
    handler = ROUTES.get(path.lower())
    if handler is None:
        gw.log(f'xweb: nothing answers {path}')
        return http(404, b'')
    reply = handler(gw, sa, body)
    if DEBUG:
        gw.log(f'xweb: reply {reply.hex()}')
    return http(200, reply)


def http(code, body, err=None):
    return xuacs.http(code, body, err)


# ---- presence and notification (/xpnfront/*.srf) ----------------------------
# Messages keep inc/xonlinep.h's packed BASE_MSG_HEADER {dwMsgType, dwMsgLen
# (bytes after the header), dwSeqNum, SGADDR (20 bytes)}, but retail sends one
# message per POST with no HELLO, and its sign-in check-in is type 0x401
# (built at xonlinedash 0x11ee71) {qwUserID, ...XNADDR, keys, gamertag}.  The
# reply parser (0x11efc8) wants {header, qwUserID, WORD, HRESULT hr at 0x2a,
# WORD cItems at 0x34, DWORD cTitles at 0x3a, DWORD titles[cTitles], items},
# dwMsgLen exact; it does not check the reply's type.  Nobody has friends
# here, so the reply is empty.
PMSG_ALIVE_RETAIL, PMSG_ALIVE_REPLY = 0x401, 1101
HEADER = struct.Struct('<III20s')


def messages(body):
    pos = 0
    while pos + HEADER.size <= len(body):
        mtype, mlen, seq, sgaddr = HEADER.unpack_from(body, pos)
        yield mtype, seq, sgaddr, body[pos + HEADER.size:pos + HEADER.size + mlen]
        pos += HEADER.size + mlen


def xpresence(gw, sa, body):
    out = b''
    for mtype, seq, sgaddr, data in messages(body):
        gw.log(f'xweb: presence message {mtype} seq {seq} ({len(data)} bytes)')
        if mtype == PMSG_ALIVE_RETAIL:
            reply = data[:8] + struct.pack('<Hi6xH4xI', 0, 0, 0, 0)
            out += HEADER.pack(PMSG_ALIVE_REPLY, len(reply), seq, sgaddr) + reply
    return out


ROUTES = {
    '/xpnfront/xpresence.srf': xpresence,
}
