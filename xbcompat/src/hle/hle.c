/*
 * High-level emulation: replacing statically linked XDK library functions.
 *
 * tools/findsigs.py locates each library function (and the globals those
 * functions reference) by matching the leak's own .lib files against the
 * XBE.  Its --map output is a list of "name address" lines, which is read
 * here.  Every public API function of a replaced library gets a jmp to our
 * implementation, or, if we have none yet, to a trap that names it.
 */
#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "../xbcompat.h"
#include "hle.h"

/* regs: where each argument arrives when some come in registers ("eax,s,ecx":
   the first in eax, the second on the stack, the third in ecx), from an LTCG
   build; NULL for a plain stdcall/fastcall/cdecl function. */
typedef struct { char *name; ULONG va; bool data; char *regs; } sym;
static sym *syms;
static size_t nsyms;

ULONG hle_lookup(const char *name)
{
    for (size_t i = 0; i < nsyms; i++)
        if (!strcmp(syms[i].name, name)) return syms[i].va;
    return 0;
}

/* Look a symbol up by its undecorated prefix, e.g. "?g_pDevice@D3D@@". */
ULONG hle_lookup_prefix(const char *prefix)
{
    size_t n = strlen(prefix);
    for (size_t i = 0; i < nsyms; i++)
        if (!strncmp(syms[i].name, prefix, n)) return syms[i].va;
    return 0;
}

static void load_map(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) fatal("cannot open HLE map %s", path);
    char line[1024], name[1000], kind[128];
    unsigned va;
    size_t cap = 0;
    while (fgets(line, sizeof(line), f)) {
        kind[0] = 0;
        if (sscanf(line, "%999s %x %127s", name, &va, kind) < 2) continue;
        if (nsyms == cap) {
            cap = cap ? cap * 2 : 1024;
            syms = realloc(syms, cap * sizeof(*syms));
        }
        syms[nsyms].name = strdup(name);
        syms[nsyms].va = va;
        syms[nsyms].data = !strcmp(kind, "data");
        syms[nsyms].regs = !strncmp(kind, "regs=", 5) ? strdup(kind + 5) : NULL;
        nsyms++;
    }
    fclose(f);
}

static void CDECLAPI trap(const char *name, void *return_address)
{
    fatal("guest called %s from %p, which is not implemented yet", name, return_address);
}

static uint8_t *code_page, *code_cur;

static void *alloc_code(size_t n)
{
    if (!code_page || code_cur + n > code_page + 65536) {
        code_page = code_cur = mmap(NULL, 65536, PROT_READ | PROT_WRITE | PROT_EXEC,
                                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (code_page == MAP_FAILED) fatal("mmap HLE code page");
    }
    void *p = code_cur;
    code_cur += (n + 15) & ~15u;
    return p;
}

static void *make_trap(const char *name)
{
    uint8_t *s = alloc_code(16);
    uint32_t n = (uint32_t)name, h = (uint32_t)trap;
    uint8_t code[] = { 0xFF, 0x34, 0x24, 0x68, 0, 0, 0, 0, 0xB8, 0, 0, 0, 0, 0xFF, 0xD0 };
    memcpy(code + 4, &n, 4);
    memcpy(code + 9, &h, 4);
    memcpy(s, code, sizeof(code));
    return s;
}

/* A thunk from a function taking some arguments in registers (spec as in
   sym.regs) to the stdcall host function `target`: it pushes every argument
   in order, calls, and returns popping the caller's stack arguments. */
static void *make_reg_thunk(const char *spec, void *target)
{
    static const char *const reg_names[] = { "eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi" };
    enum { MAXARGS = 16 };
    int reg[MAXARGS], off[MAXARGS], n = 0, stack = 0;
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", spec);
    for (char *save, *t = strtok_r(buf, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
        if (n + 2 > MAXARGS) return NULL;
        if (!strcmp(t, "s") || !strcmp(t, "s2")) {
            for (int k = 0; k < (t[1] ? 2 : 1); k++) { reg[n] = -1; off[n++] = stack; stack += 4; }
            continue;
        }
        int r = -1;
        for (int k = 0; k < 8; k++) if (!strcmp(t, reg_names[k]) && k != 4) r = k;
        if (r < 0) return NULL;
        reg[n] = r; off[n++] = 0;
    }
    uint8_t *code = alloc_code(16 + n * 8), *c = code;
    for (int i = n - 1, pushed = 0; i >= 0; i--, pushed++) {
        if (reg[i] >= 0) {
            *c++ = 0x50 + reg[i];                        /* push reg */
        } else {
            uint32_t d = 4 + off[i] + 4 * pushed;        /* past the return address and our pushes */
            *c++ = 0xFF; *c++ = 0xB4; *c++ = 0x24;       /* push dword [esp + d] */
            memcpy(c, &d, 4); c += 4;
        }
    }
    int32_t rel = (int32_t)((uint32_t)target - ((uint32_t)c + 5));
    *c++ = 0xE8; memcpy(c, &rel, 4); c += 4;             /* call target (stdcall: pops n dwords) */
    if (stack) { *c++ = 0xC2; *c++ = stack & 0xFF; *c++ = stack >> 8; }   /* ret stack */
    else *c++ = 0xC3;
    return code;
}

static void write_jmp(ULONG at, void *target)
{
    uint8_t *p = (uint8_t *)at;
    int32_t rel = (int32_t)((uint32_t)target - (at + 5));
    p[0] = 0xE9;
    memcpy(p + 1, &rel, 4);
}

const struct hle_func *hle_find(const char *name)
{
    static const struct hle_func *const tables[] = { d3d8_funcs, xinput_funcs, dsound_funcs };
    for (unsigned t = 0; t < sizeof(tables) / sizeof(tables[0]); t++)
        for (const struct hle_func *f = tables[t]; f->name; f++)
            if (!strcmp(f->name, name)) return f;
    return NULL;
}

static bool replaced_library_api(const char *name)
{
    static const char *prefixes[] = {
        /* d3d8 */
        "_D3DDevice_", "@D3DDevice_", "_D3DResource_", "_D3DVertexBuffer_", "_D3DIndexBuffer_",
        "_D3DTexture_", "_D3DSurface_", "_D3DBaseTexture_", "_D3DCubeTexture_", "_D3DVolumeTexture_",
        "_D3DPalette_", "_D3DPushBuffer_", "_D3DFixup_", "_Direct3D", "_D3D_", "_D3DPERF_", "_XMETAL_",
        "_D3DRDI_", "_PerfGet", "_Get2DSurfaceDesc@", "_Lock2DSurface@", "_Lock3DSurface@",
        /* xapilib: the input device API sits on a USB stack we do not run */
        "_XInitDevices@", "_XGetDevices@", "_XGetDeviceChanges@", "_XInput",
        /* dsound */
        "_DirectSound", "_IDirectSound", "_XAudio", "_XWaveFile", "_XFileCreateMediaObject",
        "_Ac97CreateMediaObject@",
    };
    for (unsigned i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++)
        if (!strncmp(name, prefixes[i], strlen(prefixes[i]))) return true;
    /* dsound: the public methods of the C++ classes behind the C thunks
       ("?Method@Class@DirectSound@@QAG..." / "...@@UAG..."), since a thunk
       findsigs could not tell apart still lands in its method. */
    static const char *classes[] = {
        "@CDirectSound@DirectSound@@", "@CDirectSoundBuffer@DirectSound@@",
        "@CDirectSoundStream@DirectSound@@", "@CDirectSoundVoice@DirectSound@@",
    };
    const char *at = name[0] == '?' && name[1] != '?' ? strchr(name, '@') : NULL;
    for (unsigned i = 0; at && i < sizeof(classes) / sizeof(classes[0]); i++) {
        size_t n = strlen(classes[i]);
        if (!strncmp(at, classes[i], n) && (!strncmp(at + n, "QAG", 3) || !strncmp(at + n, "UAG", 3)))
            return true;
    }
    return false;
}

void hle_patch(xbe_image *img, const char *mapfile)
{
    (void)img;
    if (!mapfile) {
        xlog("no --hle map given: statically linked libraries run unmodified");
        return;
    }
    load_map(mapfile);
    unsigned replaced = 0, trapped = 0;
    for (size_t i = 0; i < nsyms; i++) {
        if (syms[i].data || !replaced_library_api(syms[i].name)) continue;
        const struct hle_func *h = hle_find(syms[i].name);
        /* Never patch one address twice: the names sharing an address are
           decided once, by the first of them, and must agree on the
           implementation (bodies folded by the linker); otherwise the
           placement is in doubt and the original code is left alone. */
        void *impl = h ? h->impl : NULL;
        bool first = true, clash = false;
        for (size_t j = 0; j < nsyms; j++) {
            if (j == i || syms[j].data || syms[j].va != syms[i].va || !replaced_library_api(syms[j].name))
                continue;
            if (j < i) { first = false; break; }
            const struct hle_func *o = hle_find(syms[j].name);
            if (!o) continue;
            if (!impl) impl = o->impl;
            else if (o->impl != impl) clash = true;
        }
        if (!first) continue;
        if (clash) {
            xlog("HLE: %s shares %#x with a name implemented differently; not patched",
                 syms[i].name, (unsigned)syms[i].va);
            continue;
        }
        if (impl && syms[i].regs) {
            impl = make_reg_thunk(syms[i].regs, impl);
            if (!impl) xlog("HLE: cannot read the argument registers of %s (%s)", syms[i].name, syms[i].regs);
        }
        if (impl) {
            write_jmp(syms[i].va, impl);
            replaced++;
        } else {
            write_jmp(syms[i].va, make_trap(syms[i].name));
            trapped++;
        }
    }
    xlog("HLE: %zu symbols mapped, %u library functions replaced, %u trapped", nsyms, replaced, trapped);
    d3d_bind_globals();
}
