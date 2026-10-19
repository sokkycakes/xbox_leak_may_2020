"""The security gateway on UDP 3074: Kerberos-authenticated Diffie-Hellman key
exchange, then ESP-in-UDP carrying bare TCP to the services behind it, which
this gateway terminates itself (xuacs.py).  Layouts follow the leak's
private/ntos/net/ip.cpp and inc/xonlinep.h."""
import hashlib
import hmac
import os
import struct
import time

import der
from der import app, ctx, fields, integer, octets, seq
from kcrypto import Des, des_parity, hmac_sha1, rc4hmac_decrypt, rc4hmac_encrypt
import xuacs

SG_PORT = 3074
SERVICE_PORT = 80          # the TCP port the KDC hands out for every service

KEYEX_XBTOSG_INIT, KEYEX_SGTOXB_RESP = 0x0102, 0x0103
KEYEX_DH_GX, KEYEX_KERB_APREQ, KEYEX_KERB_APREP = 0x0110, 0x0112, 0x0113
SXRF_ENCRYPT_DES = 1
SECMSG_DELETE, SECMSG_PULSE, SECMSG_XBTOSG_PULSE, SECMSG_SGTOXB_PULSE = 0x0200, 0x0201, 0x0202, 0x0203
PROTO_SECMSG, PROTO_TCP, PROTO_UDP = 0, 6, 17
U_SG_AUTH, U_AP_REP = 11, 12
CKSUM_SHA1 = -131

# Oakley group 1, as the client's little-endian bignum code reads the byte arrays
# in ntos/net/base.cpp.
_MOD = bytes.fromhex(
    'ffffffffffffffffc90fdaa22168c234c4c6628b80dc1cd129024e088a67cc74020bbea63b139b22514a08798e3404dd'
    'ef9519b3cd3a431b302b0a6df25f14374fe1356d6d51c245e485b576625e7ec6f44c42e9a63a3620ffffffffffffffff')
P = int.from_bytes(_MOD, 'little')
G = 2 << (95 * 8)

TCP_FIN, TCP_SYN, TCP_RST, TCP_PSH, TCP_ACK = 1, 2, 4, 8, 16
MSS = 1024
DEBUG = bool(os.environ.get('LIVE_DEBUG'))


class Tcp:
    """One TCP connection inside a security association, ending at a service."""

    def __init__(self, sa, sport, dport, seq0):
        self.sa, self.sport, self.dport = sa, sport, dport
        self.rcv_nxt = (seq0 + 1) & 0xFFFFFFFF
        self.snd_nxt = int.from_bytes(os.urandom(4), 'big')
        self.inbuf = b''
        self.closed = False

    def send(self, flags, data=b'', opts=b''):
        hdr = struct.pack('>HHIIBBHHH', self.dport, self.sport, self.snd_nxt, self.rcv_nxt,
                          (5 + len(opts) // 4) << 4, flags, 8192, 0, 0) + opts
        self.sa.send(PROTO_TCP, hdr + data)
        self.snd_nxt = (self.snd_nxt + len(data) + (1 if flags & (TCP_SYN | TCP_FIN) else 0)) & 0xFFFFFFFF

    def segment(self, seqn, flags, data):
        if flags & TCP_RST:
            self.closed = True
            return
        if data and seqn == self.rcv_nxt:
            self.inbuf += data
            self.rcv_nxt = (self.rcv_nxt + len(data)) & 0xFFFFFFFF
            self.send(TCP_ACK)
            reply = xuacs.handle(self.sa.gw, self.sa, self.inbuf)
            if reply is not None:
                for i in range(0, len(reply), MSS):
                    self.send(TCP_ACK | TCP_PSH, reply[i:i + MSS])
                self.send(TCP_ACK | TCP_FIN)
        elif data:
            self.send(TCP_ACK)     # out of order or a resend: say what we expect
        if flags & TCP_FIN and seqn + len(data) == self.rcv_nxt:
            self.rcv_nxt = (self.rcv_nxt + 1) & 0xFFFFFFFF
            self.send(TCP_ACK)
            self.closed = True


class SecurityAssociation:
    def __init__(self, gw, peer, spi_init, spi_resp, ticket, keys, cb_des):
        self.gw, self.peer = gw, peer
        self.spi_init, self.spi_resp = spi_init, spi_resp
        self.ticket = ticket          # the service ticket's contents (xuid, users, services)
        self.sha_recv, des_recv, self.sha_xmit, des_xmit = keys
        self.des_recv = Des(des_recv[:cb_des])
        self.des_xmit = Des(des_xmit[:cb_des])
        self.seq_xmit = 0
        self.seq_seen = set()
        self.tcp = {}
        self.last_recv = time.time()

    def send(self, proto, payload):
        pad = (-(len(payload) + 2)) % 8
        plain = payload + bytes(range(1, pad + 1)) + bytes([pad, proto])
        iv = os.urandom(8)
        self.seq_xmit += 1
        body = struct.pack('<I', self.spi_init) + struct.pack('>I', self.seq_xmit) + iv + \
            self.des_xmit.cbc_encrypt(iv, plain)
        self.gw.sock.sendto(body + hmac_sha1(self.sha_xmit, body)[:12], self.peer)

    def esp(self, data, peer):
        body, mac = data[:-12], data[-12:]
        if len(body) < 16 or (len(body) - 16) % 8 or not hmac.compare_digest(hmac_sha1(self.sha_recv, body)[:12], mac):
            self.gw.log(f'sg: ESP packet ({len(data)} bytes) failed authentication')
            return
        seqn = struct.unpack_from('>I', body, 4)[0]
        if seqn in self.seq_seen:
            return
        self.seq_seen.add(seqn)
        self.peer = peer
        self.last_recv = time.time()
        plain = self.des_recv.cbc_decrypt(body[8:16], body[16:])
        pad, proto = plain[-2], plain[-1]
        payload = plain[:-2 - pad]
        if proto == PROTO_SECMSG:
            self.secmsg(payload)
        elif proto == PROTO_TCP:
            self.tcp_in(payload)
        else:
            self.gw.log(f'sg: ignoring protocol {proto}')

    def secmsg(self, payload):
        p = 0
        while p + 4 <= len(payload):
            t, cb = struct.unpack_from('<HH', payload, p)
            if t == SECMSG_DELETE:
                self.gw.log(f'sg: console closed the association {self.spi_resp:#x}')
                self.gw.drop(self)
                return
            if t == SECMSG_XBTOSG_PULSE:
                seq_ack = struct.unpack_from('<I', payload, p + 4)[0] if cb >= 8 else 0
                self.send(PROTO_SECMSG, struct.pack('<HHI', SECMSG_SGTOXB_PULSE, 8, seq_ack))
            if cb < 4:
                break
            p += cb

    def tcp_in(self, seg):
        sport, dport, seqn, ackn, off, flags = struct.unpack_from('>HHIIBB', seg)
        data = seg[(off >> 4) * 4:]
        if DEBUG:
            self.gw.log(f'sg: tcp {sport}->{dport} flags {flags:#04x} seq {seqn} ack {ackn} len {len(data)}')
        key = (sport, dport)
        conn = self.tcp.get(key)
        if flags & TCP_SYN:
            if conn is None or conn.closed:
                conn = self.tcp[key] = Tcp(self, sport, dport, seqn)
                conn.send(TCP_SYN | TCP_ACK, opts=struct.pack('>BBH', 2, 4, MSS))
                self.gw.log(f'sg: TCP connection from port {sport} to service port {dport}')
            return
        if conn is None:
            return
        conn.segment(seqn, flags, data)
        if conn.closed:
            self.tcp.pop(key, None)


class Gateway:
    def __init__(self, db, site_ip, log, sock):
        self.db, self.site_ip, self.log, self.sock = db, site_ip, log, sock
        self.sas = {}        # our SPI -> SecurityAssociation
        self.inits = {}      # (peer, client SPI, nonce) -> reply, for retransmitted INITs

    def drop(self, sa):
        self.sas.pop(sa.spi_resp, None)

    def tick(self):
        now = time.time()
        for sa in list(self.sas.values()):
            if now - sa.last_recv > 900:
                self.drop(sa)

    def packet(self, data, peer):
        if len(data) < 8:
            return
        spi = struct.unpack_from('<I', data)[0]
        if spi == 0:
            self.keyex(data[4:], peer)
            return
        sa = self.sas.get(spi)
        if sa is None:
            self.log(f'sg: ESP for unknown SPI {spi:#x}')
            return
        sa.esp(data, peer)

    def keyex(self, data, peer):
        entries = {}
        raw = {}
        p = 0
        while p + 4 <= len(data):
            t, cb = struct.unpack_from('<HH', data, p)
            if cb < 4 or p + cb > len(data):
                break
            entries[t] = data[p + 4:p + cb]
            raw[t] = data[p:p + cb]
            p += cb
        if KEYEX_XBTOSG_INIT not in entries or KEYEX_DH_GX not in entries or KEYEX_KERB_APREQ not in entries:
            self.log(f'sg: key exchange without INIT/DH/AP-REQ ({sorted(hex(t) for t in entries)})')
            return
        init = entries[KEYEX_XBTOSG_INIT]
        flags, spi_init = struct.unpack_from('<II', init)
        nonce_init = init[8:16]
        cache = (peer, spi_init, nonce_init)
        if cache in self.inits:
            self.sock.sendto(self.inits[cache], peer)
            return

        # The AP-REQ: our service ticket and an authenticator whose checksum covers INIT and g^X.
        af = der.ctx_fields(der.unwrap(der.unwrap(entries[KEYEX_KERB_APREQ])))
        tf = der.ctx_fields(der.unwrap(der.unwrap(af[3][1])))
        import json
        ef = der.ctx_fields(tf[3][0])
        plain = rc4hmac_decrypt(self.db.sg_key(), 2, ef[2][0])
        if plain is None:
            self.log('sg: service ticket does not decrypt (old server keys?)')
            return
        ticket = json.loads(plain)
        skey = bytes.fromhex(ticket['k'])
        auth = rc4hmac_decrypt(skey, U_SG_AUTH, der.ctx_fields(af[4][0])[2][0])
        if auth is None:
            self.log('sg: authenticator does not decrypt')
            return
        auf = der.ctx_fields(der.unwrap(der.unwrap(auth)))
        ck = der.ctx_fields(auf[3][0])
        want = hmac_sha1(skey, raw[KEYEX_XBTOSG_INIT] + raw[KEYEX_DH_GX])
        if der.unwrap(ck[1][1]) != want:
            self.log('sg: key exchange checksum mismatch')
            return

        y = int.from_bytes(os.urandom(96), 'little')
        gy = pow(G, y, P).to_bytes(96, 'little')
        gxy = bytearray(pow(int.from_bytes(entries[KEYEX_DH_GX], 'little'), y, P).to_bytes(96, 'little'))
        spi_resp = int.from_bytes(os.urandom(4), 'little') | 1
        nonce_resp = os.urandom(8)
        d = b''
        for _ in range(4):
            d += hmac_sha1(skey, bytes(gxy) + nonce_init + nonce_resp)
            gxy[0] = (gxy[0] + 1) & 0xFF
        # The console's transmit keys are our receive keys.
        keys = (d[0:16], des_parity(d[16:40]), d[40:56], des_parity(d[56:80]))

        resp = struct.pack('<HHIII', KEYEX_SGTOXB_RESP, 62, SXRF_ENCRYPT_DES, spi_init, spi_resp) + \
            nonce_init + nonce_resp + struct.pack('<HH', 900, 30) + \
            self.site_ip + struct.pack('<IQI', spi_resp, ticket.get('x', 0), 0) + \
            socket_addr(peer)
        dh = struct.pack('<HH', KEYEX_DH_GX, 100) + gy
        subkey = hmac_sha1(skey, resp + dh)
        part = app(27, fields((0, auf[5][1]), (1, auf[4][1]),
                              (2, fields((0, integer(CKSUM_SHA1)), (1, octets(subkey))))))
        aprep = app(15, fields((0, integer(5)), (1, integer(15)),
                               (2, fields((0, integer(23)), (2, octets(rc4hmac_encrypt(skey, U_AP_REP, part)))))))
        reply = bytes(4) + resp + dh + struct.pack('<HH', KEYEX_KERB_APREP, 4 + len(aprep)) + aprep
        self.inits[cache] = reply
        self.sock.sendto(reply, peer)

        sa = SecurityAssociation(self, peer, spi_init, spi_resp, ticket, keys, 8)
        self.sas[spi_resp] = sa
        self.log(f'sg: {ticket.get("c")} connected, services {ticket.get("s")}, SPI {spi_resp:#x}')
        sa.send(PROTO_SECMSG, struct.pack('<HH', SECMSG_PULSE, 4))


def socket_addr(peer):
    import socket
    return socket.inet_aton(peer[0]) + struct.pack('>H', peer[1])
