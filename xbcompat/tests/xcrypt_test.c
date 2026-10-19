/*
 * xcrypt_test: the Xc* crypto exports (src/kernel/xcrypt.c).
 *
 * Covers SHA-1 and HMAC (RFC 2202, and the ROM's truncation of long keys),
 * DES and 3DES block and CBC modes against published FIPS 46-3 / FIPS 81 /
 * SP 800-67 vectors, RSA on BSAFE key blobs (raw encryption and PKCS #1
 * v1.5 SHA-1 signatures cross-checked with keys and signatures from the
 * openssl CLI, skipped when it is missing), and XcUpdateCrypto's vector.
 *
 * Build (from xbcompat/):
 *   gcc -m32 -std=gnu11 -Wall -mstackrealign -Isrc -o xcrypt_test tests/xcrypt_test.c \
 *       src/kernel/xcrypt.c -lpthread
 * Run:  ./xcrypt_test
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xbcompat.h"

static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

void NTAPI XcSHAInit(UCHAR *);
void NTAPI XcSHAUpdate(UCHAR *, const UCHAR *, ULONG);
void NTAPI XcSHAFinal(UCHAR *, UCHAR *);
void NTAPI XcHMAC(const UCHAR *, ULONG, const UCHAR *, ULONG, const UCHAR *, ULONG, UCHAR *);
ULONG NTAPI XcPKEncPublic(const UCHAR *, const UCHAR *, UCHAR *);
ULONG NTAPI XcPKDecPrivate(const UCHAR *, const UCHAR *, UCHAR *);
ULONG NTAPI XcPKGetKeyLen(const UCHAR *);
BOOLEAN NTAPI XcVerifyPKCS1Signature(const UCHAR *, const UCHAR *, const UCHAR *);
void NTAPI XcDESKeyParity(UCHAR *, ULONG);
void NTAPI XcKeyTable(ULONG, UCHAR *, const UCHAR *);
void NTAPI XcBlockCrypt(ULONG, UCHAR *, const UCHAR *, const UCHAR *, ULONG);
void NTAPI XcBlockCryptCBC(ULONG, ULONG, UCHAR *, const UCHAR *, const UCHAR *, ULONG, UCHAR *);
ULONG NTAPI XcCryptService(ULONG, PVOID);
void NTAPI XcUpdateCrypto(const ULONG *, ULONG *);

enum { DES = 0, DES3 = 1, DECRYPT = 0, ENCRYPT = 1 };

static size_t unhex(UCHAR *out, const char *hex)
{
    size_t n = 0;
    for (; hex[0] && hex[1]; hex += 2) {
        if (hex[0] == ' ' || hex[0] == ':') { hex--; continue; }
        unsigned v;
        sscanf(hex, "%2x", &v);
        out[n++] = (UCHAR)v;
    }
    return n;
}

static int eqhex(const UCHAR *p, const char *hex)
{
    UCHAR want[256];
    size_t n = unhex(want, hex);
    return !memcmp(p, want, n);
}

static void sha1(const void *p, size_t n, UCHAR *digest)
{
    UCHAR ctx[116];
    XcSHAInit(ctx);
    XcSHAUpdate(ctx, p, n);
    XcSHAFinal(ctx, digest);
}

static void test_sha_hmac(void)
{
    UCHAR d[20], d2[20];
    sha1("abc", 3, d);
    CHECK(eqhex(d, "a9993e364706816aba3e25717850c26c9cd0d89d"));
    UCHAR key[80];
    memset(key, 0x0b, 20);
    XcHMAC(key, 20, (const UCHAR *)"Hi ", 3, (const UCHAR *)"There", 5, d);   /* RFC 2202 case 1, split */
    CHECK(eqhex(d, "b617318655057264e28bc0b6fb378c8ef146be00"));
    /* The ROM truncates keys to the 64-byte block instead of hashing them. */
    for (int i = 0; i < 80; i++) key[i] = (UCHAR)i;
    XcHMAC(key, 80, (const UCHAR *)"msg", 3, NULL, 0, d);
    XcHMAC(key, 64, (const UCHAR *)"msg", 3, NULL, 0, d2);
    CHECK(!memcmp(d, d2, 20));
}

/* One ECB block each way. */
static void des_vector(ULONG cipher, const char *key, const char *pt, const char *ct)
{
    UCHAR k[24], p[8], c[8], out[8], table[384];
    unhex(k, key); unhex(p, pt); unhex(c, ct);
    XcKeyTable(cipher, table, k);
    XcBlockCrypt(cipher, out, p, table, ENCRYPT);
    CHECK(!memcmp(out, c, 8));
    XcBlockCrypt(cipher, out, c, table, DECRYPT);
    CHECK(!memcmp(out, p, 8));
}

static void test_des(void)
{
    des_vector(DES, "133457799BBCDFF1", "0123456789ABCDEF", "85E813540F0AB405");
    des_vector(DES, "0123456789ABCDEF", "4E6F772069732074", "3FA40E8A984D4815");   /* FIPS 81 ECB */
    des_vector(DES, "0101010101010101", "8000000000000000", "95F8A5E5DD31D900");   /* SP 800-17 */
    des_vector(DES, "0101010101010101", "0000000000000001", "166B40B44ABA4BD6");
    /* SP 800-67 example: three distinct keys. */
    const char *k3 = "0123456789ABCDEF23456789ABCDEF01456789ABCDEF0123";
    des_vector(DES3, k3, "5468652071756663", "A826FD8CE53B855F");
    des_vector(DES3, k3, "6B2062726F776E20", "CCE21C8112256FE6");
    des_vector(DES3, k3, "666F78206A756D70", "68D5C05DD9B6B900");
    /* K1 = K2 = K3 is single DES. */
    des_vector(DES3, "133457799BBCDFF1133457799BBCDFF1133457799BBCDFF1", "0123456789ABCDEF", "85E813540F0AB405");

    /* FIPS 81 CBC: "Now is the time for all " with IV 1234567890abcdef. */
    UCHAR k[8], iv[8], table[384], buf[24], ct[24];
    const char *pt = "Now is the time for all ";
    unhex(k, "0123456789abcdef");
    XcKeyTable(DES, table, k);
    unhex(iv, "1234567890abcdef");
    XcBlockCryptCBC(DES, 24, ct, (const UCHAR *)pt, table, ENCRYPT, iv);
    CHECK(eqhex(ct, "e5c7cdde872bf27c43e934008c389c0f683788499a7c05f6"));
    CHECK(!memcmp(iv, ct + 16, 8));                       /* feedback holds the last block */
    /* Feedback continues across calls; decrypt in place. */
    memcpy(buf, ct, 24);
    unhex(iv, "1234567890abcdef");
    XcBlockCryptCBC(DES, 8, buf, buf, table, DECRYPT, iv);
    XcBlockCryptCBC(DES, 16, buf + 8, buf + 8, table, DECRYPT, iv);
    CHECK(!memcmp(buf, pt, 24));

    UCHAR par[8] = { 0x00, 0x01, 0x02, 0x03, 0xFE, 0xFF, 0x80, 0x7F };
    XcDESKeyParity(par, 8);
    CHECK(eqhex(par, "01010202FEFE807F"));
}

/* ---- openssl cross-checks ------------------------------------------------ */

static char tmpdir[64];

static int run(const char *cmd, char *out, size_t size)
{
    char full[1024];
    snprintf(full, sizeof(full), "cd %s && %s", tmpdir, cmd);
    FILE *f = popen(full, "r");
    if (!f) return -1;
    size_t n = out ? fread(out, 1, size - 1, f) : 0;
    if (out) out[n] = 0;
    else while (fgetc(f) != EOF) {}
    return pclose(f);
}

static int read_file(const char *name, UCHAR *buf, size_t size)
{
    char path[128];
    snprintf(path, sizeof(path), "%s/%s", tmpdir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    int n = (int)fread(buf, 1, size, f);
    fclose(f);
    return n;
}

static void write_file(const char *name, const void *buf, size_t n)
{
    char path[128];
    snprintf(path, sizeof(path), "%s/%s", tmpdir, name);
    FILE *f = fopen(path, "wb");
    fwrite(buf, 1, n, f);
    fclose(f);
}

static void test_des3_cbc_openssl(void)
{
    UCHAR key[24], iv[8], fb[8], table[384], pt[64], ct[64], ref[64];
    for (int i = 0; i < 24; i++) key[i] = (UCHAR)(i * 37 + 11);
    XcDESKeyParity(key, 24);
    for (int i = 0; i < 8; i++) iv[i] = (UCHAR)(0xA5 ^ i * 3);
    for (int i = 0; i < 64; i++) pt[i] = (UCHAR)(i * 7);
    write_file("pt.bin", pt, 64);
    char cmd[512], kh[49], ih[17];
    for (int i = 0; i < 24; i++) sprintf(kh + 2 * i, "%02x", key[i]);
    for (int i = 0; i < 8; i++) sprintf(ih + 2 * i, "%02x", iv[i]);
    snprintf(cmd, sizeof(cmd), "openssl enc -des-ede3-cbc -nopad -K %s -iv %s -in pt.bin -out ct.bin 2>/dev/null",
             kh, ih);
    if (run(cmd, NULL, 0) != 0 || read_file("ct.bin", ref, 64) != 64) {
        printf("xcrypt_test: openssl des-ede3-cbc unavailable, skipped\n");
        return;
    }
    XcKeyTable(DES3, table, key);
    memcpy(fb, iv, 8);
    XcBlockCryptCBC(DES3, 64, ct, pt, table, ENCRYPT, fb);
    CHECK(!memcmp(ct, ref, 64));
}

/* An RSA key from `openssl rsa -text`, numbers big-endian. */
typedef struct { UCHAR n[512], d[512], p[256], q[256], dp[256], dq[256], qinv[256]; int nlen, bits; ULONG e; } rsakey;

static int parse_field(const char *text, const char *name, UCHAR *out, int width)
{
    char tag[32];
    snprintf(tag, sizeof(tag), "\n%s:\n", name);
    const char *s = strstr(text, tag);
    if (!s) return -1;
    s += strlen(tag);
    UCHAR tmp[600];
    int n = 0;
    while (*s == ' ') {
        const char *eol = strchr(s, '\n');
        for (; s < eol; s++) {
            unsigned v;
            if (s[0] != ' ' && s[0] != ':' && sscanf(s, "%2x", &v) == 1) { tmp[n++] = (UCHAR)v; s++; }
        }
        s = eol + 1;
    }
    int skip = 0;
    while (skip < n - 1 && !tmp[skip]) skip++;
    n -= skip;
    if (n > width) return -1;
    memset(out, 0, width - n);                  /* right-align to width */
    memcpy(out + width - n, tmp + skip, n);
    return n;
}

static int gen_key(int bits, const char *pem, rsakey *k)
{
    char cmd[256], *text = malloc(65536);
    snprintf(cmd, sizeof(cmd), "openssl genrsa -out %s %d 2>/dev/null && openssl rsa -in %s -text -noout",
             pem, bits, pem);
    int ok = run(cmd, text, 65536) == 0;
    memset(k, 0, sizeof(*k));
    k->bits = bits;
    k->nlen = bits / 8;
    int h = k->nlen / 2;
    ok = ok && parse_field(text, "modulus", k->n, k->nlen) > 0 && parse_field(text, "privateExponent", k->d, k->nlen) > 0 &&
         parse_field(text, "prime1", k->p, h) > 0 && parse_field(text, "prime2", k->q, h) > 0 &&
         parse_field(text, "exponent1", k->dp, h) > 0 && parse_field(text, "exponent2", k->dq, h) > 0 &&
         parse_field(text, "coefficient", k->qinv, h) > 0;
    const char *e = strstr(text, "publicExponent: ");
    k->e = e ? strtoul(e + 16, NULL, 10) : 0;
    free(text);
    return ok && k->e;
}

/* Big-endian n bytes into a little-endian field. */
static void put_le(UCHAR *dst, const UCHAR *be, int n)
{
    for (int i = 0; i < n; i++) dst[i] = be[n - 1 - i];
}

/* rsa.h's blobs, laid out as PreparePrivateKeyForImport (rsabase) does. */
static ULONG make_blobs(const rsakey *k, UCHAR *pub, UCHAR *prv)
{
    ULONG tmp = 8 - k->nlen % 8;
    if (tmp != 8) tmp += 8;
    ULONG keylen = k->nlen + tmp, half = keylen / 2;
    ULONG hdr[5] = { 0x31415352 /* RSA1 */, keylen, (ULONG)k->bits, (ULONG)k->nlen - 1, k->e };
    memset(pub, 0, 20 + keylen);
    memcpy(pub, hdr, 20);
    put_le(pub + 20, k->n, k->nlen);
    memset(prv, 0, 20 + keylen * 2 + 5 * half);
    hdr[0] = 0x32415352;   /* RSA2 */
    memcpy(prv, hdr, 20);
    UCHAR *o = prv + 20;
    put_le(o, k->n, k->nlen); o += keylen;
    put_le(o, k->p, k->nlen / 2); o += half;
    put_le(o, k->q, k->nlen / 2); o += half;
    put_le(o, k->dp, k->nlen / 2); o += half;
    put_le(o, k->dq, k->nlen / 2); o += half;
    put_le(o, k->qinv, k->nlen / 2); o += half;
    put_le(o, k->d, k->nlen);
    return keylen;
}

static void test_rsa_key(int bits, int do_private)
{
    char pem[32];
    snprintf(pem, sizeof(pem), "k%d.pem", bits);
    rsakey *k = malloc(sizeof(*k));
    if (!gen_key(bits, pem, k)) {
        printf("xcrypt_test: openssl genrsa unavailable, RSA %d skipped\n", bits);
        free(k);
        return;
    }
    UCHAR pub[600], prv[1600], in[520], out[520], back[520], be[512], ref[512];
    ULONG keylen = make_blobs(k, pub, prv);
    CHECK(XcPKGetKeyLen(pub) == keylen);
    if (bits == 2048) CHECK(keylen == 264 && 20 + keylen == 284);   /* XC_PUBLIC_KEYDATA_SIZE */

    /* Raw public encryption matches openssl's (no padding). */
    memset(in, 0, sizeof(in));
    for (int i = 0; i < k->nlen - 1; i++) in[i] = (UCHAR)(i * 13 + 5);
    put_le(be, in, k->nlen);                    /* the same number big-endian */
    write_file("m.bin", be, k->nlen);
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "openssl pkeyutl -encrypt -inkey %s -pkeyopt rsa_padding_mode:none "
             "-in m.bin -out c.bin 2>/dev/null", pem);
    CHECK(XcPKEncPublic(pub, in, out) == 1);
    if (run(cmd, NULL, 0) == 0 && read_file("c.bin", ref, sizeof(ref)) == k->nlen) {
        put_le(be, out, k->nlen);
        CHECK(!memcmp(be, ref, k->nlen));
        CHECK(out[k->nlen] == 0);
    } else {
        printf("xcrypt_test: openssl pkeyutl unavailable, raw RSA compare skipped\n");
    }
    if (do_private) {
        memset(back, 0xEE, sizeof(back));
        CHECK(XcPKDecPrivate(prv, out, back) == 1);
        CHECK(!memcmp(back, in, keylen));
    }

    /* openssl's PKCS #1 v1.5 SHA-1 signature verifies. */
    const char *msg = "xbcompat signed content";
    write_file("msg.txt", msg, strlen(msg));
    snprintf(cmd, sizeof(cmd), "openssl dgst -sha1 -sign %s -out sig.bin msg.txt 2>/dev/null", pem);
    UCHAR sigbe[512], sig[520], digest[20];
    if (run(cmd, NULL, 0) == 0 && read_file("sig.bin", sigbe, sizeof(sigbe)) == k->nlen) {
        put_le(sig, sigbe, k->nlen);
        sha1(msg, strlen(msg), digest);
        CHECK(XcVerifyPKCS1Signature(sig, pub, digest) == 1);
        digest[3] ^= 1;
        CHECK(XcVerifyPKCS1Signature(sig, pub, digest) == 0);
        digest[3] ^= 1;
        sig[7] ^= 0x40;
        CHECK(XcVerifyPKCS1Signature(sig, pub, digest) == 0);
        sig[7] ^= 0x40;
        CHECK(XcVerifyPKCS1Signature(sig, pub, digest) == 1);
    } else {
        printf("xcrypt_test: openssl dgst -sign unavailable, signature check skipped\n");
    }

    /* XCSignDigest's way: a type 1 block through XcPKDecPrivate. */
    if (do_private) {
        static const UCHAR enc[] = { 0x14, 0x04, 0x00, 0x05, 0x1a, 0x02, 0x03, 0x0e, 0x2b, 0x05, 0x06, 0x09,
                                     0x30, 0x21, 0x30 };
        sha1("other", 5, digest);
        ULONG datalen = k->nlen - 1;
        memset(in, 0, sizeof(in));
        memset(in, 0xFF, datalen - 1);
        in[datalen - 1] = 1;
        for (int i = 0; i < 20; i++) in[i] = digest[19 - i];
        memcpy(in + 20, enc, sizeof(enc));
        in[20 + sizeof(enc)] = 0;
        CHECK(XcPKDecPrivate(prv, in, out) == 1);
        CHECK(XcVerifyPKCS1Signature(out, pub, digest) == 1);
    }
    free(k);
}

/* ---- XcUpdateCrypto ---------------------------------------------------- */

static ULONG NTAPI my_service(ULONG op, PVOID args) { return op * (*(UCHAR *)args << 1); }   /* cryptotest.cpp's */
static int sha_inits;
static void NTAPI my_sha_init(UCHAR *ctx) { sha_inits++; memset(ctx, 0, 116); }

static void test_update(void)
{
    UCHAR arg = 5, ctx[116];
    CHECK(XcCryptService(3, &arg) == 0);
    ULONG vec[16] = { 0 }, rom[16], rom2[16];
    vec[0] = (ULONG)(uintptr_t)my_sha_init;
    vec[15] = (ULONG)(uintptr_t)my_service;
    XcUpdateCrypto(vec, rom);
    for (int i = 0; i < 16; i++) CHECK(rom[i] != 0);
    CHECK(XcCryptService(3, &arg) == 30);
    XcSHAInit(ctx);
    CHECK(sha_inits == 1);
    UCHAR d[20];
    XcSHAInit(ctx);           /* the others still run the ROM's */
    CHECK(sha_inits == 2);
    /* The ROM routines handed back still work when called directly. */
    ULONG (NTAPI *svc)(ULONG, PVOID) = (ULONG (NTAPI *)(ULONG, PVOID))(uintptr_t)rom[15];
    CHECK(svc(3, &arg) == 0);
    XcUpdateCrypto(rom, rom2);
    CHECK(!memcmp(rom, rom2, sizeof(rom)));
    CHECK(XcCryptService(3, &arg) == 0);
    sha1("abc", 3, d);
    CHECK(sha_inits == 2 && eqhex(d, "a9993e364706816aba3e25717850c26c9cd0d89d"));
}

int main(void)
{
    test_sha_hmac();
    test_des();
    strcpy(tmpdir, "/tmp/xcrypt_test.XXXXXX");
    if (!mkdtemp(tmpdir)) { perror("mkdtemp"); return 1; }
    test_des3_cbc_openssl();
    test_rsa_key(1024, 1);
    test_rsa_key(2048, 1);
    test_rsa_key(1536, 0);
    test_update();
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", tmpdir);
    if (system(cmd)) {}
    printf("xcrypt_test: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
