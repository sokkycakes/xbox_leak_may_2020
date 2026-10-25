/* rt.h - tiny freestanding runtime for the i386 benchmarks (Linux int 0x80) */
#ifndef BENCH_RT_H
#define BENCH_RT_H
typedef unsigned int u32;
typedef unsigned long long u64;
typedef unsigned char u8;

static inline int sys3(int n, int a, int b, int c)
{
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}
static inline void sys_exit(int c) { sys3(1, c, 0, 0); for (;;) {} }
static inline void put(const char *s) { int n = 0; while (s[n]) n++; sys3(4, 1, (int)s, n); }
static inline void puthex(u32 v)
{
    char b[11] = "0x00000000";
    for (int i = 0; i < 8; i++) b[9 - i] = "0123456789abcdef"[(v >> (4 * i)) & 15];
    put(b);
}
void *memcpy(void *d, const void *s, unsigned n);
void *memset(void *d, int c, unsigned n);
#endif
