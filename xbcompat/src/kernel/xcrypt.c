/* The Xc* crypto exports: SHA-1, HMAC, RC4, RSA (BSAFE key blobs), big-number
   ModExp, DES/3DES, and the replaceable vector they all dispatch through.

   Layouts follow the leak's private/crypto/xcbase.c and private/inc/crypto
   (xcrypt.h, rsa.h, des.h, tripldes.h).  As in the ROM, each XcFoo export
   calls through a CRYPTO_VECTOR that XcUpdateCrypto can patch; the original
   routines are the XCrypt* functions below. */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "../xbcompat.h"
#include "../cpu.h"

/* ---- SHA-1 ------------------------------------------------------------- */

/* The title's 116-byte SHA context holds this. */
typedef struct { uint32_t h[5]; uint64_t len; uint8_t buf[64]; uint32_t n; } sha1_ctx;

static uint32_t rol32(uint32_t v, int s) { return (v << s) | (v >> (32 - s)); }

static void sha1_block(sha1_ctx *c, const uint8_t *p)
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = c->h[0], b = c->h[1], cc = c->h[2], d = c->h[3], e = c->h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) { f = (b & cc) | (~b & d); k = 0x5A827999; }
        else if (i < 40) { f = b ^ cc ^ d; k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & cc) | (b & d) | (cc & d); k = 0x8F1BBCDC; }
        else { f = b ^ cc ^ d; k = 0xCA62C1D6; }
        uint32_t t = rol32(a, 5) + f + e + k + w[i];
        e = d; d = cc; cc = rol32(b, 30); b = a; a = t;
    }
    c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d; c->h[4] += e;
}

static void NTAPI XCryptSHAInit(UCHAR *ctx)
{
    sha1_ctx *c = (sha1_ctx *)ctx;
    memset(ctx, 0, 116);
    c->h[0] = 0x67452301; c->h[1] = 0xEFCDAB89; c->h[2] = 0x98BADCFE; c->h[3] = 0x10325476; c->h[4] = 0xC3D2E1F0;
}

static void NTAPI XCryptSHAUpdate(UCHAR *ctx, const UCHAR *in, ULONG n)
{
    sha1_ctx *c = (sha1_ctx *)ctx;
    c->len += n;
    while (n) {
        ULONG take = 64 - c->n < n ? 64 - c->n : n;
        memcpy(c->buf + c->n, in, take);
        c->n += take; in += take; n -= take;
        if (c->n == 64) { sha1_block(c, c->buf); c->n = 0; }
    }
}

static void NTAPI XCryptSHAFinal(UCHAR *ctx, UCHAR *digest)
{
    sha1_ctx *c = (sha1_ctx *)ctx;
    uint64_t bits = c->len * 8;
    static const UCHAR pad[64] = { 0x80 };
    ULONG padlen = c->n < 56 ? 56 - c->n : 120 - c->n;
    XCryptSHAUpdate(ctx, pad, padlen);
    UCHAR lenbe[8];
    for (int i = 0; i < 8; i++) lenbe[i] = (UCHAR)(bits >> (56 - 8 * i));
    XCryptSHAUpdate(ctx, lenbe, 8);
    for (int i = 0; i < 5; i++) {
        digest[4 * i] = c->h[i] >> 24; digest[4 * i + 1] = c->h[i] >> 16;
        digest[4 * i + 2] = c->h[i] >> 8; digest[4 * i + 3] = c->h[i];
    }
}

/* HMAC-SHA1 over the concatenation of two buffers (either may be empty).
   Like the ROM, a key longer than the 64-byte block is truncated, not hashed. */
static void NTAPI XCryptHMAC(const UCHAR *key, ULONG keylen, const UCHAR *a, ULONG alen, const UCHAR *b,
                             ULONG blen, UCHAR *digest)
{
    UCHAR k[64] = { 0 }, ctx[116], inner[20];
    if (keylen > 64) keylen = 64;
    if (key) memcpy(k, key, keylen);
    UCHAR pad[64];
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    XCryptSHAInit(ctx); XCryptSHAUpdate(ctx, pad, 64);
    if (a && alen) XCryptSHAUpdate(ctx, a, alen);
    if (b && blen) XCryptSHAUpdate(ctx, b, blen);
    XCryptSHAFinal(ctx, inner);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5C;
    XCryptSHAInit(ctx); XCryptSHAUpdate(ctx, pad, 64); XCryptSHAUpdate(ctx, inner, 20);
    XCryptSHAFinal(ctx, digest);
}

/* ---- RC4: the title's key structure is 256 state bytes plus the two indices */

static void NTAPI XCryptRC4Key(UCHAR *state, ULONG keylen, const UCHAR *key)
{
    for (int i = 0; i < 256; i++) state[i] = (UCHAR)i;
    for (int i = 0, j = 0; i < 256; i++) {
        j = (j + state[i] + (keylen ? key[i % keylen] : 0)) & 0xFF;
        UCHAR t = state[i]; state[i] = state[j]; state[j] = t;
    }
    state[256] = state[257] = 0;
}

static void NTAPI XCryptRC4Crypt(UCHAR *state, ULONG len, UCHAR *data)
{
    unsigned i = state[256], j = state[257];
    for (ULONG n = 0; n < len; n++) {
        i = (i + 1) & 0xFF;
        j = (j + state[i]) & 0xFF;
        UCHAR t = state[i]; state[i] = state[j]; state[j] = t;
        data[n] ^= state[(state[i] + state[j]) & 0xFF];
    }
    state[256] = (UCHAR)i; state[257] = (UCHAR)j;
}

/* ---- big numbers: n little-endian 32-bit limbs ------------------------- */

static int bn_cmp(const uint32_t *a, const uint32_t *b, ULONG n)
{
    for (ULONG i = n; i-- > 0;) if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

static void bn_sub(uint32_t *a, const uint32_t *b, ULONG n)
{
    uint64_t borrow = 0;
    for (ULONG i = 0; i < n; i++) {
        uint64_t d = (uint64_t)a[i] - b[i] - borrow;
        a[i] = (uint32_t)d;
        borrow = (d >> 63) & 1;
    }
}

/* Bits up to and including the highest set one. */
static ULONG bn_bits(const uint32_t *a, ULONG n)
{
    while (n && !a[n - 1]) n--;
    return n ? (n - 1) * 32 + 32 - __builtin_clz(a[n - 1]) : 0;
}

/* a = (a + b) mod m, with a, b < m; `carry` handles the bit past n limbs. */
static void bn_addmod(uint32_t *a, const uint32_t *b, const uint32_t *m, ULONG n)
{
    uint64_t c = 0;
    for (ULONG i = 0; i < n; i++) { c += (uint64_t)a[i] + b[i]; a[i] = (uint32_t)c; c >>= 32; }
    if (c || bn_cmp(a, m, n) >= 0) bn_sub(a, m, n);
}

/* r = a * b mod m by double-and-add over a's bits. */
static void bn_mulmod(uint32_t *r, const uint32_t *a, const uint32_t *b, const uint32_t *m, ULONG n)
{
    uint32_t *acc = calloc(n, 4);
    for (ULONG i = bn_bits(a, n); i-- > 0;) {
        bn_addmod(acc, acc, m, n);
        if ((a[i / 32] >> (i % 32)) & 1) bn_addmod(acc, b, m, n);
    }
    memcpy(r, acc, n * 4);
    free(acc);
}

/* A = B^C mod D, all n limbs (BenalohModExp). */
static ULONG NTAPI XCryptModExp(ULONG *A, const ULONG *B, const ULONG *C, const ULONG *D, ULONG n)
{
    if (!n) return 0;
    uint32_t *base = calloc(n, 4), *res = calloc(n, 4);
    memcpy(base, B, n * 4);
    while (bn_cmp(base, (const uint32_t *)D, n) >= 0) bn_sub(base, (const uint32_t *)D, n);   /* B < D in practice */
    res[0] = 1;
    if (n == 1 && D[0] == 1) res[0] = 0;
    for (ULONG i = bn_bits((const uint32_t *)C, n); i-- > 0;) {
        bn_mulmod(res, res, res, (const uint32_t *)D, n);
        if ((C[i / 32] >> (i % 32)) & 1) bn_mulmod(res, res, base, (const uint32_t *)D, n);
    }
    memcpy(A, res, n * 4);
    free(base);
    free(res);
    return 1;
}

/* ---- RSA on BSAFE key blobs ------------------------------------------- */

/* rsa.h's BSAFE_PUB_KEY / BSAFE_PRV_KEY header; the numbers follow it,
   little-endian.  keylen is the modulus buffer size: bitlen/8 plus 8 bytes
   or more of zero padding (2048-bit keys: 264, so XC_PUBLIC_KEYDATA_SIZE is
   284).  A public key holds just the modulus.  A private key holds modulus
   [keylen], prime1, prime2, exponent1, exponent2, coefficient [keylen/2
   each] and the private exponent [keylen]. */
typedef struct { ULONG magic, keylen, bitlen, datalen, pubexp; } BSAFE_KEY;

/* out = in^exp mod modulus over the key's keylen bytes. */
static ULONG rsa_exp(const BSAFE_KEY *k, const UCHAR *in, const UCHAR *exp, ULONG explen, UCHAR *out)
{
    ULONG n = k->keylen / 4;
    if (!n || k->keylen % 4) return 0;
    uint32_t *b = calloc(n, 4), *e = calloc(n, 4), *m = calloc(n, 4), *r = calloc(n, 4);
    memcpy(b, in, n * 4);
    memcpy(e, exp, explen < n * 4 ? explen : n * 4);
    memcpy(m, k + 1, n * 4);
    ULONG ok = bn_bits(m, n) != 0;
    if (ok) XCryptModExp(r, b, e, m, n);
    memcpy(out, r, n * 4);
    free(b); free(e); free(m); free(r);
    return ok;
}

/* BSafeEncPublic: out = in^pubexp mod modulus, keylen bytes each. */
static ULONG NTAPI XCryptPKEncPublic(const UCHAR *key, const UCHAR *in, UCHAR *out)
{
    const BSAFE_KEY *k = (const BSAFE_KEY *)key;
    return rsa_exp(k, in, (const UCHAR *)&k->pubexp, 4, out);
}

/* BSafeDecPrivate: out = in^d mod modulus. */
static ULONG NTAPI XCryptPKDecPrivate(const UCHAR *key, const UCHAR *in, UCHAR *out)
{
    const BSAFE_KEY *k = (const BSAFE_KEY *)key;
    const UCHAR *d = (const UCHAR *)(k + 1) + k->keylen + 5 * (k->keylen / 2);
    return rsa_exp(k, in, d, k->keylen, out);
}

static ULONG NTAPI XCryptPKGetKeyLen(const UCHAR *key) { return ((const BSAFE_KEY *)key)->keylen; }

/* The SHA-1 DigestInfo prefixes PKCS #1 allows, reversed (the block is
   little-endian), each led by its length; xcbase.c's shaEncodings. */
static const UCHAR *const sha_encodings[] = {
    (const UCHAR *)"\x0f\x14\x04\x00\x05\x1a\x02\x03\x0e\x2b\x05\x06\x09\x30\x21\x30",
    (const UCHAR *)"\x0d\x14\x04\x1a\x02\x03\x0e\x2b\x05\x06\x07\x30\x1f\x30",
    (const UCHAR *)"\x00",
};

/* XCVerifyDigest + XCVerifyPKCS1SigningFmt: decrypt the signature with the
   public key and check for a type 1 block around the reversed digest. */
static BOOLEAN NTAPI XCryptVerifyPKCS1Signature(const UCHAR *sig, const UCHAR *key, const UCHAR *digest)
{
    const BSAFE_KEY *k = (const BSAFE_KEY *)key;
    ULONG keylen = k->keylen, siglen = (k->bitlen + 7) / 8;
    if (keylen < 64 || siglen > keylen || k->datalen + 1 > keylen) return 0;
    UCHAR *in = calloc(1, keylen), *out = calloc(1, keylen);
    memcpy(in, sig, siglen);
    BOOLEAN ok = 0;
    if (!XCryptPKEncPublic(key, in, out)) goto done;
    for (int i = 0; i < 20; i++)
        if (out[i] != digest[19 - i]) goto done;
    ULONG cb = 20;
    for (int i = 0; sha_encodings[i][0]; i++) {
        ULONG n = sha_encodings[i][0];
        if (!memcmp(out + cb, sha_encodings[i] + 1, n)) { cb += n; break; }
    }
    if (out[cb] || out[k->datalen] || out[k->datalen - 1] != 1) goto done;
    for (ULONG i = cb + 1; i < k->datalen - 1; i++)
        if (out[i] != 0xFF) goto done;
    ok = 1;
done:
    free(in);
    free(out);
    return ok;
}

/* ---- DES and triple DES (FIPS 46-3) ------------------------------------ */

/* DES keys carry odd parity in each byte's low bit. */
static void NTAPI XCryptDESKeyParity(UCHAR *key, ULONG len)
{
    for (ULONG i = 0; i < len; i++) {
        UCHAR b = key[i] & 0xFE;
        key[i] = b | !(__builtin_popcount(b) & 1);
    }
}

/* Permutation tables, 1-based bit numbers counted from the most significant. */
static const uint8_t des_ip[64] = {
    58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4,
    62, 54, 46, 38, 30, 22, 14, 6, 64, 56, 48, 40, 32, 24, 16, 8,
    57, 49, 41, 33, 25, 17, 9, 1, 59, 51, 43, 35, 27, 19, 11, 3,
    61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7 };
static const uint8_t des_fp[64] = {
    40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31,
    38, 6, 46, 14, 54, 22, 62, 30, 37, 5, 45, 13, 53, 21, 61, 29,
    36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27,
    34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9, 49, 17, 57, 25 };
static const uint8_t des_e[48] = {
    32, 1, 2, 3, 4, 5, 4, 5, 6, 7, 8, 9, 8, 9, 10, 11, 12, 13, 12, 13, 14, 15, 16, 17,
    16, 17, 18, 19, 20, 21, 20, 21, 22, 23, 24, 25, 24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1 };
static const uint8_t des_p[32] = {
    16, 7, 20, 21, 29, 12, 28, 17, 1, 15, 23, 26, 5, 18, 31, 10,
    2, 8, 24, 14, 32, 27, 3, 9, 19, 13, 30, 6, 22, 11, 4, 25 };
static const uint8_t des_pc1[56] = {
    57, 49, 41, 33, 25, 17, 9, 1, 58, 50, 42, 34, 26, 18,
    10, 2, 59, 51, 43, 35, 27, 19, 11, 3, 60, 52, 44, 36,
    63, 55, 47, 39, 31, 23, 15, 7, 62, 54, 46, 38, 30, 22,
    14, 6, 61, 53, 45, 37, 29, 21, 13, 5, 28, 20, 12, 4 };
static const uint8_t des_pc2[48] = {
    14, 17, 11, 24, 1, 5, 3, 28, 15, 6, 21, 10, 23, 19, 12, 4, 26, 8, 16, 7, 27, 20, 13, 2,
    41, 52, 31, 37, 47, 55, 30, 40, 51, 45, 33, 48, 44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32 };
static const uint8_t des_shifts[16] = { 1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1 };
static const uint8_t des_s[8][64] = {
    { 14, 4, 13, 1, 2, 15, 11, 8, 3, 10, 6, 12, 5, 9, 0, 7, 0, 15, 7, 4, 14, 2, 13, 1, 10, 6, 12, 11, 9, 5, 3, 8,
      4, 1, 14, 8, 13, 6, 2, 11, 15, 12, 9, 7, 3, 10, 5, 0, 15, 12, 8, 2, 4, 9, 1, 7, 5, 11, 3, 14, 10, 0, 6, 13 },
    { 15, 1, 8, 14, 6, 11, 3, 4, 9, 7, 2, 13, 12, 0, 5, 10, 3, 13, 4, 7, 15, 2, 8, 14, 12, 0, 1, 10, 6, 9, 11, 5,
      0, 14, 7, 11, 10, 4, 13, 1, 5, 8, 12, 6, 9, 3, 2, 15, 13, 8, 10, 1, 3, 15, 4, 2, 11, 6, 7, 12, 0, 5, 14, 9 },
    { 10, 0, 9, 14, 6, 3, 15, 5, 1, 13, 12, 7, 11, 4, 2, 8, 13, 7, 0, 9, 3, 4, 6, 10, 2, 8, 5, 14, 12, 11, 15, 1,
      13, 6, 4, 9, 8, 15, 3, 0, 11, 1, 2, 12, 5, 10, 14, 7, 1, 10, 13, 0, 6, 9, 8, 7, 4, 15, 14, 3, 11, 5, 2, 12 },
    { 7, 13, 14, 3, 0, 6, 9, 10, 1, 2, 8, 5, 11, 12, 4, 15, 13, 8, 11, 5, 6, 15, 0, 3, 4, 7, 2, 12, 1, 10, 14, 9,
      10, 6, 9, 0, 12, 11, 7, 13, 15, 1, 3, 14, 5, 2, 8, 4, 3, 15, 0, 6, 10, 1, 13, 8, 9, 4, 5, 11, 12, 7, 2, 14 },
    { 2, 12, 4, 1, 7, 10, 11, 6, 8, 5, 3, 15, 13, 0, 14, 9, 14, 11, 2, 12, 4, 7, 13, 1, 5, 0, 15, 10, 3, 9, 8, 6,
      4, 2, 1, 11, 10, 13, 7, 8, 15, 9, 12, 5, 6, 3, 0, 14, 11, 8, 12, 7, 1, 14, 2, 13, 6, 15, 0, 9, 10, 4, 5, 3 },
    { 12, 1, 10, 15, 9, 2, 6, 8, 0, 13, 3, 4, 14, 7, 5, 11, 10, 15, 4, 2, 7, 12, 9, 5, 6, 1, 13, 14, 0, 11, 3, 8,
      9, 14, 15, 5, 2, 8, 12, 3, 7, 0, 4, 10, 1, 13, 11, 6, 4, 3, 2, 12, 9, 5, 15, 10, 11, 14, 1, 7, 6, 0, 8, 13 },
    { 4, 11, 2, 14, 15, 0, 8, 13, 3, 12, 9, 7, 5, 10, 6, 1, 13, 0, 11, 7, 4, 9, 1, 10, 14, 3, 5, 12, 2, 15, 8, 6,
      1, 4, 11, 13, 12, 3, 7, 14, 10, 15, 6, 8, 0, 5, 9, 2, 6, 11, 13, 8, 1, 4, 10, 7, 9, 5, 0, 15, 14, 2, 3, 12 },
    { 13, 2, 8, 4, 6, 15, 11, 1, 10, 9, 3, 14, 5, 0, 12, 7, 1, 15, 13, 8, 10, 3, 7, 4, 12, 5, 6, 11, 0, 14, 9, 2,
      7, 11, 4, 1, 9, 12, 14, 2, 0, 6, 10, 13, 15, 3, 5, 8, 2, 1, 14, 7, 4, 10, 8, 13, 15, 12, 9, 0, 3, 5, 6, 11 },
};

/* Output bit i (from the top of n) is input bit tbl[i] (from the top of w). */
static uint64_t des_permute(uint64_t in, int w, const uint8_t *tbl, int n)
{
    uint64_t out = 0;
    for (int i = 0; i < n; i++) out = out << 1 | ((in >> (w - tbl[i])) & 1);
    return out;
}

/* deskey: des.h's DESTable is keytab[16][2]; each round's 48-bit subkey
   is kept as its high and low 24 bits.  Only XcBlockCrypt reads it. */
static void des_key(ULONG *tab, const UCHAR *key)
{
    uint64_t k = 0;
    for (int i = 0; i < 8; i++) k = k << 8 | key[i];
    uint64_t cd = des_permute(k, 64, des_pc1, 56);
    uint32_t c = cd >> 28, d = cd & 0xFFFFFFF;
    for (int r = 0; r < 16; r++) {
        for (int s = 0; s < des_shifts[r]; s++) {
            c = ((c << 1) | (c >> 27)) & 0xFFFFFFF;
            d = ((d << 1) | (d >> 27)) & 0xFFFFFFF;
        }
        uint64_t sub = des_permute((uint64_t)c << 28 | d, 56, des_pc2, 48);
        tab[2 * r] = (ULONG)(sub >> 24);
        tab[2 * r + 1] = (ULONG)(sub & 0xFFFFFF);
    }
}

static uint32_t des_f(uint32_t r, uint64_t sub)
{
    uint64_t x = des_permute(r, 32, des_e, 48) ^ sub;
    uint32_t out = 0;
    for (int i = 0; i < 8; i++) {
        unsigned b = (x >> (42 - 6 * i)) & 0x3F;
        out = out << 4 | des_s[i][(b & 0x20) | (b & 1) << 4 | (b >> 1 & 0xF)];
    }
    return (uint32_t)des_permute(out, 32, des_p, 32);
}

/* des(): one 8-byte block; op is XC_SERVICE_ENCRYPT (1) or DECRYPT (0). */
static void des_block(UCHAR *out, const UCHAR *in, const ULONG *tab, ULONG op)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = v << 8 | in[i];
    v = des_permute(v, 64, des_ip, 64);
    uint32_t l = v >> 32, r = (uint32_t)v;
    for (int i = 0; i < 16; i++) {
        int k = op ? i : 15 - i;
        uint32_t t = l ^ des_f(r, (uint64_t)tab[2 * k] << 24 | tab[2 * k + 1]);
        l = r; r = t;
    }
    v = des_permute((uint64_t)r << 32 | l, 64, des_fp, 64);
    for (int i = 0; i < 8; i++) out[i] = (UCHAR)(v >> (56 - 8 * i));
}

/* tripledes: DES3TABLE is three DESTables for K1, K2, K3; encryption is
   E(K3, D(K2, E(K1, x))), as in FIPS 46-3 / SP 800-67. */
static void des3_block(UCHAR *out, const UCHAR *in, const ULONG *tab, ULONG op)
{
    UCHAR t[8];
    if (op) {
        des_block(t, in, tab, 1);
        des_block(t, t, tab + 32, 0);
        des_block(out, t, tab + 64, 1);
    } else {
        des_block(t, in, tab + 64, 0);
        des_block(t, t, tab + 32, 1);
        des_block(out, t, tab, 0);
    }
}

#define XC_SERVICE_DES_CIPHER 0

/* deskey or tripledes3key (a 24-byte key) by cipher. */
static void NTAPI XCryptKeyTable(ULONG cipher, UCHAR *table, const UCHAR *key)
{
    ULONG *tab = (ULONG *)table;
    des_key(tab, key);
    if (cipher != XC_SERVICE_DES_CIPHER) {
        des_key(tab + 32, key + 8);
        des_key(tab + 64, key + 16);
    }
}

static void NTAPI XCryptBlockCrypt(ULONG cipher, UCHAR *out, const UCHAR *in, const UCHAR *table, ULONG op)
{
    (cipher == XC_SERVICE_DES_CIPHER ? des_block : des3_block)(out, in, (const ULONG *)table, op);
}

/* modes.h's CBC over every 8-byte block; feedback carries the chaining
   value in and out, so successive calls continue the stream. */
static void NTAPI XCryptBlockCryptCBC(ULONG cipher, ULONG len, UCHAR *out, const UCHAR *in, const UCHAR *table,
                                      ULONG op, UCHAR *feedback)
{
    void (*block)(UCHAR *, const UCHAR *, const ULONG *, ULONG) =
        cipher == XC_SERVICE_DES_CIPHER ? des_block : des3_block;
    for (ULONG off = 0; off < len; off += 8) {
        UCHAR t[8];
        if (op) {
            for (int i = 0; i < 8; i++) t[i] = in[off + i] ^ feedback[i];
            block(out + off, t, (const ULONG *)table, op);
            memcpy(feedback, out + off, 8);
        } else {
            UCHAR c[8];
            memcpy(c, in + off, 8);
            block(t, c, (const ULONG *)table, op);
            for (int i = 0; i < 8; i++) out[off + i] = t[i] ^ feedback[i];
            memcpy(feedback, c, 8);
        }
    }
}

/* Reserved for extensions; the ROM's does nothing. */
static ULONG NTAPI XCryptCryptService(ULONG op, PVOID args) { (void)op; (void)args; return 0; }

/* ---- the crypto vector ------------------------------------------------- */

/* xcrypt.h's CRYPTO_VECTOR order.  Entries are guest addresses: the ROM's
   are our XCrypt* functions' guest entries, a title's replacements are its
   own (stdcall) code. */
enum {
    XC_SHAINIT, XC_SHAUPDATE, XC_SHAFINAL, XC_RC4KEY, XC_RC4CRYPT, XC_HMAC, XC_PKENCPUBLIC,
    XC_PKDECPRIVATE, XC_PKGETKEYLEN, XC_VERIFYPKCS1, XC_MODEXP, XC_DESKEYPARITY, XC_KEYTABLE,
    XC_BLOCKCRYPT, XC_BLOCKCRYPTCBC, XC_CRYPTSERVICE, XC_COUNT
};

static void *const xc_impl[XC_COUNT] = {
    XCryptSHAInit, XCryptSHAUpdate, XCryptSHAFinal, XCryptRC4Key, XCryptRC4Crypt, XCryptHMAC,
    XCryptPKEncPublic, XCryptPKDecPrivate, XCryptPKGetKeyLen, XCryptVerifyPKCS1Signature, XCryptModExp,
    XCryptDESKeyParity, XCryptKeyTable, XCryptBlockCrypt, XCryptBlockCryptCBC, XCryptCryptService,
};

static ULONG xc_rom[XC_COUNT], xc_vec[XC_COUNT];
static pthread_once_t xc_once = PTHREAD_ONCE_INIT;

static void xc_init(void)
{
    for (int i = 0; i < XC_COUNT; i++) xc_rom[i] = xc_vec[i] = cpu_guest_entry(xc_impl[i]);
}

/* Patch the non-null entries; hand back the ROM's originals if asked. */
void NTAPI XcUpdateCrypto(const ULONG *NewVector, ULONG *RomVector)
{
    pthread_once(&xc_once, xc_init);
    for (int i = 0; i < XC_COUNT; i++)
        if (NewVector && NewVector[i]) xc_vec[i] = NewVector[i];
    if (RomVector) memcpy(RomVector, xc_rom, sizeof(xc_rom));
}

/* A title's replacement routine, called with 32-bit arguments. */
#ifdef XBC_TRANSLATED
static ULONG xc_guest(ULONG fn, int n, const uint32_t *a) { return (ULONG)cpu_call(fn, CONV_STD, n, a); }
#else
typedef ULONG (__attribute__((stdcall)) *xc_fn1)(uint32_t);
typedef ULONG (__attribute__((stdcall)) *xc_fn2)(uint32_t, uint32_t);
typedef ULONG (__attribute__((stdcall)) *xc_fn3)(uint32_t, uint32_t, uint32_t);
typedef ULONG (__attribute__((stdcall)) *xc_fn5)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
typedef ULONG (__attribute__((stdcall)) *xc_fn7)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                                  uint32_t);

static ULONG xc_guest(ULONG fn, int n, const uint32_t *a)
{
    switch (n) {
    case 1: return ((xc_fn1)fn)(a[0]);
    case 2: return ((xc_fn2)fn)(a[0], a[1]);
    case 3: return ((xc_fn3)fn)(a[0], a[1], a[2]);
    case 5: return ((xc_fn5)fn)(a[0], a[1], a[2], a[3], a[4]);
    default: return ((xc_fn7)fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6]);
    }
}
#endif

#define W(x) ((uint32_t)(uintptr_t)(x))
/* Each export runs the ROM routine directly unless the title replaced it. */
#define XC_GUEST(i, ...) xc_guest(xc_vec[i], sizeof((uint32_t[]){ __VA_ARGS__ }) / 4, (uint32_t[]){ __VA_ARGS__ })
#define XC_DISPATCH(i, call, ...)                                                       \
    pthread_once(&xc_once, xc_init);                                                    \
    if (xc_vec[i] == xc_rom[i]) return call;                                            \
    return XC_GUEST(i, __VA_ARGS__)
#define XC_DISPATCH_VOID(i, call, ...)                                                  \
    pthread_once(&xc_once, xc_init);                                                    \
    if (xc_vec[i] == xc_rom[i]) call;                                                   \
    else XC_GUEST(i, __VA_ARGS__)

void NTAPI XcSHAInit(UCHAR *ctx) { XC_DISPATCH_VOID(XC_SHAINIT, XCryptSHAInit(ctx), W(ctx)); }

void NTAPI XcSHAUpdate(UCHAR *ctx, const UCHAR *in, ULONG n)
{
    XC_DISPATCH_VOID(XC_SHAUPDATE, XCryptSHAUpdate(ctx, in, n), W(ctx), W(in), n);
}

void NTAPI XcSHAFinal(UCHAR *ctx, UCHAR *digest)
{
    XC_DISPATCH_VOID(XC_SHAFINAL, XCryptSHAFinal(ctx, digest), W(ctx), W(digest));
}

void NTAPI XcRC4Key(UCHAR *state, ULONG keylen, const UCHAR *key)
{
    XC_DISPATCH_VOID(XC_RC4KEY, XCryptRC4Key(state, keylen, key), W(state), keylen, W(key));
}

void NTAPI XcRC4Crypt(UCHAR *state, ULONG len, UCHAR *data)
{
    XC_DISPATCH_VOID(XC_RC4CRYPT, XCryptRC4Crypt(state, len, data), W(state), len, W(data));
}

void NTAPI XcHMAC(const UCHAR *key, ULONG keylen, const UCHAR *a, ULONG alen, const UCHAR *b, ULONG blen,
                  UCHAR *digest)
{
    XC_DISPATCH_VOID(XC_HMAC, XCryptHMAC(key, keylen, a, alen, b, blen, digest),
                W(key), keylen, W(a), alen, W(b), blen, W(digest));
}

ULONG NTAPI XcPKEncPublic(const UCHAR *key, const UCHAR *in, UCHAR *out)
{
    XC_DISPATCH(XC_PKENCPUBLIC, XCryptPKEncPublic(key, in, out), W(key), W(in), W(out));
}

ULONG NTAPI XcPKDecPrivate(const UCHAR *key, const UCHAR *in, UCHAR *out)
{
    XC_DISPATCH(XC_PKDECPRIVATE, XCryptPKDecPrivate(key, in, out), W(key), W(in), W(out));
}

ULONG NTAPI XcPKGetKeyLen(const UCHAR *key) { XC_DISPATCH(XC_PKGETKEYLEN, XCryptPKGetKeyLen(key), W(key)); }

BOOLEAN NTAPI XcVerifyPKCS1Signature(const UCHAR *sig, const UCHAR *key, const UCHAR *digest)
{
    XC_DISPATCH(XC_VERIFYPKCS1, XCryptVerifyPKCS1Signature(sig, key, digest), W(sig), W(key), W(digest));
}

ULONG NTAPI XcModExp(ULONG *A, const ULONG *B, const ULONG *C, const ULONG *D, ULONG n)
{
    XC_DISPATCH(XC_MODEXP, XCryptModExp(A, B, C, D, n), W(A), W(B), W(C), W(D), n);
}

void NTAPI XcDESKeyParity(UCHAR *key, ULONG len)
{
    XC_DISPATCH_VOID(XC_DESKEYPARITY, XCryptDESKeyParity(key, len), W(key), len);
}

void NTAPI XcKeyTable(ULONG cipher, UCHAR *table, const UCHAR *key)
{
    XC_DISPATCH_VOID(XC_KEYTABLE, XCryptKeyTable(cipher, table, key), cipher, W(table), W(key));
}

void NTAPI XcBlockCrypt(ULONG cipher, UCHAR *out, const UCHAR *in, const UCHAR *table, ULONG op)
{
    XC_DISPATCH_VOID(XC_BLOCKCRYPT, XCryptBlockCrypt(cipher, out, in, table, op),
                     cipher, W(out), W(in), W(table), op);
}

void NTAPI XcBlockCryptCBC(ULONG cipher, ULONG len, UCHAR *out, const UCHAR *in, const UCHAR *table, ULONG op,
                           UCHAR *feedback)
{
    XC_DISPATCH_VOID(XC_BLOCKCRYPTCBC, XCryptBlockCryptCBC(cipher, len, out, in, table, op, feedback),
                cipher, len, W(out), W(in), W(table), op, W(feedback));
}

ULONG NTAPI XcCryptService(ULONG op, PVOID args)
{
    XC_DISPATCH(XC_CRYPTSERVICE, XCryptCryptService(op, args), op, W(args));
}
