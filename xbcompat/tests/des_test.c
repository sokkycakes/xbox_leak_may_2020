/* Known-answer tests for src/kernel/des.c (FIPS 46 / SP 800-67 examples). */
#include <stdio.h>
#include <string.h>
#include "../src/xbcompat.h"

void NTAPI XcKeyTable(ULONG, UCHAR *, UCHAR *);
void NTAPI XcBlockCrypt(ULONG, UCHAR *, UCHAR *, UCHAR *, ULONG);
void NTAPI XcBlockCryptCBC(ULONG, ULONG, UCHAR *, UCHAR *, UCHAR *, ULONG, UCHAR *);

static int check(const char *what, const UCHAR *got, const UCHAR *want)
{
    if (!memcmp(got, want, 8)) return 0;
    printf("FAIL %s\n", what);
    return 1;
}

int main(void)
{
    UCHAR table[384], out[16], back[16];
    int bad = 0;
    UCHAR k1[8] = { 0x13, 0x34, 0x57, 0x79, 0x9B, 0xBC, 0xDF, 0xF1 };
    UCHAR p1[8] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF };
    UCHAR c1[8] = { 0x85, 0xE8, 0x13, 0x54, 0x0F, 0x0A, 0xB4, 0x05 };
    XcKeyTable(0, table, k1);
    XcBlockCrypt(0, out, p1, table, 1);
    bad += check("des encrypt", out, c1);
    XcBlockCrypt(0, back, out, table, 0);
    bad += check("des decrypt", back, p1);

    /* Triple DES, three distinct keys (NIST SP 800-67 example, first block). */
    UCHAR k3[24] = { 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0x01,
                     0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF, 0x01, 0x23 };
    UCHAR p3[8] = { 0x54, 0x68, 0x65, 0x20, 0x71, 0x75, 0x66, 0x63 };   /* "The qufc" */
    UCHAR c3[8] = { 0xA8, 0x26, 0xFD, 0x8C, 0xE5, 0x3B, 0x85, 0x5F };
    XcKeyTable(1, table, k3);
    XcBlockCrypt(1, out, p3, table, 1);
    bad += check("3des encrypt", out, c3);
    XcBlockCrypt(1, back, out, table, 0);
    bad += check("3des decrypt", back, p3);

    /* CBC round trip over two blocks, in place. */
    UCHAR iv[8] = { 1, 2, 3, 4, 5, 6, 7, 8 }, iv2[8], buf[16], orig[16];
    for (int i = 0; i < 16; i++) orig[i] = buf[i] = (UCHAR)(i * 7);
    memcpy(iv2, iv, 8);
    XcBlockCryptCBC(1, 16, buf, buf, table, 1, iv);
    XcBlockCryptCBC(1, 16, buf, buf, table, 0, iv2);
    bad += check("cbc round trip", buf, orig) + check("cbc round trip 2", buf + 8, orig + 8);
    puts(bad ? "des_test: FAILED" : "des_test: ok");
    return bad != 0;
}
