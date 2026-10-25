/* Exercises the real assembly entry without GL or Box86: stdcall/fastcall/
 * cdecl, 64-bit and x87 returns, stack cleanup and reentrant entry. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
struct result { uint32_t lo, hi, flo, fhi, fp, pop; };
#define STUB(n) __asm__(".text\n.globl stub" #n "\nstub" #n ":\npushl $" #n "\njmp xbr_entry\n");
STUB(0) STUB(1) STUB(2) STUB(3) STUB(4) STUB(5)
extern uint32_t __attribute__((stdcall)) stub0(uint32_t, uint32_t, uint32_t);
extern uint64_t __attribute__((fastcall)) stub1(uint32_t, uint32_t, uint32_t);
extern uint32_t stub2(uint32_t, uint32_t);
extern double __attribute__((stdcall)) stub3(double);
extern uint32_t __attribute__((stdcall)) stub4(uint32_t);
extern float __attribute__((fastcall)) stub5(uint32_t, float, uint32_t);
void xbr_do_call(uint32_t id, const uint32_t *s, uint32_t ecx, uint32_t edx, struct result *r)
{
    *r = (struct result){0};
    switch (id) {
    case 0: assert(s[0] == 11 && s[1] == 22 && s[2] == 33); r->lo = 66; r->pop = 12; break;
    case 1: assert(ecx == 44 && edx == 55 && s[0] == 66);
        r->lo = 0x89abcdef; r->hi = 0x12345678; r->pop = 4; break;
    case 2: assert(s[0] == 77 && s[1] == 88); r->lo = 165; break;
    case 3: {
        double d; memcpy(&d, s, 8); assert(d == 1.25);
        d *= 2; memcpy(&r->flo, &d, 8); r->fp = 1; r->pop = 8; break;
    }
    case 4: assert(s[0] == 99); r->lo = stub0(11,22,33); r->pop = 4; break;
    case 5: {
        float f; memcpy(&f, s, 4);
        assert(ecx == 9 && edx == 10 && f == 3.5f);
        double d = f; memcpy(&r->flo, &d, 8); r->fp = 1; r->pop = 4; break;
    }
    default: assert(0);
    }
}
int main(void)
{
    /* Repetition makes an incorrect callee/caller stack cleanup visible.
     * Optimized builds keep loop state in callee-saved registers. */
    for (int i = 0; i < 10000; i++) {
        assert(stub0(11,22,33) == 66);
        assert(stub1(44,55,66) == UINT64_C(0x1234567889abcdef));
        assert(stub2(77,88) == 165);
        assert(stub3(1.25) == 2.5);
        assert(stub4(99) == 66);
        assert(stub5(9,3.5f,10) == 3.5f);
    }
    puts("renderer entry ABI: 60000 calls passed");
}
