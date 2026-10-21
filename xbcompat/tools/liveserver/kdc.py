"""The Kerberos side of Live on UDP 88: MACS (machine account creation), the
AS that hands out TGTs and the Xbox KDC's TGS that hands out security gateway
tickets.  Message layouts follow the leak's client (private/online/kerberos.cpp
and logon.cpp); see /mnt/project-files/xbcompat/network/live-protocol.md."""
import datetime
import json
import os
import struct

import der
from der import app, bits32, ctx, fields, gstr, integer, ktime, octets, seq
from kcrypto import compound_key, nonce_key, rc4hmac_decrypt, rc4hmac_encrypt

ETYPE_RC4 = 23
NT_ENTERPRISE, NT_SRV_INST = 10, 2

# Key usages the client uses (kerberos.cpp:295-316).
U_TIMESTAMP, U_TICKET, U_REP_PART, U_TGS_AUTH = 1, 2, 8, 7
U_CLIENT_VERSION = 1027
U_SERVICE_REQUEST, U_SERVICE_ADDRESS, U_ACCOUNT_CREATION = 1201, 1202, 1203

PA_TGS_REQ, PA_ENC_TIMESTAMP = 1, 2
PA_COMPOUND_IDENTITY = 130       # the TGT of the users already signed in (kerberos.cpp:1640)
PA_CLIENT_VERSION = 132          # the 2002 client: encrypted with the nonce key of 0
PA_CLIENT_VERSION_SIGNED = 206   # retail: HMAC-SHA1(nonce key of 0, text) then the text
PA_XBOX_SERVICE_REQUEST, PA_XBOX_SERVICE_ADDRESS, PA_XBOX_ACCOUNT_CREATION = 201, 202, 203

KDC_ERR_C_PRINCIPAL_UNKNOWN, KDC_ERR_PREAUTH_FAILED, KRB_ERR_GENERIC = 6, 24, 60

MACS_REALM = 'macs.xbox.com'
MACHINE_REALM = 'machines.xbox.com'
# The leak's logon.cpp always uses the devkit identity for MACS (inc/cryptkeys.h:35).
DEVKIT_SERIAL = '999999999999'
DEVKIT_ONLINE_KEY = bytes.fromhex('7B592E4F8178B4C75788531B2E747687')

TICKET_LIFE = datetime.timedelta(days=1)
DEFAULT_COUNTRY = 103           # United States, XONLINE_COUNTRY_*


def now():
    return datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0)


def principal(name_type, *names):
    return fields((0, integer(name_type)), (1, seq(*(gstr(n) for n in names))))


def parse_principal(content):
    f = der.ctx_fields(content)
    return [der.unwrap(raw).decode() for _, _, raw in der.items(f[1][0])]


def enc_data(key, usage, plain):
    return fields((0, integer(ETYPE_RC4)), (2, octets(rc4hmac_encrypt(key, usage, plain))))


def dec_data(key, usage, content):
    f = der.ctx_fields(content)
    return rc4hmac_decrypt(key, usage, f[2][0])


class Kdc:
    def __init__(self, db, site_ip, service_port, log):
        self.db = db
        self.site_ip = site_ip
        self.service_port = service_port
        self.log = log

    # ---- tickets: opaque to the client, so ours carry JSON ----------------

    def make_ticket(self, server_key, realm, sname, info):
        return app(1, fields((0, integer(5)), (1, gstr(realm)), (2, sname),
                             (3, enc_data(server_key, U_TICKET, json.dumps(info).encode()))))

    def open_ticket(self, server_key, ticket_raw):
        f = der.ctx_fields(der.unwrap(der.unwrap(ticket_raw)))
        plain = dec_data(server_key, U_TICKET, f[3][0])
        return json.loads(plain) if plain else None

    # ---- replies -----------------------------------------------------------

    def error(self, code, realm, sname_raw):
        t = now()
        self.log(f'kdc: error {code}')
        return app(30, fields((0, integer(5)), (1, integer(30)), (4, ktime(t)), (5, integer(0)),
                              (6, integer(code)), (9, gstr(realm)), (10, sname_raw)))

    def rep(self, msg_type, part_tag, padata, crealm, cname_raw, ticket, key, session_key, nonce,
            srealm, sname_raw, end):
        t = now()
        part = app(part_tag, fields(
            (0, fields((0, integer(ETYPE_RC4)), (1, octets(session_key)))),
            (1, seq(fields((0, integer(0)), (1, ktime(t))))),
            (2, integer(nonce - (1 << 32) if nonce >= 1 << 31 else nonce)),   # an int32 on the client
            (4, bits32(0x40E00000)),
            (5, ktime(t)), (6, ktime(t)), (7, ktime(end)),
            (9, gstr(srealm)), (10, sname_raw)))
        pa = seq(*(fields((1, integer(t_)), (2, octets(v))) for t_, v in padata)) if padata else None
        return app(msg_type, fields((0, integer(5)), (1, integer(msg_type)), (2, pa), (3, gstr(crealm)),
                                    (4, cname_raw), (5, ticket), (6, enc_data(key, U_REP_PART, part))))

    # ---- requests ----------------------------------------------------------

    def handle(self, data):
        tag = data[0]
        dump = os.environ.get('LIVESERVER_DUMP')   # a directory: each request and reply, for debugging
        if dump:
            self.seq = getattr(self, 'seq', 0) + 1
            with open(os.path.join(dump, f'{self.seq:03d}-req.bin'), 'wb') as f:
                f.write(data)
        reply = self._handle(data)
        if dump and reply:
            with open(os.path.join(dump, f'{self.seq:03d}-rep.bin'), 'wb') as f:
                f.write(reply)
        return reply

    def _handle(self, data):
        tag = data[0]
        try:
            if tag == 0x6A:
                return self.as_req(data)
            if tag == 0x6C:
                return self.tgs_req(data)
        except Exception as e:     # a malformed request gets no answer, as on a real KDC
            self.log(f'kdc: bad request ({e!r})')
            return None
        self.log(f'kdc: unknown message {tag:#x}')
        return None

    def parse_req(self, data):
        f = der.ctx_fields(der.unwrap(der.unwrap(data)))
        padata = {}
        if 3 in f:
            for _, c, _ in der.items(f[3][0]):
                pf = der.ctx_fields(c)
                padata[der.as_int(pf[1][0])] = der.unwrap(pf[2][1])
        body = der.ctx_fields(f[4][0])
        return padata, body, f[4][1]

    def as_req(self, data):
        padata, body, _ = self.parse_req(data)
        cname = parse_principal(body[1][0])[0]
        realm = der.unwrap(body[2][1]).decode()
        sname_raw = body[3][1]
        sname = parse_principal(body[3][0])
        nonce = der.as_int(body[7][0]) & 0xFFFFFFFF
        cname_raw = principal(NT_ENTERPRISE, cname)
        self.log(f'kdc: AS-REQ {cname}@{realm} for {"/".join(sname)}')

        if realm.lower() == MACS_REALM:
            keys = self.db.online_keys(cname)
            if not keys:
                return self.error(KDC_ERR_C_PRINCIPAL_UNKNOWN, realm, sname_raw)
        else:
            acct = self.db.find_principal(cname, realm)
            if acct is None:
                return self.error(KDC_ERR_C_PRINCIPAL_UNKNOWN, realm, sname_raw)
            keys = [bytes.fromhex(acct['key'])]

        # A logon with users chains AS-REQs: each user, then the machine, each
        # carrying the TGT before it and keyed with that TGT's session key too.
        users = []
        chain = padata.get(PA_COMPOUND_IDENTITY)
        if chain is not None:
            for _, _, raw in der.items(der.unwrap(chain)):
                prev = self.open_ticket(self.db.krbtgt_key(), raw)
                if prev is None:
                    return self.error(KDC_ERR_PREAUTH_FAILED, realm, sname_raw)
                users += prev.get('u', [])
                keys = [compound_key(k, bytes.fromhex(prev['k'])) for k in keys]

        ts = padata.get(PA_ENC_TIMESTAMP)
        key = ts is not None and next((k for k in keys if dec_data(k, U_TIMESTAMP, der.unwrap(ts)) is not None), None)
        if not key:
            self.log(f'kdc: pre-auth failed for {cname}@{realm} ({len(keys)} keys tried, '
                     f'PA types {sorted(padata)}); request {data.hex()}')
            return self.error(KDC_ERR_PREAUTH_FAILED, realm, sname_raw)

        retail = self.retail(padata, key, nonce)
        session = os.urandom(16)
        end = now() + TICKET_LIFE
        reply_pa = []
        if realm.lower() == MACS_REALM:
            acct = self.db.new_machine(cname)
            # {qwUserID, name, kingdom (2002 only), domain, realm, key}: retail
            # dropped the kingdom, so its record is 84 bytes instead of 96.
            fmt = '<Q16s0s20s24s16s' if retail else '<Q16s12s20s24s16s'
            blob = struct.pack(fmt, acct['xuid'], acct['name'].encode(), b'',
                               acct['domain'].encode(), acct['realm'].encode(), bytes.fromhex(acct['key']))
            reply_pa.append((PA_XBOX_ACCOUNT_CREATION,
                             enc_data(nonce_key(key, nonce), U_ACCOUNT_CREATION, blob)))
            info = {'k': session.hex(), 'macs': cname}
            self.log(f'kdc: MACS made machine {acct["name"]}@{acct["domain"]} xuid {acct["xuid"]:#x}')
        else:
            if not acct.get('machine', False):
                users.append(acct['xuid'])
            info = {'k': session.hex(), 'c': cname, 'r': realm, 'x': acct['xuid'],
                    'm': acct.get('machine', False), 'u': users}
            if users:
                self.log(f'kdc: TGT for {cname} carries users {[hex(u) for u in users]}')
        ticket = self.make_ticket(self.db.krbtgt_key(), realm, sname_raw, info)
        # Clients decrypt an AS-REP with the TGS-REP usage (8), retail too: they
        # pass KERB_ENCRYPTED_AS_REPLY_PDU where kerberos.cpp:3697 tests
        # KERB_AS_REPLY_PDU.
        return self.rep(11, 25, reply_pa, realm, cname_raw, ticket, key, session, nonce, realm, sname_raw, end)

    def client_version(self, padata, key, nonce):
        """PA 132, "Xbox Version=1.00.5849.3 Title=0x... TitleVersion=...", keyed
        with the nonce key of 0 (the 2002 client) or of the request's nonce."""
        pa = padata.get(PA_CLIENT_VERSION_SIGNED)
        if pa is not None and len(pa) > 20:
            return pa[20:].split(b'\0')[0].decode('latin-1')
        pa = padata.get(PA_CLIENT_VERSION)
        if pa is None:
            return None
        for n in (0, nonce):
            plain = dec_data(nonce_key(key, n), U_CLIENT_VERSION, der.unwrap(pa))
            if plain is not None:
                return plain.split(b'\0')[0].decode('latin-1')
        return None

    def retail(self, padata, key, nonce):
        v = self.client_version(padata, key, nonce)
        self.log(f'kdc: client {v!r}')
        try:
            return int(v.split('Version=')[1].split('.')[2]) >= 5000
        except (AttributeError, IndexError, ValueError):
            return False

    def user_flags(self, xuid):
        """A signed-in user's dwUserFlags: only the country (XONLINE_USER_COUNTRY_MASK,
        bits 8-15), which the dashboard needs to pick the account's country."""
        acct = self.db.user_by_xuid(xuid) if xuid else None
        return acct.get('country', DEFAULT_COUNTRY) << 8 if acct else 0

    def tgs_req(self, data):
        padata, body, _ = self.parse_req(data)
        realm = der.unwrap(body[2][1]).decode()
        sname_raw = body[3][1]
        sname = parse_principal(body[3][0])
        nonce = der.as_int(body[7][0]) & 0xFFFFFFFF
        self.log(f'kdc: TGS-REQ for {"/".join(sname)}@{realm}')

        apreq = padata.get(PA_TGS_REQ)
        if apreq is None:
            return self.error(KRB_ERR_GENERIC, realm, sname_raw)
        af = der.ctx_fields(der.unwrap(der.unwrap(apreq)))
        tgt = self.open_ticket(self.db.krbtgt_key(), af[3][1])
        if tgt is None or 'c' not in tgt:
            return self.error(KDC_ERR_PREAUTH_FAILED, realm, sname_raw)
        tgt_key = bytes.fromhex(tgt['k'])
        if dec_data(tgt_key, U_TGS_AUTH, af[4][0]) is None:
            return self.error(KDC_ERR_PREAUTH_FAILED, realm, sname_raw)

        services = []
        users = []
        slots = (0, 0, 0, 0)
        retail = False
        req = padata.get(PA_XBOX_SERVICE_REQUEST)
        if req is not None:
            plain = dec_data(nonce_key(tgt_key, nonce), U_SERVICE_REQUEST, der.unwrap(req))
            # 2002: {ver, ethernet[6], clientVersion[4], title, titleVer, region,
            # qwUserID[4], numServices, serviceID[12]}, 112 bytes. Retail: the
            # ethernet address became a u16 size (108), the rest moves up 4.
            if plain is not None and len(plain) == 108:
                retail, o = True, -4
            elif plain is not None and len(plain) == 112:
                o = 0
            else:
                self.log(f'kdc: service request of {None if plain is None else len(plain)} bytes')
                return self.error(KDC_ERR_PREAUTH_FAILED, realm, sname_raw)
            slots = struct.unpack_from('<4Q', plain, 28 + o)
            users = [u for u in slots if u]
            n = struct.unpack_from('<I', plain, 60 + o)[0]
            services = list(struct.unpack_from('<12I', plain, 64 + o)[:min(n, 12)])
            title = struct.unpack_from('<I', plain, 16 + o)[0]
            self.log(f'kdc: title {title:#010x} wants services {services}, users {[hex(u) for u in users]}')

        session = os.urandom(16)
        end = now() + TICKET_LIFE
        info = {'k': session.hex(), 'c': tgt['c'], 'r': tgt['r'], 'x': tgt['x'], 'u': users, 's': services}
        ticket = self.make_ticket(self.db.sg_key(), realm, sname_raw, info)
        # {hr, hrUser[4], dwUserFlags[4], (retail: 36 more bytes), siteIPAddress,
        # numServices, 12 x {serviceID, hr, port, reserved}}: 188 bytes, or 224 retail.
        flags = [self.user_flags(u) for u in slots]
        addr = struct.pack('<i4i4I', 0, 0, 0, 0, 0, *flags) + bytes(36 if retail else 0) + \
            bytes(self.site_ip) + struct.pack('<I', len(services))
        for i in range(12):
            if i < len(services):
                addr += struct.pack('<IiHH', services[i], 0, self.service_port, 0)
            else:
                addr += bytes(12)
        reply_pa = [(PA_XBOX_SERVICE_ADDRESS, enc_data(nonce_key(tgt_key, nonce), U_SERVICE_ADDRESS, addr))]
        cname_raw = principal(NT_ENTERPRISE, tgt['c'])
        return self.rep(13, 26, reply_pa, tgt['r'], cname_raw, ticket, tgt_key, session, nonce,
                        realm, sname_raw, end)
