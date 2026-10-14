/*
 * difftest: random x86 instructions on the real CPU against xlat.
 *
 *   difftest gen N SEED FILE    run N random tests natively (i386 only),
 *                               write them (inputs as seeds, outputs) to FILE
 *   difftest check FILE         run FILE's tests on xlat, report differences
 *   difftest run N SEED         both in one process (i386 only)
 *   ... -f FILTER               only templates whose name contains FILTER
 *   ... -s                      sequences: up to two more instructions after it
 *
 * Each test is one instruction from a template list, with random operands
 * (registers, memory forms pointing into a test buffer, immediates) and a
 * random CPU state: general registers, flags, x87, MMX, SSE.  A test's
 * input state is regenerated from its seed, so FILE holds only the seed,
 * the instruction and the native result (registers, flags, x87/SSE state
 * and the bytes of the buffer that changed).  The checker runs on any host:
 * built for ARM it checks the ARM code generator under qemu-arm.
 *
 * Build (from xbcompat/):
 *   gcc -m32 -msse2 -mfpmath=sse -O1 -g -Isrc -o build-xlat/difftest tools/xlat/difftest.c \
 *       src/xlat/decode.c src/xlat/interp.c src/xlat/fpu.c -lm
 * and the ARM checker, with the code generator:
 *   arm-linux-gnueabihf-gcc -static -O1 -g -Isrc -DXLAT_RUN -o build-armhf-xlat/difftest \
 *       tools/xlat/difftest.c src/xlat/decode.c src/xlat/interp.c src/xlat/fpu.c src/xlat/jit_arm.c -lm -lpthread
 */
#define _GNU_SOURCE
#include <math.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "xlat/xlat.h"

#define MEM_ADDR  0x30000000u
#define MEM_SIZE  2048u
#define DATA_PTR  (MEM_ADDR + 512)          /* base registers point around here */
#define STACK_TOP (MEM_ADDR + MEM_SIZE - 64)
#define CODE_ADDR 0x30100000u

/* ---- random ----------------------------------------------------------- */

static uint64_t rs;
static uint32_t rnd(void)
{
    rs ^= rs >> 12; rs ^= rs << 25; rs ^= rs >> 27;
    return (uint32_t)((rs * 0x2545F4914F6CDD1Dull) >> 32);
}
static uint32_t rn(uint32_t n) { return n ? rnd() % n : 0; }

static uint32_t rand_int(void)
{
    static const uint32_t special[] = { 0, 1, 2, 3, 0x7F, 0x80, 0xFF, 0x100, 0x7FFF, 0x8000, 0xFFFF, 0x10000,
                                        0x7FFFFFFF, 0x80000000u, 0x80000001u, 0xFFFFFFFEu, 0xFFFFFFFFu, 31, 32,
                                        33, 63, 0x8080, 0x7F7F, 0xFF00 };
    switch (rn(4)) {
    case 0: return special[rn(sizeof(special) / 4)];
    case 1: return rn(64);
    default: return rnd();
    }
}

static double rand_double(void)
{
    static const double special[] = { 0.0, -0.0, 1.0, -1.0, 0.5, 2.0, 3.0, 10.0, 100.0, 1e10, -1e10, 1e-10,
                                      0.25, 1.5, -2.5, 65536.0, 2147483647.0, -2147483648.0, 4294967296.0,
                                      3.14159, 1e30, -1e-30, INFINITY, -INFINITY, NAN };
    switch (rn(4)) {
    case 0: return special[rn(sizeof(special) / sizeof(special[0]))];
    case 1: return (double)(int32_t)rand_int() / (1 << rn(8));
    default: {
        double m = (double)rnd() / 4294967296.0 + 0.5;
        int e = (int)rn(60) - 30;
        return (rn(2) ? -m : m) * ldexp(1.0, e);
    }
    }
}

/* ---- templates -------------------------------------------------------- */

enum { C_INT, C_X87, C_MMX, C_SSE };

/* template flags */
#define T_B      0x0001   /* byte operands: reg/rm 4-7 are AH..BH */
#define T_EB     0x0002   /* only the r/m operand is a byte (movzx) */
#define T_EW     0x0004   /* r/m is a word register */
#define T_MEM    0x0008   /* memory r/m only */
#define T_REG    0x0010   /* register r/m only */
#define T_STR    0x0020   /* string instruction: esi/edi pointers, small ecx */
#define T_EBP    0x0040   /* ebp points at the stack (leave, enter) */
#define T_EBX    0x0080   /* ebx points at data (xlat) */
#define T_GMM    0x0100   /* reg field is an MMX register */
#define T_EMM    0x0200   /* r/m register is an MMX register */
#define T_GX     0x0400   /* reg field is an XMM register */
#define T_EX     0x0800   /* r/m register is an XMM register */
#define T_ST     0x1000   /* r/m register is st(i) (x87 register forms) */
#define T_APPROX 0x2000   /* results compared with a tolerance */
#define T_CC     0x4000   /* x87 condition codes are defined */
#define T_POPF   0x8000   /* stack top holds a sane EFLAGS value */
#define T_FCW    0x10000  /* memory holds sane control words (fldcw, ldmxcsr) */
#define T_SHIFT1 0x20000  /* OF only defined for a count of 1 (imm or cl) */
#define T_MOFFS  0x40000  /* moffs into the data buffer */
#define T_ENTER  0x80000  /* small enter operands */
#define T_DIV    0x100000 /* divisor nonzero, quotient small */
#define T_RC     0x200000 /* the rounding mode decides the result: test them all */
#define T_SMALL  0x400000 /* st0 in [-1, 1] (f2xm1, fyl2xp1, transcendentals) */

struct tmpl {
    const char *name;
    const char *enc;    /* hex bytes; /r /0-/7 ModRM; ib iw id; +r (opcode+reg) +i (st index) */
    int cls;
    uint32_t flags;
    uint32_t undef;     /* EFLAGS bits left undefined */
};

#define U_LOGIC EF_AF
#define U_SHIFT EF_AF
#define U_MUL (EF_SF | EF_ZF | EF_AF | EF_PF)
#define U_DIV EF_ARITH
#define U_BT (EF_OF | EF_SF | EF_AF | EF_PF)
#define U_BSF (EF_CF | EF_OF | EF_SF | EF_AF | EF_PF)

static const struct tmpl tmpls[] = {
    /* ALU */
    { "add Eb,Gb", "00 /r", C_INT, T_B, 0 }, { "add Ev,Gv", "01 /r", C_INT, 0, 0 },
    { "add Gb,Eb", "02 /r", C_INT, T_B, 0 }, { "add Gv,Ev", "03 /r", C_INT, 0, 0 },
    { "add al,ib", "04 ib", C_INT, 0, 0 }, { "add eax,id", "05 id", C_INT, 0, 0 },
    { "add Ew,Gw", "66 01 /r", C_INT, 0, 0 }, { "add ax,iw", "66 05 iw", C_INT, 0, 0 },
    { "or Eb,Gb", "08 /r", C_INT, T_B, U_LOGIC }, { "or Ev,Gv", "09 /r", C_INT, 0, U_LOGIC },
    { "or Gv,Ev", "0B /r", C_INT, 0, U_LOGIC }, { "or eax,id", "0D id", C_INT, 0, U_LOGIC },
    { "adc Eb,Gb", "10 /r", C_INT, T_B, 0 }, { "adc Ev,Gv", "11 /r", C_INT, 0, 0 },
    { "adc Gv,Ev", "13 /r", C_INT, 0, 0 }, { "adc Ew,Gw", "66 11 /r", C_INT, 0, 0 },
    { "sbb Eb,Gb", "18 /r", C_INT, T_B, 0 }, { "sbb Ev,Gv", "19 /r", C_INT, 0, 0 },
    { "sbb Gv,Ev", "1B /r", C_INT, 0, 0 }, { "sbb Ew,Gw", "66 19 /r", C_INT, 0, 0 },
    { "and Eb,Gb", "20 /r", C_INT, T_B, U_LOGIC }, { "and Ev,Gv", "21 /r", C_INT, 0, U_LOGIC },
    { "and Gv,Ev", "23 /r", C_INT, 0, U_LOGIC }, { "and al,ib", "24 ib", C_INT, 0, U_LOGIC },
    { "sub Eb,Gb", "28 /r", C_INT, T_B, 0 }, { "sub Ev,Gv", "29 /r", C_INT, 0, 0 },
    { "sub Gv,Ev", "2B /r", C_INT, 0, 0 }, { "sub eax,id", "2D id", C_INT, 0, 0 },
    { "sub Ew,Gw", "66 29 /r", C_INT, 0, 0 },
    { "xor Eb,Gb", "30 /r", C_INT, T_B, U_LOGIC }, { "xor Ev,Gv", "31 /r", C_INT, 0, U_LOGIC },
    { "xor Gv,Ev", "33 /r", C_INT, 0, U_LOGIC },
    { "cmp Eb,Gb", "38 /r", C_INT, T_B, 0 }, { "cmp Ev,Gv", "39 /r", C_INT, 0, 0 },
    { "cmp Gb,Eb", "3A /r", C_INT, T_B, 0 }, { "cmp Gv,Ev", "3B /r", C_INT, 0, 0 },
    { "cmp al,ib", "3C ib", C_INT, 0, 0 }, { "cmp eax,id", "3D id", C_INT, 0, 0 },
    { "cmp Ew,Gw", "66 39 /r", C_INT, 0, 0 },
    { "grp1 Eb,ib", "80 /x ib", C_INT, T_B, 0 }, { "grp1 Ev,id", "81 /x id", C_INT, 0, 0 },
    { "grp1 Ev,ib", "83 /x ib", C_INT, 0, 0 }, { "grp1 Ew,iw", "66 81 /x iw", C_INT, 0, 0 },
    { "grp1 Ew,ib", "66 83 /x ib", C_INT, 0, 0 },
    { "lock add Ev,Gv", "F0 01 /r", C_INT, T_MEM, 0 }, { "lock sub Eb,Gb", "F0 28 /r", C_INT, T_MEM | T_B, 0 },
    { "lock or Ev,Gv", "F0 09 /r", C_INT, T_MEM, U_LOGIC }, { "lock adc Ev,Gv", "F0 11 /r", C_INT, T_MEM, 0 },
    { "lock grp1 Ev,ib", "F0 83 /x ib", C_INT, T_MEM, 0 }, { "lock grp1 Ew,iw", "F0 66 81 /x iw", C_INT, T_MEM, 0 },
    /* inc dec */
    { "inc r", "40+r", C_INT, 0, 0 }, { "dec r", "48+r", C_INT, 0, 0 }, { "inc rw", "66 40+r", C_INT, 0, 0 },
    { "inc Eb", "FE /0", C_INT, T_B, 0 }, { "dec Eb", "FE /1", C_INT, T_B, 0 },
    { "inc Ev", "FF /0", C_INT, 0, 0 }, { "dec Ev", "FF /1", C_INT, 0, 0 },
    { "lock inc Ev", "F0 FF /0", C_INT, T_MEM, 0 }, { "lock dec Eb", "F0 FE /1", C_INT, T_MEM | T_B, 0 },
    /* test */
    { "test Eb,Gb", "84 /r", C_INT, T_B, U_LOGIC }, { "test Ev,Gv", "85 /r", C_INT, 0, U_LOGIC },
    { "test al,ib", "A8 ib", C_INT, 0, U_LOGIC }, { "test eax,id", "A9 id", C_INT, 0, U_LOGIC },
    { "test Eb,ib", "F6 /0 ib", C_INT, T_B, U_LOGIC }, { "test Ev,id", "F7 /0 id", C_INT, 0, U_LOGIC },
    /* group 3 */
    { "not Eb", "F6 /2", C_INT, T_B, 0 }, { "neg Eb", "F6 /3", C_INT, T_B, 0 },
    { "mul Eb", "F6 /4", C_INT, T_B, U_MUL }, { "imul Eb", "F6 /5", C_INT, T_B, U_MUL },
    { "not Ev", "F7 /2", C_INT, 0, 0 }, { "neg Ev", "F7 /3", C_INT, 0, 0 },
    { "mul Ev", "F7 /4", C_INT, 0, U_MUL }, { "imul Ev", "F7 /5", C_INT, 0, U_MUL },
    { "neg Ew", "66 F7 /3", C_INT, 0, 0 }, { "mul Ew", "66 F7 /4", C_INT, 0, U_MUL },
    { "imul Ew", "66 F7 /5", C_INT, 0, U_MUL },
    { "div Eb", "F6 /6", C_INT, T_B | T_DIV, U_DIV }, { "idiv Eb", "F6 /7", C_INT, T_B | T_DIV, U_DIV },
    { "div Ev", "F7 /6", C_INT, T_DIV, U_DIV }, { "idiv Ev", "F7 /7", C_INT, T_DIV, U_DIV },
    { "div Ew", "66 F7 /6", C_INT, T_DIV, U_DIV }, { "idiv Ew", "66 F7 /7", C_INT, T_DIV, U_DIV },
    { "lock neg Ev", "F0 F7 /3", C_INT, T_MEM, 0 }, { "lock not Eb", "F0 F6 /2", C_INT, T_MEM | T_B, 0 },
    { "imul Gv,Ev", "0F AF /r", C_INT, 0, U_MUL }, { "imul Gv,Ev,id", "69 /r id", C_INT, 0, U_MUL },
    { "imul Gv,Ev,ib", "6B /r ib", C_INT, 0, U_MUL }, { "imul Gw,Ew", "66 0F AF /r", C_INT, 0, U_MUL },
    { "imul Gw,Ew,ib", "66 6B /r ib", C_INT, 0, U_MUL },
    /* shifts */
    { "grp2 Eb,ib", "C0 /x ib", C_INT, T_B | T_SHIFT1, U_SHIFT }, { "grp2 Ev,ib", "C1 /x ib", C_INT, T_SHIFT1, U_SHIFT },
    { "grp2 Eb,1", "D0 /x", C_INT, T_B, U_SHIFT }, { "grp2 Ev,1", "D1 /x", C_INT, 0, U_SHIFT },
    { "grp2 Eb,cl", "D2 /x", C_INT, T_B | T_SHIFT1, U_SHIFT }, { "grp2 Ev,cl", "D3 /x", C_INT, T_SHIFT1, U_SHIFT },
    { "grp2 Ew,ib", "66 C1 /x ib", C_INT, T_SHIFT1, U_SHIFT }, { "grp2 Ew,cl", "66 D3 /x", C_INT, T_SHIFT1, U_SHIFT },
    { "shld Ev,Gv,ib", "0F A4 /r ib", C_INT, T_SHIFT1, U_SHIFT }, { "shld Ev,Gv,cl", "0F A5 /r", C_INT, T_SHIFT1, U_SHIFT },
    { "shrd Ev,Gv,ib", "0F AC /r ib", C_INT, T_SHIFT1, U_SHIFT }, { "shrd Ev,Gv,cl", "0F AD /r", C_INT, T_SHIFT1, U_SHIFT },
    /* bits */
    { "bt Ev,Gv", "0F A3 /r", C_INT, 0, U_BT }, { "bts Ev,Gv", "0F AB /r", C_INT, 0, U_BT },
    { "btr Ev,Gv", "0F B3 /r", C_INT, 0, U_BT }, { "btc Ev,Gv", "0F BB /r", C_INT, 0, U_BT },
    { "bt Ew,Gw", "66 0F A3 /r", C_INT, 0, U_BT }, { "grp8 Ev,ib", "0F BA /y ib", C_INT, 0, U_BT },
    { "lock bts Ev,Gv", "F0 0F AB /r", C_INT, T_MEM, U_BT }, { "lock grp8 Ev,ib", "F0 0F BA /y ib", C_INT, T_MEM, U_BT },
    { "bsf Gv,Ev", "0F BC /r", C_INT, 0, U_BSF }, { "bsr Gv,Ev", "0F BD /r", C_INT, 0, U_BSF },
    { "bswap r", "0F C8+r", C_INT, 0, 0 },
    /* moves */
    { "mov Eb,Gb", "88 /r", C_INT, T_B, 0 }, { "mov Ev,Gv", "89 /r", C_INT, 0, 0 },
    { "mov Gb,Eb", "8A /r", C_INT, T_B, 0 }, { "mov Gv,Ev", "8B /r", C_INT, 0, 0 },
    { "mov Ew,Gw", "66 89 /r", C_INT, 0, 0 }, { "mov Eb,ib", "C6 /0 ib", C_INT, T_B, 0 },
    { "mov Ev,id", "C7 /0 id", C_INT, 0, 0 }, { "mov Ew,iw", "66 C7 /0 iw", C_INT, 0, 0 },
    { "mov r8,ib", "B0+R ib", C_INT, 0, 0 }, { "mov r,id", "B8+r id", C_INT, 0, 0 },
    { "mov eax,moffs", "A1 md", C_INT, T_MOFFS, 0 }, { "mov moffs,al", "A2 md", C_INT, T_MOFFS, 0 },
    { "lea Gv,M", "8D /r", C_INT, T_MEM, 0 }, { "lea Gw,M", "66 8D /r", C_INT, T_MEM, 0 },
    { "movzx Gv,Eb", "0F B6 /r", C_INT, T_EB, 0 }, { "movzx Gv,Ew", "0F B7 /r", C_INT, T_EW, 0 },
    { "movsx Gv,Eb", "0F BE /r", C_INT, T_EB, 0 }, { "movsx Gv,Ew", "0F BF /r", C_INT, T_EW, 0 },
    { "movzx Gw,Eb", "66 0F B6 /r", C_INT, T_EB, 0 },
    { "cmovcc Gv,Ev", "0F 40+c /r", C_INT, 0, 0 }, { "setcc Eb", "0F 90+c /0", C_INT, T_EB, 0 },
    { "xchg Eb,Gb", "86 /r", C_INT, T_B, 0 }, { "xchg Ev,Gv", "87 /r", C_INT, 0, 0 },
    { "xchg eax,r", "90+r", C_INT, 0, 0 },
    { "xadd Eb,Gb", "0F C0 /r", C_INT, T_B, 0 }, { "xadd Ev,Gv", "0F C1 /r", C_INT, 0, 0 },
    { "lock xadd Ev,Gv", "F0 0F C1 /r", C_INT, T_MEM, 0 },
    { "cmpxchg Eb,Gb", "0F B0 /r", C_INT, T_B, 0 }, { "cmpxchg Ev,Gv", "0F B1 /r", C_INT, 0, 0 },
    { "lock cmpxchg Ev,Gv", "F0 0F B1 /r", C_INT, T_MEM, 0 },
    { "cmpxchg8b M", "0F C7 /1", C_INT, T_MEM, EF_ARITH & ~EF_ZF },
    { "lock cmpxchg8b M", "F0 0F C7 /1", C_INT, T_MEM, EF_ARITH & ~EF_ZF },
    { "cwde", "98", C_INT, 0, 0 }, { "cbw", "66 98", C_INT, 0, 0 }, { "cdq", "99", C_INT, 0, 0 },
    { "cwd", "66 99", C_INT, 0, 0 },
    { "sahf", "9E", C_INT, 0, 0 }, { "lahf", "9F", C_INT, 0, 0 },
    { "clc", "F8", C_INT, 0, 0 }, { "stc", "F9", C_INT, 0, 0 }, { "cmc", "F5", C_INT, 0, 0 },
    { "cld", "FC", C_INT, 0, 0 }, { "std", "FD", C_INT, 0, 0 },
    /* stack */
    { "push r", "50+r", C_INT, 0, 0 }, { "pop r", "58+r", C_INT, 0, 0 }, { "push id", "68 id", C_INT, 0, 0 },
    { "push ib", "6A ib", C_INT, 0, 0 }, { "push M", "FF /6", C_INT, T_MEM, 0 }, { "pop M", "8F /0", C_INT, T_MEM, 0 },
    { "push rw", "66 50+r", C_INT, 0, 0 }, { "pusha", "60", C_INT, 0, 0 }, { "popa", "61", C_INT, 0, 0 },
    { "pushf", "9C", C_INT, 0, 0 }, { "popf", "9D", C_INT, T_POPF, 0 },
    { "leave", "C9", C_INT, T_EBP, 0 }, { "enter", "C8 iw ib", C_INT, T_EBP | T_ENTER, 0 },
    /* strings */
    { "movsb", "A4", C_INT, T_STR, 0 }, { "movsd", "A5", C_INT, T_STR, 0 }, { "movsw", "66 A5", C_INT, T_STR, 0 },
    { "rep movsb", "F3 A4", C_INT, T_STR, 0 }, { "rep movsd", "F3 A5", C_INT, T_STR, 0 },
    { "stosb", "AA", C_INT, T_STR, 0 }, { "rep stosd", "F3 AB", C_INT, T_STR, 0 }, { "rep stosb", "F3 AA", C_INT, T_STR, 0 },
    { "lodsd", "AD", C_INT, T_STR, 0 }, { "lodsb", "AC", C_INT, T_STR, 0 },
    { "cmpsb", "A6", C_INT, T_STR, 0 }, { "repe cmpsb", "F3 A6", C_INT, T_STR, 0 }, { "repne cmpsd", "F2 A7", C_INT, T_STR, 0 },
    { "scasb", "AE", C_INT, T_STR, 0 }, { "repne scasb", "F2 AE", C_INT, T_STR, 0 }, { "repe scasd", "F3 AF", C_INT, T_STR, 0 },
    /* BCD */
    { "daa", "27", C_INT, 0, EF_OF }, { "das", "2F", C_INT, 0, EF_OF },
    { "aaa", "37", C_INT, 0, EF_OF | EF_SF | EF_ZF | EF_PF }, { "aas", "3F", C_INT, 0, EF_OF | EF_SF | EF_ZF | EF_PF },
    { "aam", "D4 0A", C_INT, 0, EF_OF | EF_AF | EF_CF }, { "aad", "D5 0A", C_INT, 0, EF_OF | EF_AF | EF_CF },
    { "xlat", "D7", C_INT, T_EBX, 0 },
    /* branches to the next instruction (both ways): the translation's exits */
    { "jcc +0", "70+c 00", C_INT, 0, 0 }, { "jcc32 +0", "0F 80+c 00 00 00 00", C_INT, 0, 0 },
    { "jmp +0", "EB 00", C_INT, 0, 0 }, { "loop +0", "E2 00", C_INT, 0, 0 }, { "jecxz +0", "E3 00", C_INT, 0, 0 },

    /* x87 */
    { "fop m32", "D8 /x", C_X87, T_MEM | T_CC, 0 }, { "fop st,st(i)", "D8 /x", C_X87, T_ST | T_CC, 0 },
    { "fld m32", "D9 /0", C_X87, T_MEM, 0 }, { "fst m32", "D9 /2", C_X87, T_MEM, 0 }, { "fstp m32", "D9 /3", C_X87, T_MEM, 0 },
    { "fldcw", "D9 /5", C_X87, T_MEM | T_FCW | T_RC, 0 }, { "fnstcw", "D9 /7", C_X87, T_MEM, 0 },
    { "fld st(i)", "D9 C0+i", C_X87, 0, 0 }, { "fxch", "D9 C8+i", C_X87, 0, 0 },
    { "fchs", "D9 E0", C_X87, 0, 0 }, { "fabs", "D9 E1", C_X87, 0, 0 }, { "ftst", "D9 E4", C_X87, T_CC, 0 },
    { "fxam", "D9 E5", C_X87, T_CC, 0 }, { "fld1", "D9 E8", C_X87, 0, 0 }, { "fldl2t", "D9 E9", C_X87, T_APPROX, 0 },
    { "fldl2e", "D9 EA", C_X87, T_APPROX, 0 }, { "fldpi", "D9 EB", C_X87, T_APPROX, 0 }, { "fldlg2", "D9 EC", C_X87, T_APPROX, 0 },
    { "fldln2", "D9 ED", C_X87, T_APPROX, 0 }, { "fldz", "D9 EE", C_X87, 0, 0 },
    { "f2xm1", "D9 F0", C_X87, T_APPROX | T_SMALL, 0 }, { "fyl2x", "D9 F1", C_X87, T_APPROX | T_SMALL, 0 }, { "fptan", "D9 F2", C_X87, T_APPROX | T_SMALL, 0 },
    { "fpatan", "D9 F3", C_X87, T_APPROX, 0 }, { "fxtract", "D9 F4", C_X87, 0, 0 }, { "fprem1", "D9 F5", C_X87, T_CC, 0 },
    { "fdecstp", "D9 F6", C_X87, 0, 0 }, { "fincstp", "D9 F7", C_X87, 0, 0 }, { "fprem", "D9 F8", C_X87, T_CC, 0 },
    { "fyl2xp1", "D9 F9", C_X87, T_APPROX | T_SMALL, 0 }, { "fsqrt", "D9 FA", C_X87, 0, 0 }, { "fsincos", "D9 FB", C_X87, T_APPROX | T_SMALL, 0 },
    { "frndint", "D9 FC", C_X87, T_RC, 0 }, { "fscale", "D9 FD", C_X87, 0, 0 }, { "fsin", "D9 FE", C_X87, T_APPROX | T_SMALL, 0 },
    { "fcos", "D9 FF", C_X87, T_APPROX | T_SMALL, 0 },
    { "fiop m32", "DA /x", C_X87, T_MEM | T_CC, 0 }, { "fcmovb", "DA C0+i", C_X87, 0, 0 }, { "fcmove", "DA C8+i", C_X87, 0, 0 },
    { "fcmovbe", "DA D0+i", C_X87, 0, 0 }, { "fcmovu", "DA D8+i", C_X87, 0, 0 },
    { "fucompp", "DA E9", C_X87, T_CC, 0 },
    { "fild m32", "DB /0", C_X87, T_MEM, 0 }, { "fist m32", "DB /2", C_X87, T_MEM | T_RC, 0 }, { "fistp m32", "DB /3", C_X87, T_MEM | T_RC, 0 },
    { "fld m80", "DB /5", C_X87, T_MEM, 0 }, { "fstp m80", "DB /7", C_X87, T_MEM, 0 },
    { "fcmovnb", "DB C0+i", C_X87, 0, 0 }, { "fcmovne", "DB C8+i", C_X87, 0, 0 },
    { "fcmovnbe", "DB D0+i", C_X87, 0, 0 }, { "fcmovnu", "DB D8+i", C_X87, 0, 0 }, { "fucomi", "DB E8+i", C_X87, 0, 0 }, { "fcomi", "DB F0+i", C_X87, 0, 0 },
    { "fninit", "DB E3", C_X87, 0, 0 },
    { "fop m64", "DC /x", C_X87, T_MEM | T_CC, 0 }, { "fop st(i),st", "DC /x", C_X87, T_ST | T_CC, 0 },
    { "fld m64", "DD /0", C_X87, T_MEM, 0 }, { "fst m64", "DD /2", C_X87, T_MEM, 0 }, { "fstp m64", "DD /3", C_X87, T_MEM, 0 },
    { "fnstsw m16", "DD /7", C_X87, T_MEM, 0 }, { "ffree", "DD C0+i", C_X87, 0, 0 }, { "fst st(i)", "DD D0+i", C_X87, 0, 0 },
    { "fstp st(i)", "DD D8+i", C_X87, 0, 0 }, { "fucom", "DD E0+i", C_X87, T_CC, 0 }, { "fucomp", "DD E8+i", C_X87, T_CC, 0 },
    { "fiop m16", "DE /x", C_X87, T_MEM | T_CC, 0 }, { "fopp st(i),st", "DE /x", C_X87, T_ST | T_CC, 0 },
    { "fild m16", "DF /0", C_X87, T_MEM, 0 }, { "fist m16", "DF /2", C_X87, T_MEM | T_RC, 0 }, { "fistp m16", "DF /3", C_X87, T_MEM | T_RC, 0 },
    { "fild m64", "DF /5", C_X87, T_MEM, 0 }, { "fistp m64", "DF /7", C_X87, T_MEM | T_RC, 0 },
    { "fnstsw ax", "DF E0", C_X87, 0, 0 }, { "fucomip", "DF E8+i", C_X87, 0, 0 }, { "fcomip", "DF F0+i", C_X87, 0, 0 },

    /* MMX */
    { "movd mm,Ed", "0F 6E /r", C_MMX, T_GMM, 0 }, { "movd Ed,mm", "0F 7E /r", C_MMX, T_GMM, 0 },
    { "movq mm,Eq", "0F 6F /r", C_MMX, T_GMM | T_EMM, 0 }, { "movq Eq,mm", "0F 7F /r", C_MMX, T_GMM | T_EMM, 0 },
    { "mmx op", "0F 60+m /r", C_MMX, T_GMM | T_EMM, 0 },
    { "pshufw", "0F 70 /r ib", C_MMX, T_GMM | T_EMM, 0 },
    { "psrlw/psraw/psllw ib", "0F 71 /s ib", C_MMX, T_EMM | T_REG, 0 },
    { "psrld/psrad/pslld ib", "0F 72 /s ib", C_MMX, T_EMM | T_REG, 0 },
    { "psrlq/psllq ib", "0F 73 /q ib", C_MMX, T_EMM | T_REG, 0 },
    { "pinsrw", "0F C4 /r ib", C_MMX, T_GMM | T_EW, 0 }, { "pextrw", "0F C5 /r ib", C_MMX, T_EMM | T_REG, 0 },
    { "pmovmskb", "0F D7 /r", C_MMX, T_EMM | T_REG, 0 },
    { "mmx op2", "0F D0+n /r", C_MMX, T_GMM | T_EMM, 0 },

    /* SSE */
    { "movups", "0F 10 /r", C_SSE, T_GX | T_EX, 0 }, { "movups st", "0F 11 /r", C_SSE, T_GX | T_EX, 0 },
    { "movss", "F3 0F 10 /r", C_SSE, T_GX | T_EX, 0 }, { "movss st", "F3 0F 11 /r", C_SSE, T_GX | T_EX, 0 },
    { "movlps/movhlps", "0F 12 /r", C_SSE, T_GX | T_EX, 0 }, { "movlps st", "0F 13 /r", C_SSE, T_GX | T_MEM, 0 },
    { "unpcklps", "0F 14 /r", C_SSE, T_GX | T_EX, 0 }, { "unpckhps", "0F 15 /r", C_SSE, T_GX | T_EX, 0 },
    { "movhps/movlhps", "0F 16 /r", C_SSE, T_GX | T_EX, 0 }, { "movhps st", "0F 17 /r", C_SSE, T_GX | T_MEM, 0 },
    { "movaps", "0F 28 /r", C_SSE, T_GX | T_EX | T_REG, 0 },
    { "cvtpi2ps", "0F 2A /r", C_SSE, T_GX | T_EMM, 0 }, { "cvtsi2ss", "F3 0F 2A /r", C_SSE, T_GX, 0 },
    { "cvttps2pi", "0F 2C /r", C_SSE, T_GMM | T_EX, 0 }, { "cvttss2si", "F3 0F 2C /r", C_SSE, T_EX, 0 },
    { "cvtps2pi", "0F 2D /r", C_SSE, T_GMM | T_EX | T_RC, 0 }, { "cvtss2si", "F3 0F 2D /r", C_SSE, T_EX | T_RC, 0 },
    { "ucomiss", "0F 2E /r", C_SSE, T_GX | T_EX, 0 }, { "comiss", "0F 2F /r", C_SSE, T_GX | T_EX, 0 },
    { "movmskps", "0F 50 /r", C_SSE, T_EX | T_REG, 0 },
    { "sqrtps", "0F 51 /r", C_SSE, T_GX | T_EX, 0 }, { "sqrtss", "F3 0F 51 /r", C_SSE, T_GX | T_EX, 0 },
    { "rsqrtps", "0F 52 /r", C_SSE, T_GX | T_EX | T_APPROX, 0 }, { "rcpss", "F3 0F 53 /r", C_SSE, T_GX | T_EX | T_APPROX, 0 },
    { "logic ps", "0F 54+l /r", C_SSE, T_GX | T_EX, 0 },
    { "arith ps", "0F 58+a /r", C_SSE, T_GX | T_EX, 0 }, { "arith ss", "F3 0F 58+a /r", C_SSE, T_GX | T_EX, 0 },
    { "cmpps", "0F C2 /r ic", C_SSE, T_GX | T_EX, 0 }, { "cmpss", "F3 0F C2 /r ic", C_SSE, T_GX | T_EX, 0 },
    { "shufps", "0F C6 /r ib", C_SSE, T_GX | T_EX, 0 },
    { "ldmxcsr", "0F AE /2", C_SSE, T_MEM | T_FCW | T_RC, 0 }, { "stmxcsr", "0F AE /3", C_SSE, T_MEM, 0 },
};
#define NTMPL (sizeof(tmpls) / sizeof(tmpls[0]))

/* ---- test cases ------------------------------------------------------- */

/* Test files are written on i386 and read on ARM, where doubles and 64-bit
   integers are 8-aligned: the padding keeps the layouts the same. */
struct state {
    uint32_t r[8];
    uint32_t eflags, pad0;
    double st[8];          /* by ST(i) */
    uint32_t top, ftag;    /* ftag by physical register, 1 = empty */
    uint32_t fpcw, fpsw;
    uint64_t mm[8];
    xmm_t xmm[8];
    uint32_t mxcsr, pad1;
};

struct test {
    uint64_t seed;
    uint16_t tmpl;
    uint16_t follow[2];    /* sequences: register-only instructions after it, or NO_TMPL */
    uint8_t len, len1;     /* all the code; the first instruction */
    uint8_t code[30];
    uint8_t pad[2];
};
#define NO_TMPL 0xFFFF

struct result {
    struct state s;
    uint16_t nmem;
    uint16_t mem_off[64];
    uint8_t mem_val[64];
    uint8_t pad[6];
};

_Static_assert(sizeof(struct state) == 320 && sizeof(struct test) == 48 && sizeof(struct result) == 520,
               "test file layout");

static uint8_t mem_init[MEM_SIZE];
static uint8_t *encode(const struct tmpl *tp, uint8_t *p, struct state *s, bool reg_only);

/* Builds the instruction and the input state from t->seed. */
static void make_test(struct test *t, struct state *s)
{
    rs = t->seed * 0x9E3779B97F4A7C15ull + 1;
    const struct tmpl *tp = &tmpls[t->tmpl];
    uint32_t fl = tp->flags;
    memset(s, 0, sizeof(*s));
    for (int i = 0; i < 8; i++) s->r[i] = rand_int();
    s->r[XR_ESP] = STACK_TOP;
    s->eflags = 0x202 | (rnd() & EF_ARITH) | (rn(8) == 0 ? EF_DF : 0);
    /* x87: st0..st5 valid, two empty for pushes */
    s->top = rn(8);
    s->ftag = 0;
    for (int i = 0; i < 8; i++) {
        s->st[i] = rand_double();
        if (i >= 6) s->ftag |= 1u << ((s->top + i) & 7);
    }
    /* 53-bit precision; rounding other than to nearest only where it decides
       an integer result (arithmetic always rounds to nearest in xlat) */
    s->fpcw = 0x27F | ((fl & T_RC) ? rn(4) << 10 : 0);
    s->fpsw = (rnd() & 0x4700) | (s->top << 11);
    for (int i = 0; i < 8; i++) s->mm[i] = (uint64_t)rand_int() << 32 | rand_int();
    for (int i = 0; i < 8; i++)
        for (int k = 0; k < 4; k++) {
            if (tp->cls == C_SSE && rn(4)) s->xmm[i].f[k] = (float)rand_double();
            else s->xmm[i].u[k] = rand_int();
        }
    s->mxcsr = 0x1F80 | ((fl & T_RC) ? rn(4) << 13 : 0);
    if (fl & T_SMALL) s->st[0] = ((double)rnd() / 4294967296.0) * 2.0 - 1.0;
    if (tp->cls == C_MMX || (fl & (T_GMM | T_EMM))) { s->top = 0; s->fpsw &= ~0x3800u; s->ftag = 0; }

    /* memory: random bytes, or values of the operand type */
    for (unsigned i = 0; i < MEM_SIZE; i += 4) {
        uint32_t v = rand_int();
        if (tp->cls == C_X87 || tp->cls == C_SSE) {
            if (rn(2)) { float f = (float)rand_double(); memcpy(&v, &f, 4); }
        }
        memcpy(mem_init + i, &v, 4);
    }
    if (tp->cls == C_X87 && rn(2))
        for (unsigned i = 0; i < MEM_SIZE; i += 8) { double d = rand_double(); memcpy(mem_init + i, &d, 8); }
    if (tp->cls == C_X87 && strstr(tp->name, "m80"))
        for (unsigned i = 0; i < MEM_SIZE; i += 10) {
            if (i + 10 > MEM_SIZE) break;
            x87_store80(mem_init + i, rand_double());
        }
    if (fl & T_FCW)
        for (unsigned i = 0; i < MEM_SIZE; i += 2) {
            uint16_t w = tp->cls == C_SSE ? (uint16_t)(0x1F80 | (rn(4) << 13)) : (uint16_t)(0x27F | (rn(4) << 10));
            if (!(fl & T_RC)) w &= tp->cls == C_SSE ? ~0x6000 : ~0xC00;
            memcpy(mem_init + i, &w, 2);
        }
    if (fl & T_POPF) {
        uint32_t f = 0x202 | (rnd() & (EF_ARITH | EF_DF));
        memcpy(mem_init + (STACK_TOP - MEM_ADDR), &f, 4);
    }

    uint8_t *p = encode(tp, t->code, s, false);
    t->len = t->len1 = (uint8_t)(p - t->code);

    /* registers the instruction uses as pointers */
    if (fl & T_STR) {
        s->r[XR_ESI] = DATA_PTR + rn(64) - 32;
        s->r[XR_EDI] = DATA_PTR + rn(64) - 32 + (rn(2) ? 80 : 0);
        s->r[XR_ECX] = rn(10);
        if (s->eflags & EF_DF) { s->r[XR_ESI] += 64; s->r[XR_EDI] += 64; }
    }
    if (fl & T_EBP) s->r[XR_EBP] = STACK_TOP - 32 - rn(64);
    if (fl & T_EBX) s->r[XR_EBX] = DATA_PTR - rn(128);
    if (fl & T_DIV) {
        /* keep quotients in range: a small dividend high part */
        if (fl & T_B) s->r[XR_EAX] = (s->r[XR_EAX] & ~0xFF00u) | (rn(4) << 8);
        else { s->r[XR_EDX] = rn(4); }
        if (rn(2)) s->r[XR_EDX] = (uint32_t)-(int32_t)rn(3);
    }
    for (int i = 0; i < 2; i++)
        if (t->follow[i] != NO_TMPL) p = encode(&tmpls[t->follow[i]], p, s, true);
    t->len = (uint8_t)(p - t->code);
}

/* Encodes an instance of tp at p (its operands random, memory ones pointed
   into the buffer through s's registers; reg_only: register operands only);
   returns the end. */
static uint8_t *encode(const struct tmpl *tp, uint8_t *p, struct state *s, bool reg_only)
{
    uint32_t fl = tp->flags;
    const char *e = tp->enc;
    int reg_field = -1;
    bool byteop = fl & T_B;
    while (*e) {
        while (*e == ' ') e++;
        if (!*e) break;
        if (e[0] == '/') {
            char k = e[1];
            e += 2;
            int reg;
            if (k >= '0' && k <= '7') reg = k - '0';
            else if (k == 'x') reg = (int)rn(8);
            else if (k == 'y') reg = 4 + (int)rn(4);
            else if (k == 's') { static const int s3[3] = { 2, 4, 6 }; reg = s3[rn(3)]; }
            else if (k == 'q') reg = rn(2) ? 2 : 6;
            else {   /* r: any register operand */
                reg = (int)rn(8);
                if (!(fl & (T_GMM | T_GX)) && !byteop) while (reg == XR_ESP) reg = (int)rn(8);
            }
            reg_field = reg;
            bool mem = fl & T_MEM ? true : fl & T_REG ? false : fl & T_ST ? false : rn(5) < 3;
            if ((fl & T_ST) || reg_only) mem = false;
            if (!mem) {
                int rm = (int)rn(8);
                if (fl & T_ST) rm = (int)rn(4);
                else if (!(fl & (T_EMM | T_EX)) && !byteop && !(fl & T_EB))
                    while (rm == XR_ESP) rm = (int)rn(8);
                *p++ = (uint8_t)(0xC0 | reg << 3 | rm);
            } else {
                /* [base + disp8] or [base + index*scale + disp8], base near DATA_PTR */
                int base;
                do base = (int)rn(8); while (base == XR_ESP);
                int form = (int)rn(3);
                int32_t disp = (int32_t)rn(256) - 128;
                uint32_t bval = DATA_PTR + rn(128) - 64;
                if (form == 0 && base != XR_EBP) {
                    *p++ = (uint8_t)(0x00 | reg << 3 | base);
                    disp = 0;
                } else if (form <= 1) {
                    *p++ = (uint8_t)(0x40 | reg << 3 | base);
                    *p++ = (uint8_t)disp;
                } else {
                    int index;
                    do index = (int)rn(8); while (index == XR_ESP || index == base);
                    int scale = (int)rn(4);
                    uint32_t ival = rn(16);
                    *p++ = (uint8_t)(0x44 | reg << 3);
                    *p++ = (uint8_t)(scale << 6 | index << 3 | base);
                    *p++ = (uint8_t)disp;
                    s->r[index] = ival;
                }
                s->r[base] = bval;
            }
            continue;
        }
        if (e[0] == 'i' && (e[1] == 'b' || e[1] == 'w' || e[1] == 'd' || e[1] == 'c')) {
            uint32_t v = rand_int();
            int n = e[1] == 'w' ? 2 : e[1] == 'd' ? 4 : 1;
            if (e[1] == 'c') v = rn(8);
            if ((fl & T_SHIFT1) && n == 1 && rn(2)) v = rn(40);
            if (fl & T_ENTER) { v = n == 2 ? rn(64) : rn(4); }
            memcpy(p, &v, n);
            p += n;
            e += 2;
            continue;
        }
        if (e[0] == 'm' && e[1] == 'd') {
            uint32_t a = DATA_PTR + rn(256) - 128;
            memcpy(p, &a, 4);
            p += 4;
            e += 2;
            continue;
        }
        if (e[0] == '+') {   /* +r +R +i +c +m +n +l +a: added to the previous byte */
            char k = e[1];
            e += 2;
            uint8_t add = 0;
            switch (k) {
            case 'r': do add = (uint8_t)rn(8); while (add == XR_ESP); break;
            case 'R': add = (uint8_t)rn(8); break;
            case 'i': add = (uint8_t)rn(4); break;
            case 'c': add = (uint8_t)rn(16); break;
            case 'm': {   /* MMX 0F 60-6B, 74-76 */
                static const uint8_t ops[] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 0x14, 0x15, 0x16 };
                add = ops[rn(sizeof(ops))];
                break;
            }
            case 'n': {   /* MMX 0F D1-FE */
                static const uint8_t ops[] = { 1, 2, 3, 5, 8, 9, 10, 11, 12, 13, 14, 15, 0x10, 0x11, 0x12, 0x13, 0x14,
                                               0x15, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x21, 0x22,
                                               0x23, 0x25, 0x26, 0x28, 0x29, 0x2A, 0x2C, 0x2D, 0x2E };
                add = ops[rn(sizeof(ops))];
                break;
            }
            case 'l': add = (uint8_t)rn(4); break;
            case 'a': { static const uint8_t ops[] = { 0, 1, 4, 5, 6, 7 }; add = ops[rn(6)]; break; }
            }
            p[-1] = (uint8_t)(p[-1] + add);
            continue;
        }
        unsigned v;
        sscanf(e, "%2x", &v);
        *p++ = (uint8_t)v;
        e += 2;
    }
    (void)reg_field;
    return p;
}

/* ---- native execution (i386) ------------------------------------------ */

#ifdef __i386__
struct native {
    uint8_t fx[512];
    uint32_t r[8];
    uint32_t eflags;
} __attribute__((aligned(16)));

struct native nin, nout;
uint8_t host_fx[512] __attribute__((aligned(16)));
uint32_t saved_esp, code_ptr;

__asm__(
    ".text\n"
    ".globl run_native\n"
    "run_native:\n"
    "  pushal\n"
    "  pushfl\n"
    "  movl %esp, saved_esp\n"
    "  fxsave host_fx\n"
    "  fxrstor nin\n"
    "  movl nin+528, %esp\n"
    "  pushl nin+544\n"
    "  popfl\n"
    "  movl nin+512, %eax\n"
    "  movl nin+516, %ecx\n"
    "  movl nin+520, %edx\n"
    "  movl nin+524, %ebx\n"
    "  movl nin+532, %ebp\n"
    "  movl nin+536, %esi\n"
    "  movl nin+540, %edi\n"
    "  jmp *code_ptr\n"
    ".globl native_done\n"
    "native_done:\n"
    "  movl %eax, nout+512\n"
    "  movl %ecx, nout+516\n"
    "  movl %edx, nout+520\n"
    "  movl %ebx, nout+524\n"
    "  movl %esp, nout+528\n"
    "  movl %ebp, nout+532\n"
    "  movl %esi, nout+536\n"
    "  movl %edi, nout+540\n"
    "  pushfl\n"
    "  popl nout+544\n"
    "  movl saved_esp, %esp\n"
    "  fxsave nout\n"
    "  fxrstor host_fx\n"
    "  popfl\n"
    "  popal\n"
    "  ret\n");
void run_native(void);
void native_done(void);

static sigjmp_buf fault_jmp;
static volatile int faulted;

static void on_fault(int sig, siginfo_t *si, void *uc)
{
    (void)si; (void)uc;
    faulted = sig;
    siglongjmp(fault_jmp, 1);
}

static void state_to_fx(const struct state *s, uint8_t *fx, int cls)
{
    memset(fx, 0, 512);
    uint16_t w = (uint16_t)s->fpcw;
    memcpy(fx, &w, 2);
    w = (uint16_t)s->fpsw;
    memcpy(fx + 2, &w, 2);
    fx[4] = (uint8_t)~s->ftag;
    memcpy(fx + 24, &s->mxcsr, 4);
    uint32_t mask = 0xFFFF;
    memcpy(fx + 28, &mask, 4);
    for (int i = 0; i < 8; i++) {
        if (cls < 0) {
            memcpy(fx + 32 + 16 * i, &s->mm[i], 8);
            uint16_t ex = 0xFFFF;
            memcpy(fx + 40 + 16 * i, &ex, 2);
        } else {
            x87_store80(fx + 32 + 16 * i, s->st[i]);
        }
    }
    memcpy(fx + 160, s->xmm, 128);
}

static void fx_to_state(const uint8_t *fx, struct state *s, int cls)
{
    s->fpcw = fx[0] | fx[1] << 8;
    s->fpsw = fx[2] | fx[3] << 8;
    s->top = (s->fpsw >> 11) & 7;
    s->ftag = (uint8_t)~fx[4];
    memcpy(&s->mxcsr, fx + 24, 4);
    for (int i = 0; i < 8; i++) {
        s->st[i] = x87_load80(fx + 32 + 16 * i);
        memcpy(&s->mm[i], fx + 32 + 16 * i, 8);
    }
    (void)cls;
    memcpy(s->xmm, fx + 160, 128);
}

static uint8_t *mem, *code;

/* Runs t natively; false if it faulted. */
static bool native_run(const struct test *t, struct result *res)
{
    struct state s;
    struct test tt = *t;
    make_test(&tt, &s);
    const struct tmpl *tp = &tmpls[t->tmpl];
    memcpy(mem, mem_init, MEM_SIZE);
    memcpy(code, t->code, t->len);
    uint8_t *j = code + t->len;
    j[0] = 0xE9;
    uint32_t rel = (uint32_t)(uintptr_t)native_done - (uint32_t)(uintptr_t)(j + 5);
    memcpy(j + 1, &rel, 4);
    __builtin___clear_cache((char *)code, (char *)j + 5);
    memcpy(nin.r, s.r, 32);
    nin.eflags = s.eflags;
    state_to_fx(&s, nin.fx, (tp->cls == C_MMX || (tp->flags & (T_GMM | T_EMM))) ? -1 : tp->cls);
    code_ptr = CODE_ADDR;
    faulted = 0;
    if (sigsetjmp(fault_jmp, 1)) {
        __asm__ volatile("fxrstor host_fx");
        return false;
    }
    run_native();
    memcpy(res->s.r, nout.r, 32);
    res->s.eflags = nout.eflags;
    fx_to_state(nout.fx, &res->s, tp->cls);
    /* the bytes that changed (eflags pushed below the starting esp is part of the setup) */
    uint8_t expect[MEM_SIZE];
    memcpy(expect, mem_init, MEM_SIZE);
    memcpy(expect + (s.r[XR_ESP] - 4 - MEM_ADDR), &s.eflags, 4);
    res->nmem = 0;
    uint32_t esp_out = nout.r[XR_ESP];
    for (unsigned i = 0; i < MEM_SIZE; i++) {
        uint32_t a = MEM_ADDR + i;
        if (a >= esp_out - 4 && a < esp_out) continue;   /* the harness's pushfl */
        if (mem[i] != expect[i]) {
            if (res->nmem == 64) return false;
            res->mem_off[res->nmem] = (uint16_t)i;
            res->mem_val[res->nmem] = mem[i];
            res->nmem++;
        }
    }
    return true;
}

static void native_setup(void)
{
    mem = mmap((void *)MEM_ADDR, MEM_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    code = mmap((void *)CODE_ADDR, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (mem != (void *)MEM_ADDR || code != (void *)CODE_ADDR) { perror("mmap"); exit(1); }
    static char altstack[65536];
    stack_t ss = { .ss_sp = altstack, .ss_size = sizeof(altstack) };
    sigaltstack(&ss, NULL);
    struct sigaction sa = { .sa_sigaction = on_fault, .sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER };
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGTRAP, &sa, NULL);
}
#endif

/* ---- xlat execution -------------------------------------------------- */

static sigjmp_buf xfault_jmp;
static const char *xfault_what;

uint64_t xrt_rdtsc(xcpu *c) { (void)c; return 0; }
void xrt_cpuid(xcpu *c) { (void)c; }
void xrt_interrupt(xcpu *c, unsigned n, uint32_t next) { (void)n; c->eip = next; }
void xrt_hlt(xcpu *c) { (void)c; }
void xrt_fault(xcpu *c, const char *what)
{
    (void)c;
    xfault_what = what;
    siglongjmp(xfault_jmp, 1);
}

static uint8_t *xmem, *xcode;

/* The xlat side: interpreter, or the code generator when built with one. */
static int xlat_run(xcpu *c, uint32_t end);

#ifdef XLAT_RUN
/* The code generator: each test's code replaces the last one's. */
static int xlat_run(xcpu *c, uint32_t end)
{
    jit_init(end, end + 1, true);
    jit_invalidate(CODE_ADDR, 64);
    jit_run(c);
    return c->eip == end ? 0 : -2;
}
#else
static int xlat_run(xcpu *c, uint32_t end)
{
    for (int n = 0; n < 64 && c->eip != end; n++)
        if (xi_step(c) != XI_OK) return -1;
    return c->eip == end ? 0 : -2;
}
#endif

static bool feq(double a, double b, bool approx)
{
    if (isnan(a) || isnan(b)) return isnan(a) && isnan(b);
    if (a == b) return true;
    if (!approx) return false;
    double d = fabs(a - b), m = fmax(fabs(a), fabs(b));
    return d <= m * 1e-9 || d < 1e-300;
}

static bool feq32(float a, float b, bool approx)
{
    if (isnan(a) || isnan(b)) return isnan(a) && isnan(b);
    if (a == b) return true;
    if (!approx) return false;
    return fabsf(a - b) <= fmaxf(fabsf(a), fabsf(b)) * 1.0e-3f;
}

static int verbose;

/* Compares xlat's result for t with the native one; prints differences. */
static bool check_one(const struct test *t, const struct result *res)
{
    struct state s;
    struct test tt = *t;
    make_test(&tt, &s);
    const struct tmpl *tp = &tmpls[t->tmpl];
    bool approx = tp->flags & T_APPROX;
    memcpy(xmem, mem_init, MEM_SIZE);
    memcpy(xmem + (s.r[XR_ESP] - 4 - MEM_ADDR), &s.eflags, 4);
    memcpy(xcode, t->code, t->len);
    memset(xcode + t->len, 0x90, 8);
    xcpu c;
    xi_reset(&c);
    memcpy(c.r, s.r, 32);
    xi_set_eflags(&c, s.eflags);
    c.top = s.top;
    for (int i = 0; i < 8; i++) c.st[(s.top + i) & 7] = s.st[i];
    c.ftag = s.ftag;
    c.fpcw = s.fpcw;
    c.fpsw = s.fpsw & ~0x3800u;
    for (int i = 0; i < 8; i++) c.mm[i].q = s.mm[i];
    memcpy(c.xmm, s.xmm, 128);
    c.mxcsr = s.mxcsr;
    c.eip = CODE_ADDR;
    char problems[1024];
    int pn = 0;
#define P(...) (pn += snprintf(problems + pn, pn < (int)sizeof(problems) ? sizeof(problems) - pn : 0, __VA_ARGS__), \
                pn = pn < (int)sizeof(problems) ? pn : (int)sizeof(problems))
    if (sigsetjmp(xfault_jmp, 1)) {
        P(" xlat fault: %s;", xfault_what);
    } else {
        int r = xlat_run(&c, CODE_ADDR + t->len);
        if (r) P(" xlat %s;", r == -1 ? "invalid instruction" : "didn't reach the end");
    }
    const struct state *o = &res->s;
    static const char *rn_[] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi" };
    if (!pn) {
        for (int i = 0; i < 8; i++)
            if (c.r[i] != o->r[i]) P(" %s %08x want %08x;", rn_[i], c.r[i], o->r[i]);
        uint32_t undef = tp->undef;
        for (int i = 0; i < 2; i++)
            if (t->follow[i] != NO_TMPL) undef |= tmpls[t->follow[i]].undef;
        if ((tp->flags & T_SHIFT1)) {
            /* OF is only defined for a count of 1 */
            const uint8_t *q = t->code;
            while (*q == 0x66 || *q == 0xF0) q++;
            unsigned cnt;
            if (*q == 0x0F) cnt = (q[1] & 1) ? s.r[XR_ECX] & 31 : t->code[t->len1 - 1] & 31;
            else if (*q == 0xC0 || *q == 0xC1) cnt = t->code[t->len1 - 1] & 31;
            else cnt = s.r[XR_ECX] & 31;
            if (cnt != 1) undef |= EF_OF;
            /* 16-bit shld/shrd past 16, rcl/rcr of bytes and words: CF too */
            if (*q == 0x0F && cnt > 16 && t->code[0] == 0x66) undef |= EF_CF | EF_ARITH;
        }
        uint32_t fx = xi_get_eflags(&c), fo = o->eflags, m = (EF_ARITH | EF_DF) & ~undef;
        if ((fx ^ fo) & m) P(" eflags %03x want %03x (diff %03x);", fx & 0xFFF, fo & 0xFFF, (fx ^ fo) & m);
        bool partial_rem = false;
        if (strstr(tp->name, "fprem")) {
            /* exponents 64 or more apart: the CPU reduces partially (C2 set),
               xlat all the way (code loops until C2 is clear) */
            int ea = ilogb(s.st[0]), eb = ilogb(s.st[1]);
            partial_rem = !isfinite(s.st[0]) || !isfinite(s.st[1]) || s.st[1] == 0 || ea - eb >= 64;
        }
        if (tp->cls == C_X87 && !partial_rem) {
            if (c.top != o->top) P(" top %u want %u;", c.top, o->top);
            if ((c.ftag & 0xFF) != (o->ftag & 0xFF)) P(" ftag %02x want %02x;", c.ftag & 0xFF, o->ftag & 0xFF);
            for (int i = 0; i < 8; i++) {
                if (o->ftag & (1u << ((o->top + i) & 7))) continue;
                double v = c.st[(c.top + i) & 7];
                if (!feq(v, o->st[i], approx)) P(" st%d %.17g want %.17g;", i, v, o->st[i]);
            }
            /* C0 C2 C3 where the instruction defines them (compares, ftst,
               fxam, fprem); C1 only for fxam (sign) and fprem (a quotient
               bit): elsewhere it says which way rounding went */
            uint32_t ccm = 0;
            if (tp->flags & T_CC) {
                const uint8_t *q = t->code;
                uint8_t m = q[1], reg = (m >> 3) & 7;
                bool cmp = true;
                if (q[0] == 0xD8 || q[0] == 0xDA || q[0] == 0xDC || q[0] == 0xDE) cmp = reg == 2 || reg == 3;
                if (cmp) ccm = 0x4500;
                if (strstr(tp->name, "fxam") || strstr(tp->name, "fprem")) ccm = 0x4700;
            }
            if (((c.fpsw ^ o->fpsw) & ccm)) P(" fpsw %04x want %04x;", c.fpsw & ccm, o->fpsw & ccm);
            if ((c.fpcw ^ o->fpcw) & 0xF3F) P(" fpcw %04x want %04x;", c.fpcw, o->fpcw);
        }
        if (tp->cls == C_MMX || (tp->cls == C_SSE && (tp->flags & (T_GMM | T_EMM)))) {
            for (int i = 0; i < 8; i++)
                if (c.mm[i].q != o->mm[i]) P(" mm%d %016llx want %016llx;", i, (unsigned long long)c.mm[i].q,
                                             (unsigned long long)o->mm[i]);
        }
        if (tp->cls == C_SSE) {
            for (int i = 0; i < 8; i++)
                for (int k = 0; k < 4; k++) {
                    uint32_t a = c.xmm[i].u[k], b = o->xmm[i].u[k];
                    if (a == b) continue;
                    float fa, fb;
                    memcpy(&fa, &a, 4);
                    memcpy(&fb, &b, 4);
                    if (feq32(fa, fb, approx) && !(fa == 0 && fb == 0 && a != b)) continue;
                    P(" xmm%d[%d] %08x (%g) want %08x (%g);", i, k, a, fa, b, fb);
                }
            if ((c.mxcsr ^ o->mxcsr) & ~0x3Fu) P(" mxcsr %08x want %08x;", c.mxcsr, o->mxcsr);
        }
        /* memory */
        uint8_t want[MEM_SIZE];
        memcpy(want, mem_init, MEM_SIZE);
        memcpy(want + (s.r[XR_ESP] - 4 - MEM_ADDR), &s.eflags, 4);
        for (unsigned i = 0; i < res->nmem; i++) want[res->mem_off[i]] = res->mem_val[i];
        uint32_t esp_out = o->r[XR_ESP];
        for (unsigned i = 0; i < MEM_SIZE; i++) {
            uint32_t a = MEM_ADDR + i;
            if (a >= esp_out - 4 && a < esp_out) continue;
            if (xmem[i] == want[i]) continue;
            /* NaNs with different payloads, approximations */
            unsigned a4 = i & ~3u, a8 = i & ~7u;
            float f1, f2;
            double d1, d2;
            memcpy(&f1, xmem + a4, 4); memcpy(&f2, want + a4, 4);
            memcpy(&d1, xmem + a8, 8); memcpy(&d2, want + a8, 8);
            if (tp->cls != C_INT && ((isnan(f1) && isnan(f2)) || (isnan(d1) && isnan(d2)))) continue;
            if (approx && (feq32(f1, f2, true) || feq(d1, d2, true))) continue;
            P(" mem+%03x %02x want %02x;", i, xmem[i], want[i]);
            break;
        }
    }
    if (pn) {
        char text[100];
        int n = 0;
        for (int i = 0; i < t->len; i++) n += snprintf(text + n, sizeof(text) - n, "%02x ", t->code[i]);
        char name[96];
        snprintf(name, sizeof(name), "%s%s%s%s%s", tp->name, t->follow[0] != NO_TMPL ? " + " : "",
                 t->follow[0] != NO_TMPL ? tmpls[t->follow[0]].name : "", t->follow[1] != NO_TMPL ? " + " : "",
                 t->follow[1] != NO_TMPL ? tmpls[t->follow[1]].name : "");
        printf("FAIL %-22s seed %-10llu %s:%s\n", name, (unsigned long long)t->seed, text, problems);
        if (verbose) {
            printf("   in: eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x efl=%08x\n",
                   s.r[0], s.r[1], s.r[2], s.r[3], s.r[4], s.r[5], s.r[6], s.r[7], s.eflags);
            if (tp->cls == C_X87)
                printf("   st0=%.17g st1=%.17g fpcw=%04x\n", s.st[0], s.st[1], s.fpcw);
        }
        return false;
    }
    return true;
}

static void xlat_setup(void)
{
    xmem = mmap((void *)MEM_ADDR, MEM_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    xcode = mmap((void *)CODE_ADDR, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (xmem != (void *)MEM_ADDR || xcode != (void *)CODE_ADDR) { perror("mmap"); exit(1); }
}

/* ---- main ------------------------------------------------------------- */

/* Sequence tests (-s): register-only instructions after the first one, so
   the translator's flag bookkeeping between instructions is tested too.
   Readers use flags an earlier instruction may have left undefined. */
static const char *const follower_names[] = {
    "add Ev,Gv", "add Gv,Ev", "sub Ev,Gv", "cmp Ev,Gv", "and Ev,Gv", "or Ev,Gv", "xor Ev,Gv", "add Eb,Gb",
    "sub Eb,Gb", "cmp Eb,Gb", "test Ev,Gv", "test Eb,Gb", "cmp eax,id", "inc r", "dec r", "inc Ev", "dec Ev",
    "neg Ev", "not Ev", "mov Ev,Gv", "imul Gv,Ev", "bt Ev,Gv", "clc", "stc", "cwde", "cdq", "xchg eax,r",
    "bswap r", "movzx Gv,Eb", "sahf", "jcc +0", "jcc32 +0", "jmp +0", "loop +0",
};
static const char *const reader_names[] = {
    "adc Ev,Gv", "adc Gv,Ev", "sbb Ev,Gv", "sbb Gv,Ev", "adc Eb,Gb", "sbb Eb,Gb", "grp1 Ev,ib", "grp1 Eb,ib",
    "cmovcc Gv,Ev", "setcc Eb", "cmc", "lahf", "pushf", "grp2 Ev,1", "grp2 Eb,1",
};
#define NFOLLOW (sizeof(follower_names) / sizeof(follower_names[0]))
#define NREAD (sizeof(reader_names) / sizeof(reader_names[0]))
static uint16_t followers[NFOLLOW + NREAD];

static uint16_t find_tmpl(const char *name)
{
    for (unsigned i = 0; i < NTMPL; i++)
        if (!strcmp(tmpls[i].name, name)) return (uint16_t)i;
    fprintf(stderr, "no template %s\n", name);
    exit(2);
}

static void pick_followers(struct test *t)
{
    if (!followers[0] && !followers[1]) {
        for (unsigned i = 0; i < NFOLLOW; i++) followers[i] = find_tmpl(follower_names[i]);
        for (unsigned i = 0; i < NREAD; i++) followers[NFOLLOW + i] = find_tmpl(reader_names[i]);
    }
    const struct tmpl *tp = &tmpls[t->tmpl];
    bool undef = tp->undef || (tp->flags & T_SHIFT1);
    int n = (int)rn(3);
    for (int i = 0; i < n; i++) {
        uint16_t f = followers[rn(undef ? NFOLLOW : NFOLLOW + NREAD)];
        t->follow[i] = f;
        if (tmpls[f].undef) undef = true;
    }
}

static int pick_template(const char *filter)
{
    for (;;) {
        int i = (int)rn(NTMPL);
        if (!filter || strstr(tmpls[i].name, filter)) return i;
    }
}

int main(int argc, char **argv)
{
    const char *filter = NULL;
    bool seq = false;
    int a = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f") && i + 1 < argc) { filter = argv[++i]; continue; }
        if (!strcmp(argv[i], "-s")) { seq = true; continue; }
        if (!strcmp(argv[i], "-v")) { verbose = 1; continue; }
        argv[a++] = argv[i];
    }
    argc = a;
    if (argc < 2) {
        fprintf(stderr, "usage: difftest gen N SEED FILE | check FILE | run N SEED [-f FILTER] [-s] [-v]\n");
        return 2;
    }
#ifdef __i386__
    if (!strcmp(argv[1], "gen") || !strcmp(argv[1], "run")) {
        bool gen = !strcmp(argv[1], "gen");
        if (argc < 4 + gen) return 2;
        long n = atol(argv[2]);
        uint64_t seed = strtoull(argv[3], NULL, 0);
        FILE *f = gen ? fopen(argv[4], "wb") : NULL;
        native_setup();
        long done = 0, faults = 0, fails = 0;
        uint64_t pick = seed * 7919 + 13;
        for (long i = 0; i < n; i++) {
            struct test t = { .seed = seed + (uint64_t)i, .follow = { NO_TMPL, NO_TMPL } };
            rs = pick++ * 0x9E3779B97F4A7C15ull + 7;
            t.tmpl = (uint16_t)pick_template(filter);
            if (seq) pick_followers(&t);
            struct state s;
            make_test(&t, &s);
            struct result res;
            memset(&res, 0, sizeof(res));
            if (!native_run(&t, &res)) { faults++; continue; }
            done++;
            if (gen) {
                fwrite(&t, sizeof(t), 1, f);
                fwrite(&res, sizeof(res), 1, f);
            } else {
                /* the xlat side needs its own buffers at the same addresses: swap */
                static bool once;
                if (!once) { xmem = mem; xcode = code; once = true; }
                if (!check_one(&t, &res)) fails++;
            }
        }
        if (f) fclose(f);
        fprintf(stderr, "%ld tests, %ld skipped (native fault), %ld failed\n", done, faults, fails);
        return fails != 0;
    }
#endif
    if (!strcmp(argv[1], "check") && argc >= 3) {
        FILE *f = fopen(argv[2], "rb");
        if (!f) { perror(argv[2]); return 1; }
        xlat_setup();
        long n = 0, fails = 0;
        struct test t;
        struct result res;
        while (fread(&t, sizeof(t), 1, f) == 1 && fread(&res, sizeof(res), 1, f) == 1) {
            if (filter && !strstr(tmpls[t.tmpl].name, filter)) continue;
            n++;
            if (!check_one(&t, &res)) fails++;
        }
        fprintf(stderr, "%ld tests, %ld failed\n", n, fails);
        return fails != 0;
    }
    fprintf(stderr, "unknown command\n");
    return 2;
}
