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
import time

import xuacs

DEBUG = bool(os.environ.get('LIVE_DEBUG'))

# Country codes are the dwUserFlags country byte (XONLINE_USER_COUNTRY_MASK,
# bits 8-15) the dashboard selects a country by.
US = 103
COUNTRIES = {
    'US': {
        'LocaleCode': US,
        # Date fields in display order, counted from 0 (the date formatter at
        # 0x588c0 prints nothing for a position past 2).
        'DayPosition': 1, 'MonthPosition': 0, 'YearPosition': 2, 'DateSeparator': '/',
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
    if handler is None and path.lower().startswith(('/xuacs/', '/xbos/')):
        handler = account_default
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


# ---- the account service (/xuacs/*.ashx, /xbos/*.ashx) ----------------------
# Each call is a binary POST built at xonlinedash 0x12cf6d from the table at
# 0x19eb80 (URL, service 5); the client copies the reply body into a fixed
# buffer and reads its fields from there, so an empty 200 reads as all zeros.

def account_default(gw, sa, body):
    return b''


def ustr(s):
    b = s.encode()
    return struct.pack('<H', len(b)) + b


# What getpaymentinfo returns for every account: made up, so the account
# screens have something to show.  The reply (read at 0x120e33) is {BYTE
# country, BYTE language, DWORD, DWORD, BYTE, BYTE} then 17 strings as {WORD
# length, UTF-8}, in the order of the client's account-info struct: first name,
# last name, street 1-3, city, state, postal code, phone prefix, number and
# extension, e-mail, card number, card holder, and three more (bank fields).
BILLING = {
    'first': 'Sokky', 'last': 'Tester', 'street': ['1 Test Street', '', ''], 'city': 'Redmond',
    'state': 'WA', 'postal': '98052', 'phone': ['425', '5550100', ''], 'email': 'test@example.com',
    'card': '************1111', 'holder': 'Sokky Tester', 'bank': ['', '', ''],
}


def getpaymentinfo(gw, sa, body):
    xuid = struct.unpack_from('<Q', body)[0] if len(body) >= 8 else 0
    acct = gw.db.user_by_xuid(xuid) or {}
    b = dict(BILLING, first=acct.get('nick', BILLING['first']))
    strs = [b['first'], b['last'], *b['street'], b['city'], b['state'], b['postal'], *b['phone'],
            b['email'], b['card'], b['holder'], *b['bank']]
    return struct.pack('<BBIIBB', acct.get('country', 103), 1, 0, 0, 0, 0) + b''.join(ustr(x) for x in strs)


# Everyone has the same made-up subscription: twelve months that started a
# month ago, renewing into the same offer.  getsubscriptionstatus's reply (read
# at 0x121a9c, used by the dashboard at 0x5f2c9 and 0x68f8c) is {qwOfferID,
# BYTE status (0 pending, 1 enabled, 2 suspended, 3 expired, 4 canceled, 0x80
# expired without violations), BYTE, FILETIME start, FILETIME end (read at
# 0x5bbae/0x5bbe5), 4 bytes, BYTE renewal (0 none, 1 voucher, 2 normal),
# qwRenewalOfferID}, then the current offer's price and, with a renewal, the
# renewal's: {DWORD whole, BYTE fraction, BYTE currency format, char ISO
# code[3], DWORD flags}.  The currency format is xbosutil.cpp's: currency in
# bits 4-7 (13 = USD in the US), symbol first (bit 3), space (bit 2), has a
# fraction (bit 1), comma (bit 0).  In the flags bit 0 means the offer costs
# something (clear: "already paid for"), bits 1-3 are the frequency (0 one
# time, 1 monthly, 2 quarterly, 3 twice a year, 4 yearly; 0x5bd96), bits 4-7
# the term in months (0: no term) and bits 16-18 the tax wording (0 "Tax not
# included").
SUB_OFFER, RENEW_OFFER = 0x5849000000000001, 0x5849000000000002
OFFERS = {
    SUB_OFFER: ('Xbox Live 12-Month Subscription', 49, 99, 12),
    RENEW_OFFER: ('Xbox Live 12-Month Subscription', 49, 99, 12),
}
USD_FORMAT, YEARLY = 13 << 4 | 8 | 2, 4
DAY = 24 * 3600 * 10 ** 7
FILETIME_UNIX = 116444736000000000


def price(offer):
    _, whole, frac, months = OFFERS[offer]
    return struct.pack('<IBB3sI', whole, frac, USD_FORMAT, b'USD', 1 | YEARLY << 1 | months << 4)


def getsubscriptionstatus(gw, sa, body):
    start = FILETIME_UNIX + int(time.time()) * 10 ** 7 - 30 * DAY
    head = struct.pack('<QBxQQ4xBQ', SUB_OFFER, 1, start, start + 365 * DAY, 2, RENEW_OFFER)
    return head + price(SUB_OFFER) + price(RENEW_OFFER)


# OfferingDetails (request {qwUserID, qwMachineID, dwTitleID, qwOfferID,
# dwDescriptionIndex, WORD language}, built at 0x11a6e9) and
# OfferingDetailsNoUser ({BYTE, qwMachineID, dwTitleID, qwOfferID, DWORD,
# WORD}, 0x11a5db) reply {DWORD size, price as above minus the flags' split,
# DWORD blob size, blob} (read at 0x119fef/0x11a075); the dashboard's blob is
# an INX like the country files with a [SubInfo] section holding VERSION
# (must be 1), TITLE and DATA (the description; callback at 0x68df1).
def offer_details(gw, sa, body, offer):
    title = OFFERS.get(offer, OFFERS[SUB_OFFER])[0]
    blob = utf16('[SubInfo]\r\nVERSION="1"\r\n'
                 f'TITLE="{title}"\r\n'
                 f'DATA="{title}. Play online with friends for a year. This is a test server: '
                 'nothing is charged."\r\n')
    p = price(offer if offer in OFFERS else SUB_OFFER)
    return struct.pack('<I', 0x15 + len(blob)) + p + struct.pack('<I', len(blob)) + blob


def offeringdetails(gw, sa, body):
    offer = struct.unpack_from('<Q', body, 20)[0] if len(body) >= 28 else 0
    gw.log(f'xweb: offer details for {offer:#x}')
    return offer_details(gw, sa, body, offer)


def offeringdetailsnouser(gw, sa, body):
    offer = struct.unpack_from('<Q', body, 13)[0] if len(body) >= 21 else 0
    gw.log(f'xweb: offer details (no user) for {offer:#x}')
    return offer_details(gw, sa, body, offer)


# A subscription code (request {BYTE, ustr code}, built at 0x121b61) is good
# for the 12-month offer whatever it says; the reply starts with the offer ID
# (read at 0x121c0b).  verifyvoucher has no reply to read: an empty 200 is a
# good code.
def verifyvouchergetoffer(gw, sa, body):
    code, _ = read_ustr(body, 1) if len(body) >= 3 else ('', 0)
    gw.log(f'xweb: subscription code {code!r} -> offer {SUB_OFFER:#x}')
    return struct.pack('<Q', SUB_OFFER)


def read_ustr(body, pos):
    n = struct.unpack_from('<H', body, pos)[0]
    return body[pos + 2:pos + 2 + n].decode('utf-8', 'replace'), pos + 2 + n


# New Account.  reservename's request (built at 0x121dbc) is {qwMachineID,
# BYTE country, WORD wMaxNames, gamertag}; the reply (read at 0x120b71) is
# {WORD count, names}, each name a {WORD length (16 at most), UTF-8} string.
# A free name comes back as itself, a taken one as suggestions.
MAX_NAME = 15


def reservename(gw, sa, body):
    country, most = struct.unpack_from('<BH', body, 8)
    name, _ = read_ustr(body, 11)
    if not gw.db.find_user(name):
        names = [name]
    else:
        names = []
        n = 1
        while len(names) < max(1, min(most, 5)):
            alt = name[:MAX_NAME - len(str(n))] + str(n)
            if not gw.db.find_user(alt):
                names.append(alt)
            n += 1
    gw.log(f'xweb: reservename {name!r} (country {country}) -> {names}')
    return struct.pack('<H', len(names)) + b''.join(ustr(x) for x in names)


# createowneracct's request (built at 0x12248a) is {qwMachineID, DWORD, DWORD,
# DWORD, bytes and the account-info header, then strings from offset 0x22:
# gamertag first, then the billing strings getpaymentinfo returns}.  The reply
# (read at 0x121f40) is the new account {qwUserID, key[16], domain[20],
# realm[24]}; the dashboard then writes it to the hard disk itself.
def createowneracct(gw, sa, body):
    name, _ = read_ustr(body, 0x22)
    acct = gw.db.find_user(name) or gw.db.new_user(name, '', 'xbox.com', 'passport.net')
    gw.log(f'xweb: created {name} (xuid {acct["xuid"]:#x})')
    return struct.pack('<Q16s20s24s', acct['xuid'], bytes.fromhex(acct['key']),
                       acct['domain'].encode(), acct['realm'].encode())


ROUTES = {
    '/xpnfront/xpresence.srf': xpresence,
    '/xuacs/getpaymentinfo.ashx': getpaymentinfo,
    '/xuacs/reservename.ashx': reservename,
    '/xuacs/createowneracct.ashx': createowneracct,
    '/xuacs/getsubscriptionstatus.ashx': getsubscriptionstatus,
    '/xuacs/verifyvouchergetoffer.ashx': verifyvouchergetoffer,
    '/xbos/offeringdetails.ashx': offeringdetails,
    '/xbos/offeringdetailsnouser.ashx': offeringdetailsnouser,
}
