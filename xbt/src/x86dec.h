/*
 * x86dec.h - IA-32 instruction decoder (Pentium III feature set: integer,
 * x87, MMX, SSE). Only 32-bit protected mode with flat segments is supported,
 * which is the only mode Xbox titles execute in.
 */
#ifndef XBT_X86DEC_H
#define XBT_X86DEC_H

#include <stdint.h>

enum { R_EAX, R_ECX, R_EDX, R_EBX, R_ESP, R_EBP, R_ESI, R_EDI };
enum { SEG_NONE = 0, SEG_ES, SEG_CS, SEG_SS, SEG_DS, SEG_FS, SEG_GS };
enum { REP_NONE = 0, REP_Z = 1 /* F3 */, REP_NZ = 2 /* F2 */ };

typedef struct x86_mem {
    int8_t base;     /* -1 if none */
    int8_t index;    /* -1 if none */
    uint8_t scale;   /* shift amount 0..3 */
    uint8_t seg;     /* SEG_* override, SEG_NONE for flat */
    int32_t disp;
} x86_mem;

typedef struct x86_insn {
    uint32_t addr;
    uint8_t len;
    uint8_t opsize;      /* 2 or 4 (operand-size attribute) */
    uint8_t rep;         /* REP_* */
    uint8_t lock;
    uint8_t opsz_pfx;    /* 0x66 seen */
    uint8_t has_modrm;
    uint8_t mod, reg, rm;
    uint8_t is_mem;      /* ModRM operand is memory */
    uint8_t invalid;
    uint16_t op;         /* 0x000-0x0ff one byte, 0x100-0x1ff 0F-prefixed */
    x86_mem mem;
    uint32_t imm;        /* immediate, sign- or zero-extended as appropriate */
    uint32_t imm2;       /* second immediate (ENTER nesting level) */
    uint32_t target;     /* absolute target of relative branches */
} x86_insn;

/*
 * Decode one instruction at guest address `addr`, reading bytes from `code`
 * (host pointer to the same bytes). `avail` is the number of readable bytes.
 * Returns the instruction length, or 0 if undecodable (insn->invalid set).
 */
int x86_decode(x86_insn *insn, uint32_t addr, const uint8_t *code, int avail);

/* Format an instruction as text (for diagnostics only). */
void x86_format(const x86_insn *insn, char *out, int outlen);

#endif
