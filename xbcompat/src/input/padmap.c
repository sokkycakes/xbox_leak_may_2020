/*
 * padmap: names, the profile model and the runtime.  See padmap.h and
 * docs/project-notes/xbcompat/controller-config-strategy.md.
 */
#include "padmap.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- names --------------------------------------------------------------- */

static const char *const src_names[PM_NSRC] = {
    "left_stick", "right_stick", "dpad", "buttons", "left_trigger", "right_trigger",
};
static const char *const src_labels[PM_NSRC] = {
    "Left Stick", "Right Stick", "D-pad", "Buttons", "Left Trigger", "Right Trigger",
};
static const char *const mode_names[PM_NMODES] = {
    "default", "off", "joystick", "dpad", "buttons", "trigger", "button", "mouse",
};
static const char *const mode_labels[PM_NMODES] = {
    "Default", "Off", "Joystick", "D-pad", "Buttons", "Trigger", "Button", "Mouse",
};
static const char *const btn_names[PM_NBTN] = {
    "a", "b", "x", "y", "back", "guide", "start", "lthumb", "rthumb", "lb", "rb",
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
    "misc", "paddle1", "paddle2", "paddle3", "paddle4", "touchpad",
};
static const char *const btn_labels[PM_NBTN] = {
    "A", "B", "X", "Y", "Back", "Guide", "Start", "Left Stick Click", "Right Stick Click",
    "Left Bumper", "Right Bumper", "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right",
    "Misc", "Paddle 1", "Paddle 2", "Paddle 3", "Paddle 4", "Touchpad Click",
};
static const struct { const char *alias; int button; } btn_aliases[] = {
    { "white", PM_LB }, { "black", PM_RB }, { "left_bumper", PM_LB }, { "right_bumper", PM_RB },
    { "left_stick_click", PM_LTHUMB }, { "right_stick_click", PM_RTHUMB }, { "select", PM_BACK },
    { "up", PM_DPAD_UP }, { "down", PM_DPAD_DOWN }, { "left", PM_DPAD_LEFT }, { "right", PM_DPAD_RIGHT },
};
static const char *const ax_names[PM_NAX] = {
    "left_stick_up", "left_stick_down", "left_stick_left", "left_stick_right",
    "right_stick_up", "right_stick_down", "right_stick_left", "right_stick_right", "lt", "rt",
};
static const char *const ax_labels[PM_NAX] = {
    "Left Stick Up", "Left Stick Down", "Left Stick Left", "Left Stick Right",
    "Right Stick Up", "Right Stick Down", "Right Stick Left", "Right Stick Right",
    "Left Trigger", "Right Trigger",
};
static const char *const mouse_names[] = { "left", "right", "middle", "wheel_up", "wheel_down" };
static const char *const mouse_labels[] = { "Left Click", "Right Click", "Middle Click", "Wheel Up", "Wheel Down" };

/* Linux input-event-codes.h numbers (stable ABI). */
static const struct { const char *name; uint16_t code; } keys[] = {
    { "esc", 1 }, { "1", 2 }, { "2", 3 }, { "3", 4 }, { "4", 5 }, { "5", 6 }, { "6", 7 }, { "7", 8 },
    { "8", 9 }, { "9", 10 }, { "0", 11 }, { "minus", 12 }, { "equal", 13 }, { "backspace", 14 },
    { "tab", 15 }, { "q", 16 }, { "w", 17 }, { "e", 18 }, { "r", 19 }, { "t", 20 }, { "y", 21 },
    { "u", 22 }, { "i", 23 }, { "o", 24 }, { "p", 25 }, { "leftbrace", 26 }, { "rightbrace", 27 },
    { "enter", 28 }, { "lctrl", 29 }, { "a", 30 }, { "s", 31 }, { "d", 32 }, { "f", 33 }, { "g", 34 },
    { "h", 35 }, { "j", 36 }, { "k", 37 }, { "l", 38 }, { "semicolon", 39 }, { "apostrophe", 40 },
    { "grave", 41 }, { "lshift", 42 }, { "backslash", 43 }, { "z", 44 }, { "x", 45 }, { "c", 46 },
    { "v", 47 }, { "b", 48 }, { "n", 49 }, { "m", 50 }, { "comma", 51 }, { "dot", 52 }, { "slash", 53 },
    { "rshift", 54 }, { "lalt", 56 }, { "space", 57 }, { "capslock", 58 }, { "f1", 59 }, { "f2", 60 },
    { "f3", 61 }, { "f4", 62 }, { "f5", 63 }, { "f6", 64 }, { "f7", 65 }, { "f8", 66 }, { "f9", 67 },
    { "f10", 68 }, { "f11", 87 }, { "f12", 88 }, { "rctrl", 97 }, { "ralt", 100 }, { "home", 102 },
    { "up", 103 }, { "pageup", 104 }, { "left", 105 }, { "right", 106 }, { "end", 107 }, { "down", 108 },
    { "pagedown", 109 }, { "insert", 110 }, { "delete", 111 },
};
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

const char *padmap_source_name(int s) { return s >= 0 && s < PM_NSRC ? src_names[s] : "?"; }
const char *padmap_source_label(int s) { return s >= 0 && s < PM_NSRC ? src_labels[s] : "?"; }
const char *padmap_mode_name(int m) { return m >= 0 && m < PM_NMODES ? mode_names[m] : "?"; }
const char *padmap_mode_label(int m) { return m >= 0 && m < PM_NMODES ? mode_labels[m] : "?"; }
const char *padmap_button_name(int b) { return b >= 0 && b < PM_NBTN ? btn_names[b] : "?"; }
const char *padmap_button_label(int b) { return b >= 0 && b < PM_NBTN ? btn_labels[b] : "?"; }

int padmap_source_modes(int src, uint8_t *modes, int max)
{
    static const uint8_t stick[] = { PM_MODE_PASS, PM_MODE_JOYSTICK, PM_MODE_DPAD, PM_MODE_MOUSE, PM_MODE_OFF };
    static const uint8_t dpad[] = { PM_MODE_PASS, PM_MODE_DPAD, PM_MODE_JOYSTICK, PM_MODE_OFF };
    static const uint8_t buttons[] = { PM_MODE_PASS, PM_MODE_BUTTONS, PM_MODE_OFF };
    static const uint8_t trig[] = { PM_MODE_PASS, PM_MODE_TRIGGER, PM_MODE_BUTTON, PM_MODE_OFF };
    const uint8_t *l;
    int n;
    switch (src) {
    case PM_SRC_LSTICK: case PM_SRC_RSTICK: l = stick; n = (int)COUNT(stick); break;
    case PM_SRC_DPAD: l = dpad; n = (int)COUNT(dpad); break;
    case PM_SRC_BUTTONS: l = buttons; n = (int)COUNT(buttons); break;
    case PM_SRC_LTRIG: case PM_SRC_RTRIG: l = trig; n = (int)COUNT(trig); break;
    default: return 0;
    }
    if (n > max) n = max;
    memcpy(modes, l, (size_t)n);
    return n;
}

static int find_preset(const padmap_profile *p, const char *name)
{
    if (!p) return -1;
    for (int i = 0; i < p->npresets; i++)
        if (!strcmp(p->preset[i].name, name)) return i;
    return -1;
}

void padmap_target_name(const padmap_profile *p, uint16_t t, char *buf, size_t size)
{
    unsigned code = PM_TCODE(t);
    switch (PM_TKIND(t)) {
    case PM_T_BUTTON: snprintf(buf, size, "%s", code < PM_NBTN ? btn_names[code] : "none"); return;
    case PM_T_AXIS: snprintf(buf, size, "%s", code < PM_NAX ? ax_names[code] : "none"); return;
    case PM_T_KEY:
        for (size_t i = 0; i < COUNT(keys); i++)
            if (keys[i].code == code) { snprintf(buf, size, "key:%s", keys[i].name); return; }
        snprintf(buf, size, "key:%u", code);
        return;
    case PM_T_MOUSE: snprintf(buf, size, "mouse:%s", code < COUNT(mouse_names) ? mouse_names[code] : "left"); return;
    case PM_T_PRESET:
        if (code == PM_PRESET_NEXT || !p || (int)code >= p->npresets) snprintf(buf, size, "preset:next");
        else snprintf(buf, size, "preset:%s", p->preset[code].name);
        return;
    default: snprintf(buf, size, "none"); return;
    }
}

bool padmap_target_parse(const padmap_profile *p, const char *s, uint16_t *t)
{
    if (!s || !*s || !strcmp(s, "none")) { *t = 0; return true; }
    for (int b = 0; b < PM_NBTN; b++)
        if (!strcmp(s, btn_names[b])) { *t = PM_TARGET(PM_T_BUTTON, b); return b != PM_GUIDE; }
    for (size_t i = 0; i < COUNT(btn_aliases); i++)
        if (!strcmp(s, btn_aliases[i].alias)) { *t = PM_TARGET(PM_T_BUTTON, btn_aliases[i].button); return true; }
    for (int a = 0; a < PM_NAX; a++)
        if (!strcmp(s, ax_names[a])) { *t = PM_TARGET(PM_T_AXIS, a); return true; }
    if (!strncmp(s, "key:", 4)) {
        for (size_t i = 0; i < COUNT(keys); i++)
            if (!strcmp(s + 4, keys[i].name)) { *t = PM_TARGET(PM_T_KEY, keys[i].code); return true; }
        char *end;
        long n = strtol(s + 4, &end, 10);
        if (*end || n <= 0 || n > 0xfff) return false;
        *t = PM_TARGET(PM_T_KEY, n);
        return true;
    }
    if (!strncmp(s, "mouse:", 6)) {
        for (size_t i = 0; i < COUNT(mouse_names); i++)
            if (!strcmp(s + 6, mouse_names[i])) { *t = PM_TARGET(PM_T_MOUSE, i); return true; }
        return false;
    }
    if (!strncmp(s, "preset:", 7)) {
        if (!strcmp(s + 7, "next")) { *t = PM_TARGET(PM_T_PRESET, PM_PRESET_NEXT); return true; }
        int i = find_preset(p, s + 7);
        if (i < 0) return false;
        *t = PM_TARGET(PM_T_PRESET, i);
        return true;
    }
    return false;
}

const char *padmap_target_label(uint16_t t)
{
    static char buf[4][32];
    static int n;
    unsigned code = PM_TCODE(t);
    switch (PM_TKIND(t)) {
    case PM_T_BUTTON: return code < PM_NBTN ? btn_labels[code] : "None";
    case PM_T_AXIS: return code < PM_NAX ? ax_labels[code] : "None";
    case PM_T_MOUSE: return code < COUNT(mouse_labels) ? mouse_labels[code] : "None";
    case PM_T_PRESET: return code == PM_PRESET_NEXT ? "Next Preset" : "Switch Preset";
    case PM_T_KEY: {
        char *b = buf[n++ & 3];
        for (size_t i = 0; i < COUNT(keys); i++)
            if (keys[i].code == code) {
                snprintf(b, 32, "Key %s", keys[i].name);
                for (char *c = b + 4; *c; c++)
                    if (c == b + 4 || c[-1] == ' ') *c = (char)(*c >= 'a' && *c <= 'z' ? *c - 32 : *c);
                return b;
            }
        snprintf(b, 32, "Key %u", code);
        return b;
    }
    default: return "None";
    }
}

/* ---- the model ------------------------------------------------------------ */

void padmap_source_init(pm_source *s, int src, int mode)
{
    memset(s, 0, sizeof(*s));
    s->mode = (uint8_t)mode;
    s->outer = 1.0f;
    s->curve = 1.0f;
    s->sensitivity = 1.0f;
    s->threshold = 0.5f;
    if (src == PM_SRC_RSTICK) s->output = 1;
    if (src == PM_SRC_RTRIG) s->output = 1;
    switch (mode) {
    case PM_MODE_TRIGGER: s->threshold = 0.95f; break;     /* the full-press point */
    case PM_MODE_MOUSE: s->deadzone = 0.1f; s->curve = 1.5f; break;
    case PM_MODE_DPAD:
        if (src == PM_SRC_DPAD || src == PM_SRC_LSTICK || src == PM_SRC_RSTICK) {
            /* a fresh D-pad mode drives the D-pad, as Steam's does */
            for (int d = 0; d < 4; d++) {
                s->bind[d].set = true;
                s->bind[d].target[PM_ACT_PRESS] = PM_TARGET(PM_T_BUTTON, PM_DPAD_UP + d);
            }
        }
        break;
    case PM_MODE_BUTTON:
        s->bind[0].set = true;
        s->bind[0].target[PM_ACT_PRESS] = PM_TARGET(PM_T_AXIS, src == PM_SRC_RTRIG ? PM_AX_RT : PM_AX_LT);
        break;
    default: break;
    }
}

void padmap_profile_init(padmap_profile *p, const char *name)
{
    memset(p, 0, sizeof(*p));
    snprintf(p->name, sizeof(p->name), "%s", name ? name : "Custom");
    p->npresets = 1;
    snprintf(p->preset[0].name, sizeof(p->preset[0].name), "Default");
    for (int s = 0; s < PM_NSRC; s++) padmap_source_init(&p->preset[0].src[s], s, PM_MODE_PASS);
}

bool padmap_profile_is_identity(const padmap_profile *p)
{
    for (int i = 0; i < p->npresets; i++) {
        if (p->preset[i].nshifts) return false;
        for (int s = 0; s < PM_NSRC; s++)
            if (p->preset[i].src[s].mode != PM_MODE_PASS) return false;
    }
    return true;
}

/* ---- runtime ---------------------------------------------------------------- */

#define TAP_US 60000            /* a short press is held this long */
#define LONG_MS_DEFAULT 400
#define DOUBLE_MS_DEFAULT 250

enum { PH_IDLE, PH_WAIT_LONG, PH_SHORT, PH_LONG, PH_WAIT_DOUBLE, PH_DOUBLE };

typedef struct {
    uint8_t phase;
    bool down, toggled;
    uint16_t pulse, last_out;
    uint64_t t_down, t_up, pulse_until;
} pm_state;

#define NLAYERS (1 + PM_MAX_SHIFTS)

struct padmap {
    padmap_profile prof;
    int preset, pending_preset;
    int8_t layer[PM_NSRC];
    pm_state st[NLAYERS][PM_NSRC][PM_NSLOTS];
    uint64_t last_us;
    bool have_last;
    /* Buttons held when the layout changed count as up until released, so
       the press that switched presets doesn't press again in the new one. */
    uint32_t ignore;
    bool ignore_held;
    float mouse_fx, mouse_fy;
};

/* What the sample is building. */
typedef struct {
    uint32_t buttons;
    int32_t ax[4];              /* lx ly rx ry */
    int32_t trig[2];
    uint16_t keys[PM_MAX_KEYS];
    int nkeys;
    uint8_t mouse_buttons;
    float mouse_x, mouse_y;     /* -1..1 velocity from mouse-mode sticks */
    int wheel;
} accum;

static void put_axis(int32_t *a, int32_t v)
{
    if (abs(v) > abs(*a)) *a = v;
}

static void put_trigger(int32_t *a, int32_t v)
{
    if (v > *a) *a = v;
}

static void add_key(accum *o, uint16_t code)
{
    for (int i = 0; i < o->nkeys; i++)
        if (o->keys[i] == code) return;
    if (o->nkeys < PM_MAX_KEYS) o->keys[o->nkeys++] = code;
}

/* A target held this sample; `edge` is true on the sample it starts. */
static void emit(padmap *m, accum *o, uint16_t t, bool edge)
{
    unsigned code = PM_TCODE(t);
    switch (PM_TKIND(t)) {
    case PM_T_BUTTON:
        if (code < PM_NBTN) o->buttons |= PM_BIT(code);
        break;
    case PM_T_AXIS:
        if (code < 8) {
            int axis = (code >= 4 ? 2 : 0) + ((code & 3) >= 2 ? 0 : 1);   /* up/down: Y, left/right: X */
            int sign = (code & 1) ? 1 : -1;                                 /* down, right positive */
            put_axis(&o->ax[axis], sign * 32767);
        } else if (code == PM_AX_LT) put_trigger(&o->trig[0], 32767);
        else if (code == PM_AX_RT) put_trigger(&o->trig[1], 32767);
        break;
    case PM_T_KEY: add_key(o, (uint16_t)code); break;
    case PM_T_MOUSE:
        if (code <= PM_MS_MIDDLE) o->mouse_buttons |= (uint8_t)(1u << code);
        else if (edge) o->wheel += code == PM_MS_WHEEL_UP ? 1 : -1;
        break;
    case PM_T_PRESET:
        if (edge && m->prof.npresets > 1)
            m->pending_preset = code == PM_PRESET_NEXT ? (m->preset + 1) % m->prof.npresets
                                                       : ((int)code < m->prof.npresets ? (int)code : m->preset);
        break;
    default: break;
    }
}

/* Activators: advances one input's state and returns the target it holds. */
static uint16_t bind_step(const pm_bind *b, pm_state *s, bool down, uint64_t now)
{
    bool rise = down && !s->down, fall = !down && s->down;
    s->down = down;
    uint16_t press = b->target[PM_ACT_PRESS], lng = b->target[PM_ACT_LONG], dbl = b->target[PM_ACT_DOUBLE];
    bool has_long = PM_TKIND(lng) != PM_T_NONE, has_dbl = PM_TKIND(dbl) != PM_T_NONE;
    uint64_t long_us = (uint64_t)(b->long_ms ? b->long_ms : LONG_MS_DEFAULT) * 1000;
    uint64_t double_us = (uint64_t)(b->double_ms ? b->double_ms : DOUBLE_MS_DEFAULT) * 1000;
    uint16_t out = 0;

    if (!has_long && !has_dbl) {
        bool active;
        if (rise) s->t_down = now;
        if (b->toggle) {
            if (rise) s->toggled = !s->toggled;
            active = s->toggled;
            if (rise && s->toggled) s->t_down = now;
        } else active = down;
        if (active) {
            out = press;
            if (b->turbo_hz) {
                uint64_t period = 1000000u / b->turbo_hz;
                if ((now - s->t_down) % period >= period / 2) out = 0;
            }
        }
        return out;
    }

    if (s->phase == PH_WAIT_DOUBLE) {
        if (rise) { s->phase = PH_DOUBLE; rise = false; }
        else if (now - s->t_up >= double_us) {
            s->pulse = press;
            s->pulse_until = now + TAP_US;
            s->phase = PH_IDLE;
        }
    }
    if (s->phase == PH_IDLE && rise) {
        s->t_down = now;
        s->phase = has_long ? PH_WAIT_LONG : PH_SHORT;
    }
    if (s->phase == PH_WAIT_LONG && down && now - s->t_down >= long_us) s->phase = PH_LONG;
    if (fall) {
        if (s->phase == PH_WAIT_LONG || s->phase == PH_SHORT) {
            if (has_dbl) { s->phase = PH_WAIT_DOUBLE; s->t_up = now; }
            else {
                s->pulse = press;
                s->pulse_until = now + TAP_US;
                s->phase = PH_IDLE;
            }
        } else if (s->phase == PH_LONG || s->phase == PH_DOUBLE) s->phase = PH_IDLE;
    }
    if (s->phase == PH_LONG) out = lng;
    else if (s->phase == PH_DOUBLE) out = dbl;
    if (!out && s->pulse && now < s->pulse_until) out = s->pulse;
    if (now >= s->pulse_until) s->pulse = 0;
    return out;
}

static void run_bind(padmap *m, accum *o, const pm_bind *b, pm_state *s, bool down, uint64_t now)
{
    uint16_t t = bind_step(b, s, down, now);
    if (t) emit(m, o, t, t != s->last_out);
    s->last_out = t;
}

static float unit(int32_t v)
{
    float f = (float)v / 32767.0f;
    return f > 1 ? 1 : f < -1 ? -1 : f;
}

static int32_t raw(float f)
{
    if (f > 1) f = 1;
    if (f < -1) f = -1;
    return (int32_t)lrintf(f * 32767.0f);
}

/* Radial deadzone, outer edge, response curve and sensitivity. */
static void shape_stick(const pm_source *s, float x, float y, float *ox, float *oy)
{
    float mag = sqrtf(x * x + y * y);
    *ox = *oy = 0;
    if (mag <= s->deadzone || mag < 1e-6f) return;
    float outer = s->outer > s->deadzone ? s->outer : 1.0f;
    float r = (mag - s->deadzone) / (outer - s->deadzone);
    if (r > 1) r = 1;
    if (s->curve > 0 && s->curve != 1.0f) r = powf(r, s->curve);
    r *= s->sensitivity > 0 ? s->sensitivity : 1.0f;
    *ox = x / mag * r;
    *oy = y / mag * r;
    if (s->invert_x) *ox = -*ox;
    if (s->invert_y) *oy = -*oy;
}

static float shape_trigger(const pm_source *s, float v)
{
    if (v <= s->deadzone) return 0;
    float outer = s->outer > s->deadzone ? s->outer : 1.0f;
    float r = (v - s->deadzone) / (outer - s->deadzone);
    if (r > 1) r = 1;
    if (s->curve > 0 && s->curve != 1.0f) r = powf(r, s->curve);
    r *= s->sensitivity > 0 ? s->sensitivity : 1.0f;
    return r > 1 ? 1 : r;
}

/* The four directions of a stick (or D-pad) in dpad mode. */
static void run_dirs(padmap *m, accum *o, const pm_source *s, pm_state *st, const bool dir[4], uint64_t now)
{
    for (int d = 0; d < 4; d++)
        if (s->bind[d].set) run_bind(m, o, &s->bind[d], &st[d], dir[d], now);
}

static void run_stick(padmap *m, accum *o, const pm_source *s, pm_state *st, int which, int32_t rx, int32_t ry,
                      uint64_t now)
{
    float x = unit(rx), y = unit(ry), ox, oy;
    switch (s->mode) {
    case PM_MODE_PASS:
        put_axis(&o->ax[which * 2], rx);
        put_axis(&o->ax[which * 2 + 1], ry);
        break;
    case PM_MODE_JOYSTICK:
        shape_stick(s, x, y, &ox, &oy);
        put_axis(&o->ax[(s->output & 1) * 2], raw(ox));
        put_axis(&o->ax[(s->output & 1) * 2 + 1], raw(oy));
        break;
    case PM_MODE_DPAD: {
        float t = s->threshold > 0 ? s->threshold : 0.5f;
        bool dir[4] = { y < -t, y > t, x < -t, x > t };
        run_dirs(m, o, s, st, dir, now);
        break;
    }
    case PM_MODE_MOUSE:
        shape_stick(s, x, y, &ox, &oy);
        o->mouse_x += ox;
        o->mouse_y += oy;
        break;
    default: break;
    }
}

static void run_dpad(padmap *m, accum *o, const pm_source *s, pm_state *st, uint32_t in, uint64_t now)
{
    bool dir[4];
    for (int d = 0; d < 4; d++) dir[d] = (in & PM_BIT(PM_DPAD_UP + d)) != 0;
    switch (s->mode) {
    case PM_MODE_PASS:
        o->buttons |= in & (PM_BIT(PM_DPAD_UP) | PM_BIT(PM_DPAD_DOWN) | PM_BIT(PM_DPAD_LEFT) | PM_BIT(PM_DPAD_RIGHT));
        break;
    case PM_MODE_DPAD:
        for (int d = 0; d < 4; d++) {
            if (s->bind[d].set) run_bind(m, o, &s->bind[d], &st[d], dir[d], now);
            else if (dir[d]) o->buttons |= PM_BIT(PM_DPAD_UP + d);
        }
        break;
    case PM_MODE_JOYSTICK: {
        float x = (float)(dir[3] - dir[2]), y = (float)(dir[1] - dir[0]);
        if (x && y) { x *= 0.7071f; y *= 0.7071f; }
        float ox, oy;
        shape_stick(s, x, y, &ox, &oy);
        put_axis(&o->ax[(s->output & 1) * 2], raw(ox));
        put_axis(&o->ax[(s->output & 1) * 2 + 1], raw(oy));
        break;
    }
    default: break;
    }
}

static bool dpad_button(int b) { return b >= PM_DPAD_UP && b <= PM_DPAD_RIGHT; }

static void run_buttons(padmap *m, accum *o, const pm_source *s, pm_state *st, uint32_t in, uint64_t now)
{
    if (s->mode == PM_MODE_OFF) return;
    for (int b = 0; b < PM_NBTN; b++) {
        if (dpad_button(b) || b == PM_GUIDE) continue;
        bool down = (in & PM_BIT(b)) != 0;
        if (s->mode == PM_MODE_BUTTONS && s->bind[b].set) run_bind(m, o, &s->bind[b], &st[b], down, now);
        else if (down) o->buttons |= PM_BIT(b);
    }
}

static void run_trigger(padmap *m, accum *o, const pm_source *s, pm_state *st, int which, int32_t v,
                        uint64_t now)
{
    float f = v < 0 ? 0 : unit(v);
    switch (s->mode) {
    case PM_MODE_PASS: put_trigger(&o->trig[which], v < 0 ? 0 : v); break;
    case PM_MODE_TRIGGER:
        put_trigger(&o->trig[s->output & 1], raw(shape_trigger(s, f)));
        if (s->bind[0].set) run_bind(m, o, &s->bind[0], &st[0], f >= (s->threshold > 0 ? s->threshold : 0.95f), now);
        break;
    case PM_MODE_BUTTON:
        if (s->bind[0].set) run_bind(m, o, &s->bind[0], &st[0], f >= (s->threshold > 0 ? s->threshold : 0.5f), now);
        break;
    default: break;
    }
}

static void clear_states(padmap *m)
{
    memset(m->st, 0, sizeof(m->st));
    for (int s = 0; s < PM_NSRC; s++) m->layer[s] = 0;
    m->ignore = 0;
}

padmap *padmap_new(const padmap_profile *p)
{
    padmap *m = (padmap *)calloc(1, sizeof(*m));
    if (!m) return NULL;
    padmap_set_profile(m, p);
    return m;
}

void padmap_free(padmap *m) { free(m); }

void padmap_set_profile(padmap *m, const padmap_profile *p)
{
    if (p) m->prof = *p;
    else padmap_profile_init(&m->prof, "Standard");
    if (m->prof.npresets < 1) m->prof.npresets = 1;
    if (m->prof.npresets > PM_MAX_PRESETS) m->prof.npresets = PM_MAX_PRESETS;
    m->preset = 0;
    m->pending_preset = -1;
    m->have_last = false;
    m->mouse_fx = m->mouse_fy = 0;
    clear_states(m);
}

const padmap_profile *padmap_get_profile(const padmap *m) { return &m->prof; }
int padmap_active_preset(const padmap *m) { return m->preset; }

void padmap_reset(padmap *m)
{
    clear_states(m);
    m->have_last = false;
    m->mouse_fx = m->mouse_fy = 0;
}

void padmap_sample(padmap *m, const padmap_pad *in, uint64_t now, padmap_out *out)
{
    accum o;
    memset(&o, 0, sizeof(o));
    const pm_preset *pr = &m->prof.preset[m->preset];
    if (m->ignore_held) { m->ignore = in->buttons & ~PM_BIT(PM_GUIDE); m->ignore_held = false; }
    m->ignore &= in->buttons;
    padmap_pad eff = *in;
    eff.buttons &= ~m->ignore;
    in = &eff;

    /* The first mode shift whose button is held. */
    int shift = -1;
    for (int k = 0; k < pr->nshifts && k < PM_MAX_SHIFTS; k++)
        if (pr->shift[k].hold < PM_NBTN && (in->buttons & PM_BIT(pr->shift[k].hold))) { shift = k; break; }

    for (int src = 0; src < PM_NSRC; src++) {
        int layer = shift >= 0 && pr->shift[shift].src_set[src] ? shift + 1 : 0;
        if (layer != m->layer[src]) {
            /* the other layer lets go of what it held */
            memset(m->st[m->layer[src]][src], 0, sizeof(m->st[0][0]));
            m->layer[src] = (int8_t)layer;
        }
        const pm_source *s = layer ? &pr->shift[layer - 1].src[src] : &pr->src[src];
        pm_state *st = m->st[layer][src];
        switch (src) {
        case PM_SRC_LSTICK: run_stick(m, &o, s, st, 0, in->lx, in->ly, now); break;
        case PM_SRC_RSTICK: run_stick(m, &o, s, st, 1, in->rx, in->ry, now); break;
        case PM_SRC_DPAD: run_dpad(m, &o, s, st, in->buttons, now); break;
        case PM_SRC_BUTTONS: run_buttons(m, &o, s, st, in->buttons, now); break;
        case PM_SRC_LTRIG: run_trigger(m, &o, s, st, 0, in->lt, now); break;
        case PM_SRC_RTRIG: run_trigger(m, &o, s, st, 1, in->rt, now); break;
        }
    }
    /* The Guide button is the Guide's, whatever the profile says. */
    o.buttons = (o.buttons & ~PM_BIT(PM_GUIDE)) | (in->buttons & PM_BIT(PM_GUIDE));

    memset(out, 0, sizeof(*out));
    out->pad.buttons = o.buttons;
    out->pad.lx = (int16_t)o.ax[0];
    out->pad.ly = (int16_t)o.ax[1];
    out->pad.rx = (int16_t)o.ax[2];
    out->pad.ry = (int16_t)o.ax[3];
    out->pad.lt = (int16_t)o.trig[0];
    out->pad.rt = (int16_t)o.trig[1];
    memcpy(out->keys, o.keys, sizeof(o.keys));
    out->nkeys = o.nkeys;
    out->mouse_buttons = o.mouse_buttons;
    out->wheel = o.wheel;

    /* Mouse-mode sticks: full deflection is 1500 pixels a second. */
    float dt = m->have_last ? (float)(now - m->last_us) / 1e6f : 0;
    if (dt > 0.1f) dt = 0.1f;
    m->mouse_fx += o.mouse_x * 1500.0f * dt;
    m->mouse_fy += o.mouse_y * 1500.0f * dt;
    out->mouse_dx = (int)m->mouse_fx;
    out->mouse_dy = (int)m->mouse_fy;
    m->mouse_fx -= (float)out->mouse_dx;
    m->mouse_fy -= (float)out->mouse_dy;
    m->last_us = now;
    m->have_last = true;

    if (m->pending_preset >= 0) {
        if (m->pending_preset != m->preset) {
            m->preset = m->pending_preset;
            clear_states(m);
            m->ignore_held = true;
        }
        m->pending_preset = -1;
    }
}
