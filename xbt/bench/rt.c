#include "rt.h"
/* rep movsd / stosd, like the MSVC CRT the Xbox titles link */
void *memcpy(void *d, const void *s, unsigned n)
{
    void *r = d;
    unsigned dw = n >> 2, tail = n & 3;
    __asm__ volatile("rep movsl" : "+D"(d), "+S"(s), "+c"(dw) : : "memory");
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(tail) : : "memory");
    return r;
}
void *memset(void *d, int c, unsigned n)
{
    void *r = d;
    u32 v = (u8)c * 0x01010101u;
    unsigned dw = n >> 2, tail = n & 3;
    __asm__ volatile("rep stosl" : "+D"(d), "+c"(dw) : "a"(v) : "memory");
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(tail) : "a"(v) : "memory");
    return r;
}
int bench(void);
void _start_c(void) { u32 r = (u32)bench(); put("result "); puthex(r); put("\n"); sys_exit(0); }
__asm__(".globl _start\n_start:\n  andl $-16, %esp\n  call _start_c\n");
