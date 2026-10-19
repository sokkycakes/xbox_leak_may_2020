"""Crypto for the Live server: RC4-HMAC (Kerberos etype 23 as the Xbox client
uses it, RFC 4757 with raw key usages), the Xbox nonce HMAC key, and DES/3DES-CBC
for the security gateway's ESP packets.  Standard library only."""
import hashlib
import hmac
import os
import struct


def hmac_md5(key, data):
    return hmac.new(key, data, hashlib.md5).digest()


def hmac_sha1(key, data):
    return hmac.new(key, data, hashlib.sha1).digest()


def rc4(key, data):
    s = list(range(256))
    j = 0
    for i in range(256):
        j = (j + s[i] + key[i % len(key)]) & 0xFF
        s[i], s[j] = s[j], s[i]
    out = bytearray(len(data))
    i = j = 0
    for n, b in enumerate(data):
        i = (i + 1) & 0xFF
        j = (j + s[i]) & 0xFF
        s[i], s[j] = s[j], s[i]
        out[n] = b ^ s[(s[i] + s[j]) & 0xFF]
    return bytes(out)


def rc4hmac_encrypt(key, usage, plain):
    k1 = hmac_md5(key, struct.pack('<I', usage))
    data = os.urandom(8) + plain
    cksum = hmac_md5(k1, data)
    return cksum + rc4(hmac_md5(k1, cksum), data)


def rc4hmac_decrypt(key, usage, cipher):
    """Plaintext, or None when the checksum does not match."""
    if len(cipher) < 24:
        return None
    k1 = hmac_md5(key, struct.pack('<I', usage))
    cksum = cipher[:16]
    data = rc4(hmac_md5(k1, cksum), cipher[16:])
    if not hmac.compare_digest(hmac_md5(k1, data), cksum):
        return None
    return data[8:]


def nonce_key(key, nonce):
    """KerbComputeKdcRequestNonceHmacKey: the key for the Xbox pre-auth entries."""
    ksign = hmac_md5(key, b'signaturekey\0')
    return hmac_md5(ksign, hashlib.md5(struct.pack('<II', 1026, nonce & 0xFFFFFFFF)).digest())


# ---- DES ------------------------------------------------------------------

_IP = [58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4, 62, 54, 46, 38, 30, 22, 14, 6,
       64, 56, 48, 40, 32, 24, 16, 8, 57, 49, 41, 33, 25, 17, 9, 1, 59, 51, 43, 35, 27, 19, 11, 3,
       61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7]
_FP = [40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31, 38, 6, 46, 14, 54, 22, 62, 30,
       37, 5, 45, 13, 53, 21, 61, 29, 36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27,
       34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9, 49, 17, 57, 25]
_E = [32, 1, 2, 3, 4, 5, 4, 5, 6, 7, 8, 9, 8, 9, 10, 11, 12, 13, 12, 13, 14, 15, 16, 17,
      16, 17, 18, 19, 20, 21, 20, 21, 22, 23, 24, 25, 24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1]
_P = [16, 7, 20, 21, 29, 12, 28, 17, 1, 15, 23, 26, 5, 18, 31, 10,
      2, 8, 24, 14, 32, 27, 3, 9, 19, 13, 30, 6, 22, 11, 4, 25]
_PC1 = [57, 49, 41, 33, 25, 17, 9, 1, 58, 50, 42, 34, 26, 18, 10, 2, 59, 51, 43, 35, 27, 19, 11, 3,
        60, 52, 44, 36, 63, 55, 47, 39, 31, 23, 15, 7, 62, 54, 46, 38, 30, 22, 14, 6, 61, 53, 45, 37,
        29, 21, 13, 5, 28, 20, 12, 4]
_PC2 = [14, 17, 11, 24, 1, 5, 3, 28, 15, 6, 21, 10, 23, 19, 12, 4, 26, 8, 16, 7, 27, 20, 13, 2,
        41, 52, 31, 37, 47, 55, 30, 40, 51, 45, 33, 48, 44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32]
_SHIFTS = [1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1]
_S = [
    [14, 4, 13, 1, 2, 15, 11, 8, 3, 10, 6, 12, 5, 9, 0, 7, 0, 15, 7, 4, 14, 2, 13, 1, 10, 6, 12, 11, 9, 5, 3, 8,
     4, 1, 14, 8, 13, 6, 2, 11, 15, 12, 9, 7, 3, 10, 5, 0, 15, 12, 8, 2, 4, 9, 1, 7, 5, 11, 3, 14, 10, 0, 6, 13],
    [15, 1, 8, 14, 6, 11, 3, 4, 9, 7, 2, 13, 12, 0, 5, 10, 3, 13, 4, 7, 15, 2, 8, 14, 12, 0, 1, 10, 6, 9, 11, 5,
     0, 14, 7, 11, 10, 4, 13, 1, 5, 8, 12, 6, 9, 3, 2, 15, 13, 8, 10, 1, 3, 15, 4, 2, 11, 6, 7, 12, 0, 5, 14, 9],
    [10, 0, 9, 14, 6, 3, 15, 5, 1, 13, 12, 7, 11, 4, 2, 8, 13, 7, 0, 9, 3, 4, 6, 10, 2, 8, 5, 14, 12, 11, 15, 1,
     13, 6, 4, 9, 8, 15, 3, 0, 11, 1, 2, 12, 5, 10, 14, 7, 1, 10, 13, 0, 6, 9, 8, 7, 4, 15, 14, 3, 11, 5, 2, 12],
    [7, 13, 14, 3, 0, 6, 9, 10, 1, 2, 8, 5, 11, 12, 4, 15, 13, 8, 11, 5, 6, 15, 0, 3, 4, 7, 2, 12, 1, 10, 14, 9,
     10, 6, 9, 0, 12, 11, 7, 13, 15, 1, 3, 14, 5, 2, 8, 4, 3, 15, 0, 6, 10, 1, 13, 8, 9, 4, 5, 11, 12, 7, 2, 14],
    [2, 12, 4, 1, 7, 10, 11, 6, 8, 5, 3, 15, 13, 0, 14, 9, 14, 11, 2, 12, 4, 7, 13, 1, 5, 0, 15, 10, 3, 9, 8, 6,
     4, 2, 1, 11, 10, 13, 7, 8, 15, 9, 12, 5, 6, 3, 0, 14, 11, 8, 12, 7, 1, 14, 2, 13, 6, 15, 0, 9, 10, 4, 5, 3],
    [12, 1, 10, 15, 9, 2, 6, 8, 0, 13, 3, 4, 14, 7, 5, 11, 10, 15, 4, 2, 7, 12, 9, 5, 6, 1, 13, 14, 0, 11, 3, 8,
     9, 14, 15, 5, 2, 8, 12, 3, 7, 0, 4, 10, 1, 13, 11, 6, 4, 3, 2, 12, 9, 5, 15, 10, 11, 14, 1, 7, 6, 0, 8, 13],
    [4, 11, 2, 14, 15, 0, 8, 13, 3, 12, 9, 7, 5, 10, 6, 1, 13, 0, 11, 7, 4, 9, 1, 10, 14, 3, 5, 12, 2, 15, 8, 6,
     1, 4, 11, 13, 12, 3, 7, 14, 10, 15, 6, 8, 0, 5, 9, 2, 6, 11, 13, 8, 1, 4, 10, 7, 9, 5, 0, 15, 14, 2, 3, 12],
    [13, 2, 8, 4, 6, 15, 11, 1, 10, 9, 3, 14, 5, 0, 12, 7, 1, 15, 13, 8, 10, 3, 7, 4, 12, 5, 6, 11, 0, 14, 9, 2,
     7, 11, 4, 1, 9, 12, 14, 2, 0, 6, 10, 13, 15, 3, 5, 8, 2, 1, 14, 7, 4, 10, 8, 13, 15, 12, 9, 0, 3, 5, 6, 11],
]


def _perm(v, table, width):
    out = 0
    for t in table:
        out = (out << 1) | ((v >> (width - t)) & 1)
    return out


def _schedule(key8):
    cd = _perm(int.from_bytes(key8, 'big'), _PC1, 64)
    c, d = cd >> 28, cd & 0xFFFFFFF
    ks = []
    for s in _SHIFTS:
        c = ((c << s) | (c >> (28 - s))) & 0xFFFFFFF
        d = ((d << s) | (d >> (28 - s))) & 0xFFFFFFF
        ks.append(_perm((c << 28) | d, _PC2, 56))
    return ks


# The S-box and P permutation merged into one table per S-box.
_SP = [[_perm(_S[i][((x & 32) >> 4 | (x & 1)) * 16 + ((x >> 1) & 15)] << (28 - 4 * i), _P, 32)
        for x in range(64)] for i in range(8)]


def _block(v, ks):
    v = _perm(v, _IP, 64)
    l, r = v >> 32, v & 0xFFFFFFFF
    for k in ks:
        x = _perm(r, _E, 32) ^ k
        f = 0
        for i in range(8):
            f |= _SP[i][(x >> (42 - 6 * i)) & 63]
        l, r = r, l ^ f
    return _perm((r << 32) | l, _FP, 64)


class Des:
    """DES (8-byte key) or 3DES EDE (24-byte key, first key applied first), CBC."""

    def __init__(self, key):
        self.keys = [_schedule(key[i:i + 8]) for i in range(0, len(key), 8)]

    def _enc(self, v):
        if len(self.keys) == 1:
            return _block(v, self.keys[0])
        return _block(_block(_block(v, self.keys[0]), self.keys[1][::-1]), self.keys[2])

    def _dec(self, v):
        if len(self.keys) == 1:
            return _block(v, self.keys[0][::-1])
        return _block(_block(_block(v, self.keys[2][::-1]), self.keys[1]), self.keys[0][::-1])

    def cbc_encrypt(self, iv, data):
        fb = int.from_bytes(iv, 'big')
        out = bytearray()
        for i in range(0, len(data), 8):
            fb = self._enc(int.from_bytes(data[i:i + 8], 'big') ^ fb)
            out += fb.to_bytes(8, 'big')
        return bytes(out)

    def cbc_decrypt(self, iv, data):
        fb = int.from_bytes(iv, 'big')
        out = bytearray()
        for i in range(0, len(data), 8):
            c = int.from_bytes(data[i:i + 8], 'big')
            out += (self._dec(c) ^ fb).to_bytes(8, 'big')
            fb = c
        return bytes(out)


def des_parity(key):
    return bytes((b & 0xFE) | (bin(b & 0xFE).count('1') & 1 ^ 1) for b in key)


if __name__ == '__main__':
    # Known answers: FIPS 46 example and the SP 800-67 3DES vector.
    d = Des(bytes.fromhex('133457799BBCDFF1'))
    assert d.cbc_encrypt(bytes(8), bytes.fromhex('0123456789ABCDEF')).hex() == '85e813540f0ab405'
    t = Des(bytes.fromhex('0123456789ABCDEF23456789ABCDEF01456789ABCDEF0123'))
    c = t.cbc_encrypt(bytes(8), b'The qufc')
    assert c.hex() == 'a826fd8ce53b855f', c.hex()
    assert t.cbc_decrypt(bytes(8), c) == b'The qufc'
    k = os.urandom(16)
    assert rc4hmac_decrypt(k, 7, rc4hmac_encrypt(k, 7, b'hello')) == b'hello'
    print('kcrypto: ok')
