/*
 * The xlat interpreter: runs one x86 instruction on an xcpu.
 *
 * Flags are computed when an instruction sets them, into the form the ARM
 * code generator keeps them in (xcpu.nzcv holds SF ZF CF OF as N Z C V, PF
 * is the parity of pf_res's low byte, AF bit 4 of af).  Flags Intel leaves
 * undefined get what is cheapest here; the difference tests mask them.
 */
#include <stdlib.h>

#include "xlat.h"

/* ---- operands --------------------------------------------------------- */

static inline uint32_t szmask(int sz) { return sz == 4 ? 0xFFFFFFFFu : sz == 2 ? 0xFFFFu : 0xFFu; }
static inline uint32_t szsign(int sz) { return sz == 4 ? 0x80000000u : sz == 2 ? 0x8000u : 0x80u; }
static inline int32_t sext(uint32_t v, int sz) { return sz == 4 ? (int32_t)v : sz == 2 ? (int16_t)v : (int8_t)v; }

static inline uint32_t getr(const xcpu *c, int r, int sz)
{
    if (sz == 4) return c->r[r];
    if (sz == 2) return c->r[r] & 0xFFFF;
    return r < 4 ? c->r[r] & 0xFF : (c->r[r - 4] >> 8) & 0xFF;
}

static inline void setr(xcpu *c, int r, int sz, uint32_t v)
{
    if (sz == 4) c->r[r] = v;
    else if (sz == 2) c->r[r] = (c->r[r] & ~0xFFFFu) | (v & 0xFFFF);
    else if (r < 4) c->r[r] = (c->r[r] & ~0xFFu) | (v & 0xFF);
    else c->r[r - 4] = (c->r[r - 4] & ~0xFF00u) | ((v & 0xFF) << 8);
}

static inline uint32_t rdm(uint32_t a, int sz) { return sz == 4 ? rd32(a) : sz == 2 ? rd16(a) : rd8(a); }

static inline void wrm(uint32_t a, int sz, uint32_t v)
{
    if (sz == 4) wr32(a, v);
    else if (sz == 2) wr16(a, (uint16_t)v);
    else wr8(a, (uint8_t)v);
}

/* The operand's offset (what lea computes) and its linear address. */
static inline uint32_t ea_offset(const xcpu *c, const xinsn *in)
{
    uint32_t a = (uint32_t)in->disp;
    if (in->base >= 0) a += c->r[in->base];
    if (in->index >= 0) a += c->r[in->index] << in->scale;
    if (in->ad16) a &= 0xFFFF;
    return a;
}

static inline uint32_t seg_base(const xcpu *c, int seg) { return seg == XSEG_FS ? c->fs_base : 0; }

static inline uint32_t ea_linear(const xcpu *c, const xinsn *in) { return ea_offset(c, in) + seg_base(c, in->seg); }

/* r/m operand */
static inline uint32_t e_rd(const xcpu *c, const xinsn *in, uint32_t ea, int sz)
{
    return in->mod == 3 ? getr(c, in->rm, sz) : rdm(ea, sz);
}

static inline void e_wr(xcpu *c, const xinsn *in, uint32_t ea, int sz, uint32_t v)
{
    if (in->mod == 3) setr(c, in->rm, sz, v);
    else wrm(ea, sz, v);
}

static inline void push32(xcpu *c, uint32_t v)
{
    c->r[XR_ESP] -= 4;
    wr32(c->r[XR_ESP], v);
}

static inline uint32_t pop32(xcpu *c)
{
    uint32_t v = rd32(c->r[XR_ESP]);
    c->r[XR_ESP] += 4;
    return v;
}

static inline void pushv(xcpu *c, uint32_t v, int sz)
{
    c->r[XR_ESP] -= sz;
    wrm(c->r[XR_ESP], sz, v);
}

static inline uint32_t popv(xcpu *c, int sz)
{
    uint32_t v = rdm(c->r[XR_ESP], sz);
    c->r[XR_ESP] += sz;
    return v;
}

/* ---- flags ------------------------------------------------------------ */

static inline void set_flags(xcpu *c, uint32_t r, int sz, uint32_t cv)
{
    if (r & szsign(sz)) cv |= XF_N;
    if (!(r & szmask(sz))) cv |= XF_Z;
    c->nzcv = cv;
    c->pf_res = r;
}

static inline uint32_t cf(const xcpu *c) { return (c->nzcv >> 29) & 1; }

bool xi_cond(const xcpu *c, unsigned cc)
{
    uint32_t f = c->nzcv;
    bool n = f & XF_N, z = f & XF_Z, cy = f & XF_C, v = f & XF_V, r;
    switch (cc >> 1) {
    case 0: r = v; break;
    case 1: r = cy; break;
    case 2: r = z; break;
    case 3: r = cy || z; break;
    case 4: r = n; break;
    case 5: r = parity8(c->pf_res); break;
    case 6: r = n != v; break;
    default: r = z || n != v; break;
    }
    return (cc & 1) ? !r : r;
}

uint32_t xi_get_eflags(const xcpu *c)
{
    uint32_t f = (c->eflags & ~(EF_ARITH | EF_DF)) | 2;
    if (c->nzcv & XF_C) f |= EF_CF;
    if (parity8(c->pf_res)) f |= EF_PF;
    f |= c->af & EF_AF;
    if (c->nzcv & XF_Z) f |= EF_ZF;
    if (c->nzcv & XF_N) f |= EF_SF;
    if (c->nzcv & XF_V) f |= EF_OF;
    if (c->df) f |= EF_DF;
    return f;
}

void xi_set_eflags(xcpu *c, uint32_t f)
{
    c->nzcv = (f & EF_CF ? XF_C : 0) | (f & EF_ZF ? XF_Z : 0) | (f & EF_SF ? XF_N : 0) | (f & EF_OF ? XF_V : 0);
    c->pf_res = (f & EF_PF) ? 0 : 1;
    c->af = f & EF_AF;
    c->df = (f & EF_DF) ? 1 : 0;
    c->eflags = (f & ~(EF_ARITH | EF_DF)) | 2;
}

void xi_reset(xcpu *c)
{
    memset(c, 0, sizeof(*c));
    c->eflags = 0x202;
    c->fpcw = 0x37F;   /* all exceptions masked, as a Linux thread starts */
    c->ftag = 0xFF;
    c->mxcsr = 0x1F80;
}

/* add/or/adc/sbb/and/sub/xor/cmp */
static uint32_t alu(xcpu *c, int op, uint32_t a, uint32_t b, int sz)
{
    uint32_t m = szmask(sz), s = szsign(sz), r = 0, cv = 0, cin = cf(c);
    a &= m;
    b &= m;
    switch (op) {
    case 0:
        r = (a + b) & m;
        if (r < a) cv |= XF_C;
        if ((a ^ r) & (b ^ r) & s) cv |= XF_V;
        c->af = (a ^ b ^ r) & 0x10;
        break;
    case 2: {
        uint64_t t = (uint64_t)a + b + cin;
        r = (uint32_t)t & m;
        if (t > m) cv |= XF_C;
        if ((a ^ r) & (b ^ r) & s) cv |= XF_V;
        c->af = (a ^ b ^ r) & 0x10;
        break;
    }
    case 3:
        r = (a - b - cin) & m;
        if ((uint64_t)a < (uint64_t)b + cin) cv |= XF_C;
        if ((a ^ b) & (a ^ r) & s) cv |= XF_V;
        c->af = (a ^ b ^ r) & 0x10;
        break;
    case 5:
    case 7:
        r = (a - b) & m;
        if (a < b) cv |= XF_C;
        if ((a ^ b) & (a ^ r) & s) cv |= XF_V;
        c->af = (a ^ b ^ r) & 0x10;
        break;
    case 1: r = a | b; c->af = 0; break;
    case 4: r = a & b; c->af = 0; break;
    case 6: r = a ^ b; c->af = 0; break;
    }
    set_flags(c, r, sz, cv);
    return r;
}

static uint32_t incdec(xcpu *c, uint32_t a, bool dec, int sz)
{
    uint32_t m = szmask(sz), s = szsign(sz);
    a &= m;
    uint32_t r = (dec ? a - 1 : a + 1) & m;
    uint32_t cv = c->nzcv & XF_C;
    if (!dec && r == s) cv |= XF_V;
    if (dec && r == s - 1) cv |= XF_V;
    c->af = (a ^ 1 ^ r) & 0x10;
    set_flags(c, r, sz, cv);
    return r;
}

/* rol ror rcl rcr shl shr sal sar; count already masked to 5 bits, not 0 */
static uint32_t shift(xcpu *c, int op, uint32_t a, unsigned n, int sz)
{
    unsigned w = sz * 8;
    uint32_t m = szmask(sz), s = szsign(sz), r;
    a &= m;
    uint32_t f = c->nzcv;
    switch (op) {
    case 0: {   /* rol: SF ZF PF AF unchanged */
        unsigned k = n % w;
        r = k ? ((a << k) | (a >> (w - k))) & m : a;
        f &= ~(XF_C | XF_V);
        if (r & 1) f |= XF_C;
        if (((r & s) != 0) != ((r & 1) != 0)) f |= XF_V;
        c->nzcv = f;
        return r;
    }
    case 1: {   /* ror */
        unsigned k = n % w;
        r = k ? ((a >> k) | (a << (w - k))) & m : a;
        f &= ~(XF_C | XF_V);
        if (r & s) f |= XF_C;
        if (((r & s) != 0) != ((r & (s >> 1)) != 0)) f |= XF_V;
        c->nzcv = f;
        return r;
    }
    case 2: {   /* rcl */
        unsigned k = n % (w + 1);
        uint64_t v = (uint64_t)a | ((uint64_t)cf(c) << w);   /* w+1 bit value, CF on top */
        uint64_t mm = (2ull << w) - 1;
        if (k) v = ((v << k) | (v >> (w + 1 - k))) & mm;
        r = (uint32_t)v & m;
        uint32_t cy = (uint32_t)(v >> w) & 1;
        f &= ~(XF_C | XF_V);
        if (cy) f |= XF_C;
        if (((r & s) != 0) != cy) f |= XF_V;
        c->nzcv = f;
        return r;
    }
    case 3: {   /* rcr */
        unsigned k = n % (w + 1);
        uint32_t cin = cf(c);
        bool of = ((a & s) != 0) != cin;
        uint64_t v = (uint64_t)a | ((uint64_t)cin << w);
        uint64_t mm = (2ull << w) - 1;
        if (k) v = ((v >> k) | (v << (w + 1 - k))) & mm;
        r = (uint32_t)v & m;
        f &= ~(XF_C | XF_V);
        if ((v >> w) & 1) f |= XF_C;
        if (of) f |= XF_V;
        c->nzcv = f;
        return r;
    }
    case 4:
    case 6: {   /* shl */
        uint64_t v = (uint64_t)a << n;
        r = (uint32_t)v & m;
        uint32_t cv = ((v >> w) & 1) ? XF_C : 0;
        if (((r & s) != 0) != ((cv & XF_C) != 0)) cv |= XF_V;
        c->af = 0;
        set_flags(c, r, sz, cv);
        return r;
    }
    case 5: {   /* shr */
        r = n < 32 ? a >> n : 0;
        uint32_t cv = (n <= 32 && ((a >> (n - 1)) & 1)) ? XF_C : 0;
        if (a & s) cv |= XF_V;   /* defined for n == 1 */
        c->af = 0;
        set_flags(c, r, sz, cv);
        return r;
    }
    default: {  /* sar */
        int32_t sa = sext(a, sz);
        r = (uint32_t)(sa >> (n < 32 ? n : 31)) & m;
        uint32_t cv = ((sa >> (n - 1 < 31 ? n - 1 : 31)) & 1) ? XF_C : 0;
        c->af = 0;
        set_flags(c, r, sz, cv);
        return r;
    }
    }
}

static uint32_t shld(xcpu *c, uint32_t a, uint32_t b, unsigned n, int sz, bool right)
{
    unsigned w = sz * 8;
    uint32_t m = szmask(sz), s = szsign(sz), r, cv;
    a &= m;
    b &= m;
    if (sz == 2) {
        /* 16-bit with a count over 16: undefined; done as a 32-bit pair */
        uint32_t pair = right ? (b << 16 | a) : (a << 16 | b);
        if (right) {
            uint64_t v = (uint64_t)pair | ((uint64_t)pair << 32);
            r = (uint32_t)(v >> n) & m;
            cv = ((v >> (n - 1)) & 1) ? XF_C : 0;
        } else {
            uint64_t v = ((uint64_t)pair << 32) | pair;
            r = (uint32_t)((v << n) >> 48) & m;
            cv = ((v << (n - 1)) >> 63) ? XF_C : 0;
        }
    } else if (right) {
        r = (a >> n) | (b << (w - n));
        cv = ((a >> (n - 1)) & 1) ? XF_C : 0;
    } else {
        r = (a << n) | (b >> (w - n));
        cv = ((a >> (w - n)) & 1) ? XF_C : 0;
    }
    if ((r ^ a) & s) cv |= XF_V;   /* sign changed (defined for n == 1) */
    c->af = 0;
    set_flags(c, r, sz, cv);
    return r;
}

/* ---- atomics (lock prefix, xchg with memory) ---------------------------- */

#define CAS(p, o, n) __atomic_compare_exchange_n((p), (o), (n), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)

static bool cas_mem(uint32_t a, int sz, uint32_t *old, uint32_t nv)
{
    if (a & (sz - 1)) {
        /* Misaligned: the host can't do it atomically. */
        uint32_t cur = rdm(a, sz);
        if (cur != *old) { *old = cur; return false; }
        wrm(a, sz, nv);
        return true;
    }
    if (sz == 4) return CAS((uint32_t *)(uintptr_t)a, old, nv);
    if (sz == 2) {
        uint16_t o = (uint16_t)*old;
        bool ok = CAS((uint16_t *)(uintptr_t)a, &o, (uint16_t)nv);
        *old = o;
        return ok;
    }
    uint8_t o = (uint8_t)*old;
    bool ok = CAS((uint8_t *)(uintptr_t)a, &o, (uint8_t)nv);
    *old = o;
    return ok;
}

static uint32_t xchg_mem(uint32_t a, int sz, uint32_t v)
{
    if (a & (sz - 1)) {
        uint32_t old = rdm(a, sz);
        wrm(a, sz, v);
        return old;
    }
    if (sz == 4) return __atomic_exchange_n((uint32_t *)(uintptr_t)a, v, __ATOMIC_SEQ_CST);
    if (sz == 2) return __atomic_exchange_n((uint16_t *)(uintptr_t)a, (uint16_t)v, __ATOMIC_SEQ_CST);
    return __atomic_exchange_n((uint8_t *)(uintptr_t)a, (uint8_t)v, __ATOMIC_SEQ_CST);
}

/* ---- string instructions --------------------------------------------- */

static void string_op(xcpu *c, const xinsn *in, int sz)
{
    int op = in->op & ~1;
    bool rep = in->pfx & (XP_REP | XP_REPNE);
    int step = c->df ? -sz : sz;
    uint32_t src_base = seg_base(c, in->seg ? in->seg : XSEG_DS);
    if (in->pfx & XP_ADSZ) xrt_fault(c, "16-bit string addressing");
    if (rep && !c->r[XR_ECX]) return;
    switch (op) {
    case 0xA4: {   /* movs */
        uint32_t n = rep ? c->r[XR_ECX] : 1;
        uint32_t s = c->r[XR_ESI], d = c->r[XR_EDI];
        uint32_t bytes = n * (uint32_t)sz;
        if (step > 0 && n > 1 && (d + bytes <= s + src_base || d >= s + src_base + bytes)) {
            memcpy((void *)(uintptr_t)d, (const void *)(uintptr_t)(s + src_base), bytes);
            s += bytes;
            d += bytes;
        } else {
            for (uint32_t i = 0; i < n; i++, s += step, d += step) wrm(d, sz, rdm(s + src_base, sz));
        }
        c->r[XR_ESI] = s;
        c->r[XR_EDI] = d;
        if (rep) c->r[XR_ECX] = 0;
        return;
    }
    case 0xAA: {   /* stos */
        uint32_t n = rep ? c->r[XR_ECX] : 1, d = c->r[XR_EDI], v = getr(c, XR_EAX, sz);
        if (sz == 1 && step > 0) {
            memset((void *)(uintptr_t)d, (int)v, n);
            d += n;
        } else {
            for (uint32_t i = 0; i < n; i++, d += step) wrm(d, sz, v);
        }
        c->r[XR_EDI] = d;
        if (rep) c->r[XR_ECX] = 0;
        return;
    }
    case 0xAC: {   /* lods */
        uint32_t n = rep ? c->r[XR_ECX] : 1, s = c->r[XR_ESI];
        for (uint32_t i = 0; i < n; i++, s += step) setr(c, XR_EAX, sz, rdm(s + src_base, sz));
        c->r[XR_ESI] = s;
        if (rep) c->r[XR_ECX] = 0;
        return;
    }
    case 0xA6:     /* cmps */
    case 0xAE: {   /* scas */
        bool cmps = op == 0xA6;
        bool want_eq = (in->pfx & XP_REP) != 0;
        for (;;) {
            uint32_t a, b;
            if (cmps) {
                a = rdm(c->r[XR_ESI] + src_base, sz);
                b = rdm(c->r[XR_EDI], sz);
                c->r[XR_ESI] += step;
            } else {
                a = getr(c, XR_EAX, sz);
                b = rdm(c->r[XR_EDI], sz);
            }
            c->r[XR_EDI] += step;
            alu(c, 7, a, b, sz);
            if (!rep) return;
            if (--c->r[XR_ECX] == 0) return;
            if (((c->nzcv & XF_Z) != 0) != want_eq) return;
        }
    }
    }
    xrt_fault(c, "port i/o string instruction");
}

/* ---- the instructions ------------------------------------------------- */

static void bcd_adjust(xcpu *c, int op)
{
    uint32_t al = c->r[XR_EAX] & 0xFF, ah = (c->r[XR_EAX] >> 8) & 0xFF;
    uint32_t cy = cf(c), af = (c->af & 0x10) != 0, f;
    switch (op) {
    case 0x27: {   /* daa */
        uint32_t old = al, ncy = 0, naf = 0;
        if ((al & 0xF) > 9 || af) { al += 6; ncy = cy || (old + 6 > 0xFF); naf = 1; }
        if (old > 0x99 || cy) { al += 0x60; ncy = 1; }
        al &= 0xFF;
        set_flags(c, al, 1, ncy ? XF_C : 0);
        c->af = naf ? 0x10 : 0;
        break;
    }
    case 0x2F: {   /* das */
        uint32_t old = al, ncy = 0, naf = 0;
        if ((al & 0xF) > 9 || af) { ncy = cy || old < 6; al -= 6; naf = 1; }
        if (old > 0x99 || cy) { al -= 0x60; ncy = 1; }
        al &= 0xFF;
        set_flags(c, al, 1, ncy ? XF_C : 0);
        c->af = naf ? 0x10 : 0;
        break;
    }
    case 0x37:     /* aaa */
    case 0x3F: {   /* aas */
        uint32_t ax = c->r[XR_EAX] & 0xFFFF;
        if ((al & 0xF) > 9 || af) {
            if (op == 0x37) ax += 0x106;
            else ax = ((ax - 6) & 0xFFFF) - 0x100;
            f = XF_C;
            c->af = 0x10;
        } else {
            f = 0;
            c->af = 0;
        }
        ax = (ax & 0xFF00) | (ax & 0xF);
        c->r[XR_EAX] = (c->r[XR_EAX] & ~0xFFFFu) | (ax & 0xFFFF);
        set_flags(c, ax & 0xFF, 1, f);
        (void)ah;
        break;
    }
    }
    if (op == 0x27 || op == 0x2F) c->r[XR_EAX] = (c->r[XR_EAX] & ~0xFFu) | al;
}

static void enter(xcpu *c, uint32_t size, uint32_t level)
{
    level &= 31;
    push32(c, c->r[XR_EBP]);
    uint32_t frame = c->r[XR_ESP];
    if (level) {
        uint32_t ebp = c->r[XR_EBP];
        for (uint32_t i = 1; i < level; i++) {
            ebp -= 4;
            push32(c, rd32(ebp));
        }
        push32(c, frame);
    }
    c->r[XR_EBP] = frame;
    c->r[XR_ESP] -= size;
}

/* Group 3 (F6/F7): test not neg mul imul div idiv */
static void group3(xcpu *c, const xinsn *in, uint32_t ea, int sz)
{
    uint32_t v = e_rd(c, in, ea, sz), m = szmask(sz);
    switch (in->reg) {
    case 0:
    case 1:
        alu(c, 4, v, in->imm, sz);
        return;
    case 2:
        if ((in->pfx & XP_LOCK) && in->mod != 3) {
            uint32_t old = v;
            while (!cas_mem(ea, sz, &old, ~old & m)) {}
            return;
        }
        e_wr(c, in, ea, sz, ~v & m);
        return;
    case 3:
        if ((in->pfx & XP_LOCK) && in->mod != 3) {
            uint32_t old = v;
            while (!cas_mem(ea, sz, &old, (0 - old) & m)) {}
            v = old;
        } else {
            e_wr(c, in, ea, sz, (0 - v) & m);
        }
        alu(c, 5, 0, v, sz);
        return;
    case 4: {   /* mul */
        uint64_t a = getr(c, XR_EAX, sz), r = a * v;
        uint32_t hi;
        if (sz == 1) { setr(c, XR_EAX, 2, (uint32_t)r); hi = (uint32_t)(r >> 8) & 0xFF; }
        else if (sz == 2) { setr(c, XR_EAX, 2, (uint32_t)r); setr(c, XR_EDX, 2, (uint32_t)(r >> 16)); hi = (uint32_t)(r >> 16) & 0xFFFF; }
        else { c->r[XR_EAX] = (uint32_t)r; c->r[XR_EDX] = (uint32_t)(r >> 32); hi = (uint32_t)(r >> 32); }
        set_flags(c, (uint32_t)r & m, sz, hi ? XF_C | XF_V : 0);
        return;
    }
    case 5: {   /* imul */
        int64_t a = sext(getr(c, XR_EAX, sz), sz), r = a * sext(v, sz);
        bool ovf;
        if (sz == 1) { setr(c, XR_EAX, 2, (uint32_t)r); ovf = r != (int8_t)r; }
        else if (sz == 2) { setr(c, XR_EAX, 2, (uint32_t)r); setr(c, XR_EDX, 2, (uint32_t)(r >> 16)); ovf = r != (int16_t)r; }
        else { c->r[XR_EAX] = (uint32_t)r; c->r[XR_EDX] = (uint32_t)((uint64_t)r >> 32); ovf = r != (int32_t)r; }
        set_flags(c, (uint32_t)r & m, sz, ovf ? XF_C | XF_V : 0);
        return;
    }
    case 6: {   /* div */
        if (!(v & m)) xrt_fault(c, "integer divide by zero");
        if (sz == 1) {
            uint32_t a = c->r[XR_EAX] & 0xFFFF, q = a / v, r = a % v;
            if (q > 0xFF) xrt_fault(c, "divide overflow");
            setr(c, XR_EAX, 2, (r << 8) | q);
        } else if (sz == 2) {
            uint32_t a = (c->r[XR_EDX] & 0xFFFF) << 16 | (c->r[XR_EAX] & 0xFFFF), q = a / v, r = a % v;
            if (q > 0xFFFF) xrt_fault(c, "divide overflow");
            setr(c, XR_EAX, 2, q);
            setr(c, XR_EDX, 2, r);
        } else {
            uint64_t a = (uint64_t)c->r[XR_EDX] << 32 | c->r[XR_EAX], q = a / v, r = a % v;
            if (q > 0xFFFFFFFFu) xrt_fault(c, "divide overflow");
            c->r[XR_EAX] = (uint32_t)q;
            c->r[XR_EDX] = (uint32_t)r;
        }
        return;
    }
    default: {  /* idiv */
        int64_t d = sext(v, sz);
        if (!d) xrt_fault(c, "integer divide by zero");
        if (sz == 1) {
            int64_t a = (int16_t)(c->r[XR_EAX] & 0xFFFF), q = a / d, r = a % d;
            if (q != (int8_t)q) xrt_fault(c, "divide overflow");
            setr(c, XR_EAX, 2, ((uint32_t)(r & 0xFF) << 8) | ((uint32_t)q & 0xFF));
        } else if (sz == 2) {
            int64_t a = (int32_t)((c->r[XR_EDX] & 0xFFFF) << 16 | (c->r[XR_EAX] & 0xFFFF)), q = a / d, r = a % d;
            if (q != (int16_t)q) xrt_fault(c, "divide overflow");
            setr(c, XR_EAX, 2, (uint32_t)q);
            setr(c, XR_EDX, 2, (uint32_t)r);
        } else {
            int64_t a = (int64_t)((uint64_t)c->r[XR_EDX] << 32 | c->r[XR_EAX]);
            if (a == INT64_MIN && d == -1) xrt_fault(c, "divide overflow");
            int64_t q = a / d, r = a % d;
            if (q != (int32_t)q) xrt_fault(c, "divide overflow");
            c->r[XR_EAX] = (uint32_t)q;
            c->r[XR_EDX] = (uint32_t)r;
        }
        return;
    }
    }
}

/* bt bts btr btc: kind 0..3 */
static void bit_op(xcpu *c, const xinsn *in, uint32_t ea, int sz, uint32_t bit, bool reg_offset, int kind)
{
    unsigned w = sz * 8;
    uint32_t v, a = ea;
    if (in->mod == 3) {
        bit &= w - 1;
        v = getr(c, in->rm, sz);
    } else {
        if (reg_offset) {
            int32_t off = sext(bit, sz);
            a += (uint32_t)((off >> (sz == 2 ? 4 : 5)) * sz);
            bit = (uint32_t)off & (w - 1);
        } else {
            bit &= w - 1;
        }
        v = rdm(a, sz);
    }
    uint32_t mask = 1u << bit, nv = v;
    switch (kind) {
    case 1: nv = v | mask; break;
    case 2: nv = v & ~mask; break;
    case 3: nv = v ^ mask; break;
    }
    if (kind) {
        if (in->mod == 3) setr(c, in->rm, sz, nv);
        else if (in->pfx & XP_LOCK) {
            uint32_t old = v;
            for (;;) {
                nv = kind == 1 ? old | mask : kind == 2 ? old & ~mask : old ^ mask;
                if (cas_mem(a, sz, &old, nv)) break;
            }
            v = old;
        } else {
            wrm(a, sz, nv);
        }
    }
    c->nzcv = (c->nzcv & ~XF_C) | ((v & mask) ? XF_C : 0);
}

int xi_step(xcpu *c)
{
    xinsn in;
    if (!x86_decode(&in, c->eip)) return XI_INVALID;
    return xi_exec(c, &in);
}

int xi_exec(xcpu *c, const xinsn *ip)
{
    const xinsn *in = ip;
    uint32_t next = in->addr + in->len;
    int osz = (in->pfx & XP_OPSZ) ? 2 : 4;
    uint32_t ea = (in->has_modrm && in->mod != 3) ? ea_linear(c, in) : 0;
    unsigned op = in->op;
    bool locked = (in->pfx & XP_LOCK) && in->has_modrm && in->mod != 3;

    if (op < 0x40 && (op & 7) < 6) {
        /* add or adc sbb and sub xor cmp */
        int aop = op >> 3, form = op & 7, sz = (form & 1) ? osz : 1;
        uint32_t a, b, r;
        switch (form) {
        case 0: case 1:   /* Ex, Gx */
            b = getr(c, in->reg, sz);
            if (locked && aop != 7) {
                uint32_t old = rdm(ea, sz);
                xcpu tmp;
                for (;;) {
                    tmp.nzcv = c->nzcv;
                    r = alu(&tmp, aop, old, b, sz);
                    if (cas_mem(ea, sz, &old, r)) break;
                }
                alu(c, aop, old, b, sz);
                break;
            }
            a = e_rd(c, in, ea, sz);
            r = alu(c, aop, a, b, sz);
            if (aop != 7) e_wr(c, in, ea, sz, r);
            break;
        case 2: case 3:   /* Gx, Ex */
            a = getr(c, in->reg, sz);
            b = e_rd(c, in, ea, sz);
            r = alu(c, aop, a, b, sz);
            if (aop != 7) setr(c, in->reg, sz, r);
            break;
        default:          /* al/eax, imm */
            a = getr(c, XR_EAX, sz);
            r = alu(c, aop, a, in->imm, sz);
            if (aop != 7) setr(c, XR_EAX, sz, r);
            break;
        }
        c->eip = next;
        return XI_OK;
    }

    switch (op) {
    /* ---- segments and BCD ---- */
    case 0x06: case 0x0E: case 0x16: case 0x1E:
        pushv(c, 0x23, osz);   /* push es/cs/ss/ds: flat selectors */
        break;
    case 0x07: case 0x17: case 0x1F:
        popv(c, osz);
        break;
    case 0x27: case 0x2F: case 0x37: case 0x3F:
        bcd_adjust(c, op);
        break;

    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
        setr(c, op & 7, osz, incdec(c, getr(c, op & 7, osz), false, osz));
        break;
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        setr(c, op & 7, osz, incdec(c, getr(c, op & 7, osz), true, osz));
        break;
    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
        pushv(c, getr(c, op & 7, osz), osz);
        break;
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F: {
        uint32_t v = popv(c, osz);
        setr(c, op & 7, osz, v);
        break;
    }
    case 0x60: {   /* pusha */
        uint32_t sp = c->r[XR_ESP];
        for (int r = 0; r < 8; r++) pushv(c, r == XR_ESP ? sp : getr(c, r, osz), osz);
        break;
    }
    case 0x61:     /* popa */
        for (int r = 7; r >= 0; r--) {
            uint32_t v = popv(c, osz);
            if (r != XR_ESP) setr(c, r, osz, v);
        }
        break;
    case 0x62: {   /* bound */
        int32_t v = sext(getr(c, in->reg, osz), osz);
        int32_t lo = sext(rdm(ea, osz), osz), hi = sext(rdm(ea + osz, osz), osz);
        if (v < lo || v > hi) xrt_fault(c, "bound range exceeded");
        break;
    }
    case 0x68:
        pushv(c, in->imm, osz);
        break;
    case 0x6A:
        pushv(c, (uint32_t)(int8_t)in->imm, osz);
        break;
    case 0x69:
    case 0x6B:
    case 0x1AF: {  /* imul Gv, Ev(, imm) */
        int64_t a = sext(e_rd(c, in, ea, osz), osz);
        int64_t b = op == 0x1AF ? sext(getr(c, in->reg, osz), osz)
                  : op == 0x6B ? (int8_t)in->imm : sext(in->imm, osz);
        int64_t r = a * b;
        bool ovf = osz == 4 ? r != (int32_t)r : r != (int16_t)r;
        setr(c, in->reg, osz, (uint32_t)r);
        set_flags(c, (uint32_t)r & szmask(osz), osz, ovf ? XF_C | XF_V : 0);
        break;
    }
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F:
        if (xi_cond(c, op & 15)) next += (uint32_t)(int8_t)in->imm;
        break;
    case 0x180: case 0x181: case 0x182: case 0x183: case 0x184: case 0x185: case 0x186: case 0x187:
    case 0x188: case 0x189: case 0x18A: case 0x18B: case 0x18C: case 0x18D: case 0x18E: case 0x18F:
        if (xi_cond(c, op & 15)) next += osz == 2 ? (uint32_t)(int16_t)in->imm : in->imm;
        if (osz == 2) next &= 0xFFFF;
        break;

    case 0x80: case 0x81: case 0x82: case 0x83: {   /* group 1 */
        int sz = op == 0x81 || op == 0x83 ? osz : 1;
        uint32_t b = op == 0x83 ? (uint32_t)(int8_t)in->imm : in->imm;
        if (locked && in->reg != 7) {
            uint32_t old = rdm(ea, sz), r;
            xcpu tmp;
            for (;;) {
                tmp.nzcv = c->nzcv;
                r = alu(&tmp, in->reg, old, b, sz);
                if (cas_mem(ea, sz, &old, r)) break;
            }
            alu(c, in->reg, old, b, sz);
            break;
        }
        uint32_t r = alu(c, in->reg, e_rd(c, in, ea, sz), b, sz);
        if (in->reg != 7) e_wr(c, in, ea, sz, r);
        break;
    }
    case 0x84: case 0x85: {
        int sz = op & 1 ? osz : 1;
        alu(c, 4, e_rd(c, in, ea, sz), getr(c, in->reg, sz), sz);
        break;
    }
    case 0x86: case 0x87: {
        int sz = op & 1 ? osz : 1;
        uint32_t v = getr(c, in->reg, sz);
        if (in->mod == 3) {
            uint32_t o = getr(c, in->rm, sz);
            setr(c, in->rm, sz, v);
            setr(c, in->reg, sz, o);
        } else {
            setr(c, in->reg, sz, xchg_mem(ea, sz, v));
        }
        break;
    }
    case 0x88: case 0x89: {
        int sz = op & 1 ? osz : 1;
        e_wr(c, in, ea, sz, getr(c, in->reg, sz));
        break;
    }
    case 0x8A: case 0x8B: {
        int sz = op & 1 ? osz : 1;
        setr(c, in->reg, sz, e_rd(c, in, ea, sz));
        break;
    }
    case 0x8C: {   /* mov r/m, sreg */
        static const uint16_t sel[8] = { 0x23, 0x1B, 0x23, 0x23, 0x3B, 0, 0, 0 };
        uint32_t v = sel[in->reg & 7];
        if (in->mod == 3) setr(c, in->rm, osz, v);
        else wr16(ea, (uint16_t)v);
        break;
    }
    case 0x8D:
        if (in->mod == 3) return XI_INVALID;
        setr(c, in->reg, osz, ea_offset(c, in));
        break;
    case 0x8E:     /* mov sreg, r/m: flat, nothing to change */
        break;
    case 0x8F: {   /* pop Ev: the address is computed after esp moves */
        uint32_t v = popv(c, osz);
        if (in->mod == 3) setr(c, in->rm, osz, v);
        else wrm(ea_linear(c, in), osz, v);
        break;
    }
    case 0x90:
        break;   /* nop, pause */
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: {
        uint32_t a = getr(c, XR_EAX, osz);
        setr(c, XR_EAX, osz, getr(c, op & 7, osz));
        setr(c, op & 7, osz, a);
        break;
    }
    case 0x98:
        if (osz == 2) setr(c, XR_EAX, 2, (uint32_t)(int8_t)c->r[XR_EAX]);
        else c->r[XR_EAX] = (uint32_t)(int16_t)c->r[XR_EAX];
        break;
    case 0x99:
        if (osz == 2) setr(c, XR_EDX, 2, (c->r[XR_EAX] & 0x8000) ? 0xFFFF : 0);
        else c->r[XR_EDX] = (c->r[XR_EAX] & 0x80000000u) ? 0xFFFFFFFFu : 0;
        break;
    case 0x9B:
        break;   /* fwait */
    case 0x9C:
        pushv(c, xi_get_eflags(c) & 0x00FCFFFF, osz);   /* VM and RF read as 0 */
        break;
    case 0x9D: {
        uint32_t f = popv(c, osz);
        if (osz == 2) f = (xi_get_eflags(c) & 0xFFFF0000u) | f;
        /* user mode: IOPL and VM don't change */
        f = (f & ~0x23000u) | (xi_get_eflags(c) & 0x23000u);
        xi_set_eflags(c, f);
        break;
    }
    case 0x9E: {   /* sahf */
        uint32_t ah = (c->r[XR_EAX] >> 8) & 0xFF;
        c->nzcv = (c->nzcv & XF_V) | (ah & EF_CF ? XF_C : 0) | (ah & EF_ZF ? XF_Z : 0) | (ah & EF_SF ? XF_N : 0);
        c->pf_res = (ah & EF_PF) ? 0 : 1;
        c->af = ah & EF_AF;
        break;
    }
    case 0x9F:     /* lahf */
        setr(c, 4, 1, xi_get_eflags(c) & 0xFF);
        break;

    case 0xA0: case 0xA1: {
        int sz = op & 1 ? osz : 1;
        setr(c, XR_EAX, sz, rdm((uint32_t)in->disp + seg_base(c, in->seg), sz));
        break;
    }
    case 0xA2: case 0xA3: {
        int sz = op & 1 ? osz : 1;
        wrm((uint32_t)in->disp + seg_base(c, in->seg), sz, getr(c, XR_EAX, sz));
        break;
    }
    case 0xA4: case 0xA5: case 0xA6: case 0xA7: case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF:
        string_op(c, in, op & 1 ? osz : 1);
        break;
    case 0xA8: case 0xA9: {
        int sz = op & 1 ? osz : 1;
        alu(c, 4, getr(c, XR_EAX, sz), in->imm, sz);
        break;
    }
    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        setr(c, op & 7, 1, in->imm);
        break;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        setr(c, op & 7, osz, in->imm);
        break;

    case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {   /* group 2 */
        int sz = op & 1 ? osz : 1;
        unsigned n = (op <= 0xC1 ? in->imm : op <= 0xD1 ? 1 : c->r[XR_ECX]) & 31;
        if (!n) break;
        if (locked) {
            uint32_t old = rdm(ea, sz), r;
            xcpu tmp;
            for (;;) {
                tmp = *c;
                r = shift(&tmp, in->reg, old, n, sz);
                if (cas_mem(ea, sz, &old, r)) break;
            }
            c->nzcv = tmp.nzcv; c->pf_res = tmp.pf_res; c->af = tmp.af;
            break;
        }
        e_wr(c, in, ea, sz, shift(c, in->reg, e_rd(c, in, ea, sz), n, sz));
        break;
    }
    case 0xC2:
        next = pop32(c);
        c->r[XR_ESP] += in->imm;
        break;
    case 0xC3:
        next = pop32(c);
        break;
    case 0xC6: case 0xC7: {
        int sz = op & 1 ? osz : 1;
        if (in->reg) return XI_INVALID;
        e_wr(c, in, ea, sz, in->imm);
        break;
    }
    case 0xC8:
        enter(c, in->imm, in->imm2);
        break;
    case 0xC9:
        c->r[XR_ESP] = c->r[XR_EBP];
        setr(c, XR_EBP, osz, popv(c, osz));
        break;
    case 0xCC:
        /* int 3: ignored, as the Unicorn backend does */
        break;
    case 0xCD:
        c->eip = in->addr;
        xrt_interrupt(c, in->imm & 0xFF, next);
        return XI_OK;
    case 0xCE:
        if (c->nzcv & XF_V) xrt_fault(c, "into");
        break;
    case 0xD4: {   /* aam */
        uint32_t al = c->r[XR_EAX] & 0xFF, base = in->imm & 0xFF;
        if (!base) xrt_fault(c, "aam 0");
        uint32_t ah = al / base;
        al %= base;
        setr(c, XR_EAX, 2, (ah << 8) | al);
        set_flags(c, al, 1, 0);
        break;
    }
    case 0xD5: {   /* aad */
        uint32_t al = c->r[XR_EAX] & 0xFF, ah = (c->r[XR_EAX] >> 8) & 0xFF;
        al = (al + ah * (in->imm & 0xFF)) & 0xFF;
        setr(c, XR_EAX, 2, al);
        set_flags(c, al, 1, 0);
        break;
    }
    case 0xD6:     /* salc */
        setr(c, XR_EAX, 1, cf(c) ? 0xFF : 0);
        break;
    case 0xD7:     /* xlat */
        setr(c, XR_EAX, 1, rd8(((c->r[XR_EBX] + (c->r[XR_EAX] & 0xFF))) + seg_base(c, in->seg)));
        break;
    case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF:
        if (xi_x87(c, in, ea) != XI_OK) return XI_INVALID;
        break;

    case 0xE0: case 0xE1: case 0xE2: {   /* loopne loope loop */
        uint32_t cx = --c->r[XR_ECX];
        bool z = (c->nzcv & XF_Z) != 0;
        if (cx && (op == 0xE2 || (op == 0xE1 ? z : !z))) next += (uint32_t)(int8_t)in->imm;
        break;
    }
    case 0xE3:
        if (!c->r[XR_ECX]) next += (uint32_t)(int8_t)in->imm;
        break;
    case 0xE8:
        push32(c, next);
        next += osz == 2 ? (uint32_t)(int16_t)in->imm : in->imm;
        break;
    case 0xE9:
        next += osz == 2 ? (uint32_t)(int16_t)in->imm : in->imm;
        break;
    case 0xEB:
        next += (uint32_t)(int8_t)in->imm;
        break;
    case 0xF4:
        c->eip = next;
        xrt_hlt(c);
        return XI_OK;
    case 0xF5:
        c->nzcv ^= XF_C;
        break;
    case 0xF6:
        group3(c, in, ea, 1);
        break;
    case 0xF7:
        group3(c, in, ea, osz);
        break;
    case 0xF8: c->nzcv &= ~XF_C; break;
    case 0xF9: c->nzcv |= XF_C; break;
    case 0xFA: case 0xFB: break;   /* cli, sti: nothing to do in user mode */
    case 0xFC: c->df = 0; break;
    case 0xFD: c->df = 1; break;
    case 0xFE:
        if (in->reg > 1) return XI_INVALID;
        if (locked) {
            uint32_t old = rd8(ea), r;
            xcpu tmp;
            for (;;) {
                tmp = *c;
                r = incdec(&tmp, old, in->reg, 1);
                if (cas_mem(ea, 1, &old, r)) break;
            }
            c->nzcv = tmp.nzcv; c->pf_res = tmp.pf_res; c->af = tmp.af;
            break;
        }
        e_wr(c, in, ea, 1, incdec(c, e_rd(c, in, ea, 1), in->reg, 1));
        break;
    case 0xFF:
        switch (in->reg) {
        case 0: case 1:
            if (locked) {
                uint32_t old = rdm(ea, osz), r;
                xcpu tmp;
                for (;;) {
                    tmp = *c;
                    r = incdec(&tmp, old, in->reg, osz);
                    if (cas_mem(ea, osz, &old, r)) break;
                }
                c->nzcv = tmp.nzcv; c->pf_res = tmp.pf_res; c->af = tmp.af;
                break;
            }
            e_wr(c, in, ea, osz, incdec(c, e_rd(c, in, ea, osz), in->reg, osz));
            break;
        case 2: {
            uint32_t t = e_rd(c, in, ea, osz);
            push32(c, next);
            next = t;
            break;
        }
        case 4:
            next = e_rd(c, in, ea, osz);
            break;
        case 6:
            pushv(c, e_rd(c, in, ea, osz), osz);
            break;
        default:
            return XI_INVALID;   /* far call/jmp */
        }
        break;

    /* ---- 0F ---- */
    case 0x106:    /* clts */
    case 0x108:    /* invd */
    case 0x109:    /* wbinvd */
        break;
    case 0x10D:    /* prefetch (3DNow!) */
    case 0x118: case 0x119: case 0x11A: case 0x11B: case 0x11C: case 0x11D: case 0x11E: case 0x11F:
        break;     /* prefetch, hint nops */
    case 0x131: {  /* rdtsc */
        uint64_t t = xrt_rdtsc(c);
        c->r[XR_EAX] = (uint32_t)t;
        c->r[XR_EDX] = (uint32_t)(t >> 32);
        break;
    }
    case 0x140: case 0x141: case 0x142: case 0x143: case 0x144: case 0x145: case 0x146: case 0x147:
    case 0x148: case 0x149: case 0x14A: case 0x14B: case 0x14C: case 0x14D: case 0x14E: case 0x14F: {
        uint32_t v = e_rd(c, in, ea, osz);
        if (xi_cond(c, op & 15)) setr(c, in->reg, osz, v);
        break;
    }
    case 0x190: case 0x191: case 0x192: case 0x193: case 0x194: case 0x195: case 0x196: case 0x197:
    case 0x198: case 0x199: case 0x19A: case 0x19B: case 0x19C: case 0x19D: case 0x19E: case 0x19F:
        e_wr(c, in, ea, 1, xi_cond(c, op & 15));
        break;
    case 0x1A0: case 0x1A8:
        pushv(c, op == 0x1A0 ? 0x3B : 0, osz);   /* push fs/gs */
        break;
    case 0x1A1: case 0x1A9:
        popv(c, osz);
        break;
    case 0x1A2:
        xrt_cpuid(c);
        break;
    case 0x1A3: bit_op(c, in, ea, osz, getr(c, in->reg, osz), true, 0); break;
    case 0x1AB: bit_op(c, in, ea, osz, getr(c, in->reg, osz), true, 1); break;
    case 0x1B3: bit_op(c, in, ea, osz, getr(c, in->reg, osz), true, 2); break;
    case 0x1BB: bit_op(c, in, ea, osz, getr(c, in->reg, osz), true, 3); break;
    case 0x1BA:
        if (in->reg < 4) return XI_INVALID;
        bit_op(c, in, ea, osz, in->imm, false, in->reg - 4);
        break;
    case 0x1A4: case 0x1A5: case 0x1AC: case 0x1AD: {   /* shld, shrd */
        unsigned n = ((op & 1) ? c->r[XR_ECX] : in->imm) & 31;
        if (!n) break;
        e_wr(c, in, ea, osz, shld(c, e_rd(c, in, ea, osz), getr(c, in->reg, osz), n, osz, op >= 0x1AC));
        break;
    }
    case 0x1B0: case 0x1B1: {   /* cmpxchg */
        int sz = op & 1 ? osz : 1;
        uint32_t acc = getr(c, XR_EAX, sz), src = getr(c, in->reg, sz), cur;
        if (in->mod == 3) {
            cur = getr(c, in->rm, sz);
            alu(c, 7, acc, cur, sz);
            if (c->nzcv & XF_Z) setr(c, in->rm, sz, src);
            else setr(c, XR_EAX, sz, cur);
        } else {
            cur = acc;
            bool ok = cas_mem(ea, sz, &cur, src);
            alu(c, 7, acc, cur, sz);
            if (!ok) setr(c, XR_EAX, sz, cur);
        }
        break;
    }
    case 0x1B6: setr(c, in->reg, osz, e_rd(c, in, ea, 1)); break;
    case 0x1B7: setr(c, in->reg, osz, e_rd(c, in, ea, 2)); break;
    case 0x1BE: setr(c, in->reg, osz, (uint32_t)(int8_t)e_rd(c, in, ea, 1)); break;
    case 0x1BF: setr(c, in->reg, osz, (uint32_t)(int16_t)e_rd(c, in, ea, 2)); break;
    case 0x1BC: case 0x1BD: {   /* bsf, bsr */
        uint32_t v = e_rd(c, in, ea, osz);
        if (!v) { c->nzcv |= XF_Z; break; }
        c->nzcv &= ~XF_Z;
        setr(c, in->reg, osz, op == 0x1BC ? (uint32_t)__builtin_ctz(v) : 31u - (uint32_t)__builtin_clz(v));
        break;
    }
    case 0x1C0: case 0x1C1: {   /* xadd */
        int sz = op & 1 ? osz : 1;
        uint32_t src = getr(c, in->reg, sz), old;
        if (in->mod == 3) {
            old = getr(c, in->rm, sz);
            uint32_t r = alu(c, 0, old, src, sz);
            setr(c, in->reg, sz, old);
            setr(c, in->rm, sz, r);
        } else {
            old = rdm(ea, sz);
            if (locked) {
                while (!cas_mem(ea, sz, &old, (old + src) & szmask(sz))) {}
                alu(c, 0, old, src, sz);
            } else {
                wrm(ea, sz, alu(c, 0, old, src, sz));
            }
            setr(c, in->reg, sz, old);
        }
        break;
    }
    case 0x1C7: {  /* cmpxchg8b */
        if (in->reg != 1 || in->mod == 3) return XI_INVALID;
        uint64_t cmp = (uint64_t)c->r[XR_EDX] << 32 | c->r[XR_EAX];
        uint64_t nv = (uint64_t)c->r[XR_ECX] << 32 | c->r[XR_EBX], cur;
        bool ok;
        if (!(ea & 7)) {
            cur = cmp;
            ok = __atomic_compare_exchange_n((uint64_t *)(uintptr_t)ea, &cur, nv, false, __ATOMIC_SEQ_CST,
                                             __ATOMIC_SEQ_CST);
        } else {
            cur = rd64(ea);
            ok = cur == cmp;
            if (ok) wr64(ea, nv);
        }
        if (ok) c->nzcv |= XF_Z;
        else {
            c->nzcv &= ~XF_Z;
            c->r[XR_EAX] = (uint32_t)cur;
            c->r[XR_EDX] = (uint32_t)(cur >> 32);
        }
        break;
    }
    case 0x1C8: case 0x1C9: case 0x1CA: case 0x1CB: case 0x1CC: case 0x1CD: case 0x1CE: case 0x1CF:
        c->r[op & 7] = __builtin_bswap32(c->r[op & 7]);
        break;

    default:
        if (op >= 0x110) {
            int r = xi_sse(c, in, ea);
            if (r != XI_OK) return r;
            break;
        }
        return XI_INVALID;
    }
    c->eip = next;
    return XI_OK;
}
