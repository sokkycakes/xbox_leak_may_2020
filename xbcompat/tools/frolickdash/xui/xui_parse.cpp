// Values, elements and the .xui XML parser (frolick-xui's xuixml.go).
#include "xui.h"
#include <stdio.h>
#include <math.h>

namespace xui {

char* strdup2(const char* s)
{
    if (!s) s = "";
    char* d = new char[strlen(s) + 1];
    strcpy(d, s);
    return d;
}

// ---- Value / Object --------------------------------------------------------

Value::Value(const Value& a) : type(0), s(0), o(0), l(0), pts(0) { *this = a; }

Value& Value::operator=(const Value& a)
{
    if (this == &a) return *this;
    free();
    type = a.type; q = a.q;
    if (a.s) s = strdup2(a.s);
    if (a.o) o = a.o->clone();
    if (a.l) { l = new List; for (int i = 0; i < a.l->v.n; i++) l->v.push(a.l->v[i]); }
    if (a.pts) { pts = new Points; pts->w = a.pts->w; pts->h = a.pts->h; for (int i = 0; i < a.pts->pts.n; i++) pts->pts.push(a.pts->pts[i]); }
    return *this;
}

Value::~Value() { free(); }

void Value::free()
{
    delete [] s; s = 0;
    delete o; o = 0;
    delete l; l = 0;
    delete pts; pts = 0;
    type = 0;
}

Value* Object::get(const char* name)
{
    for (int i = 0; i < props.n; i++)
        if (strcmp(props[i].name, name) == 0)
            return &props[i].val;
    return 0;
}

Value& Object::set(const char* name)
{
    Value* v = get(name);
    if (v) return *v;
    Prop p; p.name = name;
    props.push(p);
    return props[props.n - 1].val;
}

Object* Object::clone() const
{
    Object* c = new Object;
    for (int i = 0; i < props.n; i++) c->props.push(props[i]);
    return c;
}

float propF(Object& o, const char* k, float def) { Value* v = o.get(k); return v && v->type == TFloat ? v->f : def; }
unsigned int propU(Object& o, const char* k, unsigned int def) { Value* v = o.get(k); return v && (v->type == TUint || v->type == TInt) ? v->u : def; }
bool propB(Object& o, const char* k, bool def) { Value* v = o.get(k); return v && v->type == TBool ? v->b != 0 : def; }
Color propC(Object& o, const char* k, Color def) { Value* v = o.get(k); return v && v->type == TColor ? v->c : def; }
const char* propS(Object& o, const char* k) { Value* v = o.get(k); return v && v->type == TString && v->s ? v->s : ""; }
Vec3 propV(Object& o, const char* k, Vec3 def) { Value* v = o.get(k); return v && v->type == TVector ? v->v : def; }

// ---- Element -------------------------------------------------------------

Element::~Element()
{
    for (int i = 0; i < children.n; i++) delete children[i];
    for (int i = 0; i < frames.n; i++) { delete [] frames[i].name; delete [] frames[i].target; }
    for (int i = 0; i < timelines.n; i++) {
        Timeline* t = timelines[i];
        delete [] t->target;
        for (int j = 0; j < t->props.n; j++) delete t->props[j];
        for (int j = 0; j < t->keys.n; j++) delete t->keys[j];
        delete t;
    }
}

const char* Element::id() const { return propS(const_cast<Object&>(props), "Id"); }

Element* Element::find(const char* id)
{
    if (strcmp(this->id(), id) == 0) return this;
    for (int i = 0; i < children.n; i++) { Element* f = children[i]->find(id); if (f) return f; }
    return 0;
}

// ---- a tiny XML reader -----------------------------------------------------

struct XNode {
    char* name;
    char* text;         // character data (entities decoded)
    Vec<XNode*> kids;
    XNode() : name(0), text(0) {}
    ~XNode() { delete [] name; delete [] text; for (int i = 0; i < kids.n; i++) delete kids[i]; }
    XNode* kid(const char* n) { for (int i = 0; i < kids.n; i++) if (strcmp(kids[i]->name, n) == 0) return kids[i]; return 0; }
};

struct XParser {
    const char* p; const char* end; char* err; int errlen; int line;
    bool fail(const char* msg) { if (err) _snprintf(err, errlen, "xui xml line %d: %s", line, msg); return false; }
    void skipws() { while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) { if (*p == '\n') line++; p++; } }
    bool skipMisc() {
        for (;;) {
            skipws();
            if (p + 4 <= end && strncmp(p, "<!--", 4) == 0) {
                const char* e = strstr(p, "-->"); if (!e) return fail("unterminated comment");
                p = e + 3; continue;
            }
            if (p + 2 <= end && (strncmp(p, "<?", 2) == 0 || strncmp(p, "<!", 2) == 0)) {
                const char* e = strchr(p, '>'); if (!e) return fail("bad declaration");
                p = e + 1; continue;
            }
            return true;
        }
    }
    static int entity(const char* s, int n, char* out) { // returns bytes written, 0 if unknown
        if (n >= 2 && s[0] == '#') {
            unsigned cp = (unsigned)strtoul(s + (s[1] == 'x' ? 2 : 1), 0, s[1] == 'x' ? 16 : 10);
            if (cp < 0x80) { out[0] = (char)cp; return 1; }
            if (cp < 0x800) { out[0] = (char)(0xc0 | cp >> 6); out[1] = (char)(0x80 | (cp & 0x3f)); return 2; }
            out[0] = (char)(0xe0 | cp >> 12); out[1] = (char)(0x80 | ((cp >> 6) & 0x3f)); out[2] = (char)(0x80 | (cp & 0x3f)); return 3;
        }
        if (n == 2 && strncmp(s, "lt", 2) == 0) { *out = '<'; return 1; }
        if (n == 2 && strncmp(s, "gt", 2) == 0) { *out = '>'; return 1; }
        if (n == 3 && strncmp(s, "amp", 3) == 0) { *out = '&'; return 1; }
        if (n == 4 && strncmp(s, "quot", 4) == 0) { *out = '"'; return 1; }
        if (n == 4 && strncmp(s, "apos", 4) == 0) { *out = '\''; return 1; }
        return 0;
    }
    char* decode(const char* s, const char* e) {
        char* out = new char[e - s + 1]; char* o = out;
        while (s < e) {
            if (*s == '&') {
                const char* semi = (const char*)memchr(s, ';', e - s);
                int n;
                if (semi && (n = entity(s + 1, (int)(semi - s - 1), o)) > 0) { o += n; s = semi + 1; continue; }
            }
            if (*s == '\n') line++;
            *o++ = *s++;
        }
        *o = 0;
        return out;
    }
    XNode* element() {
        if (!skipMisc()) return 0;
        if (p >= end || *p != '<') { fail("expected element"); return 0; }
        p++;
        const char* n0 = p;
        while (p < end && *p != '>' && *p != '/' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
        XNode* x = new XNode;
        x->name = new char[p - n0 + 1]; memcpy(x->name, n0, p - n0); x->name[p - n0] = 0;
        // attributes are skipped (XuiCanvas version="...")
        while (p < end && *p != '>' && *p != '/') {
            if (*p == '"') { p++; while (p < end && *p != '"') p++; }
            p++;
        }
        if (p < end && *p == '/') { p += 2; x->text = strdup2(""); return x; }
        p++; // '>'
        const char* t0 = p;
        for (;;) {
            const char* lt = (const char*)memchr(p, '<', end - p);
            if (!lt) { fail("unterminated element"); delete x; return 0; }
            if (lt + 1 < end && lt[1] == '/') {
                if (!x->text) x->text = decode(t0, lt); else { char* t = decode(t0, lt); if (!*x->text) { delete [] x->text; x->text = t; } else delete [] t; }
                p = lt + 2;
                const char* ce = (const char*)memchr(p, '>', end - p);
                if (!ce || strncmp(p, x->name, strlen(x->name)) != 0) { fail("mismatched close tag"); delete x; return 0; }
                p = ce + 1;
                return x;
            }
            if (lt + 4 <= end && strncmp(lt, "<!--", 4) == 0) { p = lt; if (!skipMisc()) { delete x; return 0; } t0 = p; continue; }
            if (lt + 9 <= end && strncmp(lt, "<![CDATA[", 9) == 0) {
                const char* ce = strstr(lt, "]]>"); if (!ce) { fail("bad CDATA"); delete x; return 0; }
                delete [] x->text; x->text = new char[ce - lt - 9 + 1]; memcpy(x->text, lt + 9, ce - lt - 9); x->text[ce - lt - 9] = 0;
                p = ce + 3; t0 = p; continue;
            }
            if (!x->text) x->text = decode(t0, lt);
            p = lt;
            XNode* k = element();
            if (!k) { delete x; return 0; }
            x->kids.push(k);
            t0 = p;
        }
    }
};

// ---- values ----------------------------------------------------------------

static const char* trim(const char* s, int* len)
{
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    int n = (int)strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r' || s[n-1] == '\n')) n--;
    *len = n;
    return s;
}

static int floats(const char* s, float* out, int n)
{
    int i = 0;
    while (i < n) {
        char* e;
        out[i++] = (float)strtod(s, &e);
        if (e == s) break;
        s = e;
        while (*s == ' ' || *s == '\t') s++;
        if (*s != ',') break;
        s++;
    }
    for (int k = i; k < n; k++) out[k] = 0;
    return i;
}

static Points* parsePoints(const char* s)
{
    Points* p = new Points;
    float head[2]; floats(s, head, 2);
    p->w = head[0]; p->h = head[1];
    const char* g = strchr(s, ';');
    while (g) {
        g++;
        const char* ge = strchr(g, ';');
        int len = ge ? (int)(ge - g) : (int)strlen(g);
        char buf[256]; if (len > 255) len = 255; memcpy(buf, g, len); buf[len] = 0;
        int tl; const char* t = trim(buf, &tl);
        if (tl > 0) {
            int commas = 0; for (int i = 0; i < tl; i++) if (t[i] == ',') commas++;
            float f[6]; int n = commas + 1; if (n > 6) n = 6;
            floats(t, f, n);
            Point pt; pt.p[0] = f[0]; pt.p[1] = f[1];
            pt.c1[0] = pt.p[0]; pt.c1[1] = pt.p[1]; pt.c2[0] = pt.c2[1] = 0;
            if (n >= 6) { pt.c1[0] = f[2]; pt.c1[1] = f[3]; pt.c2[0] = f[4]; pt.c2[1] = f[5]; }
            p->pts.push(pt);
        }
        g = ge;
    }
    // Straight edges: a control left unset points at the next vertex.
    for (int i = 0; i < p->pts.n; i++)
        if (p->pts[i].c2[0] == 0 && p->pts[i].c2[1] == 0) {
            Point& nx = p->pts[(i + 1) % p->pts.n];
            p->pts[i].c2[0] = nx.p[0]; p->pts[i].c2[1] = nx.p[1];
        }
    return p;
}

static bool parseValue(const PropDef* d, const char* raw, Value& v, char* err, int errlen)
{
    int len; const char* s = trim(raw, &len);
    char buf[512]; if (len > 511) len = 511; memcpy(buf, s, len); buf[len] = 0;
    v.free();
    v.type = d->type;
    float f[4];
    switch (d->type) {
    case TBool: v.b = (strcmp(buf, "true") == 0 || strcmp(buf, "1") == 0); break;
    case TInt: v.i = (int)strtol(buf, 0, 0); break;
    case TUint: v.u = (unsigned)strtoul(buf, 0, 0); break;
    case TFloat: v.f = (float)strtod(buf, 0); break;
    case TString: { int n; const char* t = trim(raw, &n); v.s = new char[n + 1]; memcpy(v.s, t, n); v.s[n] = 0; break; }
    case TColor: v.c = (Color)strtoul(buf, 0, 0); break;
    case TVector: floats(buf, f, 3); v.v.x = f[0]; v.v.y = f[1]; v.v.z = f[2]; break;
    case TQuat: floats(buf, f, 4); v.q.x = f[0]; v.q.y = f[1]; v.q.z = f[2]; v.q.w = f[3]; break;
    case TCustom:
        if (strcmp(d->name, "Points") == 0) v.pts = parsePoints(buf);
        else v.s = strdup2(buf);
        break;
    default:
        if (err) _snprintf(err, errlen, "property %s has type %d", d->name, d->type);
        return false;
    }
    return true;
}

static const PropDef* findDef(const ClassDef* cls, const char* name)
{
    const PropDef* found = 0;
    for (int i = 0; i < cls->nlevels; i++)
        for (const PropDef* d = cls->levels[i]; d->name; d++)
            if (strcmp(d->name, name) == 0) found = d;
    return found;
}

static const PropDef* findSub(const PropDef* sub, const char* name)
{
    for (const PropDef* d = sub; d && d->name; d++) if (strcmp(d->name, name) == 0) return d;
    return 0;
}

static bool props(XNode* x, const ClassDef* cls, const PropDef* sub, Object& out, char* err, int errlen)
{
    for (int i = 0; i < x->kids.n; i++) {
        XNode* k = x->kids[i];
        const PropDef* d = cls ? findDef(cls, k->name) : findSub(sub, k->name);
        if (!d) { if (err) _snprintf(err, errlen, "unknown property %s", k->name); return false; }
        Value v;
        if (d->type == TObject) {
            v.type = TObject; v.o = new Object;
            if (!props(k, 0, d->sub, *v.o, err, errlen)) return false;
        } else if (!parseValue(d, k->text ? k->text : "", v, err, errlen)) {
            return false;
        }
        if (d->flags & 1) {
            Value& slot = out.set(d->name);
            if (slot.type != TList) { slot.free(); slot.type = TList; slot.l = new List; }
            slot.l->v.push(v);
        } else {
            out.set(d->name) = v;
        }
    }
    return true;
}

static int command(const char* s)
{
    char b[32]; int i = 0;
    for (; s[i] && i < 31; i++) b[i] = (char)((s[i] >= 'A' && s[i] <= 'Z') ? s[i] + 32 : s[i]);
    b[i] = 0;
    if (strcmp(b, "stop") == 0) return 1;
    if (strcmp(b, "goto") == 0) return 2;
    if (strcmp(b, "gotoandplay") == 0) return 3;
    if (strcmp(b, "gotoandstop") == 0) return 4;
    return 0;
}

// resolvePath turns "Fill.Gradient.StopColor[1]" into a PropPath.
static PropPath* resolvePath(const ClassDef* cls, const char* path, const PropDef** def, char* err, int errlen)
{
    PropPath* pp = new PropPath; pp->index = -1; pp->type = 0;
    char buf[256]; strncpy(buf, path, 255); buf[255] = 0;
    char* br = strchr(buf, '[');
    if (br && buf[strlen(buf) - 1] == ']') { pp->index = atoi(br + 1); *br = 0; }
    const PropDef* d = 0; const PropDef* sub = 0;
    char* tok = strtok(buf, ".");
    while (tok) {
        d = sub ? findSub(sub, tok) : findDef(cls, tok);
        if (!d) { if (err) _snprintf(err, errlen, "%s has no property %s", cls->name, path); delete pp; return 0; }
        pp->names.push(d->name);
        sub = d->sub ? d->sub : (const PropDef*)0;
        if (!d->sub) { static const PropDef none[] = { { 0, 0, 0, 0 } }; sub = none; }
        tok = strtok(0, ".");
    }
    pp->type = d->type; *def = d;
    return pp;
}

static bool timelines(XNode* t, Element* e, char* err, int errlen)
{
    XNode* nfs = t->kid("NamedFrames");
    if (nfs) {
        for (int i = 0; i < nfs->kids.n; i++) {
            XNode* k = nfs->kids[i];
            NamedFrame nf; nf.name = 0; nf.time = 0; nf.command = 0; nf.target = 0;
            int len; XNode* n;
            n = k->kid("Name"); nf.name = strdup2(n && n->text ? trim(n->text, &len) : "");
            if (nf.name[0]) { trim(nf.name, &len); nf.name[len] = 0; }
            n = k->kid("Time"); if (n && n->text) nf.time = atoi(n->text);
            n = k->kid("Command"); if (n && n->text) { const char* c = trim(n->text, &len); nf.command = command(c); }
            n = k->kid("CommandParams"); nf.target = strdup2(n && n->text ? trim(n->text, &len) : ""); if (nf.target[0]) { trim(nf.target, &len); nf.target[len] = 0; }
            e->frames.push(nf);
        }
    }
    for (int i = 0; i < t->kids.n; i++) {
        XNode* k = t->kids[i];
        if (strcmp(k->name, "Timeline") != 0) continue;
        Timeline* tl = new Timeline; tl->target = 0;
        int len; XNode* n = k->kid("Id");
        tl->target = strdup2(n && n->text ? trim(n->text, &len) : ""); if (tl->target[0]) { trim(tl->target, &len); tl->target[len] = 0; }
        e->timelines.push(tl);
        Element* target = 0;
        for (int c = 0; c < e->children.n; c++) if (strcmp(e->children[c]->id(), tl->target) == 0) target = e->children[c];
        if (!target) { if (err) _snprintf(err, errlen, "%s timeline: no child %s", e->id(), tl->target); return false; }
        Vec<const PropDef*> defs;
        for (int j = 0; j < k->kids.n; j++) {
            XNode* kk = k->kids[j];
            if (strcmp(kk->name, "TimelineProp") == 0) {
                const PropDef* d;
                PropPath* pp = resolvePath(target->cls, trim(kk->text ? kk->text : "", &len), &d, err, errlen);
                if (!pp) return false;
                tl->props.push(pp); defs.push(d);
            } else if (strcmp(kk->name, "KeyFrame") == 0) {
                Keyframe* kf = new Keyframe; kf->time = 0; kf->interp = 0; kf->ease[0] = kf->ease[1] = kf->ease[2] = 0;
                tl->keys.push(kf);
                int vi = 0;
                for (int m = 0; m < kk->kids.n; m++) {
                    XNode* f = kk->kids[m];
                    const char* txt = f->text ? f->text : "";
                    if (strcmp(f->name, "Time") == 0) kf->time = atoi(txt);
                    else if (strcmp(f->name, "Interpolation") == 0) kf->interp = atoi(txt);
                    else if (strcmp(f->name, "EaseIn") == 0) kf->ease[0] = (signed char)atoi(txt);
                    else if (strcmp(f->name, "Prop") == 0) {
                        if (vi >= defs.n) { if (err) _snprintf(err, errlen, "too many Prop values at time %d", kf->time); return false; }
                        Value v;
                        if (!parseValue(defs[vi], txt, v, err, errlen)) return false;
                        kf->values.push(v); vi++;
                    }
                }
            }
        }
    }
    return true;
}

static Element* element(XNode* x, char* err, int errlen)
{
    const ClassDef* cls = FindClass(x->name);
    if (!cls) { if (err) _snprintf(err, errlen, "unknown class %s", x->name); return 0; }
    Element* e = new Element; e->cls = cls;
    for (int i = 0; i < x->kids.n; i++) {
        XNode* k = x->kids[i];
        if (strcmp(k->name, "Properties") == 0) {
            if (!props(k, cls, 0, e->props, err, errlen)) { delete e; return 0; }
        } else if (strcmp(k->name, "Timelines") == 0) {
        } else {
            Element* c = element(k, err, errlen);
            if (!c) { delete e; return 0; }
            e->children.push(c);
        }
    }
    XNode* t = x->kid("Timelines");
    if (t && !timelines(t, e, err, errlen)) { delete e; return 0; }
    return e;
}

Element* ParseXUI(const char* text, int len, char* err, int errlen)
{
    if (err && errlen) err[0] = 0;
    XParser p; p.p = text; p.end = text + len; p.err = err; p.errlen = errlen; p.line = 1;
    XNode* root = p.element();
    if (!root) return 0;
    Element* e = element(root, err, errlen);
    delete root;
    return e;
}

} // namespace xui
