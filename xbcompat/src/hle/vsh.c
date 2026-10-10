/*
 * NV2A vertex program microcode to GLSL 1.20.
 *
 * The input is what the Xbox assembler produces for D3DDevice_CreateVertexShader
 * (minus the one-dword header): 16-byte instructions, each with a vector
 * (MAC) and a scalar (ILU) opcode that run together.  See nv2a_shaders.h for
 * the conventions of the generated shader.
 *
 * Instruction layout (dword:bits, low bit first):
 *   1:25-27 ILU opcode      1:21-24 MAC opcode     1:13-20 constant index
 *   1:9-12  vertex index    1:8 A negate            1:0-7  A swizzle
 *   2:28-31 A register      2:26-27 A source kind   2:25   B negate
 *   2:17-24 B swizzle       2:13-16 B register      2:11-12 B source kind
 *   2:10    C negate        2:2-9   C swizzle       2:0-1 C register (high)
 *   3:30-31 C register low  3:28-29 C source kind
 *   3:24-27 MAC temp mask   3:20-23 temp register   3:16-19 ILU temp mask
 *   3:12-15 output mask     3:11 output kind (1 = o*, 0 = constant)
 *   3:3-10  output address  3:2 output from ILU     3:1 relative addressing
 *   3:0     last instruction
 * Source kinds: 1 = temporary R, 2 = vertex attribute v, 3 = constant c.
 * Swizzle bytes hold x in bits 6-7 down to w in bits 0-1 (0 = x .. 3 = w).
 * The constant index is already in hardware numbering (title c0 is 96),
 * and so is a constant written as the output (state shaders).
 *
 * Rules of the hardware that the assembler's own verifier documents:
 *   - all constant reads of an instruction share the one index, and the
 *     relative-addressing bit adds A0 to that index for reads only; a
 *     constant written as the output is always addressed absolutely;
 *   - when both units run, the ILU's temp result goes to R1 whatever the
 *     temp register field says, and a MAC result aimed at R1 is dropped if
 *     the ILU also writes a temp ("ilu wins; mac writes nothing");
 *   - the ILU of an unpaired instruction uses the temp register field;
 *   - R12 is a mirror of oPos;
 *   - both units read their inputs before either one writes.
 * Scalar ILU ops (RCP, RCC, RSQ, EXP, LOG) take the x component of the
 * swizzled input.  Writes to the scalar outputs oFog and oPts take the most
 * significant masked component (an "oFog.x" write is by far the common case).
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv2a_shaders.h"

/* ---------------------------------------------------------------- strings */

typedef struct {
    char *s;
    size_t len, cap;
    bool oom;
} strbuf;

static void sb_put(strbuf *b, const char *fmt, ...)
{
    va_list ap;
    for (;;) {
        if (b->oom) return;
        va_start(ap, fmt);
        int n = vsnprintf(b->s + b->len, b->cap - b->len, fmt, ap);
        va_end(ap);
        if (n < 0) { b->oom = true; return; }
        if ((size_t)n < b->cap - b->len) { b->len += n; return; }
        size_t cap = b->cap ? b->cap * 2 : 4096;
        while (cap <= b->len + n) cap *= 2;
        char *s = realloc(b->s, cap);
        if (!s) { b->oom = true; return; }
        b->s = s;
        b->cap = cap;
    }
}

/* ------------------------------------------------------------- decoding */

enum { ILU_NOP, ILU_MOV, ILU_RCP, ILU_RCC, ILU_RSQ, ILU_EXP, ILU_LOG, ILU_LIT };
enum { MAC_NOP, MAC_MOV, MAC_MUL, MAC_ADD, MAC_MAD, MAC_DP3, MAC_DPH, MAC_DP4,
       MAC_DST, MAC_MIN, MAC_MAX, MAC_SLT, MAC_SGE, MAC_ARL };
enum { SRC_NONE, SRC_R, SRC_V, SRC_C };

static const char *const ilu_name[8] = { "nop", "mov", "rcp", "rcc", "rsq", "exp", "log", "lit" };
static const char *const mac_name[16] = { "nop", "mov", "mul", "add", "mad", "dp3", "dph", "dp4",
                                          "dst", "min", "max", "slt", "sge", "arl", "?14", "?15" };
/* Which of the A, B, C inputs each MAC opcode reads. */
static const unsigned char mac_inputs[16] = {
    0, 1, 3, 5, 7, 3, 3, 3, 3, 3, 3, 3, 3, 1, 0, 0  /* bit 0 = A, 1 = B, 2 = C */
};
/* Output register names by address (output kind 1). */
static const char *const out_name[16] = {
    "oPos", NULL, NULL, "oD0", "oD1", "oFog", "oPts", "oB0", "oB1",
    "oT0", "oT1", "oT2", "oT3", NULL, NULL, NULL
};
enum { OUT_POS = 0, OUT_D0 = 3, OUT_D1 = 4, OUT_FOG = 5, OUT_PTS = 6, OUT_B0 = 7, OUT_B1 = 8, OUT_T0 = 9 };

/* Write masks: bit 3 = x, 2 = y, 1 = z, 0 = w. */
static const char *const mask_str[16] = {
    "", "w", "z", "zw", "y", "yw", "yz", "yzw", "x", "xw", "xz", "xzw", "xy", "xyw", "xyz", "xyzw"
};

typedef struct {
    unsigned kind, reg, neg, swz;
} operand;

typedef struct {
    unsigned ilu, mac, cidx, vidx;
    operand a, b, c;
    unsigned mac_mask, out_r, ilu_mask, o_mask, o_is_out, o_addr, o_from_ilu, a0x, final;
} vinstr;

#define FIELD(w, dw, lo, n) (((w)[dw] >> (lo)) & ((1u << (n)) - 1))

static void decode(const uint32_t *w, vinstr *in)
{
    in->ilu = FIELD(w, 1, 25, 3);
    in->mac = FIELD(w, 1, 21, 4);
    in->cidx = FIELD(w, 1, 13, 8);
    in->vidx = FIELD(w, 1, 9, 4);
    in->a.neg = FIELD(w, 1, 8, 1);
    in->a.swz = FIELD(w, 1, 0, 8);
    in->a.reg = FIELD(w, 2, 28, 4);
    in->a.kind = FIELD(w, 2, 26, 2);
    in->b.neg = FIELD(w, 2, 25, 1);
    in->b.swz = FIELD(w, 2, 17, 8);
    in->b.reg = FIELD(w, 2, 13, 4);
    in->b.kind = FIELD(w, 2, 11, 2);
    in->c.neg = FIELD(w, 2, 10, 1);
    in->c.swz = FIELD(w, 2, 2, 8);
    in->c.reg = (FIELD(w, 2, 0, 2) << 2) | FIELD(w, 3, 30, 2);
    in->c.kind = FIELD(w, 3, 28, 2);
    in->mac_mask = FIELD(w, 3, 24, 4);
    in->out_r = FIELD(w, 3, 20, 4);
    in->ilu_mask = FIELD(w, 3, 16, 4);
    in->o_mask = FIELD(w, 3, 12, 4);
    in->o_is_out = FIELD(w, 3, 11, 1);
    in->o_addr = FIELD(w, 3, 3, 8);
    in->o_from_ilu = FIELD(w, 3, 2, 1);
    in->a0x = FIELD(w, 3, 1, 1);
    in->final = FIELD(w, 3, 0, 1);
}

/* A paired instruction (both units busy) can only put the ILU result in R1. */
static bool paired(const vinstr *in)
{
    return in->mac != MAC_NOP && in->ilu != ILU_NOP;
}

static bool mac_writes_temp(const vinstr *in)
{
    if (in->mac == MAC_NOP || in->mac == MAC_ARL || in->mac_mask == 0) return false;
    /* In a pair R1 belongs to the ILU: a MAC result aimed at it is dropped
       when the ILU writes a temp. */
    if (paired(in) && in->out_r == 1 && in->ilu_mask != 0) return false;
    return true;
}

static unsigned ilu_temp_reg(const vinstr *in)
{
    return paired(in) ? 1 : in->out_r;
}

/* ---------------------------------------------------------- emitting */

typedef struct {
    strbuf b;
    bool cshadow;        /* the program writes constants: use a local copy */
    bool bad;            /* something the translator cannot express */
    unsigned cur;        /* the instruction being emitted, for vsh_error */
} emitter;

/* Why the last vsh_translate returned NULL. */
char vsh_error[96];

#define BAD(e, ...) do { if (!(e)->bad) { int n_ = snprintf(vsh_error, sizeof vsh_error, "instruction %u: ", (e)->cur); \
                          snprintf(vsh_error + n_, sizeof vsh_error - n_, __VA_ARGS__); } (e)->bad = true; } while (0)

static const char *temp_name(unsigned r, char *buf)
{
    if (r == 12) return "oPos";   /* R12 mirrors oPos */
    sprintf(buf, "R%u", r);
    return buf;
}

static void emit_operand(emitter *e, const vinstr *in, const operand *o, int scalar)
{
    char rbuf[8];
    const char *name;
    char cbuf[48];
    if (o->neg) sb_put(&e->b, "-");
    switch (o->kind) {
    case SRC_R:
        if (o->reg > 12) { BAD(e, "reads R%u", o->reg); return; }
        name = temp_name(o->reg, rbuf);
        break;
    case SRC_V:
        sprintf(rbuf, "v%u", in->vidx);
        name = rbuf;
        break;
    case SRC_C:
        if (in->a0x) {
            sprintf(cbuf, "%s[ridx(A0 + %u)]", e->cshadow ? "cc" : "c", in->cidx);
        } else {
            if (in->cidx > 191) { BAD(e, "reads c%u", in->cidx); return; }
            sprintf(cbuf, "%s[%u]", e->cshadow ? "cc" : "c", in->cidx);
        }
        name = cbuf;
        break;
    default:
        BAD(e, "operand kind %u", o->kind);
        return;
    }
    sb_put(&e->b, "%s", name);
    static const char comp[4] = { 'x', 'y', 'z', 'w' };
    unsigned sx = (o->swz >> 6) & 3, sy = (o->swz >> 4) & 3, sz = (o->swz >> 2) & 3, sw = o->swz & 3;
    if (scalar) {
        sb_put(&e->b, ".%c", comp[sx]);
    } else if (sx != 0 || sy != 1 || sz != 2 || sw != 3) {
        sb_put(&e->b, ".%c%c%c%c", comp[sx], comp[sy], comp[sz], comp[sw]);
    }
}

/* The MAC result as an expression of its inputs. */
static void emit_mac_expr(emitter *e, const vinstr *in)
{
#define A() emit_operand(e, in, &in->a, 0)
#define B() emit_operand(e, in, &in->b, 0)
#define C() emit_operand(e, in, &in->c, 0)
    strbuf *b = &e->b;
    switch (in->mac) {
    case MAC_MOV: A(); break;
    case MAC_MUL: sb_put(b, "_mul("); A(); sb_put(b, ", "); B(); sb_put(b, ")"); break;
    case MAC_ADD: A(); sb_put(b, " + "); C(); break;
    case MAC_MAD: sb_put(b, "_mul("); A(); sb_put(b, ", "); B(); sb_put(b, ") + "); C(); break;
    case MAC_DP3: sb_put(b, "vec4(dot(("); A(); sb_put(b, ").xyz, ("); B(); sb_put(b, ").xyz))"); break;
    case MAC_DPH: sb_put(b, "vec4(dot(vec4(("); A(); sb_put(b, ").xyz, 1.0), "); B(); sb_put(b, "))"); break;
    case MAC_DP4: sb_put(b, "vec4(dot("); A(); sb_put(b, ", "); B(); sb_put(b, "))"); break;
    case MAC_DST: sb_put(b, "_dst("); A(); sb_put(b, ", "); B(); sb_put(b, ")"); break;
    case MAC_MIN: sb_put(b, "min("); A(); sb_put(b, ", "); B(); sb_put(b, ")"); break;
    case MAC_MAX: sb_put(b, "max("); A(); sb_put(b, ", "); B(); sb_put(b, ")"); break;
    case MAC_SLT: sb_put(b, "vec4(lessThan("); A(); sb_put(b, ", "); B(); sb_put(b, "))"); break;
    case MAC_SGE: sb_put(b, "vec4(greaterThanEqual("); A(); sb_put(b, ", "); B(); sb_put(b, "))"); break;
    default: BAD(e, "MAC opcode %u", in->mac); break;
    }
#undef A
#undef B
#undef C
}

static void emit_ilu_expr(emitter *e, const vinstr *in)
{
    strbuf *b = &e->b;
    switch (in->ilu) {
    case ILU_MOV: emit_operand(e, in, &in->c, 0); break;
    case ILU_RCP: sb_put(b, "_rcp("); emit_operand(e, in, &in->c, 1); sb_put(b, ")"); break;
    case ILU_RCC: sb_put(b, "_rcc("); emit_operand(e, in, &in->c, 1); sb_put(b, ")"); break;
    case ILU_RSQ: sb_put(b, "_rsq("); emit_operand(e, in, &in->c, 1); sb_put(b, ")"); break;
    case ILU_EXP: sb_put(b, "_exp("); emit_operand(e, in, &in->c, 1); sb_put(b, ")"); break;
    case ILU_LOG: sb_put(b, "_log("); emit_operand(e, in, &in->c, 1); sb_put(b, ")"); break;
    case ILU_LIT: sb_put(b, "_lit("); emit_operand(e, in, &in->c, 0); sb_put(b, ")"); break;
    default: BAD(e, "ILU opcode %u", in->ilu); break;
    }
}

/* dest.mask = src.mask */
static void emit_masked(emitter *e, const char *dest, unsigned mask, const char *src)
{
    if (mask == 15)
        sb_put(&e->b, "  %s = %s;\n", dest, src);
    else
        sb_put(&e->b, "  %s.%s = %s.%s;\n", dest, mask_str[mask], src, mask_str[mask]);
}

/* The output write of an instruction: an o* register or a constant. */
static void emit_output(emitter *e, const vinstr *in, const char *src)
{
    char dest[48];
    if (in->o_is_out) {
        unsigned idx = in->o_addr & 15;
        const char *name = out_name[idx];
        if (!name) { BAD(e, "writes output %u", idx); return; }
        if (idx == OUT_FOG || idx == OUT_PTS) {
            /* Scalar outputs take the most significant masked component. */
            static const char comp[4] = { 'x', 'y', 'z', 'w' };
            int first = 0;
            while (first < 4 && !(in->o_mask & (8 >> first))) first++;
            sb_put(&e->b, "  %s.x = %s.%c;\n", name, src, comp[first]);
            return;
        }
        emit_masked(e, name, in->o_mask, src);
    } else {
        /* A constant destination is always absolute; A0 only applies to
           reads. */
        if (in->o_addr > 191) { BAD(e, "writes c%u", in->o_addr); return; }
        sprintf(dest, "cc[%u]", in->o_addr);
        emit_masked(e, dest, in->o_mask, src);
    }
}

/* One line of disassembly for the comment above each instruction. */
static void emit_disasm(emitter *e, const vinstr *in)
{
    char rbuf[8];
    if (in->mac != MAC_NOP) {
        sb_put(&e->b, "%s", mac_name[in->mac]);
        if (in->mac == MAC_ARL) {
            sb_put(&e->b, " A0.x");
        } else {
            if (in->mac_mask)
                sb_put(&e->b, " %s.%s", temp_name(in->out_r, rbuf), mask_str[in->mac_mask]);
            if (in->o_mask && !in->o_from_ilu) {
                if (in->o_is_out)
                    sb_put(&e->b, " %s.%s", out_name[in->o_addr & 15] ? out_name[in->o_addr & 15] : "o?",
                           mask_str[in->o_mask]);
                else
                    sb_put(&e->b, " c[%u].%s", in->o_addr, mask_str[in->o_mask]);
            }
            if (!in->mac_mask && !(in->o_mask && !in->o_from_ilu)) sb_put(&e->b, " (none)");
        }
        unsigned uses = mac_inputs[in->mac];
        if (uses & 1) { sb_put(&e->b, ", "); emit_operand(e, in, &in->a, 0); }
        if (uses & 2) { sb_put(&e->b, ", "); emit_operand(e, in, &in->b, 0); }
        if (uses & 4) { sb_put(&e->b, ", "); emit_operand(e, in, &in->c, 0); }
    }
    if (in->ilu != ILU_NOP) {
        if (in->mac != MAC_NOP) sb_put(&e->b, " + ");
        sb_put(&e->b, "%s", ilu_name[in->ilu]);
        if (in->ilu_mask)
            sb_put(&e->b, " %s.%s", temp_name(ilu_temp_reg(in), rbuf), mask_str[in->ilu_mask]);
        if (in->o_mask && in->o_from_ilu) {
            if (in->o_is_out)
                sb_put(&e->b, " %s.%s", out_name[in->o_addr & 15] ? out_name[in->o_addr & 15] : "o?",
                       mask_str[in->o_mask]);
            else
                sb_put(&e->b, " c[%u].%s", in->o_addr, mask_str[in->o_mask]);
        }
        if (!in->ilu_mask && !(in->o_mask && in->o_from_ilu)) sb_put(&e->b, " (none)");
        sb_put(&e->b, ", ");
        emit_operand(e, in, &in->c, in->ilu != ILU_MOV && in->ilu != ILU_LIT);
    }
}

static const char *const prologue =
    "#version 120\n"
    "/* NV2A vertex program, translated by xbcompat */\n"
    "uniform vec4 c[192];\n"
    "uniform float flip_y;\n"
    "uniform vec4 vp_scale, vp_offset, wdepth;\n";

static const char *const helpers =
    "\n"
    "int ridx(int i) { return int(clamp(float(i), 0.0, 191.0)); }\n"
    "/* The hardware multiplies anything by zero to zero, even inf and NaN. */\n"
    "vec4 _mul(vec4 a, vec4 b) {\n"
    "  vec4 r = a * b;\n"
    "  if (a.x == 0.0 || b.x == 0.0) r.x = 0.0;\n"
    "  if (a.y == 0.0 || b.y == 0.0) r.y = 0.0;\n"
    "  if (a.z == 0.0 || b.z == 0.0) r.z = 0.0;\n"
    "  if (a.w == 0.0 || b.w == 0.0) r.w = 0.0;\n"
    "  return r;\n"
    "}\n"
    "vec4 _dst(vec4 a, vec4 b) { return vec4(1.0, a.y * b.y, a.z, b.w); }\n"
    "vec4 _rcp(float s) { return vec4(1.0 / s); }\n"
    "/* Reciprocal clamped to [2^-64, 2^64] with the sign of the input. */\n"
    "vec4 _rcc(float s) {\n"
    "  float t = 1.0 / s;\n"
    "  if (t > 0.0 || (t == 0.0 && s > 0.0))\n"
    "    t = clamp(t, 5.42101086e-20, 1.84467441e19);\n"
    "  else\n"
    "    t = clamp(t, -1.84467441e19, -5.42101086e-20);\n"
    "  return vec4(t);\n"
    "}\n"
    "vec4 _rsq(float s) { return vec4(1.0 / sqrt(abs(s))); }\n"
    "vec4 _exp(float s) {\n"
    "  float f = floor(s);\n"
    "  return vec4(exp2(f), s - f, exp2(s), 1.0);\n"
    "}\n"
    "vec4 _log(float s) {\n"
    "  float a = abs(s);\n"
    "  if (a == 0.0) return vec4(-3.4028235e38, 1.0, -3.4028235e38, 1.0);\n"
    "  float l = log2(a);\n"
    "  float f = floor(l);\n"
    "  return vec4(f, a / exp2(f), l, 1.0);\n"
    "}\n"
    "vec4 _lit(vec4 s) {\n"
    "  float x = max(s.x, 0.0);\n"
    "  float y = max(s.y, 0.0);\n"
    "  float w = clamp(s.w, -127.9961, 127.9961);\n"
    "  float z = 0.0;\n"
    "  if (x > 0.0) z = (y > 0.0) ? exp2(w * log2(y)) : ((w == 0.0) ? 1.0 : 0.0);\n"
    "  return vec4(1.0, x, z, 1.0);\n"
    "}\n";

char *vsh_translate(const uint32_t *code, unsigned count)
{
    vsh_error[0] = 0;
    if (!code || !count) return NULL;

    /* First pass: decode, find the end, and see what the program touches. */
    vinstr *ins = calloc(count, sizeof(*ins));
    if (!ins) return NULL;
    unsigned n = 0;
    unsigned attrs = 0, temps = 0, outs = 0;
    bool cwrites = false, uses_a0 = false;
    for (unsigned i = 0; i < count; i++) {
        vinstr *in = &ins[i];
        decode(code + i * 4, in);
        n = i + 1;
        if (in->mac == MAC_NOP && in->ilu == ILU_NOP) {
            if (in->final) break;
            continue;
        }
        unsigned uses = (in->mac != MAC_NOP ? mac_inputs[in->mac] : 0) | (in->ilu != ILU_NOP ? 4 : 0);
        const operand *ops[3] = { &in->a, &in->b, &in->c };
        for (int k = 0; k < 3; k++) {
            if (!(uses & (1u << k))) continue;
            if (ops[k]->kind == SRC_V) attrs |= 1u << in->vidx;
            else if (ops[k]->kind == SRC_R && ops[k]->reg < 12) temps |= 1u << ops[k]->reg;
            else if (ops[k]->kind == SRC_C && in->a0x) uses_a0 = true;
        }
        if (mac_writes_temp(in) && in->out_r < 12) temps |= 1u << in->out_r;
        if (in->ilu != ILU_NOP && in->ilu_mask && ilu_temp_reg(in) < 12) temps |= 1u << ilu_temp_reg(in);
        if (in->mac == MAC_ARL) uses_a0 = true;
        if (in->o_mask && (in->o_from_ilu ? in->ilu != ILU_NOP : (in->mac != MAC_NOP && in->mac != MAC_ARL))) {
            if (in->o_is_out) outs |= 1u << (in->o_addr & 15);
            else cwrites = true;
        }
        if (in->final) break;
    }

    emitter e = { .b = { 0 }, .cshadow = cwrites, .bad = false };
    sb_put(&e.b, "%s", prologue);
    for (unsigned i = 0; i < 16; i++)
        if (attrs & (1u << i)) sb_put(&e.b, "attribute vec4 v%u;\n", i);
    sb_put(&e.b, "%s", helpers);

    sb_put(&e.b, "\nvoid main() {\n");
    sb_put(&e.b, "  vec4 oPos = vec4(0.0, 0.0, 0.0, 1.0);\n");
    static const char *const outs_decl[] = { "oD0", "oD1", "oFog", "oPts", "oB0", "oB1", "oT0", "oT1", "oT2", "oT3" };
    for (unsigned i = 0; i < sizeof(outs_decl) / sizeof(outs_decl[0]); i++)
        sb_put(&e.b, "  vec4 %s = vec4(0.0, 0.0, 0.0, 1.0);\n", outs_decl[i]);
    for (unsigned i = 0; i < 12; i++)
        if (temps & (1u << i)) sb_put(&e.b, "  vec4 R%u = vec4(0.0);\n", i);
    if (uses_a0) sb_put(&e.b, "  int A0 = 0;\n");
    if (cwrites) {
        sb_put(&e.b, "  vec4 cc[192];\n");
        sb_put(&e.b, "  for (int i = 0; i < 192; i++) cc[i] = c[i];\n");
    }
    sb_put(&e.b, "  vec4 t = vec4(0.0);\n  vec4 u = vec4(0.0);\n");

    /* Second pass: the instructions. */
    for (unsigned i = 0; i < n; i++) {
        const vinstr *in = &ins[i];
        const uint32_t *w = code + i * 4;
        if (in->mac == MAC_NOP && in->ilu == ILU_NOP) continue;
        e.cur = i;
        sb_put(&e.b, "  /* %u: %08x %08x %08x %08x  ", i, w[0], w[1], w[2], w[3]);
        emit_disasm(&e, in);
        sb_put(&e.b, " */\n");

        /* Both units read their inputs before either writes. */
        bool have_t = false, have_u = false;
        if (in->mac == MAC_ARL) {
            sb_put(&e.b, "  A0 = int(floor(");
            emit_operand(&e, in, &in->a, 1);
            sb_put(&e.b, " + 0.001));\n");
        } else if (in->mac != MAC_NOP) {
            sb_put(&e.b, "  t = ");
            emit_mac_expr(&e, in);
            sb_put(&e.b, ";\n");
            have_t = true;
        }
        if (in->ilu != ILU_NOP) {
            sb_put(&e.b, "  u = ");
            emit_ilu_expr(&e, in);
            sb_put(&e.b, ";\n");
            have_u = true;
        }
        char rbuf[8];
        if (have_t && mac_writes_temp(in)) {
            if (in->out_r > 12) { BAD(&e, "MAC writes R%u", in->out_r); break; }
            emit_masked(&e, temp_name(in->out_r, rbuf), in->mac_mask, "t");
        }
        if (have_u && in->ilu_mask) {
            unsigned r = ilu_temp_reg(in);
            if (r > 12) { BAD(&e, "ILU writes R%u", r); break; }
            emit_masked(&e, temp_name(r, rbuf), in->ilu_mask, "u");
        }
        if (in->o_mask) {
            if (in->o_from_ilu && have_u) emit_output(&e, in, "u");
            else if (!in->o_from_ilu && have_t) emit_output(&e, in, "t");
        }
        if (e.bad) break;
    }
    free(ins);

    /* Back from screen space to clip space, then the varyings.  oPos.xyz
       is already divided by w, so the clip w only scales everything back:
       keep it away from zero and infinity (as the hardware's RCC did for the
       divide) so a program that leaves w at 0 does not collapse its vertices
       into (0, 0, 0, 0). */
    sb_put(&e.b,
           "\n"
           "  vec3 vs_n = oPos.xyz - vp_offset.xyz;\n"
           "  vs_n.x = (vp_scale.x != 0.0) ? vs_n.x / vp_scale.x : 0.0;\n"
           "  vs_n.y = (vp_scale.y != 0.0) ? vs_n.y / vp_scale.y : 0.0;\n"
           "  vs_n.z = (vp_scale.z != 0.0) ? vs_n.z / vp_scale.z : 0.0;\n"
           "  float vs_w = (oPos.w >= 0.0) ? clamp(oPos.w, 5.42101086e-20, 1.84467441e19)\n"
           "                               : clamp(oPos.w, -1.84467441e19, -5.42101086e-20);\n"
           "  if (wdepth.x != 0.0) vs_n.z = wdepth.y + wdepth.z / vs_w;\n"
           "  gl_Position = vec4(vs_n.x, vs_n.y * flip_y, 2.0 * vs_n.z - 1.0, 1.0) * vs_w;\n"
           "  gl_FrontColor = clamp(oD0, 0.0, 1.0);\n"
           "  gl_FrontSecondaryColor = clamp(oD1, 0.0, 1.0);\n");
    sb_put(&e.b, "  gl_BackColor = clamp(%s, 0.0, 1.0);\n", outs & (1u << OUT_B0) ? "oB0" : "oD0");
    sb_put(&e.b, "  gl_BackSecondaryColor = clamp(%s, 0.0, 1.0);\n", outs & (1u << OUT_B1) ? "oB1" : "oD1");
    sb_put(&e.b,
           "  gl_TexCoord[0] = oT0;\n"
           "  gl_TexCoord[1] = oT1;\n"
           "  gl_TexCoord[2] = oT2;\n"
           "  gl_TexCoord[3] = oT3;\n"
           "  gl_FogFragCoord = oFog.x;\n"
           "  gl_PointSize = oPts.x;\n"
           "}\n");

    if (e.b.oom) snprintf(vsh_error, sizeof vsh_error, "out of memory");
    if (e.bad || e.b.oom) {
        free(e.b.s);
        return NULL;
    }

    /* Debugging aid: XBCOMPAT_VSH_DUMP=dir writes every program translated
       as dir/vsh-N.xvu (the CreateVertexShader blob, loadable by the tests)
       and dir/vsh-N.glsl. */
    const char *dump = getenv("XBCOMPAT_VSH_DUMP");
    if (dump && *dump) {
        static unsigned serial;
        char path[1024];
        snprintf(path, sizeof path, "%s/vsh-%u.xvu", dump, serial);
        FILE *f = fopen(path, "wb");
        if (f) {
            unsigned char hdr[4] = { 0x78, 0x20, (unsigned char)n, (unsigned char)(n >> 8) };
            fwrite(hdr, 1, 4, f);
            fwrite(code, 16, n, f);
            fclose(f);
        }
        snprintf(path, sizeof path, "%s/vsh-%u.glsl", dump, serial);
        f = fopen(path, "w");
        if (f) { fputs(e.b.s, f); fclose(f); }
        serial++;
    }
    return e.b.s;
}
