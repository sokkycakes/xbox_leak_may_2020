/* integer workloads: crc32, quicksort, string hashing, 64-bit math, interpreter */
#include "rt.h"
static u32 crc_table[256];
static u8 buf[65536];
static int arr[20000];

static void crc_init(void)
{
    for (u32 i = 0; i < 256; i++) {
        u32 c = i;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}
static u32 crc32(const u8 *p, u32 n)
{
    u32 c = 0xffffffffu;
    while (n--) c = crc_table[(c ^ *p++) & 0xff] ^ (c >> 8);
    return ~c;
}
static void qsort_int(int *a, int lo, int hi)
{
    while (lo < hi) {
        int p = a[(lo + hi) >> 1], i = lo, j = hi;
        while (i <= j) {
            while (a[i] < p) i++;
            while (a[j] > p) j--;
            if (i <= j) { int t = a[i]; a[i] = a[j]; a[j] = t; i++; j--; }
        }
        if (j - lo < hi - i) { qsort_int(a, lo, j); lo = i; } else { qsort_int(a, i, hi); hi = j; }
    }
}
static u32 strhash(void)
{
    static char words[512][16];
    u32 seed = 12345, h = 2166136261u;
    for (int i = 0; i < 512; i++) {
        int len = 3 + (int)((seed = seed * 1103515245u + 12345u) >> 28);
        for (int k = 0; k < len; k++) words[i][k] = (char)('a' + ((seed = seed * 1103515245u + 12345u) >> 27));
        words[i][len] = 0;
    }
    for (int rep = 0; rep < 60; rep++)
        for (int i = 0; i < 512; i++) {
            const char *s = words[i];
            int n = 0;
            while (s[n]) n++;
            for (int k = 0; k < n; k++) h = (h ^ (u8)s[k]) * 16777619u;
            if (n > 5 && s[0] == s[n - 1]) h += (u32)n;
        }
    return h;
}
static u32 math64(void)
{
    u64 x = 88172645463325252ull, acc = 0;
    for (int i = 0; i < 300000; i++) {
        x = x * 6364136223846793005ull + 1442695040888963407ull;
        acc += (x >> 33) ^ (acc << 1);
        if ((i & 255) == 0) acc /= 3;
    }
    return (u32)acc ^ (u32)(acc >> 32);
}
/* bytecode interpreter: switch dispatch through a jump table */
static u32 interp(void)
{
    static const u8 prog[] = { 1, 7, 2, 3, 4, 1, 5, 6, 2, 8, 3, 1, 9, 4, 6, 0 };
    u32 acc = 1, r1 = 3, steps = 0;
    for (int rep = 0; rep < 40000; rep++) {
        for (int pc = 0; prog[pc]; pc++) {
            switch (prog[pc]) {
            case 1: acc += r1; break;
            case 2: acc ^= acc << 3; break;
            case 3: r1 = acc >> 5; break;
            case 4: acc = acc * 33 + 7; break;
            case 5: r1 ^= acc; break;
            case 6: acc -= r1 & 0xff; break;
            case 7: acc = (acc << 7) | (acc >> 25); break;
            case 8: r1 += 13; break;
            case 9: if (acc & 1) acc >>= 1; else acc += 3; break;
            }
            steps++;
        }
    }
    return acc ^ r1 ^ steps;
}
int bench(void)
{
    u32 r = 0, seed = 1;
    crc_init();
    for (u32 i = 0; i < sizeof buf; i++) buf[i] = (u8)((seed = seed * 1664525u + 1013904223u) >> 24);
    for (int k = 0; k < 6; k++) r ^= crc32(buf, sizeof buf);
    for (int k = 0; k < 2; k++) {
        for (int i = 0; i < 20000; i++) arr[i] = (int)((seed = seed * 1664525u + 1013904223u) >> 8);
        qsort_int(arr, 0, 19999);
        r += (u32)arr[100] ^ (u32)arr[19000];
    }
    r ^= strhash();
    r ^= math64();
    r ^= interp();
    static u8 dst[65536];
    for (int k = 0; k < 40; k++) { memcpy(dst, buf + k, 60000); r += dst[k * 7]; }
    return (int)r;
}
