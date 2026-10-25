/*
 * xlat: xbcompat's own x86 translator (the CPU backend of the translated
 * builds, see cpu.h).
 *
 * Guest threads run at the same time, each on its own host thread with its
 * own guest CPU state (struct xcpu), as they do in the native i386 build:
 * guest memory is the process's own memory at the same addresses, and
 * lock-prefixed instructions are atomic on the host.
 *
 *   decode.c   x86 instruction decoder (the Pentium III's instruction set)
 *   interp.c   the interpreter: every instruction, the reference the code
 *              generators are checked against and their fallback
 *   fpu.c      x87, MMX and SSE for the interpreter
 *   runtime.c  cpu.h: host functions (stubs), cpu_call, threads
 *   jit_arm.c  the code generator for 32-bit ARM, with arm.h
 */
#ifndef XLAT_H
#define XLAT_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* ---- guest CPU state -------------------------------------------------- */

enum { XR_EAX, XR_ECX, XR_EDX, XR_EBX, XR_ESP, XR_EBP, XR_ESI, XR_EDI };

/* x86 SF ZF CF OF, kept where ARM keeps N Z C V. */
#define XF_N 0x80000000u
#define XF_Z 0x40000000u
#define XF_C 0x20000000u
#define XF_V 0x10000000u

/* EFLAGS bits */
#define EF_CF 0x0001u
#define EF_PF 0x0004u
#define EF_AF 0x0010u
#define EF_ZF 0x0040u
#define EF_SF 0x0080u
#define EF_TF 0x0100u
#define EF_IF 0x0200u
#define EF_DF 0x0400u
#define EF_OF 0x0800u
#define EF_ARITH (EF_CF | EF_PF | EF_AF | EF_ZF | EF_SF | EF_OF)

typedef union {
    float f[4];
    uint32_t u[4];
    int32_t i[4];
    uint64_t q[2];
    uint16_t w[8];
    int16_t sw[8];
    uint8_t b[16];
    int8_t sb[16];
} xmm_t;

typedef union {
    uint64_t q;
    uint32_t u[2];
    int32_t i[2];
    uint16_t w[4];
    int16_t sw[4];
    uint8_t b[8];
    int8_t sb[8];
} mmx_t;

typedef struct xcpu {
    uint32_t r[8];          /* 0x00 eax ecx edx ebx esp ebp esi edi */
    uint32_t eip;           /* 0x20 */
    uint32_t nzcv;          /* 0x24 SF ZF CF OF as XF_N XF_Z XF_C XF_V */
    uint32_t pf_res;        /* 0x28 PF is the even parity of its low byte */
    uint32_t af;            /* 0x2c AF in bit 4 */
    uint32_t df;            /* 0x30 0 or 1 */
    uint32_t eflags;        /* 0x34 the other EFLAGS bits (IF, ID, AC, ...) */
    uint32_t fs_base;       /* 0x38 */
    uint32_t top;           /* 0x3c x87 TOP */
    double st_unused[8];    /* 0x40 */
    /* 0x80 x87 registers by physical number: st(i) is st[(top + i) & 7].
       While translated code runs they are in d16-d23 (jit_arm.c), and
       written here whenever it calls out. */
    double st[8];
    double st_unused2[16];  /* 0xc0 */
    uint32_t fpcw;          /* 0x140 */
    uint32_t fpsw;          /* 0x144 status word without TOP */
    uint32_t ftag;          /* 0x148 bit i set: physical register i is empty */
    uint32_t mxcsr;         /* 0x14c */
    mmx_t mm[8];            /* 0x150 */
    xmm_t xmm[8];           /* 0x190 */
    uint32_t jit_site;      /* 0x210 the code generator's last block exit (jit_arm.c) */
    /* XLAT_FLAG_CHECK (jit_arm.c): the flags the last block exit left
       defined and that block's address; a block that then read others */
    uint32_t jit_fvalid, jit_fexit, jit_fbad_at, jit_fbad_from, jit_fbad_mask;
} xcpu;

_Static_assert(__builtin_offsetof(xcpu, st) == 0x80, "xcpu layout");
_Static_assert(__builtin_offsetof(xcpu, xmm) == 0x190, "xcpu layout");

/* ---- decoder ---------------------------------------------------------- */

/* prefixes */
#define XP_LOCK  0x01
#define XP_REP   0x02   /* F3 */
#define XP_REPNE 0x04   /* F2 */
#define XP_OPSZ  0x08   /* 66 */
#define XP_ADSZ  0x10   /* 67 */

/* segments (xinsn.seg) */
enum { XSEG_NONE, XSEG_ES, XSEG_CS, XSEG_SS, XSEG_DS, XSEG_FS, XSEG_GS };

typedef struct xinsn {
    uint32_t addr;
    uint8_t len;
    uint8_t pfx;            /* XP_* */
    uint8_t seg;            /* XSEG_* override */
    uint8_t has_modrm;
    uint16_t op;            /* 0x000-0x0ff one byte opcodes, 0x100-0x1ff after 0F */
    uint8_t modrm, mod, reg, rm;
    int8_t base, index;     /* memory operand registers, -1 for none */
    uint8_t scale;          /* index shift, 0-3 */
    uint8_t ad16;           /* 16-bit addressing (67 prefix) */
    int32_t disp;
    uint32_t imm;           /* immediate, zero-extended from its size */
    uint32_t imm2;          /* ENTER's nesting level, a far pointer's selector */
    uint8_t imm_size;       /* bytes of imm */
} xinsn;

/* Decodes the instruction at addr.  Returns false for an undefined or
   unsupported opcode (in->len then covers what was read). */
bool x86_decode(xinsn *in, uint32_t addr);
/* Instruction text for logs ("add eax, [ebx+4]"); for dumps, not exact. */
void x86_format(const xinsn *in, char *out, size_t size);

/* ---- interpreter ------------------------------------------------------ */

enum {
    XI_OK,        /* executed; eip is the next instruction */
    XI_INVALID,   /* undefined or unsupported instruction; eip unchanged */
};

/* Run the one instruction at c->eip. */
int xi_step(xcpu *c);
/* Run an instruction already decoded (in->addr == c->eip). */
int xi_exec(xcpu *c, const xinsn *in);
uint32_t xi_get_eflags(const xcpu *c);
void xi_set_eflags(xcpu *c, uint32_t f);
void xi_reset(xcpu *c);   /* power-on state: FPU initialized, SSE on, flags clear */
bool xi_cond(const xcpu *c, unsigned cc);

/* fpu.c */
int xi_x87(xcpu *c, const xinsn *in, uint32_t ea);
int xi_sse(xcpu *c, const xinsn *in, uint32_t ea);   /* 0F opcodes on MMX/XMM registers */
void xi_fpu_push(xcpu *c, double v);
double x87_load80(const void *p);
void x87_store80(void *p, double v);

/* ---- code generator (jit_arm.c) -------------------------------------- */

/* Guest code runs until eip reaches [stop_lo, stop_hi) (host functions'
   stubs); flags_at_stop: whether the code there reads the guest's flags.
   Calling it again changes the stop range. */
void jit_init(uint32_t stop_lo, uint32_t stop_hi, bool flags_at_stop);
/* Runs this thread's guest code from c->eip until it reaches the stop range. */
void jit_run(xcpu *c);
/* Guest code in [addr, addr + len) changed: drops its translations. */
void jit_invalidate(uint32_t addr, uint32_t len);
/* XLAT_STATS: what was translated, and what still runs in the interpreter. */
void jit_print_stats(void);
/* finishes an unaligned VFP access that faulted in translated code */
bool jit_fix_unaligned(void *ucontext);

/* ---- runtime services the interpreter calls (runtime.c) --------------- */

uint64_t xrt_rdtsc(xcpu *c);
void xrt_cpuid(xcpu *c);
void xrt_interrupt(xcpu *c, unsigned n, uint32_t next);   /* int n; eip is the int */
void xrt_hlt(xcpu *c);
void xrt_fault(xcpu *c, const char *what) __attribute__((noreturn));

static inline bool parity8(uint32_t v) { return !(__builtin_popcount(v & 0xFF) & 1); }

/* Guest memory is host memory at the same address. */
static inline uint8_t rd8(uint32_t a) { return *(const volatile uint8_t *)(uintptr_t)a; }
static inline uint16_t rd16(uint32_t a) { uint16_t v; memcpy(&v, (const void *)(uintptr_t)a, 2); return v; }
static inline uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, (const void *)(uintptr_t)a, 4); return v; }
static inline uint64_t rd64(uint32_t a) { uint64_t v; memcpy(&v, (const void *)(uintptr_t)a, 8); return v; }
static inline void wr8(uint32_t a, uint8_t v) { *(volatile uint8_t *)(uintptr_t)a = v; }
static inline void wr16(uint32_t a, uint16_t v) { memcpy((void *)(uintptr_t)a, &v, 2); }
static inline void wr32(uint32_t a, uint32_t v) { memcpy((void *)(uintptr_t)a, &v, 4); }
static inline void wr64(uint32_t a, uint64_t v) { memcpy((void *)(uintptr_t)a, &v, 8); }

#endif
