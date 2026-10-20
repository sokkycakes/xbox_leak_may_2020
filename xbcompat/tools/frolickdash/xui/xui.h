// A C++ port of frolick-xui (the project's Go XUI runtime) for the Xbox
// dashboard's xapp engine: it loads Xbox 360 .xui scenes (the XML the XUI
// Tool saves) with an optional skin, keeps their timelines and focus, and
// draws them through a small backend that XuiPanel.cpp implements on D3D8.
// Built with the leak's VC7, so this is plain C++98 without the STL.
#pragma once
#include <string.h>
#include <stdlib.h>

namespace xui {

// Property types as stored in XUIB / classes.json.
enum { TBool = 1, TInt = 2, TUint = 3, TFloat = 4, TString = 5, TColor = 6,
       TVector = 7, TQuat = 8, TObject = 9, TCustom = 10, TList = 11 };

struct PropDef {
    const char* name;
    int type;
    int flags;          // bit 0: indexed (counted array)
    const PropDef* sub; // nested properties for TObject (NULL-terminated)
};

struct ClassDef {
    const char* name;
    const char* base;            // direct base class, "" for the root
    const PropDef* const* levels; // base class first, each NULL-terminated
    int nlevels;
};

const ClassDef* FindClass(const char* name);
bool IsA(const char* cls, const char* base);

// ---- containers ----------------------------------------------------------

template <class T> struct Vec {
    T* p; int n, cap;
    Vec() : p(0), n(0), cap(0) {}
    ~Vec() { delete [] p; }
    void push(const T& v) {
        if (n == cap) {
            cap = cap ? cap * 2 : 8;
            T* q = new T[cap];
            for (int i = 0; i < n; i++) q[i] = p[i];
            delete [] p; p = q;
        }
        p[n++] = v;
    }
    T& operator[](int i) { return p[i]; }
    const T& operator[](int i) const { return p[i]; }
    void clear() { n = 0; }
private:
    Vec(const Vec&); Vec& operator=(const Vec&);
};

struct Vec3 { float x, y, z; };
struct Quat { float x, y, z, w; };
typedef unsigned int Color; // 0xAARRGGBB

struct Point { float p[2], c1[2], c2[2]; };
struct Points { float w, h; Vec<Point> pts; };

struct Object;
struct Value;
struct List { Vec<Value> v; };

// A property value.  Strings, objects, lists and points are owned.
struct Value {
    int type;           // 0 = unset
    union { int b; int i; unsigned int u; float f; Color c; Vec3 v; Quat q; };
    char* s;
    Object* o;
    List* l;
    Points* pts;
    Value() : type(0), s(0), o(0), l(0), pts(0) { q.x = q.y = q.z = q.w = 0; }
    Value(const Value& a);
    Value& operator=(const Value& a);
    ~Value();
    void free();
};

struct Prop { const char* name; Value val; };

// A property bag; names are the PropDef table's strings.
struct Object {
    Vec<Prop> props;
    Value* get(const char* name);
    Value& set(const char* name);   // existing slot or a new one
    Object* clone() const;
};

struct NamedFrame { char* name; int time; int command; char* target; };

struct PropPath { Vec<const char*> names; int type; int index; };

struct Keyframe { int time; int interp; signed char ease[3]; Vec<Value> values; };

struct Timeline {
    char* target;
    Vec<PropPath*> props;
    Vec<Keyframe*> keys;
};

// Element: a node of the loaded scene description (immutable template).
struct Element {
    const ClassDef* cls;
    Object props;
    Vec<Element*> children;
    Vec<NamedFrame> frames;
    Vec<Timeline*> timelines;
    ~Element();
    const char* id() const;
    Element* find(const char* id);
};

// ParseXUI parses .xui XML.  On failure it returns NULL and err names why.
Element* ParseXUI(const char* text, int len, char* err, int errlen);

// ---- live scene ----------------------------------------------------------

struct Node {
    Element* tmpl;
    const ClassDef* cls;
    Object props;
    Vec<Node*> children;
    Node* parent;
    Node* visual;   // skin visual drawn for a control
    Node* owner;    // the control a visual subtree belongs to
    double time; bool playing; bool timeline;
    bool focused;

    Node();
    ~Node();
    const char* id();
    float width(); float height();
    Node* find(const char* id);
    bool focusable();
    bool play(const char* frame);
    void advance(double frames);
    void apply();
    void playState(const char* a, const char* b = 0);
    void setPath(PropPath* pp, const Value& v);
    void anchor(float dw, float dh);
    void markOwner(Node* owner);
    double frameTime(const char* name, double def);
    double lastKey();
    bool hasCommandsAfter(double t);
    bool stopsAt(double t);
    template <class F> bool walk(F& f);
};

struct Scene {
    Node* root;
    Element* skin;
    Node* focus;
    void (*onPress)(Scene*, Node*, void*);
    void (*onFocus)(Scene*, Node*, void*);
    void* user;

    Scene(Element* root, Element* skin);
    ~Scene();
    Node* find(const char* id) { return root->find(id); }
    void setFocus(Node* n);
    bool navigate(const char* dir); // "left", "right", "up", "down"
    void press();
    void update(double dt);
    Node* controlAt(float x, float y);
private:
    Node* instantiate(Element* e, Node* parent);
    void attachVisual(Node* n);
    Element* skinVisual(const char* name);
};

// ---- drawing -------------------------------------------------------------

struct Vertex { float x, y, u, v; float r, g, b, a; };
typedef unsigned int Texture; // 0 = none (white)
enum Blend { BlendNormal, BlendAdd, BlendMultiply };

// An 8-bit-per-channel image in D3D A8R8G8B8 memory order (B,G,R,A per
// pixel), not premultiplied; w*h*4 bytes, no padding.
struct Image { int w, h; unsigned char* pix; Image() : w(0), h(0), pix(0) {} ~Image() { delete [] pix; } };

struct Backend {
    virtual Texture upload(const Image& img) = 0;
    virtual void triangles(Texture tex, Blend blend, const Vertex* v, int nv, const unsigned short* idx, int ni) = 0;
    virtual void clip(const float* r) = 0; // x0 y0 x1 y1 in scene pixels, or NULL
    // A texture the host decodes itself (an Xbox .xbx), 0 for none.
    virtual Texture native(const char* path, int* w, int* h) { (void)path; *w = *h = 0; return 0; }
    virtual ~Backend() {}
};

// Resources resolves scene, image and font files; the host supplies it.
struct Resources {
    virtual bool read(const char* path, unsigned char** data, int* len) = 0; // caller frees with delete []
    virtual ~Resources() {}
    Image* image(const char* path);     // decodes an .xtx (see tools/mkassets.py)
    Element* scene(const char* path, char* err, int errlen);
};

struct Affine { float a, b, c, d, tx, ty; };

struct Font;

struct Renderer {
    Backend* be;
    Resources* res;
    Renderer(Backend* b, Resources* r);
    ~Renderer();
    void draw(Scene* s, float x, float y, float scale);
    void setFont(const char* path); // the TTF text elements use unless their Font names another

private:
    struct TexEntry { char* key; Texture tex; int w, h; };
    Vec<TexEntry> texes;
    Font* font;
    // <Font>Name</Font> on a text element picks Name.ttf (spaces dropped)
    // from the scene's files, the default font when there is no such file.
    struct FontEntry { char* name; Font* f; };
    Vec<FontEntry> fonts;
    Font* fontFor(const char* name);
    bool clipping; float clipBuf[4];

    Texture lookup(const char* key, int* w = 0, int* h = 0);
    void remember(const char* key, Texture t, int w, int h);
    void node(Node* n, const Affine& parent, float alpha, Blend blend);
    void figure(Node* n, const Affine& m, float alpha, Blend blend);
    void image(Node* n, const Affine& m, float alpha, Blend blend);
    void text(Node* n, const Affine& m, float alpha, Blend blend);
    void quad(Texture tex, Blend blend, const Affine& m, float x0, float y0, float x1, float y1,
              float u0, float v0, float u1, float v1, Color c, float alpha);
    void stroke(const float* poly, int n, bool closed, const Affine& m, float w, Color c, float alpha, Blend blend);
    Texture lineTex(Font* f, const char* line, float px, int* w, int* h, int* adv);
    Texture gradient(Object* g, bool radial);
    Texture imageTex(const char* path, int* w, int* h);
};

// ---- helpers -------------------------------------------------------------

float propF(Object& o, const char* k, float def);
unsigned int propU(Object& o, const char* k, unsigned int def);
bool propB(Object& o, const char* k, bool def);
Color propC(Object& o, const char* k, Color def);
const char* propS(Object& o, const char* k);
Vec3 propV(Object& o, const char* k, Vec3 def);
char* strdup2(const char* s);
int utf8decode(const char* s, int* adv);

} // namespace xui
