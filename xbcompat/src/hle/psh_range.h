/*
 * Interval arithmetic for the register combiner translation: the range of
 * values each register can hold, so psh.c only emits the clamps that can
 * actually change a value.  Unknown is (-inf, inf); anything involving an
 * infinite bound stays unknown.
 */
#include <math.h>

typedef struct { double lo, hi; } psh_range;
static psh_range range_unknown(void) { return (psh_range){ -INFINITY, INFINITY }; }
static psh_range range_union(psh_range a, psh_range b)
{
    return (psh_range){ fmin(a.lo, b.lo), fmax(a.hi, b.hi) };
}
static psh_range range_add(psh_range a, psh_range b)
{
    return (psh_range){ a.lo + b.lo, a.hi + b.hi };
}
static psh_range range_scale(psh_range a, double scale)
{
    if (scale >= 0.0) return (psh_range){ a.lo * scale, a.hi * scale };
    return (psh_range){ a.hi * scale, a.lo * scale };
}
static psh_range range_mul(psh_range a, psh_range b)
{
    if (!isfinite(a.lo) || !isfinite(a.hi) || !isfinite(b.lo) || !isfinite(b.hi))
        return range_unknown();
    double v[4] = { a.lo * b.lo, a.lo * b.hi, a.hi * b.lo, a.hi * b.hi };
    psh_range r = { v[0], v[0] };
    for (int i = 1; i < 4; i++) r.lo = fmin(v[i], r.lo), r.hi = fmax(v[i], r.hi);
    return r;
}
static psh_range range_clip(psh_range a, double lo, double hi)
{
    return (psh_range){ fmin(fmax(a.lo, lo), hi), fmin(fmax(a.hi, lo), hi) };
}
static psh_range range_input_map(psh_range a, int mapping)
{
    psh_range positive = { fmax(a.lo, 0.0), fmax(a.hi, 0.0) };   /* PS_INPUTMAPPING_*: unsigned ones clamp at 0 */
    switch (mapping) {
    case 0x20: { psh_range c = range_clip(a, 0.0, 1.0); return (psh_range){ 1.0 - c.hi, 1.0 - c.lo }; }
    case 0x40: return range_add((psh_range){ 2.0 * positive.lo, 2.0 * positive.hi }, (psh_range){ -1.0, -1.0 });
    case 0x60: return range_add((psh_range){ -2.0 * positive.hi, -2.0 * positive.lo }, (psh_range){ 1.0, 1.0 });
    case 0x80: return range_add(positive, (psh_range){ -0.5, -0.5 });
    case 0xa0: return (psh_range){ 0.5 - positive.hi, 0.5 - positive.lo };
    case 0xc0: return a;
    case 0xe0: return range_scale(a, -1.0);
    default: return positive;
    }
}
static psh_range range_output_map(psh_range a, int mapping)
{
    switch (mapping) {   /* PS_COMBINEROUTPUT_*, before the clamp to -1..1 */
    case 0x08: return range_add(a, (psh_range){ -0.5, -0.5 });
    case 0x10: return range_scale(a, 2.0);
    case 0x18: return range_scale(range_add(a, (psh_range){ -0.5, -0.5 }), 2.0);
    case 0x20: return range_scale(a, 4.0);
    case 0x30: return range_scale(a, 0.5);
    }
    return a;
}
/* x clamped to lo..hi, leaving out the bounds its range already
   respects (the clamp then costs nothing when it cannot matter). */
static void range_clip_expr(char *out, size_t n, const char *x, psh_range a, double lo, double hi)
{
    int need_lo = a.lo < lo, need_hi = a.hi > hi;
    if (!need_lo && !need_hi) snprintf(out, n, "%s", x);
    else if (!need_lo) snprintf(out, n, "min(%s, %.1f)", x, hi);
    else if (!need_hi) snprintf(out, n, "max(%s, %.1f)", x, lo);
    else snprintf(out, n, "clamp(%s, %.1f, %.1f)", x, lo, hi);
}
