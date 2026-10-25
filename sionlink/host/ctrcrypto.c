// See ctrcrypto.h.

#include <string.h>

#include "ctrcrypto.h"

// AES-128
// =======

static uint8_t sbox[256];

static uint8_t Xtime(uint8_t x)
{
    return (x << 1) ^ ((x & 0x80) ? 0x1B : 0);
}

static uint8_t Gmul(uint8_t a, uint8_t b)
{
    uint8_t p = 0;
    while (b)
    {
        if (b & 1)
            p ^= a;
        a = Xtime(a);
        b >>= 1;
    }
    return p;
}

// The S-box: the multiplicative inverse in GF(2^8), then the affine map
static void MakeSbox(void)
{
    for (int i = 0; i < 256; i++)
    {
        uint8_t inv = 0;
        for (int j = 1; j < 256 && i; j++)
            if (Gmul(i, j) == 1)
            {
                inv = j;
                break;
            }
        uint8_t s = inv;
        for (int k = 1; k < 5; k++)
            s ^= (uint8_t)((inv << k) | (inv >> (8 - k)));
        sbox[i] = s ^ 0x63;
    }
}

void Aes128Init(aes128 *a, const uint8_t key[16])
{
    if (!sbox[0])
        MakeSbox();
    uint8_t rcon = 1;
    for (int i = 0; i < 4; i++)
        a->rk[i] = (key[i * 4] << 24) | (key[i * 4 + 1] << 16) | (key[i * 4 + 2] << 8) | key[i * 4 + 3];
    for (int i = 4; i < 44; i++)
    {
        uint32_t t = a->rk[i - 1];
        if (i % 4 == 0)
        {
            t = (t << 8) | (t >> 24);
            t = (sbox[t >> 24] << 24) | (sbox[(t >> 16) & 255] << 16) | (sbox[(t >> 8) & 255] << 8)
                | sbox[t & 255];
            t ^= (uint32_t)rcon << 24;
            rcon = Xtime(rcon);
        }
        a->rk[i] = a->rk[i - 4] ^ t;
    }
}

void Aes128Block(const aes128 *a, const uint8_t in[16], uint8_t out[16])
{
    uint8_t s[16];
    for (int i = 0; i < 16; i++)
        s[i] = in[i] ^ (a->rk[i / 4] >> (24 - 8 * (i % 4)));
    for (int round = 1; round <= 10; round++)
    {
        uint8_t t[16];
        // SubBytes and ShiftRows (column-major state)
        for (int c = 0; c < 4; c++)
            for (int r = 0; r < 4; r++)
                t[c * 4 + r] = sbox[s[((c + r) % 4) * 4 + r]];
        // MixColumns
        if (round != 10)
            for (int c = 0; c < 4; c++)
            {
                uint8_t *col = t + c * 4;
                uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
                col[0] = Xtime(a0) ^ Xtime(a1) ^ a1 ^ a2 ^ a3;
                col[1] = a0 ^ Xtime(a1) ^ Xtime(a2) ^ a2 ^ a3;
                col[2] = a0 ^ a1 ^ Xtime(a2) ^ Xtime(a3) ^ a3;
                col[3] = Xtime(a0) ^ a0 ^ a1 ^ a2 ^ Xtime(a3);
            }
        for (int i = 0; i < 16; i++)
            s[i] = t[i] ^ (a->rk[round * 4 + i / 4] >> (24 - 8 * (i % 4)));
    }
    memcpy(out, s, 16);
}

static void Increment(uint8_t ctr[16])
{
    for (int i = 15; i >= 0 && !++ctr[i]; i--)
        ;
}

void Aes128Ctr(const uint8_t key[16], const uint8_t ctr_in[16], uint8_t *buf, size_t len)
{
    aes128 a;
    Aes128Init(&a, key);
    uint8_t ctr[16], ks[16];
    memcpy(ctr, ctr_in, 16);
    for (size_t off = 0; off < len; off += 16)
    {
        Aes128Block(&a, ctr, ks);
        for (size_t i = 0; i < 16 && off + i < len; i++)
            buf[off + i] ^= ks[i];
        Increment(ctr);
    }
}

// CCM (RFC 3610), M = 8, L = 2
// ============================

static void CbcMac(const aes128 *a, uint8_t x[16], const uint8_t *data, size_t len)
{
    for (size_t off = 0; off < len; off += 16)
    {
        for (size_t i = 0; i < 16 && off + i < len; i++)
            x[i] ^= data[off + i];
        Aes128Block(a, x, x);
    }
}

static void CcmTag(const aes128 *a, const uint8_t nonce[13], const uint8_t *aad, size_t aad_len,
                   const uint8_t *msg, size_t len, uint8_t tag[8])
{
    uint8_t x[16];
    x[0] = (aad_len ? 0x40 : 0) | (((8 - 2) / 2) << 3) | (2 - 1);
    memcpy(x + 1, nonce, 13);
    x[14] = len >> 8;
    x[15] = len;
    Aes128Block(a, x, x);
    if (aad_len)
    {
        // The AAD with its 2-byte length in front, zero padded to blocks
        uint8_t first[16] = { aad_len >> 8, aad_len & 255 };
        size_t n = aad_len < 14 ? aad_len : 14;
        memcpy(first + 2, aad, n);
        CbcMac(a, x, first, 16);
        if (aad_len > n)
            CbcMac(a, x, aad + n, aad_len - n);
    }
    CbcMac(a, x, msg, len);
    uint8_t a0[16] = { 2 - 1 }, s0[16];
    memcpy(a0 + 1, nonce, 13);
    Aes128Block(a, a0, s0);
    for (int i = 0; i < 8; i++)
        tag[i] = x[i] ^ s0[i];
}

static void CcmCtr(const aes128 *a, const uint8_t nonce[13], uint8_t *buf, size_t len)
{
    uint8_t ctr[16] = { 2 - 1 }, ks[16];
    memcpy(ctr + 1, nonce, 13);
    for (size_t off = 0, i = 1; off < len; off += 16, i++)
    {
        ctr[14] = i >> 8;
        ctr[15] = i;
        Aes128Block(a, ctr, ks);
        for (size_t j = 0; j < 16 && off + j < len; j++)
            buf[off + j] ^= ks[j];
    }
}

void Aes128CcmEncrypt(const uint8_t key[16], const uint8_t nonce[13], const uint8_t *aad,
                      size_t aad_len, uint8_t *buf, size_t len, uint8_t tag[8])
{
    aes128 a;
    Aes128Init(&a, key);
    CcmTag(&a, nonce, aad, aad_len, buf, len, tag);
    CcmCtr(&a, nonce, buf, len);
}

int Aes128CcmDecrypt(const uint8_t key[16], const uint8_t nonce[13], const uint8_t *aad,
                     size_t aad_len, uint8_t *buf, size_t len, const uint8_t tag[8])
{
    aes128 a;
    Aes128Init(&a, key);
    CcmCtr(&a, nonce, buf, len);
    uint8_t t[8];
    CcmTag(&a, nonce, aad, aad_len, buf, len, t);
    int diff = 0;
    for (int i = 0; i < 8; i++)
        diff |= t[i] ^ tag[i];
    return diff ? -1 : 0;
}

// MD5 and SHA-1
// =============
//
// Both pad the message the same way: 0x80, zeros, then the bit length in
// the last 8 bytes of a 64-byte block (little-endian for MD5, big-endian
// for SHA-1).

typedef void (*block_fn)(uint32_t *h, const uint8_t *blk);

static void Digest(const void *data, size_t len, uint32_t *h, block_fn fn, int big_endian)
{
    const uint8_t *p = data;
    size_t full = len & ~(size_t)63;
    for (size_t off = 0; off < full; off += 64)
        fn(h, p + off);
    uint8_t tail[128] = { 0 };
    size_t rest = len - full;
    memcpy(tail, p + full, rest);
    tail[rest] = 0x80;
    size_t total = rest + 9 <= 64 ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++)
        tail[total - 8 + i] = big_endian ? bits >> (56 - 8 * i) : bits >> (8 * i);
    for (size_t off = 0; off < total; off += 64)
        fn(h, tail + off);
}

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

static void Md5Block(uint32_t *h, const uint8_t *blk)
{
    static const uint8_t r[64] = { 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                   5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                   4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                   6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21 };
    // floor(abs(sin(i + 1)) * 2^32)
    static const uint32_t k[64] = {
            0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613,
            0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193,
            0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d,
            0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
            0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122,
            0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
            0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244,
            0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
            0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb,
            0xeb86d391,
    };
    uint32_t w[16];
    for (int i = 0; i < 16; i++)
        w[i] = blk[i * 4] | (blk[i * 4 + 1] << 8) | (blk[i * 4 + 2] << 16) | ((uint32_t)blk[i * 4 + 3] << 24);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
    for (int i = 0; i < 64; i++)
    {
        uint32_t f;
        int g;
        if (i < 16)
            f = (b & c) | (~b & d), g = i;
        else if (i < 32)
            f = (d & b) | (~d & c), g = (5 * i + 1) % 16;
        else if (i < 48)
            f = b ^ c ^ d, g = (3 * i + 5) % 16;
        else
            f = c ^ (b | ~d), g = (7 * i) % 16;
        uint32_t t = d;
        d = c;
        c = b;
        uint32_t x = a + f + k[i] + w[g];
        b = b + ROL(x, r[i]);
        a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
}

void Md5(const void *data, size_t len, uint8_t out[16])
{
    uint32_t h[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
    Digest(data, len, h, Md5Block, 0);
    for (int i = 0; i < 16; i++)
        out[i] = h[i / 4] >> (8 * (i % 4));
}

static void Sha1Block(uint32_t *h, const uint8_t *blk)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)blk[i * 4] << 24) | (blk[i * 4 + 1] << 16) | (blk[i * 4 + 2] << 8) | blk[i * 4 + 3];
    for (int i = 16; i < 80; i++)
        w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++)
    {
        uint32_t f, k;
        if (i < 20)
            f = (b & c) | (~b & d), k = 0x5A827999;
        else if (i < 40)
            f = b ^ c ^ d, k = 0x6ED9EBA1;
        else if (i < 60)
            f = (b & c) | (b & d) | (c & d), k = 0x8F1BBCDC;
        else
            f = b ^ c ^ d, k = 0xCA62C1D6;
        uint32_t t = ROL(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = ROL(b, 30);
        b = a;
        a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}

void Sha1(const void *data, size_t len, uint8_t out[20])
{
    uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    Digest(data, len, h, Sha1Block, 1);
    for (int i = 0; i < 20; i++)
        out[i] = h[i / 4] >> (24 - 8 * (i % 4));
}
