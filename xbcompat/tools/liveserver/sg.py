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
import xweb

SG_PORT = 3074
SERVICE_PORT = 80          # the TCP port the KDC hands out for every service

KEYEX_XBTOSG_INIT, KEYEX_SGTOXB_RESP = 0x0102, 0x0103
KEYEX_DH_GX, KEYEX_KERB_APREQ, KEYEX_KERB_APREP = 0x0110, 0x0112, 0x0113
# The retail xonline (5849) stack uses its own entry numbers for the same
# key exchange: INIT, g^X, AP-REQ and a padding entry going up, RESP, g^Y and
# AP-REP coming back.  It also 3DES-encrypts the address tail of the RESP that
# the beta leaves in clear.  See network/live-protocol.md.
RETAIL_INIT, RETAIL_RESP = 0x5802, 0x5803
RETAIL_DH, RETAIL_APREQ, RETAIL_APREP, RETAIL_PAD = 0x5880, 0x5882, 0x5883, 0x5886
SXRF_ENCRYPT_DES = 1
SECMSG_DELETE, SECMSG_PULSE, SECMSG_XBTOSG_PULSE, SECMSG_SGTOXB_PULSE = 0x0200, 0x0201, 0x0202, 0x0203
# Retail renumbers the secmsg types to a 0x53xx base but keeps the layouts from
# inc/xonlinep.h.  From xonlinedash's own xnet recv dispatch (0x1387a4):
# 0x5300 = DELETE ({_wType,_cbEnt=8,_dwReason}; the console tears the SA down
# at 0x136367 and, for the logon SA, keeps the reason at +0xd0c); 0x5301 =
# bare PULSE (cbEnt=4, ignored on receipt, only refreshes the SA's last-receive
# time); 0x5307 = XbToSg service-connect/poll (console->gateway, built at
# 0x13b392); 0x5308 = SgToXb service reply (gateway->console, parsed at
# 0x13c061).  Other recv types: 0x5304 re-key, 0x5306 disconnect, 0x5309
# SgToXb pulse with change list (logon SA only), 0x530c state ack.
RETAIL_SECMSG_DELETE, RETAIL_SECMSG_PULSE = 0x5300, 0x5301
RETAIL_SVC_REQ, RETAIL_SVC_RESP = 0x5307, 0x5308
PROTO_SECMSG, PROTO_TCP, PROTO_UDP = 0, 6, 17
U_SG_AUTH, U_AP_REP = 11, 12
CKSUM_SHA1 = -131

# Oakley group 1 (RFC 2409).  The retail 5849 dashboard calls XcModExp with the
# generator as the plain integer 2 and the modulus as the real group-1 prime in
# natural (big-endian) form; both confirmed live from the console's own XcModExp.
# (The leak trunk's base.cpp arrays, read as the little-endian bignum, give a
# different generator and modulus that the retail console does not use.)
_MOD = bytes.fromhex(
    'ffffffffffffffffc90fdaa22168c234c4c6628b80dc1cd129024e088a67cc74020bbea63b139b22514a08798e3404dd'
    'ef9519b3cd3a431b302b0a6df25f14374fe1356d6d51c245e485b576625e7ec6f44c42e9a63a3620ffffffffffffffff')
P = int.from_bytes(_MOD, 'big')
G = 2

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
            reply = (xweb if self.sa.retail else xuacs).handle(self.sa.gw, self.sa, self.inbuf)
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
    def __init__(self, gw, peer, spi_init, spi_resp, ticket, keys, cb_des, retail=False, ivs=(None, None)):
        self.gw, self.peer = gw, peer
        self.spi_init, self.spi_resp = spi_init, spi_resp
        self.ticket = ticket          # the service ticket's contents (xuid, users, services)
        self.retail = retail
        self.sha_recv, des_recv, self.sha_xmit, des_xmit = keys
        self.des_recv = Des(des_recv[:cb_des])
        self.des_xmit = Des(des_xmit[:cb_des])
        self.recv_iv, self.xmit_iv = ivs    # retail: chained CBC IVs, not sent on the wire
        if os.environ.get('LIVE_DEBUG'):
            gw.log(f'sg: SA keys sha_recv={self.sha_recv.hex()} des_recv={des_recv[:cb_des].hex()} '
                   f'sha_xmit={self.sha_xmit.hex()} des_xmit={des_xmit[:cb_des].hex()} '
                   f'recv_iv={self.recv_iv.hex() if self.recv_iv else None}')
        self.seq_xmit = 0
        self.seq_seen = set()
        self.tcp = {}
        self.last_recv = time.time()

    @staticmethod
    def _pkt_iv(seed, ctr):
        # The retail per-packet IV is a sequence-keyed mix of the 8-byte seed:
        # iv = (s1*ctr).hi ^ (s0*ctr).lo, (s1*ctr).lo ^ (s0*ctr).hi.  ctr==1 yields
        # the seed itself.  Nothing travels on the wire.
        s0 = int.from_bytes(seed[0:4], 'little')
        s1 = int.from_bytes(seed[4:8], 'little')
        p0, p1 = s0 * ctr, s1 * ctr
        d0 = ((p1 >> 32) ^ (p0 & 0xFFFFFFFF)) & 0xFFFFFFFF
        d1 = ((p1 & 0xFFFFFFFF) ^ (p0 >> 32)) & 0xFFFFFFFF
        return struct.pack('<II', d0, d1)

    def send(self, proto, payload):
        self.seq_xmit += 1
        if self.retail:
            self.send_retail(proto, payload)
            return
        pad = (-(len(payload) + 2)) % 8
        plain = payload + bytes(range(1, pad + 1)) + bytes([pad, proto])
        iv = os.urandom(8)
        body = struct.pack('<I', self.spi_init) + struct.pack('>I', self.seq_xmit) + iv + \
            self.des_xmit.cbc_encrypt(iv, plain)
        self.gw.sock.sendto(body + hmac_sha1(self.sha_xmit, body)[:12], self.peer)

    def esp(self, data, peer):
        if self.retail:
            return self.esp_retail(data, peer)
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
        self.dispatch(proto, payload)

    # Retail packets are spi(4) + cipher(8n) + clear header + seq(2 LE) + HMAC[0:10].
    # There is no ESP trailer.  The low byte of the SPI is a control byte
    # (xonlinedash 0x138a06): p = ctrl & 0x1f says what is inside (0 = secmsg,
    # else IP header compression: r = (p-1) % 3 ports carried, 0 = both 1000,
    # 1 = 1000 + one byte each, 2 = two big-endian words; q = p - r is the
    # protocol, 1 = TCP with seq, ack, offset/flags and window, 4 = UDP).  The
    # compressed header, H = 2r (+12 for TCP) bytes, is split: its first
    # m = min(v, H) bytes sit at the end of the cipher, where v = ctrl >> 5 counts
    # them plus the zero padding before them, and the rest follows the cipher in
    # clear.  The HMAC covers clear + seq(4 LE) + spi (zero padded to 4) + cipher.
    # The per-packet IV comes from the sequence and is never sent.

    @staticmethod
    def _layout(ctrl):
        p, v = ctrl & 0x1F, ctrl >> 5
        if p == 0:
            return p, v, 0, 0, 0
        r = (p - 1) % 3
        q = p - r
        hlen = 2 * r + (12 if q == 1 else 0)
        return q, v, r, hlen, min(v, hlen)

    def _mac(self, key, spi, seq, cipher, clear):
        head = clear + struct.pack('<I', seq) + spi
        return hmac_sha1(key, head + bytes((-len(head)) % 4) + cipher)[:10]

    def send_retail(self, proto, payload):
        if proto == PROTO_TCP:
            # Our own segments come in as plain TCP; send them as compressed
            # TCP with both ports given (p = 3) and the header tucked into the
            # cipher's padding as far as it goes.
            hdr, body = payload[:20], payload[20:]
            h = hdr[0:4] + hdr[4:16]
            v = (-len(body)) % 8
            plain, clear, ctrl = body + h[:v], h[v:], 3 | (v << 5)
        else:
            v = (-len(payload)) % 8
            plain, clear, ctrl = payload + bytes(v), b'', (v << 5)
        spi = struct.pack('<I', (self.spi_init & 0xFFFFFF00) | ctrl)
        cipher = self.des_xmit.cbc_encrypt(self._pkt_iv(self.xmit_iv, self.seq_xmit), plain) if plain else b''
        wire = spi + cipher + clear + struct.pack('<H', self.seq_xmit) + \
            self._mac(self.sha_xmit, spi, self.seq_xmit, cipher, clear)
        if os.environ.get('LIVE_DEBUG'):
            self.gw.log(f'sg: retail xmit seq={self.seq_xmit} ctrl={ctrl:#04x} plain={plain.hex()} '
                        f'clear={clear.hex()}')
        self.gw.sock.sendto(wire, self.peer)

    def esp_retail(self, data, peer):
        spi, seqn, mac = data[:4], data[-12:-10], data[-10:]
        ctrl = spi[0]
        q, v, r, hlen, m = self._layout(ctrl)
        clear_len = hlen - m
        clen = len(data) - 16 - clear_len
        if clen < 0 or clen % 8 or clen < v:
            self.gw.log(f'sg: retail ESP bad length {len(data)} (ctrl {ctrl:#04x})')
            return
        cipher, clear = data[4:4 + clen], data[4 + clen:4 + clen + clear_len]
        ctr = int.from_bytes(seqn, 'little')
        if not hmac.compare_digest(self._mac(self.sha_recv, spi, ctr, cipher, clear), mac):
            self.gw.log(f'sg: retail ESP ({len(data)} bytes) failed authentication')
            return
        plain = self.des_recv.cbc_decrypt(self._pkt_iv(self.recv_iv, ctr), cipher) if cipher else b''
        self.peer = peer
        self.last_recv = time.time()
        if os.environ.get('LIVE_DEBUG'):
            self.gw.log(f'sg: retail recv seq={ctr} ctrl={ctrl:#04x} proto={q} '
                        f'plain={plain.hex()} clear={clear.hex()}')
        if q == 0:
            self.dispatch(PROTO_SECMSG, plain, ctr)
            return
        h = plain[clen - m:clen] + clear
        body = plain[:clen - v]
        if r == 0:
            sport = dport = 1000
        elif r == 1:
            sport, dport = 1000 + h[0], 1000 + h[1]
        else:
            sport, dport = struct.unpack_from('>HH', h)
        if q == 1:
            # A plain TCP segment again: ports, seq, ack, offset/flags, window,
            # no checksum, then the options and data.
            self.tcp_in(struct.pack('>HH', sport, dport) + h[2 * r:2 * r + 12] + bytes(4) + body)
        else:
            self.gw.log(f'sg: retail compressed proto {q} {sport}->{dport} {body.hex()}')

    def dispatch(self, proto, payload, seq=0):
        if proto == PROTO_SECMSG:
            self.secmsg(payload, seq)
        elif self.retail:
            # Retail TCP/IP arrives under the compressed header variants (proto
            # 1/4/7/10), not a bare protocol number.  Not yet reconstructed.
            self.gw.log(f'sg: retail inner proto {proto} (compressed header) '
                        f'{payload.hex()}')
        elif proto == PROTO_TCP:
            self.tcp_in(payload)
        else:
            self.gw.log(f'sg: ignoring protocol {proto}')

    def secmsg(self, payload, seq=0):
        p = 0
        while p + 4 <= len(payload):
            t, cb = struct.unpack_from('<HH', payload, p)
            if t == 0 or cb < 4:
                break           # zero padding after the last entry
            if t in (SECMSG_DELETE,):
                self.gw.log(f'sg: console closed the association {self.spi_resp:#x}')
                self.gw.drop(self)
                return
            if t == SECMSG_XBTOSG_PULSE:
                seq_ack = struct.unpack_from('<I', payload, p + 4)[0] if cb >= 8 else 0
                self.send(PROTO_SECMSG, struct.pack('<HHI', SECMSG_SGTOXB_PULSE, 8, seq_ack))
            elif t == RETAIL_SECMSG_PULSE:
                # The console's keepalive.  Answer with our own bare pulse: any
                # authenticated packet refreshes its inbound timer.  Never answer
                # with 0x5300, which is DELETE and drops the association.
                self.send(PROTO_SECMSG, struct.pack('<HH', RETAIL_SECMSG_PULSE, 4))
            elif t == RETAIL_SECMSG_DELETE:
                self.gw.log(f'sg: console closed the association {self.spi_resp:#x}')
                self.gw.drop(self)
                return
            elif t == RETAIL_SVC_REQ:
                self.svc_req(payload[p:p + cb], seq)
            p += cb

    def svc_req(self, entry, seq=0):
        """A QoS probe to the data center (secmsg 0x5307, built at 0x13b392):
        this is XNetQosServiceLookup, which the connection test and sign-in use
        to decide whether Xbox Live is reachable.  Entry (16 bytes, more when a
        bandwidth probe pads it with zeros): type, cbEnt, 8-byte request nonce,
        16-bit target index, probe counter, flags (bit 0 = second packet of the
        pair, bit 1 = no probes wanted, only the target's QoS data).

        The console times each probe pair (0x13c061): a 0x5308 reply echoing
        nonce, index and counter, with flags bit 0 copied from the probe, and
        two dwords of time the responder held the packets (we answer at once,
        so 0).  A request for data only gets one reply with flags bit 2 set
        (data reply; the bytes after the 24-byte header are the target's QoS
        data, none here)."""
        nonce, index, counter, flags = entry[4:12], entry[12:14], entry[14], entry[15]
        if os.environ.get('LIVE_DEBUG'):
            self.gw.log(f'sg: qos probe nonce={nonce.hex()} target={index.hex()} '
                        f'n={counter} flags={flags:#x} cb={len(entry)}')
        rflags = 0x04 if flags & 0x02 else flags & 0x01
        resp = struct.pack('<HH', RETAIL_SVC_RESP, 0x18) + nonce + index + \
            bytes([counter, rflags]) + bytes(8)
        self.send(PROTO_SECMSG, resp)

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
        # The retail console transmits on its assigned SGADDR with the low byte
        # of the SPI set (a flag in the SGADDR encoding); our SA is keyed on the
        # SPI we handed out, whose low byte is zero.
        sa = self.sas.get(spi) or self.sas.get(spi & 0xFFFFFF00)
        if sa is None:
            self.log(f'sg: ESP for unknown SPI {spi:#x}')
            return
        if os.environ.get('LIVE_DEBUG'):
            self.log(f'sg: ESP spi={spi:#x} len={len(data)} {data.hex()}')
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

        retail = RETAIL_INIT in entries
        t_init = RETAIL_INIT if retail else KEYEX_XBTOSG_INIT
        t_dh = RETAIL_DH if retail else KEYEX_DH_GX
        t_apreq = RETAIL_APREQ if retail else KEYEX_KERB_APREQ
        if t_init not in entries or t_dh not in entries or t_apreq not in entries:
            self.log(f'sg: key exchange without INIT/DH/AP-REQ ({sorted(hex(t) for t in entries)})')
            return
        if retail:
            self._dump(data)

        init = entries[t_init]
        flags, spi_init = struct.unpack_from('<II', init)
        nonce_init = init[8:16]
        cache = (peer, spi_init, nonce_init)
        if cache in self.inits:
            self.sock.sendto(self.inits[cache], peer)
            return

        # The AP-REQ: our service ticket and an authenticator whose checksum covers INIT and g^X.
        af = der.ctx_fields(der.unwrap(der.unwrap(entries[t_apreq])))
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
        want = hmac_sha1(skey, raw[t_init] + raw[t_dh])
        if der.unwrap(ck[1][1]) != want:
            self.log('sg: key exchange checksum mismatch')
            return

        y = int.from_bytes(os.urandom(96), 'little')
        gy = pow(G, y, P).to_bytes(96, 'little')
        gxy = bytearray(pow(int.from_bytes(entries[t_dh], 'little'), y, P).to_bytes(96, 'little'))
        # Retail checks the responder SPI has a zero low byte and nonzero rest;
        # the beta only wants it nonzero.
        spi_resp = ((int.from_bytes(os.urandom(3), 'little') + 1) & 0xFFFFFF) << 8 if retail \
            else int.from_bytes(os.urandom(4), 'little') | 1
        nonce_resp = os.urandom(8)
        if os.environ.get('LIVE_DEBUG'):
            self.log(f'sg: dh y={y:x}')
            self.log(f'sg: dh gx={entries[t_dh].hex()}')
            self.log(f'sg: dh gy={gy.hex()}')
            self.log(f'sg: dh gxy={bytes(gxy).hex()}')
        d = b''
        # Retail needs a fifth HMAC: its last 16 bytes are the CBC IV seeds (the
        # console puts H4[0:8] on its transmit side, H4[8:16] on its receive side,
        # and the IVs are never sent on the wire).  The beta used four.
        for _ in range(5 if retail else 4):
            d += hmac_sha1(skey, bytes(gxy) + nonce_init + nonce_resp)
            gxy[0] = (gxy[0] + 1) & 0xFF
        # The console's transmit keys are our receive keys.
        keys = (d[0:16], des_parity(d[16:40]), d[40:56], des_parity(d[56:80]))
        ivs = (d[80:88], d[88:96]) if retail else (None, None)

        if retail:
            resp, dh, t_rep = self._retail_resp(spi_init, spi_resp, nonce_init, nonce_resp,
                                                keys, gy, ticket, peer)
        else:
            resp = struct.pack('<HHIII', KEYEX_SGTOXB_RESP, 62, SXRF_ENCRYPT_DES, spi_init, spi_resp) + \
                nonce_init + nonce_resp + struct.pack('<HH', 900, 30) + \
                self.site_ip + struct.pack('<IQI', spi_resp, ticket.get('x', 0), 0) + \
                socket_addr(peer)
            dh = struct.pack('<HH', KEYEX_DH_GX, 100) + gy
            t_rep = KEYEX_KERB_APREP
        subkey = hmac_sha1(skey, resp + dh)
        part = app(27, fields((0, auf[5][1]), (1, auf[4][1]),
                              (2, fields((0, integer(CKSUM_SHA1)), (1, octets(subkey))))))
        aprep = app(15, fields((0, integer(5)), (1, integer(15)),
                               (2, fields((0, integer(23)), (2, octets(rc4hmac_encrypt(skey, U_AP_REP, part)))))))
        reply = bytes(4) + resp + dh + struct.pack('<HH', t_rep, 4 + len(aprep)) + aprep
        self.inits[cache] = reply
        self.sock.sendto(reply, peer)

        sa = SecurityAssociation(self, peer, spi_init, spi_resp, ticket, keys, 8, retail, ivs)
        self.sas[spi_resp] = sa
        self.log(f'sg: {ticket.get("c")} connected ({"retail" if retail else "beta"}), '
                 f'services {ticket.get("s")}, SPI {spi_resp:#x}')
        if retail:
            # Retail: nothing to send until the console speaks.  It pulses
            # first (0x5301) and then probes QoS (0x5307); see secmsg().
            pass
        else:
            sa.send(PROTO_SECMSG, struct.pack('<HH', SECMSG_PULSE, 4))

    def _retail_resp(self, spi_init, spi_resp, nonce_init, nonce_resp, keys, gy, ticket, peer):
        """Build the retail SgToXbResp: a clear header through the two nonces, then a
        32-byte address tail 3DES-encrypted with our transmit key (the console's
        receive key), IV = nonce_resp."""
        import socket
        sgaddr = self.site_ip + struct.pack('<I', spi_resp) + struct.pack('<Q', ticket.get('x', 0)) + bytes(4)
        tail = sgaddr + socket.inet_aton(peer[0]) + struct.pack('>H', peer[1]) + \
            struct.pack('<HHH', 900, 30, 0)        # wXbToSgTimeoutInSecs, wXbToSgPulseTimeoutInSecs, pad
        enc = Des(keys[3]).cbc_encrypt(nonce_resp, tail)
        if os.environ.get('LIVE_DEBUG'):
            self.log(f'sg: retail tail key={keys[3].hex()} iv={nonce_resp.hex()} '
                     f'plain={tail.hex()} enc={enc.hex()}')
        resp = struct.pack('<HHHHII', RETAIL_RESP, 0x40, 0, SXRF_ENCRYPT_DES, spi_init, spi_resp) + \
            nonce_init + nonce_resp + enc
        dh = struct.pack('<HH', RETAIL_DH, 100) + gy
        return resp, dh, RETAIL_APREP

    def _dump(self, data):
        d = os.environ.get('LIVESERVER_DUMP')
        if not d:
            return
        import pathlib
        p = pathlib.Path(d)
        p.mkdir(parents=True, exist_ok=True)
        n = len(list(p.glob('sg-*.bin')))
        (p / f'sg-{n:03d}.bin').write_bytes(data)


def socket_addr(peer):
    import socket
    return socket.inet_aton(peer[0]) + struct.pack('>H', peer[1])
