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
#include <pthread.h>

#include "../xbcompat.h"
#include "hle.h"
#include "../cpu.h"

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

#ifdef XBC_TRANSLATED
static void *make_trap(const char *name)
{
    char *msg;
    if (asprintf(&msg, "%s, which is not implemented yet", name) < 0) fatal("out of memory");
    return (void *)(uintptr_t)cpu_guest_trap(msg);
}

static void *make_reg_thunk(const char *spec, void *target)
{
    return (void *)(uintptr_t)cpu_guest_entry_regs(target, spec);
}
#else
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
#endif

#ifndef XBC_TRANSLATED
/* D3D on a stack of the title's own.  Ninja Gaiden runs its game code on
   stacks it allocates itself and switches to; the GL work behind a draw
   (llvmpipe compiles shaders with LLVM there) needs far more than those
   hold, ran off their end and wrecked the host heap.  D3D entry points go
   through a thunk that, when the caller's stack is neither the thread's
   host stack nor its spare, copies the stack arguments to the spare one,
   calls there and switches back. */
#define HLE_SPARE_STACK (1u << 20)
static __thread uintptr_t hle_stack[4];   /* host stack low end, its size, top of the spare stack, its low end */
static uint32_t hle_stack_tls;            /* hle_stack's offset from the thread pointer (gs) */

static uintptr_t thread_pointer(void)
{
    uintptr_t tp;
    __asm__("movl %%gs:0, %0" : "=r"(tp));
    return tp;
}

void hle_stack_thread_init(void)
{
    pthread_attr_t a;
    void *lo;
    size_t size;
    if (pthread_getattr_np(pthread_self(), &a)) return;
    pthread_attr_getstack(&a, &lo, &size);
    pthread_attr_destroy(&a);
    void *spare = mmap(NULL, HLE_SPARE_STACK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (spare == MAP_FAILED) return;
    hle_stack[0] = (uintptr_t)lo;
    hle_stack[1] = size;
    hle_stack[2] = (uintptr_t)spare + HLE_SPARE_STACK - 16;
    hle_stack[3] = (uintptr_t)spare;
}

/* The thunk.  The spare stack's top moves down by one frame for the call,
   so a guest callback that switches back to its own stack and calls D3D
   again gets a frame below.  A frame: the stack arguments (and a few more,
   in case the implementation takes more than the name says), the caller's
   esp, the previous top, a spot for edx and the esp to return on.  How
   much the implementation popped is measured, not assumed. */
static void *make_stack_thunk(void *impl, unsigned nargs)
{
    unsigned n = nargs + 4;
    uint32_t lo = hle_stack_tls, sz = hle_stack_tls + 4, top = hle_stack_tls + 8, base = hle_stack_tls + 12;
    uint32_t o_esp = 4 * n, o_top = o_esp + 4, o_edx = o_esp + 8, o_ret = o_esp + 12;
    uint8_t *code = alloc_code(256 + n * 13), *c = code;
#define B(...) do { const uint8_t b_[] = { __VA_ARGS__ }; memcpy(c, b_, sizeof b_); c += sizeof b_; } while (0)
#define D(v) do { uint32_t d_ = (v); memcpy(c, &d_, 4); c += 4; } while (0)
    uint8_t *to_direct[3];
    B(0x50);                                    /* push eax */
    B(0x8D, 0x44, 0x24, 0x04);                  /* lea eax, [esp+4] */
    B(0x65, 0x2B, 0x05); D(lo);                 /* sub eax, gs:[lo] */
    B(0x65, 0x3B, 0x05); D(sz);                 /* cmp eax, gs:[size] */
    B(0x0F, 0x82); to_direct[0] = c; D(0);      /* jb direct: on the host stack */
    B(0x65, 0x8B, 0x05); D(base);               /* mov eax, gs:[spare low end] */
    B(0x85, 0xC0);                              /* test eax, eax */
    B(0x0F, 0x84); to_direct[1] = c; D(0);      /* jz direct: no spare stack on this thread */
    B(0xF7, 0xD8);                              /* neg eax */
    B(0x8D, 0x44, 0x04, 0x04);                  /* lea eax, [esp+eax+4]: esp - low end */
    B(0x3D); D(HLE_SPARE_STACK);                /* cmp eax, spare size */
    B(0x0F, 0x82); to_direct[2] = c; D(0);      /* jb direct: on the spare stack already */
    B(0x51);                                    /* push ecx */
    B(0x65, 0x8B, 0x0D); D(top);                /* mov ecx, gs:[top] */
    B(0x81, 0xE9); D(o_ret + 4);                /* sub ecx, frame size */
    for (unsigned i = 0; i < n; i++) {
        B(0x8B, 0x84, 0x24); D(12 + 4 * i);     /* mov eax, [esp+12+4i] */
        B(0x89, 0x81); D(4 * i);                /* mov [ecx+4i], eax */
    }
    B(0x89, 0xA1); D(o_esp);                    /* mov [ecx+o_esp], esp */
    B(0x65, 0x8B, 0x05); D(top);                /* mov eax, gs:[top] */
    B(0x89, 0x81); D(o_top);                    /* mov [ecx+o_top], eax */
    B(0x65, 0x89, 0x0D); D(top);                /* mov gs:[top], ecx */
    B(0x8B, 0xC1);                              /* mov eax, ecx */
    B(0x94);                                    /* xchg eax, esp */
    B(0x8B, 0x08);                              /* mov ecx, [eax] */
    B(0x8B, 0x40, 0x04);                        /* mov eax, [eax+4] */
    B(0xE8); D((uint32_t)impl - ((uint32_t)c + 4));                 /* call impl */
    B(0x65, 0x8B, 0x0D); D(top);                /* mov ecx, gs:[top]: this frame */
    B(0x89, 0x91); D(o_edx);                    /* mov [ecx+o_edx], edx */
    B(0x8B, 0xD4);                              /* mov edx, esp */
    B(0x2B, 0xD1);                              /* sub edx, ecx: what the call popped */
    B(0x03, 0x91); D(o_esp);                    /* add edx, [ecx+o_esp] */
    B(0x50);                                    /* push eax */
    B(0x8B, 0x81); D(o_esp);                    /* mov eax, [ecx+o_esp] */
    B(0x8B, 0x40, 0x08);                        /* mov eax, [eax+8]: the return address */
    B(0x89, 0x42, 0x08);                        /* mov [edx+8], eax */
    B(0x58);                                    /* pop eax */
    B(0x83, 0xC2, 0x08);                        /* add edx, 8 */
    B(0x89, 0x91); D(o_ret);                    /* mov [ecx+o_ret], edx */
    B(0x8B, 0x91); D(o_top);                    /* mov edx, [ecx+o_top] */
    B(0x65, 0x89, 0x15); D(top);                /* mov gs:[top], edx */
    B(0x8B, 0x91); D(o_edx);                    /* mov edx, [ecx+o_edx] */
    B(0x8B, 0xA1); D(o_ret);                    /* mov esp, [ecx+o_ret] */
    B(0xC3);                                    /* ret */
    for (int k = 0; k < 3; k++) { uint32_t rel = (uint32_t)(c - (to_direct[k] + 4)); memcpy(to_direct[k], &rel, 4); }
    B(0x58);                                    /* direct: pop eax */
    B(0xE9); D((uint32_t)impl - ((uint32_t)c + 4));                 /* jmp impl */
    if (c > code + 256 + n * 13) fatal("HLE: stack thunk overran its space");
#undef B
#undef D
    return code;
}

/* Stack argument dwords of a D3D entry point, from its decorated name (and
   its register spec), or -1 when the name does not say. */
static int d3d_stack_args(const sym *s)
{
    const char *n = s->name;
    if (strncmp(n, "_D3D", 4) && strncmp(n, "@D3D", 4) && strncmp(n, "_Direct3D", 9)) return -1;
    const char *at = strrchr(n, '@');
    if (!at || at == n || !at[1]) return -1;
    int bytes = atoi(at + 1);
    if (s->regs) {
        int k = 0;
        for (const char *p = s->regs; *p; p++)
            if (*p == 's' && (p == s->regs || p[-1] == ',')) k += p[1] == '2' ? 2 : 1;
        return k;
    }
    if (n[0] == '@') return bytes > 8 ? (bytes - 8) / 4 : 0;
    return bytes / 4;
}
#else
void hle_stack_thread_init(void) {}
#endif

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
        "_D3DPalette_", "_D3DPushBuffer_", "_D3DFixup_", "_Direct3D", "_D3D_", "@D3D_", "_D3DPERF_", "_XMETAL_",
        "_IDirect3DVertexBuffer8_",
        "_D3DRDI_", "_PerfGet", "_Get2DSurfaceDesc@", "_Lock2DSurface@", "_Lock3DSurface@",
        "?Get2DSurfaceDesc@PixelJar@D3D@@", "?Lock2DSurface@PixelJar@D3D@@",
        /* xapilib: the input device API sits on a USB stack we do not run */
        "_XInitDevices@", "_XGetDevices@", "_XGetDeviceChanges@", "_XInput",
        "_USBD_Init@", "_XPeekDevices@", "_XGetDeviceEnumerationStatus@", "_XMountMU", "_XUnmountMU@",
        "_XMUPortFromDriveLetterA@", "_XMUSlotFromDriveLetterA@", "_XMUNameFromDriveLetter@", "_XReadMUMetaData@",
        /* dsound */
        "_DirectSound", "_IDirectSound", "_XAudio", "_XWaveFile", "_XFileCreateMediaObject",
        "_Ac97CreateMediaObject@",
    };
    for (unsigned i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++)
        if (!strncmp(name, prefixes[i], strlen(prefixes[i]))) return true;
    /* dsound: the public methods of the C++ classes behind the C thunks
       ("?Method@Class@DirectSound@@QAG..." / "...@@UAG...", static "...@@SG..."),
       since a thunk findsigs could not tell apart still lands in its method,
       and 5xxx XACT and LTCG titles call the methods themselves. */
    static const char *classes[] = {
        "@CDirectSound@DirectSound@@", "@CDirectSoundBuffer@DirectSound@@",
        "@CDirectSoundStream@DirectSound@@", "@CDirectSoundVoice@DirectSound@@",
        "@CDirectSound3DCalculator@DirectSound@@",
    };
    const char *at = name[0] == '?' && name[1] != '?' ? strchr(name, '@') : NULL;
    for (unsigned i = 0; at && i < sizeof(classes) / sizeof(classes[0]); i++) {
        size_t n = strlen(classes[i]);
        if (!strncmp(at, classes[i], n) && (!strncmp(at + n, "QAG", 3) || !strncmp(at + n, "UAG", 3) ||
                                            !strncmp(at + n, "SG", 2)))
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
#ifndef XBC_TRANSLATED
    hle_stack_tls = (uint32_t)((uintptr_t)&hle_stack[0] - thread_pointer());
#endif
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
        } else if (impl) {
            impl = (void *)(uintptr_t)cpu_guest_entry(impl);
        }
#ifndef XBC_TRANSLATED
        int nargs = impl ? d3d_stack_args(&syms[i]) : -1;
        if (nargs >= 0 && !getenv("XBCOMPAT_NO_STACK_THUNKS")) impl = make_stack_thunk(impl, (unsigned)nargs);
#endif
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
