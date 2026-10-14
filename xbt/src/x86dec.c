/*
 * x86dec.c - IA-32 instruction decoder.
 */
#include <stdio.h>
#include <string.h>
#include "x86dec.h"

/* operand encoding flags */
#define F_MODRM  0x01
#define F_IMM8   0x02
#define F_IMM16  0x04
#define F_IMMZ   0x08   /* 16 or 32 bits depending on operand size */
#define F_REL8   0x10
#define F_RELZ   0x20
#define F_MOFFS  0x40
#define F_ENTER  0x80
#define F_FAR    0x100
#define F_GRP3   0x200  /* F6/F7: immediate only for /0 and /1 */
#define F_PFX    0x400
#define F_BAD    0x800

#define M F_MODRM
static const uint16_t op1[256] = {
    /* 00 */ M, M, M, M, F_IMM8, F_IMMZ, 0, 0, M, M, M, M, F_IMM8, F_IMMZ, 0, 0,
    /* 10 */ M, M, M, M, F_IMM8, F_IMMZ, 0, 0, M, M, M, M, F_IMM8, F_IMMZ, 0, 0,
    /* 20 */ M, M, M, M, F_IMM8, F_IMMZ, F_PFX, 0, M, M, M, M, F_IMM8, F_IMMZ, F_PFX, 0,
    /* 30 */ M, M, M, M, F_IMM8, F_IMMZ, F_PFX, 0, M, M, M, M, F_IMM8, F_IMMZ, F_PFX, 0,
    /* 40 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 50 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 60 */ 0, 0, M, M, F_PFX, F_PFX, F_PFX, F_PFX, F_IMMZ, M | F_IMMZ, F_IMM8, M | F_IMM8, 0, 0, 0, 0,
    /* 70 */ F_REL8, F_REL8, F_REL8, F_REL8, F_REL8, F_REL8, F_REL8, F_REL8,
             F_REL8, F_REL8, F_REL8, F_REL8, F_REL8, F_REL8, F_REL8, F_REL8,
    /* 80 */ M | F_IMM8, M | F_IMMZ, M | F_IMM8, M | F_IMM8, M, M, M, M, M, M, M, M, M, M, M, M,
    /* 90 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, F_FAR, 0, 0, 0, 0, 0,
    /* a0 */ F_MOFFS, F_MOFFS, F_MOFFS, F_MOFFS, 0, 0, 0, 0, F_IMM8, F_IMMZ, 0, 0, 0, 0, 0, 0,
    /* b0 */ F_IMM8, F_IMM8, F_IMM8, F_IMM8, F_IMM8, F_IMM8, F_IMM8, F_IMM8,
             F_IMMZ, F_IMMZ, F_IMMZ, F_IMMZ, F_IMMZ, F_IMMZ, F_IMMZ, F_IMMZ,
    /* c0 */ M | F_IMM8, M | F_IMM8, F_IMM16, 0, M, M, M | F_IMM8, M | F_IMMZ,
             F_ENTER, 0, F_IMM16, 0, 0, F_IMM8, 0, 0,
    /* d0 */ M, M, M, M, F_IMM8, F_IMM8, 0, 0, M, M, M, M, M, M, M, M,
    /* e0 */ F_REL8, F_REL8, F_REL8, F_REL8, F_IMM8, F_IMM8, F_IMM8, F_IMM8,
             F_RELZ, F_RELZ, F_FAR, F_REL8, 0, 0, 0, 0,
    /* f0 */ F_PFX, 0, F_PFX, F_PFX, 0, 0, M | F_GRP3, M | F_GRP3, 0, 0, 0, 0, 0, 0, M, M,
};

static const uint16_t op2[256] = {
    /* 00 */ M, M, M, M, F_BAD, 0, 0, 0, 0, 0, F_BAD, 0, F_BAD, M, 0, M | F_IMM8,
    /* 10 */ M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M,
    /* 20 */ M, M, M, M, F_BAD, F_BAD, F_BAD, F_BAD, M, M, M, M, M, M, M, M,
    /* 30 */ 0, 0, 0, 0, 0, 0, F_BAD, F_BAD, F_BAD, F_BAD, F_BAD, F_BAD, F_BAD, F_BAD, F_BAD, F_BAD,
    /* 40 */ M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M,
    /* 50 */ M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M,
    /* 60 */ M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M,
    /* 70 */ M | F_IMM8, M | F_IMM8, M | F_IMM8, M | F_IMM8, M, M, M, 0, M, M, F_BAD, F_BAD, M, M, M, M,
    /* 80 */ F_RELZ, F_RELZ, F_RELZ, F_RELZ, F_RELZ, F_RELZ, F_RELZ, F_RELZ,
             F_RELZ, F_RELZ, F_RELZ, F_RELZ, F_RELZ, F_RELZ, F_RELZ, F_RELZ,
    /* 90 */ M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M,
    /* a0 */ 0, 0, 0, M, M | F_IMM8, M, F_BAD, F_BAD, 0, 0, 0, M, M | F_IMM8, M, M, M,
    /* b0 */ M, M, M, M, M, M, M, M, M, M, M | F_IMM8, M, M, M, M, M,
    /* c0 */ M, M, M | F_IMM8, M, M | F_IMM8, M | F_IMM8, M | F_IMM8, M, 0, 0, 0, 0, 0, 0, 0, 0,
    /* d0 */ M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M,
    /* e0 */ M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M,
    /* f0 */ M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, F_BAD,
};
#undef M

int x86_decode(x86_insn *in, uint32_t addr, const uint8_t *code, int avail)
{
    int pos = 0;
    int addr16 = 0;
    memset(in, 0, sizeof(*in));
    in->addr = addr;
    in->opsize = 4;
    in->mem.base = -1;
    in->mem.index = -1;

#define NEED(n) do { if (pos + (n) > avail || pos + (n) > 15) goto bad; } while (0)
#define U8()  (code[pos++])

    /* prefixes */
    for (;;) {
        NEED(1);
        uint8_t b = code[pos];
        if (b == 0x66) { in->opsize = 2; in->opsz_pfx = 1; }
        else if (b == 0x67) addr16 = 1;
        else if (b == 0xf0) in->lock = 1;
        else if (b == 0xf2) in->rep = REP_NZ;
        else if (b == 0xf3) in->rep = REP_Z;
        else if (b == 0x26) in->mem.seg = SEG_ES;
        else if (b == 0x2e) in->mem.seg = SEG_CS;
        else if (b == 0x36) in->mem.seg = SEG_SS;
        else if (b == 0x3e) in->mem.seg = SEG_DS;
        else if (b == 0x64) in->mem.seg = SEG_FS;
        else if (b == 0x65) in->mem.seg = SEG_GS;
        else break;
        pos++;
    }

    NEED(1);
    uint8_t b = U8();
    uint16_t flags;
    if (b == 0x0f) {
        NEED(1);
        b = U8();
        in->op = 0x100 | b;
        flags = op2[b];
    } else {
        in->op = b;
        flags = op1[b];
    }
    if (flags & (F_BAD | F_PFX))
        goto bad;
    if (flags & F_MOFFS) {
        /* moffs: address-size absolute address */
        if (addr16)
            goto bad;
        NEED(4);
        uint32_t a;
        memcpy(&a, code + pos, 4);
        pos += 4;
        in->is_mem = 1;
        in->mem.disp = (int32_t)a;
        goto done;
    }

    if (flags & F_MODRM) {
        NEED(1);
        uint8_t m = U8();
        in->has_modrm = 1;
        in->mod = m >> 6;
        in->reg = (m >> 3) & 7;
        in->rm = m & 7;
        if (in->mod != 3) {
            if (addr16)
                goto bad;          /* 16-bit addressing never used by Xbox code */
            in->is_mem = 1;
            int rm = in->rm;
            if (rm == 4) {
                NEED(1);
                uint8_t sib = U8();
                int ss = sib >> 6, idx = (sib >> 3) & 7, base = sib & 7;
                if (idx != 4) {
                    in->mem.index = (int8_t)idx;
                    in->mem.scale = (uint8_t)ss;
                }
                if (base == 5 && in->mod == 0) {
                    NEED(4);
                    int32_t d;
                    memcpy(&d, code + pos, 4);
                    pos += 4;
                    in->mem.disp = d;
                } else {
                    in->mem.base = (int8_t)base;
                }
            } else if (rm == 5 && in->mod == 0) {
                NEED(4);
                int32_t d;
                memcpy(&d, code + pos, 4);
                pos += 4;
                in->mem.disp = d;
            } else {
                in->mem.base = (int8_t)rm;
            }
            if (in->mod == 1) {
                NEED(1);
                in->mem.disp = (int8_t)U8();
            } else if (in->mod == 2) {
                NEED(4);
                int32_t d;
                memcpy(&d, code + pos, 4);
                pos += 4;
                in->mem.disp = d;
            }
        }
    }

    if (flags & F_GRP3) {
        if (in->reg == 0 || in->reg == 1)
            flags |= (b == 0xf6) ? F_IMM8 : F_IMMZ;
    }
    if (flags & F_IMM8) {
        NEED(1);
        int8_t v = (int8_t)U8();
        in->imm = (uint32_t)(int32_t)v;
    }
    if (flags & F_IMM16) {
        NEED(2);
        uint16_t v;
        memcpy(&v, code + pos, 2);
        pos += 2;
        in->imm = v;
    }
    if (flags & F_IMMZ) {
        if (in->opsize == 2) {
            NEED(2);
            uint16_t v;
            memcpy(&v, code + pos, 2);
            pos += 2;
            in->imm = (uint32_t)(int32_t)(int16_t)v;
        } else {
            NEED(4);
            memcpy(&in->imm, code + pos, 4);
            pos += 4;
        }
    }
    if (flags & F_ENTER) {
        NEED(3);
        uint16_t v;
        memcpy(&v, code + pos, 2);
        pos += 2;
        in->imm = v;
        in->imm2 = U8();
    }
    if (flags & F_FAR) {
        NEED(6);
        memcpy(&in->imm, code + pos, 4);
        pos += 4;
        uint16_t s;
        memcpy(&s, code + pos, 2);
        pos += 2;
        in->imm2 = s;
    }
    if (flags & F_REL8) {
        NEED(1);
        int8_t r = (int8_t)U8();
        in->target = addr + (uint32_t)pos + (uint32_t)(int32_t)r;
        if (in->opsize == 2)
            in->target &= 0xffff;
    }
    if (flags & F_RELZ) {
        if (in->opsize == 2) {
            NEED(2);
            int16_t r;
            memcpy(&r, code + pos, 2);
            pos += 2;
            in->target = (addr + (uint32_t)pos + (uint32_t)(int32_t)r) & 0xffff;
        } else {
            NEED(4);
            int32_t r;
            memcpy(&r, code + pos, 4);
            pos += 4;
            in->target = addr + (uint32_t)pos + (uint32_t)r;
        }
    }
done:
    in->len = (uint8_t)pos;
    return pos;
bad:
    in->invalid = 1;
    in->len = (uint8_t)(pos ? pos : 1);
    return 0;
#undef NEED
#undef U8
}

static const char *reg32[8] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi" };

void x86_format(const x86_insn *in, char *out, int outlen)
{
    char m[96] = "";
    if (in->is_mem) {
        int n = snprintf(m, sizeof m, "%s[", in->mem.seg == SEG_FS ? "fs:" : "");
        if (in->mem.base >= 0)
            n += snprintf(m + n, sizeof m - n, "%s", reg32[in->mem.base]);
        if (in->mem.index >= 0)
            n += snprintf(m + n, sizeof m - n, "%s%s*%d", in->mem.base >= 0 ? "+" : "",
                          reg32[in->mem.index], 1 << in->mem.scale);
        if (in->mem.disp || (in->mem.base < 0 && in->mem.index < 0))
            n += snprintf(m + n, sizeof m - n, "%s0x%x",
                          (in->mem.base >= 0 || in->mem.index >= 0) ? "+" : "",
                          (uint32_t)in->mem.disp);
        snprintf(m + n, sizeof m - n, "]");
    }
    snprintf(out, outlen, "%08x: op=%s%02x%s%s len=%d modrm(mod=%d reg=%d rm=%d) %s imm=0x%x tgt=0x%x",
             in->addr, in->op & 0x100 ? "0f " : "", in->op & 0xff,
             in->rep == REP_Z ? " rep" : in->rep == REP_NZ ? " repne" : "",
             in->opsz_pfx ? " o16" : "", in->len, in->mod, in->reg, in->rm, m,
             in->imm, in->target);
}
