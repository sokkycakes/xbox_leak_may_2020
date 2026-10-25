#!/usr/bin/env python3
"""Put a Live account (gamertag) on an xbcompat hard disk, the way a console
stores one after signing up, and give it to the Live server so it can sign in.

    mkuser.py --hdd DIR --db live.json GAMERTAG [GAMERTAG...]

The retail (5849) xonline library keeps accounts in the config sectors of
partition 0 (xonlinedash 0x1197c0): config sector 3 is a directory of eight
12-byte entries {DWORD in use, BYTE iv[8]}, and the account of entry i is a
108-byte record at the start of config sector 4 + i: {qwUserID, dwUserFlags,
name[16], dwUserOptions, passcode[4], domain[20], realm[24], key[16],
dwSignatureTime, signature[8]}.  The key is stored 3DES-CBC encrypted under
the console's hard disk key (MorphKeyWithKey in inc/cryptkeys.h) with the
entry's IV, and the signature is HMAC-SHA1(hard disk key, the first 100 bytes
with the key in clear)[0:8] (verify at 0x119396; the same scheme, with the
fixed KEY_MORPH_IV, checks out on the machine account the console writes).
The hard disk key is xbcompat's, from DIR/console-id.
"""
import argparse
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from db import Db                                    # noqa: E402
from kcrypto import Des, des_parity, hmac_sha1      # noqa: E402

USER_DOMAIN = 'xbox.com'
USER_REALM = 'passport.net'

CONFIG_SECTOR_INDEX = 8          # XBOX_CONFIG_SECTOR_INDEX
DIRECTORY_SECTOR, FIRST_USER_SECTOR, MAX_USERS, RECORD = 3, 4, 8, 108
BEGIN_SIG, END_SIG = 0x79132568, 0xAA550000

KEY_MORPH_RAND_KEY1 = bytes.fromhex('2BB8D9EFD2046D9D1F39B15B465801D7')
KEY_MORPH_RAND_KEY2 = bytes.fromhex('1E05D73AA4206A7BA05BCDDFAD26D3DE')
KEY_MORPH_IV = bytes.fromhex('7b35a8b727ed437a')
BASE_SIGNATURE_TIME = 0x01BF5C72FEFB6A60   # Jan 2000, in FILETIME units
FILETIME_UNIX = 116444736000000000


def morph_cipher(hdd_key):
    k = bytearray(24)
    k[0:20] = hmac_sha1(KEY_MORPH_RAND_KEY1, hdd_key)
    k[4:24] = hmac_sha1(KEY_MORPH_RAND_KEY2, hdd_key)
    return Des(des_parity(bytes(k)))


def config_checksum(sector):
    s = 0
    for (v,) in struct.iter_unpack('<I', sector):
        s += v
    while s >> 32:
        s = (s & 0xFFFFFFFF) + (s >> 32)
    return s


def write_config_sector(img, index, data):
    sector = struct.pack('<III', BEGIN_SIG, 1, 1) + data.ljust(492, b'\0') + struct.pack('<II', 0, END_SIG)
    sector = sector[:504] + struct.pack('<I', ~config_checksum(sector) & 0xFFFFFFFF) + sector[508:]
    img.seek((CONFIG_SECTOR_INDEX + index) * 512)
    img.write(sector)


def read_config_sector(img, index):
    img.seek((CONFIG_SECTOR_INDEX + index) * 512)
    s = img.read(512)
    if len(s) < 512 or struct.unpack_from('<I', s)[0] != BEGIN_SIG:
        return bytes(492)
    return s[12:504]


def record(acct, hdd_key, iv):
    stamp = (time.time_ns() // 100 + FILETIME_UNIX - BASE_SIGNATURE_TIME) // 20000000
    key = bytes.fromhex(acct['key'])
    head = struct.pack('<QI16sI4s20s24s', acct['xuid'], 0, acct['nick'].encode()[:15], 0, bytes(4),
                       acct['domain'].encode(), acct['realm'].encode())
    sig = hmac_sha1(hdd_key, head + key + struct.pack('<I', stamp))[:8]
    return head + morph_cipher(hdd_key).cbc_encrypt(iv, key) + struct.pack('<I', stamp) + sig


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--hdd', required=True, help="xbcompat hard disk directory (console-id, partition0.img)")
    ap.add_argument('--db', required=True, help="the Live server's database (live.json)")
    ap.add_argument('gamertags', nargs='+')
    a = ap.parse_args()

    ident = dict(l.split(None, 1) for l in open(os.path.join(a.hdd, 'console-id')).read().splitlines() if ' ' in l)
    hdd_key = bytes.fromhex(ident['hdd-key'].strip())
    db = Db(a.db)
    path = os.path.join(a.hdd, 'partition0.img')
    if not os.path.exists(path):
        with open(path, 'wb') as f:
            f.truncate(1 << 20)
    with open(path, 'r+b') as img:
        directory = bytearray(read_config_sector(img, DIRECTORY_SECTOR))
        for tag in a.gamertags:
            acct = db.find_user(tag) or db.new_user(tag, '', USER_DOMAIN, USER_REALM)
            users = [read_config_sector(img, FIRST_USER_SECTOR + i) for i in range(MAX_USERS)]
            used = [struct.unpack_from('<I', directory, 12 * i)[0] != 0 for i in range(MAX_USERS)]
            mine = [i for i in range(MAX_USERS) if used[i] and users[i][:8] == struct.pack('<Q', acct['xuid'])]
            free = [i for i in range(MAX_USERS) if not used[i]]
            if not (mine or free):
                sys.exit('mkuser: the hard disk already holds 8 accounts')
            i = (mine or free)[0]
            iv = os.urandom(8)
            directory[12 * i:12 * i + 12] = struct.pack('<I', 1) + iv
            write_config_sector(img, FIRST_USER_SECTOR + i, record(acct, hdd_key, iv))
            print(f'mkuser: {tag} (xuid {acct["xuid"]:#x}) in slot {i}')
        write_config_sector(img, DIRECTORY_SECTOR, bytes(directory))


if __name__ == '__main__':
    main()
