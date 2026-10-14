/*
 * xbt.h - shared definitions for the Xbox x86 -> AArch64 translator.
 *
 * Host register assignment inside translated code:
 *   w19..w26  guest EAX, ECX, EDX, EBX, ESP, EBP, ESI, EDI (upper 32 bits zero)
 *   w27       x87 TOP-of-stack index (0..7)
 *   x28       pointer to the xbt_cpu context
 *   x0..x17   scratch (x16/x17 hold effective addresses)
 * Guest memory is identity mapped: guest address A lives at host address A,
 * so a guest register can be used directly as a host base register.
 */
#ifndef XBT_H
#define XBT_H

#include <stdint.h>
#include <stddef.h>
#include "x86dec.h"

/* ---------------------------------------------------------------- */
/* Xbox memory map (guest == host addresses)                         */
/* ---------------------------------------------------------------- */
#define XBOX_RAM_SIZE_RETAIL   (64u << 20)
#define XBOX_RAM_SIZE_DEVKIT   (128u << 20)
#define XBOX_LOW_BASE          0x00010000u   /* lowest mapped guest address */
#define XBOX_CONTIG_BASE       0x80000000u   /* physical memory mirror */
#define XBOX_WC_BASE           0xF0000000u   /* write-combined physical mirror */
#define XBOX_NV2A_BASE         0xFD000000u   /* GPU registers (trapped) */
#define XBOX_HLE_BASE          0x8F000000u   /* kernel thunk targets + data */
#define XBOX_HLE_SIZE          0x00400000u
#define XBOX_HLE_THUNK_END     (XBOX_HLE_BASE + 0x1000u) /* call targets: base + ordinal*4 */

/* Translator-owned regions inside the low 4 GB (never touched by titles). */
#define XBT_CODE_BASE          0x90000000u
#define XBT_CODE_SIZE          (120u << 20)
#define XBT_JTAB_BASE          0xA0000000u   /* guest addr -> host code, 4 bytes/entry */
#define XBT_JTAB_RANGE         (128u << 20)  /* guest addresses covered by the table */

/* ---------------------------------------------------------------- */
/* Lazy flags                                                        */
/* ---------------------------------------------------------------- */
/* lf_op = kind << 2 | size (0 = 8 bit, 1 = 16 bit, 2 = 32 bit) */
enum {
    LFK_NONE = 0,  /* flags fully materialized in eflags */
    LFK_ADD,       /* res, b  (a = res - b) */
    LFK_SUB,       /* res, b  (a = res + b); also CMP and NEG (a = 0) */
    LFK_ADC,       /* res, a, b */
    LFK_SBB,       /* res, a, b */
    LFK_LOGIC,     /* res */
    LFK_INC,       /* res; CF from eflags */
    LFK_DEC,       /* res; CF from eflags */
    LFK_SHL,       /* res, a = original, b = count (1..31) */
    LFK_SHR,
    LFK_SAR,
    LFK_SHRD,      /* like SHR, OF = sign change */
    LFK_ROL,       /* res; only CF/OF defined, rest from eflags */
    LFK_ROR,
    LFK_MUL,       /* res = low part, b = CF/OF value */
    LFK_BT,        /* res & 1 = CF, rest from eflags */
    LFK_COUNT
};
#define LF_OP(kind, sz) ((uint32_t)(kind) << 2 | (uint32_t)(sz))

/* x86 EFLAGS bits */
#define XF_CF 0x0001u
#define XF_PF 0x0004u
#define XF_AF 0x0010u
#define XF_ZF 0x0040u
#define XF_SF 0x0080u
#define XF_TF 0x0100u
#define XF_IF 0x0200u
#define XF_DF 0x0400u
#define XF_OF 0x0800u
#define XF_STATUS (XF_CF | XF_PF | XF_AF | XF_ZF | XF_SF | XF_OF)

/* ---------------------------------------------------------------- */
/* Exit reasons from translated code                                 */
/* ---------------------------------------------------------------- */
enum {
    EXIT_NONE = 0,
    EXIT_LINK,        /* direct branch to an untranslated/unlinked target */
    EXIT_INDIRECT,    /* indirect branch missed the lookup table */
    EXIT_PENDING,     /* asynchronous request (scheduler, SMC flush) */
    EXIT_HLT,
    EXIT_INT,         /* INT n / INT3; exit_info = vector */
    EXIT_UD,          /* undefined / unsupported instruction */
    EXIT_DIVIDE,      /* #DE */
    EXIT_STOP,        /* reached the configured stop address */
};

/* Helper function table indices (called from translated code). */
enum {
    H_FLAGS_SYNC,     /* void (cpu) : materialize lazy flags into eflags */
    H_COND,           /* int (cpu, cc) */
    H_GET_FLAGS,      /* uint32 (cpu) */
    H_SET_FLAGS,      /* void (cpu, value, mask) */
    H_DIV,            /* uint32 (cpu, insn_kind, divisor) returns 0 ok, 1 #DE */
    H_STRING,         /* void (cpu, packed op, ea) */
    H_CPUID,
    H_RDTSC,
    H_MISC,           /* void (cpu, op, arg): rare instructions */
    H_RCx,            /* rcl/rcr */
    H_X87,            /* x87 operations implemented in C */
    H_X87_MEM,        /* x87 ops with a memory operand */
    H_SSE,            /* rare SSE ops */
    H_CMPXCHG8B,
    H_COUNT
};

typedef struct xbt_cpu {
    /* ---- the first 256 bytes are reachable with ldp/stp immediates ---- */
    double   fpr[8];        /* x87 registers by physical index; ST(i) = fpr[(top+i)&7] */
    uint32_t r[8];          /* guest GPRs (valid outside translated code) */
    uint32_t eip;
    uint32_t eflags;        /* materialized flags + DF/IF */
    uint32_t lf_op;         /* lazy flags record: op, res, a, b (keep together) */
    uint32_t lf_res;
    uint32_t lf_a;
    uint32_t lf_b;
    uint32_t ftop;          /* x87 TOP (lives in w27 inside translated code) */
    uint16_t fcw;
    uint16_t fsw;           /* condition codes and exception bits; TOP kept in ftop */
    uint16_t ftw;           /* tag word (abridged: bit i set = register i empty) */
    uint8_t  pending;       /* nonzero: translated code exits at the next check */
    uint8_t  pad0;
    uint32_t fs_base;       /* linear base of FS (KPCR/TEB of the current thread) */
    uint32_t mxcsr;
    uint32_t exit_reason;
    uint32_t exit_info;
    uint32_t link_site;     /* host address of the branch to patch (EXIT_LINK) */
    uint32_t stop_eip;      /* EXIT_STOP when reached (test harness) */
    uint32_t pad1;
    /* ---- larger state ---- */
    uint8_t  xmm[8][16] __attribute__((aligned(16)));
    uint64_t mm[8];
    void    *helpers[H_COUNT];
    uint8_t  parity[256];   /* 1 if the byte has even parity (PF set) */
    uint64_t tsc;
    void    *user;          /* owner (thread object) */
} xbt_cpu;

/* translator options */
typedef struct xbt_options {
    int abi_flags;      /* flags are dead across CALL/RET (MSVC ABI) */
    int link_blocks;    /* patch direct branches between blocks */
    int flag_fusion;    /* use host NZCV for in-block flag consumers */
    int flag_elim;      /* skip flag computation when no reader exists */
    int max_insns;      /* max guest instructions per block */
    int trace;          /* print each translated block */
    int stats;
    const char *dump_dir;   /* write each block's host code here (debugging) */
} xbt_options;

extern xbt_options xbt_opt;

typedef struct xbt_stats {
    uint64_t blocks, guest_insns, host_insns;
    uint64_t flag_producers, flag_elided, flag_fused_only, flag_stored;
    uint64_t consumers, consumers_fused, consumers_helper;
    uint64_t links, dispatches, indirect_miss, flushes, smc_faults;
    uint64_t exits_link, exits_indirect, exits_other;
} xbt_stats;
extern xbt_stats xbt_st;

/* memory.c */
int  mem_init(uint32_t ram_size);
void *mem_host(uint32_t guest);       /* identity: returns (void*)guest */
int  mem_map_anon(uint32_t addr, uint32_t size);
void mem_protect_code_page(uint32_t page);
void mem_unprotect_all_code(void);
int  mem_readable(uint32_t addr, uint32_t len);
uint32_t mem_ram_size(void);

/* flags.c */
uint32_t xbt_flags(xbt_cpu *cpu);
void     xbt_flags_sync(xbt_cpu *cpu);
int      xbt_cond(xbt_cpu *cpu, int cc);
void     xbt_set_flags(xbt_cpu *cpu, uint32_t value, uint32_t mask);

/* runtime.c */
int  xbt_runtime_init(void);
void xbt_cpu_init(xbt_cpu *cpu);
int  xbt_run(xbt_cpu *cpu);           /* returns exit reason that stopped execution */
void xbt_flush(const char *why);
void xbt_print_stats(void);
typedef int (*xbt_hle_fn)(xbt_cpu *cpu, uint32_t eip);
void xbt_set_hle_handler(xbt_hle_fn fn);

/* translate.c */
typedef struct xbt_block xbt_block;
struct xbt_block {
    uint32_t guest_start, guest_end;
    uint32_t *host;
    uint32_t host_words;
    uint16_t ninsn;
    uint16_t *insn_off;     /* host word offset of each guest instruction */
    uint32_t *insn_eip;
    xbt_block *next;
};
xbt_block *xbt_translate(uint32_t eip, uint32_t **cache_ptr, uint32_t *cache_end);
void xbt_translate_init(void);

/* helpers.c */
void xbt_helpers_install(xbt_cpu *cpu);

#define XBT_LOG(...) fprintf(stderr, "[xbt] " __VA_ARGS__)

#endif
