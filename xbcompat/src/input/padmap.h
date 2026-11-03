/*
 * padmap: per-game controller remapping (Guide > Now Playing > Controller
 * Configuration).  Design: docs/project-notes/xbcompat/controller-config-strategy.md
 *
 * The data model follows Steam Input's: each physical source (a stick, the
 * D-pad, the buttons, a trigger) has a mode, its inputs carry bindings with
 * activators (press, long press, double press, turbo, toggle), a preset is a
 * whole layout, and a mode shift swaps sources while a button is held.  The
 * runtime evaluates it once per sample, the way sc-controller's mapper does:
 * diff the inputs, feed every binding its pressed state, run the timers, and
 * collect the outputs.  Written from scratch; no code from either.
 *
 * Plain C99 with no SDL or OS calls in padmap.c, so xbcompat (i386, C) and
 * kazeta-guide (x86-64, C++) build the same files.  Input and output use the
 * SDL standard controller's conventions: buttons are bits numbered like
 * SDL_GameControllerButton, stick Y is positive down, triggers are 0..32767.
 * Each host converts to what its title reads (XINPUT_GAMEPAD, a uinput pad).
 */
#ifndef PADMAP_H
#define PADMAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Buttons, numbered as SDL_GameControllerButton (SDL 2.0.14+). */
enum {
    PM_A, PM_B, PM_X, PM_Y, PM_BACK, PM_GUIDE, PM_START, PM_LTHUMB, PM_RTHUMB, PM_LB, PM_RB,
    PM_DPAD_UP, PM_DPAD_DOWN, PM_DPAD_LEFT, PM_DPAD_RIGHT,
    PM_MISC, PM_PADDLE1, PM_PADDLE2, PM_PADDLE3, PM_PADDLE4, PM_TOUCHPAD,
    PM_NBTN
};
#define PM_BIT(b) (1u << (b))

/* A pad's state: the real pad going in, the virtual one coming out. */
typedef struct {
    uint32_t buttons;               /* PM_BIT(PM_*) */
    int16_t lx, ly, rx, ry;         /* -32768..32767, Y positive down */
    int16_t lt, rt;                 /* 0..32767 */
} padmap_pad;

#define PM_MAX_KEYS 16
typedef struct {
    padmap_pad pad;
    /* Windows game cards only (kazeta-guide's keyboard and mouse) */
    uint16_t keys[PM_MAX_KEYS];     /* Linux KEY_* codes held this sample */
    int nkeys;
    uint8_t mouse_buttons;          /* bit 0 left, 1 right, 2 middle */
    int mouse_dx, mouse_dy, wheel;  /* this sample's motion, wheel ticks (up positive) */
} padmap_out;

/* ---- profile (the editable model, saved as JSON) ----------------------- */

/* Sources */
enum { PM_SRC_LSTICK, PM_SRC_RSTICK, PM_SRC_DPAD, PM_SRC_BUTTONS, PM_SRC_LTRIG, PM_SRC_RTRIG, PM_NSRC };

/* Modes.  PASS (a source the profile leaves out) passes the input through
   unchanged; OFF drops it. */
enum { PM_MODE_PASS, PM_MODE_OFF, PM_MODE_JOYSTICK, PM_MODE_DPAD, PM_MODE_BUTTONS,
       PM_MODE_TRIGGER, PM_MODE_BUTTON, PM_MODE_MOUSE, PM_NMODES };

/* Binding targets: kind in the top 4 bits, code below. */
enum { PM_T_NONE, PM_T_BUTTON, PM_T_AXIS, PM_T_KEY, PM_T_MOUSE, PM_T_PRESET };
#define PM_TARGET(kind, code) ((uint16_t)(((kind) << 12) | ((code) & 0xfff)))
#define PM_TKIND(t) ((t) >> 12)
#define PM_TCODE(t) ((t) & 0xfff)
/* PM_T_AXIS codes: a stick pushed fully one way, or a trigger pulled */
enum { PM_AX_LUP, PM_AX_LDOWN, PM_AX_LLEFT, PM_AX_LRIGHT, PM_AX_RUP, PM_AX_RDOWN, PM_AX_RLEFT, PM_AX_RRIGHT,
       PM_AX_LT, PM_AX_RT, PM_NAX };
/* PM_T_MOUSE codes */
enum { PM_MS_LEFT, PM_MS_RIGHT, PM_MS_MIDDLE, PM_MS_WHEEL_UP, PM_MS_WHEEL_DOWN };
#define PM_PRESET_NEXT 0xfff

/* Activators of one input: press (the plain binding), long press and
   double press, plus options on the press binding. */
enum { PM_ACT_PRESS, PM_ACT_LONG, PM_ACT_DOUBLE, PM_NACT };
typedef struct {
    bool set;                       /* false: the input passes through (buttons) or is unbound */
    uint16_t target[PM_NACT];
    uint16_t long_ms, double_ms;    /* 0: the defaults */
    uint8_t turbo_hz;               /* 0: off */
    bool toggle;
} pm_bind;

/* Slots of a source's bindings: the four directions of a stick or D-pad in
   dpad mode (up, down, left, right), a trigger's button (slot 0), or one per
   button (PM_* numbering) for the buttons source. */
#define PM_NSLOTS PM_NBTN

typedef struct {
    uint8_t mode;
    uint8_t output;                 /* joystick: 0 left, 1 right; trigger: 0 lt, 1 rt */
    float deadzone, outer, curve, sensitivity, threshold;
    bool invert_x, invert_y;
    pm_bind bind[PM_NSLOTS];
} pm_source;

#define PM_MAX_SHIFTS 4
#define PM_MAX_PRESETS 4
#define PM_NAME 32

typedef struct {
    uint8_t hold;                   /* the button held for it (PM_*) */
    bool src_set[PM_NSRC];          /* sources this shift replaces */
    pm_source src[PM_NSRC];
} pm_shift;

typedef struct {
    char name[PM_NAME];
    pm_source src[PM_NSRC];
    pm_shift shift[PM_MAX_SHIFTS];
    int nshifts;
} pm_preset;

typedef struct {
    char name[PM_NAME];
    pm_preset preset[PM_MAX_PRESETS];
    int npresets;
} padmap_profile;

/* An empty profile: one preset, every source passing through. */
void padmap_profile_init(padmap_profile *p, const char *name);
/* A source reset to its mode's defaults (deadzone, threshold, ...). */
void padmap_source_init(pm_source *s, int src, int mode);
/* True when the profile changes nothing (every source passes through). */
bool padmap_profile_is_identity(const padmap_profile *p);

/* JSON.  Parse returns false and writes a message to err (if given) on a
   bad file; the profile then holds whatever parsed before the error. */
bool padmap_profile_parse(padmap_profile *p, const char *json, char *err, size_t errsize);
/* Writes the profile; returns the length (like snprintf), or -1. */
int padmap_profile_write(const padmap_profile *p, char *buf, size_t size);

/* Names, for JSON and the Guide. */
const char *padmap_source_name(int src);        /* "left_stick" ... */
const char *padmap_source_label(int src);       /* "Left Stick" ... */
const char *padmap_mode_name(int mode);         /* "joystick" ... */
const char *padmap_mode_label(int mode);        /* "Joystick" ... */
const char *padmap_button_name(int button);     /* "a", "lb", "dpad_up", "paddle1" ... */
const char *padmap_button_label(int button);    /* "A", "Left Bumper" ... */
/* A target as text ("a", "left_stick_up", "key:w", "mouse:left", "preset:next",
   "none"); parse returns false on an unknown name. */
void padmap_target_name(const padmap_profile *p, uint16_t t, char *buf, size_t size);
bool padmap_target_parse(const padmap_profile *p, const char *s, uint16_t *t);
const char *padmap_target_label(uint16_t t);    /* "A", "Left Stick Up", "Key W", "None" ... */
/* Modes a source can take, in the order the Guide lists them. */
int padmap_source_modes(int src, uint8_t *modes, int max);

/* ---- runtime ------------------------------------------------------------ */

typedef struct padmap padmap;   /* a profile compiled for one pad */

padmap *padmap_new(const padmap_profile *p);
void padmap_free(padmap *m);
/* Installs another profile; held outputs are released first. */
void padmap_set_profile(padmap *m, const padmap_profile *p);
const padmap_profile *padmap_get_profile(const padmap *m);
int padmap_active_preset(const padmap *m);
/* One sample: the real pad in, the virtual pad (and keys, mouse) out.
   now_us is a monotonic clock in microseconds. */
void padmap_sample(padmap *m, const padmap_pad *in, uint64_t now_us, padmap_out *out);
/* Lets go of everything held (the Guide opened, the pad left). */
void padmap_reset(padmap *m);

/* ---- storage (padmap_store.c) -------------------------------------------
 * <dir>/<TITLEID>/selected          the active profile's name
 * <dir>/<TITLEID>/<name>.json       the title's profiles
 * <dir>/default-<type>.json         per controller family (xbox, playstation, ...)
 * <dir>/default.json                any pad, any title
 * <dir> is $XBCOMPAT_PADMAP_DIR, or <hdd>/../input (/data/xbox/input on the Sion).
 */
bool padmap_dir(char *out, size_t size, const char *hdd_root);
/* The profile for this title and pad family.  Writes the file it came from
   to `from` (or "" when nothing applies) and returns false when no file
   applies (p is then the identity profile). */
bool padmap_load_title(padmap_profile *p, const char *dir, uint32_t title_id, const char *pad_type,
                       char *from, size_t fromsize);
bool padmap_load_file(padmap_profile *p, const char *path, char *err, size_t errsize);
/* Saves p as <dir>/<TITLEID>/<p->name>.json and selects it. */
bool padmap_save_title(const padmap_profile *p, const char *dir, uint32_t title_id);
/* Removes the title's selection, so it falls back to the defaults. */
bool padmap_reset_title(const char *dir, uint32_t title_id);

/* Built-in templates. */
int padmap_template_count(bool windows);
const char *padmap_template_name(int i, bool windows);
bool padmap_template(padmap_profile *p, int i, bool windows);

#ifdef __cplusplus
}
#endif
#endif
