#!/usr/bin/env python3
"""Make a 256-byte Xbox EEPROM (xemu's eeprom.bin) holding an xbcompat
console's identity, or check which kernel family an EEPROM is sealed for.

    mkeeprom.py CONSOLE-ID OUT.bin [--version r1|r2|r3|debug]
    mkeeprom.py --check EEPROM.bin

Insignia lets a console sign in only once its Setup Assistant has registered
it, and that homebrew drives the GPU itself, so it runs in xemu rather than
xbcompat.  With an EEPROM made from xbcompat's <hdd>/console-id, xemu is the
same console as far as the service is concerned: same serial, MAC, online key
and hard disk key.

Layout (xboxdevwiki, "EEPROM"): an encrypted section {hash[20], confounder[8],
hdd_key[16], region} sealed with HMAC-SHA1 under the kernel's EEPROM key and
RC4, then the factory section {checksum, serial[12], mac[6], pad, online_key[16],
video_standard, pad} and the user section {checksum, 156 bytes}.  The kernel
families use different EEPROM keys; their HMAC inner and outer states (after
the 64-byte key block) are below, as published in "The Middle Message".
"""
import argparse
import os
import struct
import sys

# HMAC-SHA1 states for each kernel family's EEPROM key: (inner, outer).
STATES = {
    'debug': ((0x85F9E51A, 0xE04613D2, 0x6D86A50C, 0x77C32E3C, 0x4BD717A4),
              (0x5D7A9C6B, 0xE1922BEB, 0xB82CCDBC, 0x3137AB34, 0x486B52B3)),
    'r1': ((0x72127625, 0x336472B9, 0xBE609BEA, 0xF55E226B, 0x99958DAC),     # 1.0 kernels
           (0x76441D41, 0x4DE82659, 0x2E8EF85E, 0xB256FACA, 0xC4FE2DE8)),
    'r2': ((0x39B06E79, 0xC9BD25E8, 0xDBC6B498, 0x40B4389D, 0x86BBD7ED),     # 1.1-1.4
           (0x9B49BED3, 0x84B430FC, 0x6B8749CD, 0xEBFE5FE5, 0xD96E7393)),
    'r3': ((0x8058763A, 0xF97D4E0E, 0x865A9762, 0x8A3D920D, 0x08995B2C),     # 1.6
           (0x01075307, 0xA2F1E037, 0x1186EEEA, 0x88DA9992, 0x168A5609)),
}


def _rol(v, s):
    return ((v << s) | (v >> (32 - s))) & 0xFFFFFFFF


def _sha1(state, data):
    """SHA-1 of data, continuing from state after one 64-byte block."""
    h = list(state)
    msg = data + b'\x80' + bytes((55 - len(data)) % 64) + struct.pack('>Q', (64 + len(data)) * 8)
    for off in range(0, len(msg), 64):
        w = list(struct.unpack('>16I', msg[off:off + 64]))
        for i in range(16, 80):
            w.append(_rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1))
        a, b, c, d, e = h
        for i in range(80):
            if i < 20:
                f, k = (b & c) | (~b & d), 0x5A827999
            elif i < 40:
                f, k = b ^ c ^ d, 0x6ED9EBA1
            elif i < 60:
                f, k = (b & c) | (b & d) | (c & d), 0x8F1BBCDC
            else:
                f, k = b ^ c ^ d, 0xCA62C1D6
            a, b, c, d, e = (_rol(a, 5) + f + e + k + w[i]) & 0xFFFFFFFF, a, _rol(b, 30), c, d
        h = [(x + y) & 0xFFFFFFFF for x, y in zip(h, (a, b, c, d, e))]
    return struct.pack('>5I', *h)


def hmac(version, data):
    inner, outer = STATES[version]
    return _sha1(outer, _sha1(inner, data))


def rc4(key, data):
    s = list(range(256))
    j = 0
    for i in range(256):
        j = (j + s[i] + key[i % len(key)]) & 0xFF
        s[i], s[j] = s[j], s[i]
    out, i, j = bytearray(), 0, 0
    for b in data:
        i = (i + 1) & 0xFF
        j = (j + s[i]) & 0xFF
        s[i], s[j] = s[j], s[i]
        out.append(b ^ s[(s[i] + s[j]) & 0xFF])
    return bytes(out)


def checksum(data):
    """The factory and user sections' checksum: a 64-bit end-around-carry sum, inverted."""
    high = low = 0
    for (v,) in struct.iter_unpack('<I', data):
        total = ((high << 32) | low) + v
        high = (total >> 32) & 0xFFFFFFFF
        low = (low + v) & 0xFFFFFFFF
    return ~(high + low) & 0xFFFFFFFF


def build(ident, version):
    serial = ident['serial'].encode().ljust(12, b'0')[:12]
    mac = bytes.fromhex(ident['mac'])
    online_key = bytes.fromhex(ident['online-key'])
    hdd_key = bytes.fromhex(ident['hdd-key'])
    secret = os.urandom(8) + hdd_key + struct.pack('<I', 1)             # confounder, key, region NA
    digest = hmac(version, secret)
    sealed = digest + rc4(hmac(version, digest), secret)
    factory = serial + mac + bytes(2) + online_key + struct.pack('<II', 0x00400100, 0)   # NTSC-M
    # user section: GMT time zone, English (xemu's defaults)
    user = bytearray(156)
    user[:44] = bytes.fromhex('00000000474d5400425354000000000000000000' '0a050002030500010000'
                              '00000000000000000000c4ffffff')
    struct.pack_into('<I', user, 0x2C, 1)
    eeprom = sealed + struct.pack('<I', checksum(factory)) + factory + \
        struct.pack('<I', checksum(bytes(user[:0x5C]))) + bytes(user)
    assert len(eeprom) == 256
    return eeprom


def check(eeprom):
    """The kernel families whose key opens this EEPROM's encrypted section."""
    digest, sealed = eeprom[:20], eeprom[20:48]
    found = []
    for version in STATES:
        secret = rc4(hmac(version, digest), sealed)
        if hmac(version, secret) == digest:
            found.append((version, secret))
    return found


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--check', metavar='EEPROM', help='say which kernel family an EEPROM is sealed for')
    ap.add_argument('--version', choices=sorted(STATES), default='r1',
                    help="the kernel family of the BIOS xemu runs (default r1, 1.0 kernels)")
    ap.add_argument('console_id', nargs='?')
    ap.add_argument('out', nargs='?')
    a = ap.parse_args()

    if a.check:
        eeprom = open(a.check, 'rb').read()
        for version, secret in check(eeprom):
            print(f'{version}: hdd key {secret[8:24].hex()}, serial {eeprom[52:64].decode(errors="replace")}, '
                  f'mac {eeprom[64:70].hex()}, online key {eeprom[72:88].hex()}')
            return
        sys.exit('mkeeprom: no known EEPROM key opens it')
    if not (a.console_id and a.out):
        ap.error('give CONSOLE-ID and OUT, or --check')
    ident = dict(l.split(None, 1) for l in open(a.console_id).read().splitlines() if ' ' in l)
    ident = {k: v.strip() for k, v in ident.items()}
    with open(a.out, 'wb') as f:
        f.write(build(ident, a.version))
    print(f'mkeeprom: {a.out} ({a.version}) for serial {ident["serial"]}')


if __name__ == '__main__':
    main()
