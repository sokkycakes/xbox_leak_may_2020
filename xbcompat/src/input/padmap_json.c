/*
 * padmap profiles as JSON: a small reader (objects, arrays, strings,
 * numbers, true/false/null) and the writer.  The format is described in
 * docs/project-notes/xbcompat/controller-config-strategy.md, section 4.4:
 *
 *   { "name": "Southpaw",
 *     "presets": [ { "name": "Default",
 *       "sources": { "left_stick": { "mode": "joystick", "output": "right_stick", "deadzone": 0.1 },
 *                    "buttons": { "mode": "buttons", "a": "b",
 *                                 "x": { "press": "x", "long_press": "back", "turbo_hz": 10 } },
 *                    "right_trigger": { "mode": "button", "button": "a", "threshold": 0.5 } },
 *       "mode_shifts": [ { "hold": "lb", "sources": { ... } } ] } ] }
 *
 * A file with "sources" at the top and no "presets" is one preset.
 */
#include "padmap.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- reader ------------------------------------------------------------- */

enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };
typedef struct jv jv;
struct jv {
    int type;
    double num;
    char *str;              /* J_STR value */
    char *key;              /* the member's name inside an object */
    jv *child, *next;
};

typedef struct {
    const char *s;
    const char *err;
    int line;
} jp;

static void jfree(jv *v)
{
    while (v) {
        jv *n = v->next;
        jfree(v->child);
        free(v->str);
        free(v->key);
        free(v);
        v = n;
    }
}

static void skip(jp *p)
{
    for (;;) {
        while (*p->s && isspace((unsigned char)*p->s)) {
            if (*p->s == '\n') p->line++;
            p->s++;
        }
        if (p->s[0] == '/' && p->s[1] == '/') {        /* comments, for hand-written files */
            while (*p->s && *p->s != '\n') p->s++;
            continue;
        }
        return;
    }
}

static char *jstring(jp *p)
{
    if (*p->s != '"') { p->err = "expected a string"; return NULL; }
    p->s++;
    size_t cap = 32, n = 0;
    char *b = (char *)malloc(cap);
    while (b && *p->s && *p->s != '"') {
        char c = *p->s++;
        if (c == '\\') {
            c = *p->s++;
            switch (c) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u': {     /* \uXXXX: ASCII only, the rest becomes '?' */
                unsigned u = 0;
                for (int i = 0; i < 4 && isxdigit((unsigned char)*p->s); i++, p->s++)
                    u = u * 16 + (unsigned)(isdigit((unsigned char)*p->s) ? *p->s - '0' : (tolower((unsigned char)*p->s) - 'a' + 10));
                c = u < 128 ? (char)u : '?';
                break;
            }
            case 0: p->err = "unfinished string"; free(b); return NULL;
            default: break;
            }
        }
        if (n + 2 > cap) {
            char *nb = (char *)realloc(b, cap *= 2);
            if (!nb) { free(b); return NULL; }
            b = nb;
        }
        b[n++] = c;
    }
    if (!b) { p->err = "out of memory"; return NULL; }
    if (*p->s != '"') { p->err = "unfinished string"; free(b); return NULL; }
    p->s++;
    b[n] = 0;
    return b;
}

static jv *jvalue(jp *p, int depth)
{
    skip(p);
    if (depth > 32) { p->err = "nested too deep"; return NULL; }
    jv *v = (jv *)calloc(1, sizeof(*v));
    if (!v) { p->err = "out of memory"; return NULL; }
    char c = *p->s;
    if (c == '{' || c == '[') {
        bool obj = c == '{';
        v->type = obj ? J_OBJ : J_ARR;
        p->s++;
        jv **tail = &v->child;
        skip(p);
        if (*p->s == (obj ? '}' : ']')) { p->s++; return v; }
        for (;;) {
            char *key = NULL;
            if (obj) {
                skip(p);
                key = jstring(p);
                if (!key) { jfree(v); return NULL; }
                skip(p);
                if (*p->s != ':') { p->err = "expected ':'"; free(key); jfree(v); return NULL; }
                p->s++;
            }
            jv *m = jvalue(p, depth + 1);
            if (!m) { free(key); jfree(v); return NULL; }
            m->key = key;
            *tail = m;
            tail = &m->next;
            skip(p);
            if (*p->s == ',') { p->s++; skip(p); if (*p->s == (obj ? '}' : ']')) { p->s++; return v; } continue; }
            if (*p->s == (obj ? '}' : ']')) { p->s++; return v; }
            p->err = obj ? "expected ',' or '}'" : "expected ',' or ']'";
            jfree(v);
            return NULL;
        }
    }
    if (c == '"') {
        v->type = J_STR;
        v->str = jstring(p);
        if (!v->str) { jfree(v); return NULL; }
        return v;
    }
    if (!strncmp(p->s, "true", 4)) { v->type = J_BOOL; v->num = 1; p->s += 4; return v; }
    if (!strncmp(p->s, "false", 5)) { v->type = J_BOOL; p->s += 5; return v; }
    if (!strncmp(p->s, "null", 4)) { v->type = J_NULL; p->s += 4; return v; }
    char *end;
    v->num = strtod(p->s, &end);
    if (end == p->s) { p->err = "unexpected character"; jfree(v); return NULL; }
    v->type = J_NUM;
    p->s = end;
    return v;
}

static jv *jget(const jv *o, const char *key)
{
    if (!o || o->type != J_OBJ) return NULL;
    for (jv *m = o->child; m; m = m->next)
        if (m->key && !strcmp(m->key, key)) return m;
    return NULL;
}

/* ---- profile from the tree ------------------------------------------------ */

typedef struct {
    padmap_profile *p;
    char *err;
    size_t errsize;
    bool ok;
} pctx;

static void fail(pctx *c, const char *fmt, ...)
{
    if (c->ok && c->err && c->errsize) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(c->err, c->errsize, fmt, ap);
        va_end(ap);
    }
    c->ok = false;
}

static int button_index(const char *name)
{
    for (int b = 0; b < PM_NBTN; b++)
        if (!strcmp(name, padmap_button_name(b))) return b;
    uint16_t t;
    if (padmap_target_parse(NULL, name, &t) && PM_TKIND(t) == PM_T_BUTTON) return (int)PM_TCODE(t);
    return -1;
}

static void target(pctx *c, const jv *v, uint16_t *t, const char *where)
{
    if (!v || v->type == J_NULL) { *t = 0; return; }
    if (v->type != J_STR || !padmap_target_parse(c->p, v->str, t))
        fail(c, "%s: unknown binding '%s'", where, v->type == J_STR ? v->str : "(not a string)");
}

static void binding(pctx *c, const jv *v, pm_bind *b, const char *where)
{
    memset(b, 0, sizeof(*b));
    if (!v) return;
    b->set = true;
    if (v->type != J_OBJ) { target(c, v, &b->target[PM_ACT_PRESS], where); return; }
    target(c, jget(v, "press"), &b->target[PM_ACT_PRESS], where);
    target(c, jget(v, "long_press"), &b->target[PM_ACT_LONG], where);
    target(c, jget(v, "double_press"), &b->target[PM_ACT_DOUBLE], where);
    const jv *n;
    if ((n = jget(v, "long_ms")) && n->type == J_NUM) b->long_ms = (uint16_t)(n->num < 0 ? 0 : n->num > 5000 ? 5000 : n->num);
    if ((n = jget(v, "double_ms")) && n->type == J_NUM) b->double_ms = (uint16_t)(n->num < 0 ? 0 : n->num > 2000 ? 2000 : n->num);
    if ((n = jget(v, "turbo_hz")) && n->type == J_NUM) b->turbo_hz = (uint8_t)(n->num < 0 ? 0 : n->num > 30 ? 30 : n->num);
    if ((n = jget(v, "toggle")) && n->type == J_BOOL) b->toggle = n->num != 0;
}

static float fnum(const jv *o, const char *key, float def, float lo, float hi)
{
    const jv *n = jget(o, key);
    if (!n || n->type != J_NUM) return def;
    float f = (float)n->num;
    return f < lo ? lo : f > hi ? hi : f;
}

static bool fbool(const jv *o, const char *key, bool def)
{
    const jv *n = jget(o, key);
    return n && n->type == J_BOOL ? n->num != 0 : def;
}

static void source(pctx *c, const jv *v, pm_source *s, int src)
{
    const jv *mv = jget(v, "mode");
    int mode = -1;
    if (v && v->type == J_STR) mv = v;      /* "left_stick": "off" */
    if (mv && mv->type == J_STR)
        for (int m = 0; m < PM_NMODES; m++)
            if (!strcmp(mv->str, padmap_mode_name(m))) mode = m;
    if (mode < 0) { fail(c, "%s: unknown mode", padmap_source_name(src)); mode = PM_MODE_PASS; }
    uint8_t ok[PM_NMODES];
    int n = padmap_source_modes(src, ok, PM_NMODES);
    bool allowed = false;
    for (int i = 0; i < n; i++) allowed |= ok[i] == mode;
    if (!allowed) { fail(c, "%s: mode %s doesn't fit it", padmap_source_name(src), padmap_mode_name(mode)); mode = PM_MODE_PASS; }
    padmap_source_init(s, src, mode);
    if (v->type != J_OBJ) return;
    char where[64];
    snprintf(where, sizeof where, "%s", padmap_source_name(src));
    const jv *out = jget(v, "output");
    if (out && out->type == J_STR) {
        if (!strcmp(out->str, "left_stick") || !strcmp(out->str, "lt")) s->output = 0;
        else if (!strcmp(out->str, "right_stick") || !strcmp(out->str, "rt")) s->output = 1;
        else fail(c, "%s: unknown output '%s'", where, out->str);
    }
    s->deadzone = fnum(v, "deadzone", s->deadzone, 0, 0.95f);
    s->outer = fnum(v, "outer", s->outer, 0.05f, 1);
    s->curve = fnum(v, "curve", s->curve, 0.2f, 5);
    s->sensitivity = fnum(v, "sensitivity", s->sensitivity, 0.05f, 10);
    s->threshold = fnum(v, "threshold", s->threshold, 0.01f, 1);
    s->invert_x = fbool(v, "invert_x", false);
    s->invert_y = fbool(v, "invert_y", false);
    static const char *const dirs[4] = { "up", "down", "left", "right" };
    switch (mode) {
    case PM_MODE_DPAD:
        for (int d = 0; d < 4; d++) {
            const jv *b = jget(v, dirs[d]);
            if (b) binding(c, b, &s->bind[d], where);
        }
        break;
    case PM_MODE_BUTTONS:
        for (const jv *m = v->child; m; m = m->next) {
            if (!strcmp(m->key, "mode")) continue;
            int b = button_index(m->key);
            if (b < 0 || b == PM_GUIDE || (b >= PM_DPAD_UP && b <= PM_DPAD_RIGHT)) {
                fail(c, "buttons: '%s' isn't a button here", m->key);
                continue;
            }
            binding(c, m, &s->bind[b], where);
        }
        break;
    case PM_MODE_TRIGGER:
        if (jget(v, "full_press")) binding(c, jget(v, "full_press"), &s->bind[0], where);
        break;
    case PM_MODE_BUTTON:
        if (jget(v, "button")) binding(c, jget(v, "button"), &s->bind[0], where);
        break;
    default: break;
    }
}

static void sources(pctx *c, const jv *o, pm_source *src, bool *set)
{
    if (!o) return;
    if (o->type != J_OBJ) { fail(c, "sources: not an object"); return; }
    for (const jv *m = o->child; m; m = m->next) {
        int s;
        for (s = 0; s < PM_NSRC; s++)
            if (!strcmp(m->key, padmap_source_name(s))) break;
        if (s == PM_NSRC) { fail(c, "unknown source '%s'", m->key); continue; }
        source(c, m, &src[s], s);
        if (set) set[s] = true;
    }
}

static void preset(pctx *c, const jv *o, pm_preset *pr)
{
    for (int s = 0; s < PM_NSRC; s++) padmap_source_init(&pr->src[s], s, PM_MODE_PASS);
    sources(c, jget(o, "sources"), pr->src, NULL);
    const jv *shifts = jget(o, "mode_shifts");
    if (!shifts) return;
    if (shifts->type != J_ARR) { fail(c, "mode_shifts: not a list"); return; }
    for (const jv *m = shifts->child; m; m = m->next) {
        if (pr->nshifts >= PM_MAX_SHIFTS) { fail(c, "more than %d mode shifts", PM_MAX_SHIFTS); break; }
        pm_shift *sh = &pr->shift[pr->nshifts];
        memset(sh, 0, sizeof(*sh));
        const jv *h = jget(m, "hold");
        int b = h && h->type == J_STR ? button_index(h->str) : -1;
        if (b < 0 || b == PM_GUIDE) { fail(c, "mode shift: 'hold' must name a button"); continue; }
        sh->hold = (uint8_t)b;
        for (int s = 0; s < PM_NSRC; s++) padmap_source_init(&sh->src[s], s, PM_MODE_PASS);
        sources(c, jget(m, "sources"), sh->src, sh->src_set);
        pr->nshifts++;
    }
}

bool padmap_profile_parse(padmap_profile *p, const char *json, char *err, size_t errsize)
{
    pctx c = { p, err, errsize, true };
    padmap_profile_init(p, "Custom");
    if (err && errsize) err[0] = 0;
    jp ps = { json, NULL, 1 };
    jv *root = jvalue(&ps, 0);
    if (!root) {
        fail(&c, "line %d: %s", ps.line, ps.err ? ps.err : "bad JSON");
        return false;
    }
    if (root->type != J_OBJ) { fail(&c, "not a JSON object"); jfree(root); return false; }
    const jv *n = jget(root, "name");
    if (n && n->type == J_STR) snprintf(p->name, sizeof(p->name), "%s", n->str);
    const jv *presets = jget(root, "presets");
    if (presets && presets->type == J_ARR && presets->child) {
        /* names first, so a binding can switch to a preset defined after it */
        p->npresets = 0;
        for (const jv *m = presets->child; m && p->npresets < PM_MAX_PRESETS; m = m->next) {
            const jv *pn = jget(m, "name");
            snprintf(p->preset[p->npresets].name, PM_NAME, "%s",
                     pn && pn->type == J_STR ? pn->str : (p->npresets ? "Preset" : "Default"));
            p->npresets++;
        }
        int i = 0;
        for (const jv *m = presets->child; m && i < PM_MAX_PRESETS; m = m->next) preset(&c, m, &p->preset[i++]);
        if (presets->child && i == PM_MAX_PRESETS && presets->child->next) {
            int count = 0;
            for (const jv *m = presets->child; m; m = m->next) count++;
            if (count > PM_MAX_PRESETS) fail(&c, "more than %d presets", PM_MAX_PRESETS);
        }
    } else preset(&c, root, &p->preset[0]);
    jfree(root);
    return c.ok;
}

/* ---- writer --------------------------------------------------------------- */

typedef struct {
    char *b;
    size_t size, len;
} wbuf;

static void w(wbuf *o, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(o->len < o->size ? o->b + o->len : NULL, o->len < o->size ? o->size - o->len : 0, fmt, ap);
    va_end(ap);
    if (n > 0) o->len += (size_t)n;
}

static void wstr(wbuf *o, const char *s)
{
    w(o, "\"");
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') w(o, "\\%c", *s);
        else if ((unsigned char)*s < 32) w(o, "\\u%04x", (unsigned char)*s);
        else w(o, "%c", *s);
    }
    w(o, "\"");
}

static void wtarget(wbuf *o, const padmap_profile *p, uint16_t t)
{
    char name[48];
    padmap_target_name(p, t, name, sizeof name);
    wstr(o, name);
}

static void wbinding(wbuf *o, const padmap_profile *p, const pm_bind *b)
{
    bool simple = !b->target[PM_ACT_LONG] && !b->target[PM_ACT_DOUBLE] && !b->turbo_hz && !b->toggle &&
                  !b->long_ms && !b->double_ms;
    if (simple) { wtarget(o, p, b->target[PM_ACT_PRESS]); return; }
    w(o, "{ \"press\": ");
    wtarget(o, p, b->target[PM_ACT_PRESS]);
    if (b->target[PM_ACT_LONG]) { w(o, ", \"long_press\": "); wtarget(o, p, b->target[PM_ACT_LONG]); }
    if (b->target[PM_ACT_DOUBLE]) { w(o, ", \"double_press\": "); wtarget(o, p, b->target[PM_ACT_DOUBLE]); }
    if (b->long_ms) w(o, ", \"long_ms\": %u", b->long_ms);
    if (b->double_ms) w(o, ", \"double_ms\": %u", b->double_ms);
    if (b->turbo_hz) w(o, ", \"turbo_hz\": %u", b->turbo_hz);
    if (b->toggle) w(o, ", \"toggle\": true");
    w(o, " }");
}

static void wsource(wbuf *o, const padmap_profile *p, const pm_source *s, int src, const char *ind)
{
    pm_source d;
    padmap_source_init(&d, src, s->mode);
    w(o, "%s\"%s\": { \"mode\": \"%s\"", ind, padmap_source_name(src), padmap_mode_name(s->mode));
    bool stickish = s->mode == PM_MODE_JOYSTICK || s->mode == PM_MODE_MOUSE;
    if (s->mode == PM_MODE_JOYSTICK && s->output != d.output)
        w(o, ", \"output\": \"%s\"", s->output ? "right_stick" : "left_stick");
    if (s->mode == PM_MODE_TRIGGER && s->output != d.output) w(o, ", \"output\": \"%s\"", s->output ? "rt" : "lt");
    if (stickish || s->mode == PM_MODE_TRIGGER) {
        if (s->deadzone != d.deadzone) w(o, ", \"deadzone\": %.3g", s->deadzone);
        if (s->outer != d.outer) w(o, ", \"outer\": %.3g", s->outer);
        if (s->curve != d.curve) w(o, ", \"curve\": %.3g", s->curve);
        if (s->sensitivity != d.sensitivity) w(o, ", \"sensitivity\": %.3g", s->sensitivity);
    }
    if ((s->mode == PM_MODE_DPAD && src != PM_SRC_DPAD) || s->mode == PM_MODE_TRIGGER || s->mode == PM_MODE_BUTTON)
        if (s->threshold != d.threshold) w(o, ", \"threshold\": %.3g", s->threshold);
    if (stickish || s->mode == PM_MODE_DPAD) {
        if (s->invert_x) w(o, ", \"invert_x\": true");
        if (s->invert_y) w(o, ", \"invert_y\": true");
    }
    static const char *const dirs[4] = { "up", "down", "left", "right" };
    if (s->mode == PM_MODE_DPAD)
        for (int i = 0; i < 4; i++)
            if (s->bind[i].set) { w(o, ", \"%s\": ", dirs[i]); wbinding(o, p, &s->bind[i]); }
    if (s->mode == PM_MODE_BUTTONS)
        for (int b = 0; b < PM_NBTN; b++)
            if (s->bind[b].set) { w(o, ",\n%s    \"%s\": ", ind, padmap_button_name(b)); wbinding(o, p, &s->bind[b]); }
    if ((s->mode == PM_MODE_TRIGGER || s->mode == PM_MODE_BUTTON) && s->bind[0].set) {
        w(o, ", \"%s\": ", s->mode == PM_MODE_TRIGGER ? "full_press" : "button");
        wbinding(o, p, &s->bind[0]);
    }
    w(o, " }");
}

static void wsources(wbuf *o, const padmap_profile *p, const pm_source *src, const bool *set, const char *ind)
{
    char inner[32];
    snprintf(inner, sizeof inner, "%s  ", ind);
    w(o, "{");
    bool first = true;
    for (int s = 0; s < PM_NSRC; s++) {
        if (set ? !set[s] : src[s].mode == PM_MODE_PASS) continue;
        w(o, first ? "\n" : ",\n");
        first = false;
        wsource(o, p, &src[s], s, inner);
    }
    w(o, first ? "}" : "\n%s}", ind);
}

int padmap_profile_write(const padmap_profile *p, char *buf, size_t size)
{
    wbuf o = { buf, size, 0 };
    if (buf && size) buf[0] = 0;
    w(&o, "{\n  \"version\": 1,\n  \"name\": ");
    wstr(&o, p->name);
    w(&o, ",\n  \"presets\": [");
    for (int i = 0; i < p->npresets; i++) {
        const pm_preset *pr = &p->preset[i];
        w(&o, i ? ",\n    { \"name\": " : "\n    { \"name\": ");
        wstr(&o, pr->name);
        w(&o, ",\n      \"sources\": ");
        wsources(&o, p, pr->src, NULL, "      ");
        if (pr->nshifts) {
            w(&o, ",\n      \"mode_shifts\": [");
            for (int k = 0; k < pr->nshifts; k++) {
                w(&o, k ? ",\n        { \"hold\": \"%s\", \"sources\": " : "\n        { \"hold\": \"%s\", \"sources\": ",
                  padmap_button_name(pr->shift[k].hold));
                wsources(&o, p, pr->shift[k].src, pr->shift[k].src_set, "          ");
                w(&o, " }");
            }
            w(&o, "\n      ]");
        }
        w(&o, " }");
    }
    w(&o, "\n  ]\n}\n");
    if (o.len >= size) return size ? -1 : (int)o.len;
    return (int)o.len;
}
