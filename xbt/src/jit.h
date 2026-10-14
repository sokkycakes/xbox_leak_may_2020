/*
 * jit.h - internal interface of the x86 -> AArch64 block translator.
 */
#ifndef XBT_JIT_H
#define XBT_JIT_H

#include <stdio.h>
#include <stdlib.h>
#include "a64.h"
#include "xbt.h"

#define GREG(r)  (19 + (r))     /* host register holding guest GPR r */
#define RTOP     27             /* x87 TOP */
#define RCPU     28             /* xbt_cpu * */
#define RA       16             /* effective-address scratch */
#define RB       17             /* secondary address scratch */
#define CPUOFF(f) ((int)offsetof(xbt_cpu, f))

/* compact flag masks used by the analysis */
enum { FCF = 1, FPF = 2, FAF = 4, FZF = 8, FSF = 16, FOF = 32, FALL = 63 };

/* How a producer's flags are represented in host NZCV. */
enum {
    NM_NONE = 0,
    NM_SUB,     /* N=SF Z=ZF C=!CF V=OF (subs/cmp/neg/sbcs) */
    NM_ADD,     /* N=SF Z=ZF C=CF  V=OF (adds/adcs) */
    NM_LOGIC,   /* N=SF Z=ZF C=0   V=0 */
    NM_INC,     /* N=SF Z=ZF V=OF, C meaningless */
    NM_SAHF,    /* N=SF Z=ZF C=!CF V=PF */
    NM_FCMP,    /* raw AArch64 fcmp result (FCOMI/UCOMISS semantics) */
};

/* Host conditions beyond the 16 AArch64 ones. */
#define HC_EQ_OR_VS  16
#define HC_NE_AND_VC 17
#define HC_INVALID   -1

enum { CK_NONE = 0, CK_COND, CK_CARRY, CK_OTHER };

typedef struct jinsn {
    x86_insn in;
    uint8_t rd, wr;         /* flags read / written (written includes undefined) */
    uint8_t cond_wr;        /* writes are conditional (shift by CL, REP) */
    uint8_t live_after;
    uint8_t mode;           /* NM_* this producer can provide in NZCV */
    uint8_t hostfl;         /* producer must leave its flags in NZCV */
    uint8_t store;          /* producer must store the lazy record */
    uint8_t sync;           /* materialize flags before this partial producer */
    uint8_t clobber;        /* codegen may clobber NZCV (helper calls etc.) */
    uint8_t consumer;       /* CK_* */
    uint8_t cc;             /* condition code read by a CK_COND consumer */
    uint8_t replay;         /* consumer may re-derive flags from the record of `single` */
    uint8_t need;           /* flags this producer must make available (wr & live_after) */
    uint8_t rec_kind;       /* LFK_* actually stored by the producer (0 = none) */
    uint8_t rec_size;       /* operand size of the stored record */
    int16_t fuse;           /* consumer: producer index providing NZCV, or -1 */
    int16_t single;         /* consumer: unique in-block producer, or -1 */
} jinsn;

#define JIT_MAX_INSNS 256
#define JIT_MAX_EXITS 64
#define JIT_MAX_STUBS 256

typedef struct jexit {
    uint32_t *site;     /* `b` instruction to point at the target */
    uint32_t target;    /* guest address */
} jexit;

enum { SITE_B, SITE_CBZ, SITE_BCOND, SITE_TBZ };
typedef struct jstub {
    uint32_t *site;
    uint8_t  type;
    uint8_t  reason;
    uint32_t eip;
    uint32_t info;
} jstub;

typedef struct amode {
    int base;           /* host register (its 64-bit value is the guest address) */
    int32_t off;
} amode;

typedef struct jit_t {
    a64buf b;
    uint32_t start_eip;
    uint32_t end_eip;
    uint32_t *host_start;
    jinsn ji[JIT_MAX_INSNS];
    int n;
    int cur;
    int nzcv_owner;     /* index of producer whose flags are in NZCV, -1 = none */
    jexit ex[JIT_MAX_EXITS];
    int nex;
    jstub st[JIT_MAX_STUBS];
    int nst;
    uint16_t insn_off[JIT_MAX_INSNS];
    int ended;          /* last instruction transferred control */
    int x87_used;
} jit_t;

/* stubs shared by all blocks (runtime.c) */
typedef struct xbt_stubs {
    uint32_t *enter;            /* uint32 enter(xbt_cpu *, void *code) */
    uint32_t *exit_eip;         /* w0 = guest eip */
    uint32_t *link_handler;     /* bl'd from per-exit stubs */
    uint32_t *indirect_miss;    /* w0 = guest eip */
    uint32_t *special_exit;     /* w0 = eip, w1 = reason, w2 = info */
} xbt_stubs;
extern xbt_stubs xbt_stub;

#define JB(j) (&(j)->b)

/* translate.c: infrastructure shared by the instruction generators */
void jit_ea(jit_t *j, const x86_insn *in, amode *am);
void jit_ea_pin(jit_t *j, amode *am);
void jit_ld(jit_t *j, int size, int rt, amode am);
void jit_ld_sx(jit_t *j, int size, int rt, amode am);
void jit_st(jit_t *j, int size, int rt, amode am);
void jit_fld(jit_t *j, int fsz, int vt, amode am);
void jit_fst(jit_t *j, int fsz, int vt, amode am);
void jit_call_helper(jit_t *j, int helper, int spill_regs);
void jit_nzcv_clobber(jit_t *j);
void jit_stub(jit_t *j, uint32_t *site, int type, uint32_t eip, int reason, uint32_t info);
void jit_exit_special(jit_t *j, uint32_t eip, int reason, uint32_t info);
int  jit_get_reg(jit_t *j, int size, int r, int tmp);
int  jit_get_reg_dirty(jit_t *j, int size, int r, int tmp);
void jit_set_reg(jit_t *j, int size, int r, int src);
int  jit_ld_rm(jit_t *j, const x86_insn *in, int size, int tmp, amode *am);
int  jit_ld_rm_dirty(jit_t *j, const x86_insn *in, int size, int tmp, amode *am);
void jit_st_rm(jit_t *j, const x86_insn *in, int size, int src, const amode *am);

/* flags framework */
void jit_flags_record(jit_t *j, jinsn *ji, int kind, int size, int r_res, int r_a, int r_b);
void jit_flags_produced(jit_t *j, jinsn *ji);
int  jit_cond(jit_t *j, jinsn *ji, int cc);
void jit_cond_reg(jit_t *j, jinsn *ji, int cc, int rd);
void jit_carry_in(jit_t *j, jinsn *ji, int sbc);
void jit_sync(jit_t *j);
int  jit_szidx(int size);
void jit_bcond(jit_t *j, int hc, uint32_t **site1, uint32_t **site2);

/* jit_int.c */
int  jit_int(jit_t *j, jinsn *ji);

/* jit_x87.c */
int  jit_x87(jit_t *j, jinsn *ji);
void jit_x87_classify(jinsn *ji);
void jit_x87_block_end(jit_t *j);

/* jit_sse.c */
int  jit_sse(jit_t *j, jinsn *ji);
int  jit_sse_classify(jinsn *ji);

/* runtime.c */
uint32_t *xbt_lookup_host(uint32_t eip);

#endif
