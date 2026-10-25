/*
 * x86 instruction decoder for xlat (32-bit protected mode, the Pentium III's
 * instructions: integer, x87, MMX, SSE).  Fills an xinsn with the opcode,
 * prefixes, the ModRM operand split into base/index/scale/displacement and
 * the immediate.  What the opcode means is the interpreter's business.
 */
#include <stdio.h>

#include "xlat.h"

/* Per opcode: M = ModRM follows; immediate kind in the low bits. */
#define M     0x10
#define BAD   0x80
#define PFX   0x40
#define I_B   1
#define I_W   2
#define I_Z   3     /* 4 bytes, 2 with an operand size prefix */
#define I_ENT 4     /* ENTER: iw, ib */
#define I_PTR 5     /* far pointer: iz, iw */
#define I_MOF 6     /* moffs: 4 bytes, 2 with an address size prefix */
#define I_G3  7     /* F6/F7: test takes an immediate (ib / iz) */

static const uint8_t one_byte[256] = {
    /* 00 */ M, M, M, M, I_B, I_Z, 0, 0, M, M, M, M, I_B, I_Z, 0, 0,
    /* 10 */ M, M, M, M, I_B, I_Z, 0, 0, M, M, M, M, I_B, I_Z, 0, 0,
    /* 20 */ M, M, M, M, I_B, I_Z, PFX, 0, M, M, M, M, I_B, I_Z, PFX, 0,
    /* 30 */ M, M, M, M, I_B, I_Z, PFX, 0, M, M, M, M, I_B, I_Z, PFX, 0,
    /* 40 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 50 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    /* 60 */ 0, 0, M, M, PFX, PFX, PFX, PFX, I_Z, M | I_Z, I_B, M | I_B, 0, 0, 0, 0,
    /* 70 */ I_B, I_B, I_B, I_B, I_B, I_B, I_B, I_B, I_B, I_B, I_B, I_B, I_B, I_B, I_B, I_B,
    /* 80 */ M | I_B, M | I_Z, M | I_B, M | I_B, M, M, M, M, M, M, M, M, M, M, M, M,
    /* 90 */ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, I_PTR, 0, 0, 0, 0, 0,
    /* a0 */ I_MOF, I_MOF, I_MOF, I_MOF, 0, 0, 0, 0, I_B, I_Z, 0, 0, 0, 0, 0, 0,
    /* b0 */ I_B, I_B, I_B, I_B, I_B, I_B, I_B, I_B, I_Z, I_Z, I_Z, I_Z, I_Z, I_Z, I_Z, I_Z,
    /* c0 */ M | I_B, M | I_B, I_W, 0, M, M, M | I_B, M | I_Z, I_ENT, 0, I_W, 0, 0, I_B, 0, 0,
    /* d0 */ M, M, M, M, I_B, I_B, 0, 0, M, M, M, M, M, M, M, M,
    /* e0 */ I_B, I_B, I_B, I_B, I_B, I_B, I_B, I_B, I_Z, I_Z, I_PTR, I_B, 0, 0, 0, 0,
    /* f0 */ PFX, 0, PFX, PFX, 0, 0, M | I_G3, M | I_G3, 0, 0, 0, 0, 0, 0, M, M,
};

static const uint8_t two_byte[256] = {
    /* 00 */ M, M, M, M, BAD, BAD, 0, BAD, 0, 0, BAD, 0, BAD, M, BAD, BAD,
    /* 10 */ M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M,
    /* 20 */ M, M, M, M, BAD, BAD, BAD, BAD, M, M, M, M, M, M, M, M,
    /* 30 */ 0, 0, 0, 0, 0, 0, BAD, BAD, BAD, BAD, BAD, BAD, BAD, BAD, BAD, BAD,
    /* 40 */ M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M,
    /* 50 */ M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M,
    /* 60 */ M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M,
    /* 70 */ M | I_B, M | I_B, M | I_B, M | I_B, M, M, M, 0, BAD, BAD, BAD, BAD, BAD, BAD, M, M,
    /* 80 */ I_Z, I_Z, I_Z, I_Z, I_Z, I_Z, I_Z, I_Z, I_Z, I_Z, I_Z, I_Z, I_Z, I_Z, I_Z, I_Z,
    /* 90 */ M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M,
    /* a0 */ 0, 0, 0, M, M | I_B, M, BAD, BAD, 0, 0, 0, M, M | I_B, M, M, M,
    /* b0 */ M, M, M, M, M, M, M, M, BAD, M, M | I_B, M, M, M, M, M,
    /* c0 */ M, M, M | I_B, M, M | I_B, M | I_B, M | I_B, M, 0, 0, 0, 0, 0, 0, 0, 0,
    /* d0 */ M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M,
    /* e0 */ M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, M,
    /* f0 */ M, M, M, M, M, M, M, M, M, M, M, M, M, M, M, BAD,
};

static const int8_t ad16_base[8] = { XR_EBX, XR_EBX, XR_EBP, XR_EBP, XR_ESI, XR_EDI, XR_EBP, XR_EBX };
static const int8_t ad16_index[8] = { XR_ESI, XR_EDI, XR_ESI, XR_EDI, -1, -1, -1, -1 };

bool x86_decode(xinsn *in, uint32_t addr)
{
    const uint8_t *p = (const uint8_t *)(uintptr_t)addr, *start = p;
    memset(in, 0, sizeof(*in));
    in->addr = addr;
    in->base = in->index = -1;
    uint8_t b;
    /* prefixes */
    for (;;) {
        b = *p;
        if (p - start >= 14) goto bad;
        switch (b) {
        case 0xF0: in->pfx |= XP_LOCK; p++; continue;
        case 0xF2: in->pfx = (in->pfx & ~XP_REP) | XP_REPNE; p++; continue;
        case 0xF3: in->pfx = (in->pfx & ~XP_REPNE) | XP_REP; p++; continue;
        case 0x66: in->pfx |= XP_OPSZ; p++; continue;
        case 0x67: in->pfx |= XP_ADSZ; p++; continue;
        case 0x26: in->seg = XSEG_ES; p++; continue;
        case 0x2E: in->seg = XSEG_CS; p++; continue;
        case 0x36: in->seg = XSEG_SS; p++; continue;
        case 0x3E: in->seg = XSEG_DS; p++; continue;
        case 0x64: in->seg = XSEG_FS; p++; continue;
        case 0x65: in->seg = XSEG_GS; p++; continue;
        }
        break;
    }
    uint8_t kind;
    p++;
    if (b == 0x0F) {
        b = *p++;
        in->op = 0x100 | b;
        kind = two_byte[b];
    } else {
        in->op = b;
        kind = one_byte[b];
    }
    if (kind & BAD) goto bad;
    if (kind & M) {
        uint8_t m = *p++;
        in->has_modrm = 1;
        in->modrm = m;
        in->mod = m >> 6;
        in->reg = (m >> 3) & 7;
        in->rm = m & 7;
        if (in->mod != 3) {
            if (in->pfx & XP_ADSZ) {
                in->ad16 = 1;
                if (in->mod == 0 && in->rm == 6) {
                    in->disp = (int16_t)(p[0] | p[1] << 8);
                    p += 2;
                } else {
                    in->base = ad16_base[in->rm];
                    in->index = ad16_index[in->rm];
                    if (in->mod == 1) in->disp = (int8_t)*p++;
                    else if (in->mod == 2) { in->disp = (int16_t)(p[0] | p[1] << 8); p += 2; }
                }
            } else {
                if (in->rm == 4) {
                    uint8_t sib = *p++;
                    in->scale = sib >> 6;
                    int idx = (sib >> 3) & 7, base = sib & 7;
                    if (idx != 4) in->index = (int8_t)idx;
                    if (base == 5 && in->mod == 0) {
                        memcpy(&in->disp, p, 4);
                        p += 4;
                    } else {
                        in->base = (int8_t)base;
                    }
                } else if (in->rm == 5 && in->mod == 0) {
                    memcpy(&in->disp, p, 4);
                    p += 4;
                } else {
                    in->base = (int8_t)in->rm;
                }
                if (in->mod == 1) in->disp = (int8_t)*p++;
                else if (in->mod == 2) { memcpy(&in->disp, p, 4); p += 4; }
            }
        }
    }
    int isz = 0;
    switch (kind & 0x0F) {
    case I_B: isz = 1; break;
    case I_W: isz = 2; break;
    case I_Z: isz = (in->pfx & XP_OPSZ) ? 2 : 4; break;
    case I_ENT:
        in->imm = p[0] | p[1] << 8;
        in->imm2 = p[2];
        in->imm_size = 2;
        p += 3;
        break;
    case I_PTR:
        isz = (in->pfx & XP_OPSZ) ? 2 : 4;
        break;
    case I_MOF:
        if (in->pfx & XP_ADSZ) { in->disp = p[0] | p[1] << 8; p += 2; }
        else { memcpy(&in->disp, p, 4); p += 4; }
        break;
    case I_G3:
        if (in->reg < 2) isz = b == 0xF6 ? 1 : (in->pfx & XP_OPSZ) ? 2 : 4;
        break;
    }
    if (isz) {
        uint32_t v = 0;
        memcpy(&v, p, isz);
        in->imm = v;
        in->imm_size = (uint8_t)isz;
        p += isz;
    }
    if ((kind & 0x0F) == I_PTR) { in->imm2 = p[0] | p[1] << 8; p += 2; }
    in->len = (uint8_t)(p - start);
    return true;
bad:
    in->len = (uint8_t)(p - start);
    return false;
}

void x86_format(const xinsn *in, char *out, size_t size)
{
    size_t n = 0;
    const uint8_t *p = (const uint8_t *)(uintptr_t)in->addr;
    n += snprintf(out + n, size - n, "%08x:", in->addr);
    for (int i = 0; i < in->len && i < 15 && n < size; i++) n += snprintf(out + n, size - n, " %02x", p[i]);
}
