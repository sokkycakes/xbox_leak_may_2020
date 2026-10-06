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

typedef struct { char *name; ULONG va; bool data; } sym;
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
    char line[1024], name[1000], kind[16];
    unsigned va;
    size_t cap = 0;
    while (fgets(line, sizeof(line), f)) {
        kind[0] = 0;
        if (sscanf(line, "%999s %x %15s", name, &va, kind) < 2) continue;
        if (nsyms == cap) {
            cap = cap ? cap * 2 : 1024;
            syms = realloc(syms, cap * sizeof(*syms));
        }
        syms[nsyms].name = strdup(name);
        syms[nsyms].va = va;
        syms[nsyms].data = !strcmp(kind, "data");
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
        "_D3DRDI_", "_PerfGet",
        /* xapilib: the input device API sits on a USB stack we do not run */
        "_XInitDevices@", "_XGetDevices@", "_XGetDeviceChanges@", "_XInput",
        /* dsound */
        "_DirectSound", "_IDirectSound", "_XAudio", "_XWaveFile", "_XFileMediaObject", "_XMediaObject",
    };
    for (unsigned i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); i++)
        if (!strncmp(name, prefixes[i], strlen(prefixes[i]))) return true;
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
        if (h) {
            write_jmp(syms[i].va, h->impl);
            replaced++;
        } else {
            write_jmp(syms[i].va, make_trap(syms[i].name));
            trapped++;
        }
    }
    xlog("HLE: %zu symbols mapped, %u library functions replaced, %u trapped", nsyms, replaced, trapped);
    d3d_bind_globals();
}
