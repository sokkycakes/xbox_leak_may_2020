// The DS geometry engine's state as the client holds it: what the 3D commands it
// was sent leave behind (matrices and their stacks, lights, material, polygon and
// texture attributes, viewport, vertex color). The arithmetic follows melonDS's
// GPU3D.cpp, which follows the DS.
//
// mirrorrun runs every command the client gets through one of these, so that when
// frames are left out, the state commands of those frames can go to the client as
// the few commands that set where they ended (Rebuild) instead of all of them.

#ifndef MIRROR_GEOSTATE_H
#define MIRROR_GEOSTATE_H

#include <cstdint>
#include <cstring>
#include <vector>

struct GeoCmd
{
    uint8_t cmd;
    std::vector<uint32_t> params;
};

struct GeoState
{
    struct Mtx
    {
        int32_t m[16];
        bool known;   // false: the client has whatever it had before we could tell
        bool operator==(const Mtx& o) const { return known == o.known && (!known || !memcmp(m, o.m, sizeof m)); }
    };
    struct Val
    {
        uint32_t v;
        bool known;
        bool operator==(const Val& o) const { return known == o.known && (!known || v == o.v); }
    };
    struct Light
    {
        uint32_t param;   // LIGHT_VECTOR's parameter...
        Mtx vec;          // ...and the vector matrix it was given (only the 3x3 part counts)
        bool known;
        bool operator==(const Light& o) const { return known == o.known && (!known || (param == o.param && vec == o.vec)); }
    };

    Val mode {0, false};
    Mtx proj {}, pos {}, vec {}, tex {};
    Mtx projStack {}, texStack {}, posStack[32] {}, vecStack[32] {};
    int32_t projSP = 0, posSP = 0, texSP = 0;   // as melonDS keeps them: 1, 6 and 1 bits
    Light light[4] {};
    Val lightColor[4] {}, difAmb {}, speEmi {}, polyAttr {}, texParam {}, texPal {}, viewport {}, color {};
    uint32_t shininess[32] {};
    bool shininessKnown = false;
    uint32_t unknownOps = 0;   // commands whose result can't be told (they used state nobody set yet)

    static void Identity(Mtx& m)
    {
        memset(m.m, 0, sizeof m.m);
        m.m[0] = m.m[5] = m.m[10] = m.m[15] = 0x1000;
        m.known = true;
    }
    static void Load(Mtx& m, const uint32_t* p, bool four)
    {
        if (four)
            for (int i = 0; i < 16; i++) m.m[i] = (int32_t)p[i];
        else
        {
            for (int r = 0; r < 4; r++)
            {
                for (int c = 0; c < 3; c++) m.m[r * 4 + c] = (int32_t)p[r * 3 + c];
                m.m[r * 4 + 3] = r == 3 ? 0x1000 : 0;
            }
        }
        m.known = true;
    }
    // m = s * m (melonDS MatrixMult4x4/4x3/3x3): s is 4 rows of 4, 4 rows of 3 with
    // (0,0,0,1) as its last column, or 3 rows of 3 that leave m's last row alone
    void Mult(Mtx& m, const uint32_t* p, int rows, int cols)
    {
        if (!m.known) { unknownOps++; return; }
        const int32_t* s = (const int32_t*)p;
        int32_t t[16];
        memcpy(t, m.m, sizeof t);
        for (int r = 0; r < rows; r++)
            for (int c = 0; c < 4; c++)
            {
                int64_t a = 0;
                for (int k = 0; k < 3; k++) a += (int64_t)s[r * cols + k] * t[k * 4 + c];
                if (cols == 4) a += (int64_t)s[r * 4 + 3] * t[12 + c];
                else if (r == 3) a += (int64_t)0x1000 * t[12 + c];
                m.m[r * 4 + c] = (int32_t)(a >> 12);
            }
    }
    void Scale(Mtx& m, const uint32_t* p)
    {
        if (!m.known) { unknownOps++; return; }
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 4; c++) m.m[r * 4 + c] = (int32_t)(((int64_t)(int32_t)p[r] * m.m[r * 4 + c]) >> 12);
    }
    void Translate(Mtx& m, const uint32_t* p)
    {
        if (!m.known) { unknownOps++; return; }
        for (int c = 0; c < 4; c++)
            m.m[12 + c] += (int32_t)(((int64_t)(int32_t)p[0] * m.m[c] + (int64_t)(int32_t)p[1] * m.m[4 + c] +
                                      (int64_t)(int32_t)p[2] * m.m[8 + c]) >> 12);
    }
    void Copy(Mtx& to, const Mtx& from)
    {
        to = from;
        if (!from.known) unknownOps++;
    }

    // the matrices a command acts on in the current mode
    template <typename F> void Current(F f)
    {
        switch (mode.v & 3)
        {
        case 0: f(proj); break;
        case 1: f(pos); break;
        case 2: f(pos); f(vec); break;
        case 3: f(tex); break;
        }
    }

    void Exec(uint8_t c, const uint32_t* p, size_t n)
    {
        if (c >= 0x11 && c <= 0x1C && !mode.known)
        {
            // which matrix it meant isn't known: everything the mode chooses between is
            unknownOps++;
            proj.known = pos.known = vec.known = tex.known = projStack.known = texStack.known = false;
            for (int i = 0; i < 32; i++) posStack[i].known = vecStack[i].known = false;
            return;
        }
        uint32_t m = mode.v & 3;
        switch (c)
        {
        case 0x10: mode = {p[0] & 3, true}; break;
        case 0x11:   // PUSH
            if (m == 0) { Copy(projStack, proj); projSP = (projSP + 1) & 1; }
            else if (m == 3) { Copy(texStack, tex); texSP = (texSP + 1) & 1; }
            else
            {
                Copy(posStack[posSP & 31], pos);
                Copy(vecStack[posSP & 31], vec);
                posSP = (posSP + 1) & 63;
            }
            break;
        case 0x12:   // POP
            if (m == 0) { projSP = (projSP - 1) & 1; Copy(proj, projStack); }
            else if (m == 3) { texSP = (texSP - 1) & 1; Copy(tex, texStack); }
            else
            {
                int32_t off = (int32_t)(p[0] << 26) >> 26;
                posSP = (posSP - off) & 63;
                Copy(pos, posStack[posSP & 31]);
                Copy(vec, vecStack[posSP & 31]);
            }
            break;
        case 0x13:   // STORE
            if (m == 0) Copy(projStack, proj);
            else if (m == 3) Copy(texStack, tex);
            else { Copy(posStack[p[0] & 31], pos); Copy(vecStack[p[0] & 31], vec); }
            break;
        case 0x14:   // RESTORE
            if (m == 0) Copy(proj, projStack);
            else if (m == 3) Copy(tex, texStack);
            else { Copy(pos, posStack[p[0] & 31]); Copy(vec, vecStack[p[0] & 31]); }
            break;
        case 0x15:   // IDENTITY
            if (m == 0) Identity(proj);
            else if (m == 3) Identity(tex);
            else { Identity(pos); if (m == 2) Identity(vec); }
            break;
        case 0x16: case 0x17:   // LOAD_4x4, LOAD_4x3
            if (m == 0) Load(proj, p, c == 0x16);
            else if (m == 3) Load(tex, p, c == 0x16);
            else { Load(pos, p, c == 0x16); if (m == 2) Load(vec, p, c == 0x16); }
            break;
        case 0x18: case 0x19: case 0x1A:   // MULT_4x4, MULT_4x3, MULT_3x3
        {
            int rows = c == 0x18 ? 4 : c == 0x19 ? 4 : 3, cols = c == 0x18 ? 4 : 3;
            if (m == 0) Mult(proj, p, rows, cols);
            else if (m == 3) Mult(tex, p, rows, cols);
            else { Mult(pos, p, rows, cols); if (m == 2) Mult(vec, p, rows, cols); }
            break;
        }
        case 0x1B:   // SCALE: never the vector matrix
            if (m == 0) Scale(proj, p);
            else if (m == 3) Scale(tex, p);
            else Scale(pos, p);
            break;
        case 0x1C:   // TRANS
            if (m == 0) Translate(proj, p);
            else if (m == 3) Translate(tex, p);
            else { Translate(pos, p); if (m == 2) Translate(vec, p); }
            break;
        case 0x20: color = {p[0] & 0x7FFF, true}; break;
        case 0x21: color.known = false; break;   // NORMAL lights the vertex color; not worked out here
        case 0x29: polyAttr = {p[0], true}; break;
        case 0x2A: texParam = {p[0], true}; break;
        case 0x2B: texPal = {p[0] & 0x1FFF, true}; break;
        case 0x30:
            difAmb = {p[0] & 0x7FFF7FFF, true};
            if (p[0] & 0x8000) color = {p[0] & 0x7FFF, true};
            break;
        case 0x31: speEmi = {p[0] & 0x7FFFFFFF, true}; break;
        case 0x32:
        {
            Light& l = light[p[0] >> 30];
            l.param = p[0];
            l.vec = vec;
            l.known = vec.known;
            if (!vec.known) unknownOps++;
            // only the 3x3 part takes part
            l.vec.m[3] = l.vec.m[7] = l.vec.m[11] = l.vec.m[12] = l.vec.m[13] = l.vec.m[14] = 0;
            l.vec.m[15] = 0x1000;
            break;
        }
        case 0x33: lightColor[p[0] >> 30] = {p[0] & 0x7FFF, true}; break;
        case 0x34:
            if (n >= 32) { memcpy(shininess, p, sizeof shininess); shininessKnown = true; }
            break;
        case 0x60: viewport = {p[0], true}; break;
        }
    }

    static bool Is4x3(const Mtx& m) { return !m.m[3] && !m.m[7] && !m.m[11] && m.m[15] == 0x1000; }

    // commands that take `s` (what the client holds) to this state; each is applied to
    // `s` as well. The matrix stack pointer moves relative to where the client's is, so
    // that's only right if `s` has the client's pointer.
    void Rebuild(GeoState& s, std::vector<GeoCmd>& out) const
    {
        auto send = [&](uint8_t c, std::vector<uint32_t> p) {
            s.Exec(c, p.data(), p.size());
            out.push_back({c, std::move(p)});
        };
        auto setMode = [&](uint32_t m) {
            if (!(s.mode.known && s.mode.v == m)) send(0x10, {m});
        };
        auto load = [&](const Mtx& m) {
            std::vector<uint32_t> p;
            if (Is4x3(m))
            {
                for (int r = 0; r < 4; r++)
                    for (int c = 0; c < 3; c++) p.push_back((uint32_t)m.m[r * 4 + c]);
                send(0x17, std::move(p));
            }
            else
            {
                p.assign((const uint32_t*)m.m, (const uint32_t*)m.m + 16);
                send(0x16, std::move(p));
            }
        };
        // the current position and vector matrices to (p, v): mode 2 sets both, mode 1 then
        // the position matrix alone
        auto loadPosVec = [&](const Mtx& p, const Mtx& v) {
            if (!(s.vec == v)) { setMode(2); load(v); }
            if (!(s.pos == p)) { setMode(1); load(p); }
        };

        // lights: their direction is turned by the vector matrix when it's set
        for (int l = 0; l < 4; l++)
            if (light[l].known && !(s.light[l] == light[l]))
            {
                if (!(s.vec == light[l].vec)) { setMode(2); load(light[l].vec); }
                send(0x32, {light[l].param});
            }
        // stack entries the client doesn't have
        for (int i = 0; i < 32; i++)
            if ((posStack[i].known || vecStack[i].known) && !(s.posStack[i] == posStack[i] && s.vecStack[i] == vecStack[i]))
            {
                if (!posStack[i].known || !vecStack[i].known) continue;   // half known: can't be set alone
                loadPosVec(posStack[i], vecStack[i]);
                if (!s.mode.known || (s.mode.v & 3) == 0 || (s.mode.v & 3) == 3) setMode(1);
                send(0x13, {(uint32_t)i});
            }
        if (projStack.known && !(s.projStack == projStack))
        {
            if (!(s.proj == projStack)) { setMode(0); load(projStack); }
            setMode(0);
            send(0x13, {0});
        }
        if (texStack.known && !(s.texStack == texStack))
        {
            if (!(s.tex == texStack)) { setMode(3); load(texStack); }
            setMode(3);
            send(0x13, {0});
        }
        // the stack pointers, with POP (it changes the current matrices, set below)
        if (s.posSP != posSP)
        {
            if (!s.mode.known || (s.mode.v & 3) == 0 || (s.mode.v & 3) == 3) setMode(1);
            send(0x12, {(uint32_t)(s.posSP - posSP) & 63});
        }
        if (s.projSP != projSP) { setMode(0); send(0x12, {1}); }
        if (s.texSP != texSP) { setMode(3); send(0x12, {1}); }
        // the current matrices
        if (proj.known && !(s.proj == proj)) { setMode(0); load(proj); }
        if (tex.known && !(s.tex == tex)) { setMode(3); load(tex); }
        if ((pos.known || vec.known) && !(s.pos == pos && s.vec == vec) && pos.known && vec.known)
            loadPosVec(pos, vec);
        if (mode.known && !(s.mode == mode)) send(0x10, {mode.v});
        // the rest: whatever was set last
        auto set = [&](const Val& v, const Val& have, uint8_t c, uint32_t param) {
            if (v.known && !(have == v)) send(c, {param});
        };
        set(polyAttr, s.polyAttr, 0x29, polyAttr.v);
        set(texParam, s.texParam, 0x2A, texParam.v);
        set(texPal, s.texPal, 0x2B, texPal.v);
        set(viewport, s.viewport, 0x60, viewport.v);
        set(difAmb, s.difAmb, 0x30, difAmb.v);   // bit 15 clear: the vertex color is set last
        set(speEmi, s.speEmi, 0x31, speEmi.v);
        for (int l = 0; l < 4; l++)
            set(lightColor[l], s.lightColor[l], 0x33, lightColor[l].v | (uint32_t)l << 30);
        if (shininessKnown && (!s.shininessKnown || memcmp(s.shininess, shininess, sizeof shininess)))
            send(0x34, std::vector<uint32_t>(shininess, shininess + 32));
        set(color, s.color, 0x20, color.v);
    }

    // equal as far as the client's picture can tell
    bool Same(const GeoState& o) const
    {
        if (!(mode == o.mode) || !(proj == o.proj) || !(pos == o.pos) || !(vec == o.vec) || !(tex == o.tex) ||
            !(projStack == o.projStack) || !(texStack == o.texStack) || projSP != o.projSP || posSP != o.posSP ||
            texSP != o.texSP || !(difAmb == o.difAmb) || !(speEmi == o.speEmi) || !(polyAttr == o.polyAttr) ||
            !(texParam == o.texParam) || !(texPal == o.texPal) || !(viewport == o.viewport) || !(color == o.color) ||
            shininessKnown != o.shininessKnown || (shininessKnown && memcmp(shininess, o.shininess, sizeof shininess)))
            return false;
        for (int i = 0; i < 32; i++)
            if (!(posStack[i] == o.posStack[i]) || !(vecStack[i] == o.vecStack[i])) return false;
        for (int l = 0; l < 4; l++)
            if (!(light[l] == o.light[l]) || !(lightColor[l] == o.lightColor[l])) return false;
        return true;
    }
};

#endif
