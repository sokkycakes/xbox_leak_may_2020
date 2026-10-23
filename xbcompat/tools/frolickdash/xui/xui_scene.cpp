// Live scenes: instantiation with skin visuals, timelines, focus (scene.go).
#include "xui.h"
#include <math.h>
#include <stdio.h>

namespace xui {

static const double FPS = 30;

Node::Node() : tmpl(0), cls(0), parent(0), visual(0), owner(0), time(0), playing(false), timeline(false), focused(false) {}

Node::~Node()
{
    delete visual;
    for (int i = 0; i < children.n; i++) delete children[i];
}

const char* Node::id() { return propS(props, "Id"); }
float Node::width() { return propF(props, "Width", 60); }
float Node::height() { return propF(props, "Height", 30); }

Node* Node::find(const char* id)
{
    if (strcmp(this->id(), id) == 0) return this;
    for (int i = 0; i < children.n; i++) { Node* f = children[i]->find(id); if (f) return f; }
    return 0;
}

template <class F> bool Node::walk(F& f)
{
    if (!f(this)) return false;
    if (visual && !visual->walk(f)) return false;
    for (int i = 0; i < children.n; i++) if (!children[i]->walk(f)) return false;
    return true;
}

bool Node::focusable()
{
    const char* c = cls->name;
    if (!IsA(c, "XuiControl") || strcmp(c, "XuiScene") == 0 || IsA(c, "XuiTabScene") || owner) return false;
    if (strcmp(c, "XuiLabel") == 0 || strcmp(c, "XuiText") == 0 || strcmp(c, "XuiImagePresenter") == 0 || strcmp(c, "XuiTextPresenter") == 0) return false;
    if (!propB(props, "Enabled", true)) return false;
    if (!propB(props, "Show", true)) return false;
    return true;
}

// ---- construction ----------------------------------------------------------

// A checked checkbox plays the Check variant of each state (NormalCheck,
// FocusCheck); Checked is set from code.
static bool checked(Node* n)
{
    return n && n->cls && strcmp(n->cls->name, "XuiCheckbox") == 0 && propB(n->props, "Checked", false);
}


// A progress bar's visual (and every timeline inside it: the Blades'
// prgbr_Memory keeps its keys on a group under the visual, which has no
// named frames of its own) is held at the point its Value picks in
// RangeMin..RangeMax: between Normal and EndNormal where a timeline has
// them, else between 0 and its last key.  Nothing in it plays on its own.
struct ParkBar { float f; bool operator()(Node* n) {
    if (!n->timeline) return true;
    double a = n->frameTime("Normal", 0), b = n->frameTime("EndNormal", n->lastKey());
    n->time = a + (b - a) * f; n->playing = false; n->apply();
    return true; } };

void Node::setBarValue()
{
    if (!visual) return;
    float lo = propN(props, "RangeMin", 0), hi = propN(props, "RangeMax", 100);
    float f = hi > lo ? (propN(props, "Value", 0) - lo) / (hi - lo) : 0;
    if (f < 0) f = 0; if (f > 1) f = 1;
    ParkBar pb; pb.f = f; visual->walk(pb);
}

static bool isBar(Node* n) { return n && n->cls && strcmp(n->cls->name, "XuiProgressBar") == 0; }

// A timeline starts at frame 0 unless it has a Normal frame; a Stop command
// at frame 0 (the Blades' meta panes rest at "Default") holds it there.
// A progress bar's visual is parked by setBarValue.
struct InitTimelines { bool operator()(Node* n) {
    if (isBar(n)) { n->setBarValue(); return true; }
    if (!n->timeline) return true;
    if (isBar(n->owner)) return true;
    if (n->owner && n->owner->visual == n && !propB(n->owner->props, "Enabled", true) && n->play("NormalDisable")) return true;
    if (n->owner && n->owner->visual == n && checked(n->owner) && n->play("NormalCheck")) return true;
    if (!n->play("Normal")) { n->time = 0; n->playing = !n->stopsAt(0); n->apply(); }
    return true; } };

struct FirstFocus { Scene* s; bool operator()(Node* n) {
    if (!s->focus && n->focusable()) s->setFocus(n);
    return s->focus == 0; } };

Scene::Scene(Element* r, Element* sk) : root(0), skin(sk), focus(0), onPress(0), onFocus(0), user(0)
{
    root = instantiate(r, 0);
    InitTimelines it; root->walk(it);
    FirstFocus ff; ff.s = this; root->walk(ff);
}

Scene::~Scene() { delete root; }

Node* Scene::instantiate(Element* e, Node* parent)
{
    Node* n = new Node;
    n->tmpl = e; n->cls = e->cls; n->parent = parent;
    for (int i = 0; i < e->props.props.n; i++) n->props.props.push(e->props.props[i]);
    n->timeline = e->frames.n > 0 || e->timelines.n > 0;
    for (int i = 0; i < e->children.n; i++) n->children.push(instantiate(e->children[i], n));
    if (IsA(e->cls->name, "XuiControl") && strcmp(e->cls->name, "XuiScene") != 0 && !IsA(e->cls->name, "XuiTabScene"))
        attachVisual(n);
    return n;
}

Element* Scene::skinVisual(const char* name)
{
    if (!skin) return 0;
    // breadth first: direct XuiVisual children of each group, then deeper
    Vec<Element*> stack; stack.push(skin);
    for (int s = 0; s < stack.n; s++) {
        Element* e = stack[s];
        for (int i = 0; i < e->children.n; i++) {
            Element* c = e->children[i];
            if (strcmp(c->cls->name, "XuiVisual") == 0) { if (strcmp(c->id(), name) == 0) return c; }
            else stack.push(c);
        }
    }
    return 0;
}

void Scene::attachVisual(Node* n)
{
    if (!skin) return;
    const char* name = propS(n->props, "Visual");
    bool explicitName = name[0] != 0;
    if (!explicitName) name = n->cls->name;
    Element* v = skinVisual(name);
    if (!v && !explicitName)
        for (const ClassDef* c = FindClass(n->cls->base); c && !v && c->base; c = FindClass(c->base)) {
            v = skinVisual(c->name);
            if (!c->base[0]) break;
        }
    if (!v) return;
    Node* vis = instantiate(v, n);
    Value& pos = vis->props.set("Position"); pos.free(); pos.type = TVector; pos.v.x = pos.v.y = pos.v.z = 0;
    vis->markOwner(n);
    float w0 = propF(v->props, "Width", 60), h0 = propF(v->props, "Height", 30);
    float w = n->width(), h = n->height();
    Value& vw = vis->props.set("Width"); vw.free(); vw.type = TFloat; vw.f = w;
    Value& vh = vis->props.set("Height"); vh.free(); vh.type = TFloat; vh.f = h;
    for (int i = 0; i < vis->children.n; i++) vis->children[i]->anchor(w - w0, h - h0);
    n->visual = vis;
}

// A visual takes its control's size; timelines that resize the control
// (the Guide's back panel widens as it opens) stretch the visual again.
void Node::fitVisual()
{
    if (!visual) return;
    float w = width(), h = height(), vw = visual->width(), vh = visual->height();
    if (w == vw && h == vh) return;
    Value& a = visual->props.set("Width"); a.free(); a.type = TFloat; a.f = w;
    Value& b = visual->props.set("Height"); b.free(); b.type = TFloat; b.f = h;
    for (int i = 0; i < visual->children.n; i++) visual->children[i]->anchor(w - vw, h - vh);
}

void Node::markOwner(Node* o)
{
    owner = o;
    for (int i = 0; i < children.n; i++) if (!children[i]->visual) children[i]->markOwner(o);
}

// Anchor bits: 1 left, 2 top, 4 right, 8 bottom, 16 centre across, 32
// centre down (the GFWL buttons keep icon and text centred in a taller row).  A node anchored to
// both sides stretches with its parent, and its own children then anchor
// to it in turn (the Blades' Metapane is a group of edge strips anchored
// inside a group that stretches to the pane).
void Node::anchor(float dw, float dh)
{
    unsigned a = propU(props, "Anchor", 0);
    Vec3 zero = { 0, 0, 0 };
    Vec3 pos = propV(props, "Position", zero);
    float mw = 0, mh = 0;
    if ((a & 1) && (a & 4)) { Value& v = props.set("Width"); v.free(); v.type = TFloat; v.f = width() + dw; mw = dw; }
    else if (a & 4) pos.x += dw;
    else if (a & 16) pos.x += dw / 2;
    if ((a & 2) && (a & 8)) { Value& v = props.set("Height"); v.free(); v.type = TFloat; v.f = height() + dh; mh = dh; }
    else if (a & 8) pos.y += dh;
    else if (a & 32) pos.y += dh / 2;
    Value& p = props.set("Position"); p.free(); p.type = TVector; p.v = pos;
    if (mw != 0 || mh != 0) for (int i = 0; i < children.n; i++) children[i]->anchor(mw, mh);
}

// ---- focus -----------------------------------------------------------------

void Scene::setFocus(Node* n)
{
    if (n == focus) return;
    bool first = focus == 0;
    if (focus) { focus->focused = false; focus->playState("KillFocus", "Normal"); }
    focus = n;
    if (n) {
        n->focused = true;
        if (first) n->playState("InitFocus", "Focus"); else n->playState("Focus");
        if (onFocus) onFocus(this, n, user);
    }
}

bool Scene::navigate(const char* dir)
{
    if (!focus || !dir || !dir[0]) return false;
    char key[16] = "Nav";
    key[3] = (char)(dir[0] >= 'a' && dir[0] <= 'z' ? dir[0] - 32 : dir[0]);
    strncpy(key + 4, dir + 1, 10); key[15] = 0;
    const char* id = propS(focus->props, key);
    if (!id[0]) return false;
    Node* t = root->find(id);
    if (!t || !t->focusable()) return false;
    setFocus(t);
    return true;
}

void Scene::press()
{
    if (!focus) return;
    focus->playState("Press");
    if (onPress) onPress(this, focus, user);
}

void Node::playState(const char* a, const char* b)
{
    Node* t = visual ? visual : this;
    if (checked(this)) {
        char c[48];
        sprintf(c, "%.40sCheck", a);
        if (t->play(c)) return;
        if (b) { sprintf(c, "%.40sCheck", b); if (t->play(c)) return; }
    }
    if (t->play(a)) return;
    if (b) t->play(b);
}

bool Node::play(const char* name)
{
    for (int i = 0; i < tmpl->frames.n; i++)
        if (strcmp(tmpl->frames[i].name, name) == 0) {
            time = tmpl->frames[i].time; playing = true; apply();
            return true;
        }
    return false;
}

// ---- timelines -------------------------------------------------------------

struct Advance { double dt; bool operator()(Node* n) {
    if (n->timeline && n->playing) n->advance(dt * FPS);
    return true; } };

void Scene::update(double dt) { Advance a; a.dt = dt; root->walk(a); }

double Node::frameTime(const char* name, double def)
{
    for (int i = 0; i < tmpl->frames.n; i++) if (strcmp(tmpl->frames[i].name, name) == 0) return tmpl->frames[i].time;
    return def;
}

double Node::lastKey()
{
    int last = 0;
    for (int i = 0; i < tmpl->timelines.n; i++) {
        Timeline* tl = tmpl->timelines[i];
        if (tl->keys.n > 0 && tl->keys[tl->keys.n - 1]->time > last) last = tl->keys[tl->keys.n - 1]->time;
    }
    for (int i = 0; i < tmpl->frames.n; i++) if (tmpl->frames[i].time > last) last = tmpl->frames[i].time;
    return last;
}

bool Node::stopsAt(double t)
{
    for (int i = 0; i < tmpl->frames.n; i++) if (tmpl->frames[i].time == t && tmpl->frames[i].command == 1) return true;
    return false;
}

bool Node::hasCommandsAfter(double t)
{
    for (int i = 0; i < tmpl->frames.n; i++) if (tmpl->frames[i].time > t && tmpl->frames[i].command != 0) return true;
    return false;
}

void Node::advance(double frames)
{
    double from = time, to = from + frames;
    for (int guard = 0; guard < 8; guard++) {
        NamedFrame* hit = 0;
        for (int i = 0; i < tmpl->frames.n; i++) {
            NamedFrame* nf = &tmpl->frames[i];
            double t = nf->time;
            if (t > from && t <= to && nf->command != 0 && (!hit || t < hit->time)) hit = nf;
        }
        if (!hit) break;
        if (hit->command == 1 || hit->command == 4) {
            time = hit->time;
            if (hit->command == 4) time = frameTime(hit->target, time);
            playing = false; apply();
            return;
        }
        if (hit->command == 2 || hit->command == 3) {
            double rest = to - hit->time;
            double t = frameTime(hit->target, hit->time);
            if (t >= hit->time) rest = 0;
            from = t; to = t + rest;
            if (hit->command == 2) { time = t; playing = false; apply(); return; }
            continue;
        }
        break;
    }
    time = to;
    double last = lastKey();
    if (time >= last && !hasCommandsAfter(from)) { time = last; playing = false; }
    apply();
}

static double ease(double f, const signed char* e)
{
    double s = f * f * (3 - 2 * f);
    double w = e[0] / 100.0; if (w < 0) w = -w; if (w == 0) w = 1;
    return f + (s - f) * w;
}

static Value lerpValue(const Value& a, const Value& b, double f)
{
    if (f == 0 || a.type != b.type) return f >= 1 ? b : a;
    float ff = (float)f;
    Value o = a;
    switch (a.type) {
    case TFloat: o.f = a.f + (b.f - a.f) * ff; return o;
    case TInt: o.i = a.i + (int)((b.i - a.i) * f); return o;
    case TVector: o.v.x = a.v.x + (b.v.x - a.v.x) * ff; o.v.y = a.v.y + (b.v.y - a.v.y) * ff; o.v.z = a.v.z + (b.v.z - a.v.z) * ff; return o;
    case TQuat: o.q.x = a.q.x + (b.q.x - a.q.x) * ff; o.q.y = a.q.y + (b.q.y - a.q.y) * ff; o.q.z = a.q.z + (b.q.z - a.q.z) * ff; o.q.w = a.q.w + (b.q.w - a.q.w) * ff; return o;
    case TColor: {
        Color out = 0;
        for (int sh = 0; sh < 32; sh += 8) {
            double ca = (a.c >> sh) & 0xff, cb = (b.c >> sh) & 0xff;
            out |= (Color)((unsigned)(ca + (cb - ca) * f + 0.5) & 0xff) << sh;
        }
        o.c = out; return o;
    }
    }
    return f >= 1 ? b : a;
}

static void setIndexed(Object& m, const char* name, int i, const Value& v)
{
    Value& slot = m.set(name);
    if (slot.type != TList) { slot.free(); slot.type = TList; slot.l = new List; }
    while (slot.l->v.n <= i) slot.l->v.push(v);
    slot.l->v[i] = v;
}

void Node::setPath(PropPath* pp, const Value& v)
{
    Object* m = &props;
    for (int i = 0; i < pp->names.n - 1; i++) {
        Value& o = m->set(pp->names[i]);
        if (o.type != TObject) { o.free(); o.type = TObject; o.o = new Object; }
        m = o.o;
    }
    const char* last = pp->names[pp->names.n - 1];
    if (pp->index >= 0) setIndexed(*m, last, pp->index, v);
    else m->set(last) = v;
}

void Node::apply()
{
    for (int ti = 0; ti < tmpl->timelines.n; ti++) {
        Timeline* tl = tmpl->timelines[ti];
        Node* target = 0;
        for (int c = 0; c < children.n; c++) if (strcmp(children[c]->id(), tl->target) == 0) { target = children[c]; break; }
        if (!target || tl->keys.n == 0) continue;
        int k0 = 0, k1 = 0;
        for (int i = 0; i < tl->keys.n; i++) {
            if (tl->keys[i]->time <= time) { k0 = i; k1 = i; }
            else { k1 = i; break; }
        }
        if (tl->keys[0]->time > time) { k0 = 0; k1 = 0; }
        Keyframe* a = tl->keys[k0]; Keyframe* b = tl->keys[k1];
        double f = 0;
        if (k1 != k0 && b->time > a->time) {
            f = (time - a->time) / (double)(b->time - a->time);
            if (a->interp == 1) f = 0; else if (a->interp == 2) f = ease(f, a->ease);
        }
        for (int pi = 0; pi < tl->props.n; pi++) {
            if (pi >= a->values.n || pi >= b->values.n) continue;
            target->setPath(tl->props[pi], lerpValue(a->values[pi], b->values[pi], f));
        }
    }
}

// ---- hit test --------------------------------------------------------------

static void visit(Node* n, float ox, float oy, float sx, float sy, float x, float y, Node** hit)
{
    if (!propB(n->props, "Show", true)) return;
    Vec3 zero = { 0, 0, 0 }, one = { 1, 1, 1 };
    Vec3 pos = propV(n->props, "Position", zero), sc = propV(n->props, "Scale", one), pv = propV(n->props, "Pivot", zero);
    float nx = ox + sx * (pos.x + pv.x - pv.x * sc.x);
    float ny = oy + sy * (pos.y + pv.y - pv.y * sc.y);
    float nsx = sx * sc.x, nsy = sy * sc.y;
    if (n->focusable() && x >= nx && y >= ny && x < nx + n->width() * nsx && y < ny + n->height() * nsy) *hit = n;
    for (int i = 0; i < n->children.n; i++) visit(n->children[i], nx, ny, nsx, nsy, x, y, hit);
}

Node* Scene::controlAt(float x, float y)
{
    Node* hit = 0;
    visit(root, 0, 0, 1, 1, x, y, &hit);
    return hit;
}

} // namespace xui
