/* x86 half: only entry stubs and runtime services remain translated. */
#include "../xbcompat.h"
#include "../cpu.h"
#include "../hle/hle.h"
#include "bridge.h"
#include <dlfcn.h>
#include <stdlib.h>
#include <stdatomic.h>
#ifndef XBC_NATIVE
#error The native renderer client requires the i386/Box86 runtime
#endif
extern int g_screenshot_frame, g_exit_after_frames;
extern const char *g_screenshot_path;
SIZE_T NTAPI MmQueryAllocationSize(PVOID p);
LONG NTAPI KeSetEvent(KEVENT *event, LONG increment, BOOLEAN wait);
static pthread_once_t once = PTHREAD_ONCE_INIT;
static atomic_bool backend_ready;
static int (*backend_init)(uint32_t, xbr_host_fn);
static const char *(*backend_name)(uint32_t);
static void (*backend_invoke)(uint32_t, struct xbr_call *);
static void (*backend_control)(uint32_t, uint32_t *);

/* Same thread, including reentrant callbacks. ARM must never directly call
 * an x86 function pointer: the Box86 wrapper bridges this service callback. */
static void host_service(uint32_t op, uint32_t *a)
{
    switch (op) {
    case XBR_LOG: xlog("%s", (const char *)a[0]); break;
    case XBR_FATAL: fatal("%s", (const char *)a[0]); break;
    case XBR_EXIT: xbc_exit(a[0]); break;
    case XBR_LOOKUP: a[0] = hle_lookup((const char *)a[0]); break;
    case XBR_LOOKUP_PREFIX: a[0] = hle_lookup_prefix((const char *)a[0]); break;
    case XBR_POOL_ALLOC: a[0] = (uint32_t)pool_alloc(a[0]); break;
    case XBR_POOL_FREE: pool_free((void *)a[0]); break;
    case XBR_POOL_OWNS: a[0] = pool_owns((void *)a[0]); break;
    case XBR_ARENA_ALLOC: a[0] = (uint32_t)arena_alloc(a[0], a[1]); break;
    case XBR_CONTIG_ALLOC:
        a[0] = (uint32_t)MmAllocateContiguousMemoryEx(a[0], a[1], a[2], a[3], a[4]); break;
    case XBR_CONTIG_FREE: MmFreeContiguousMemory((void *)a[0]); break;
    case XBR_ALLOC_SIZE: a[0] = MmQueryAllocationSize((void *)a[0]); break;
    case XBR_FRAME_PRESENTED: ke_frame_presented(); a[0] = g_guest_traps; break;
    case XBR_SET_EVENT: a[0] = KeSetEvent((KEVENT *)a[0], (LONG)a[1], (BOOLEAN)a[2]); break;
    case XBR_RESET_COMBO: xinput_check_reset_combo(); break;
    case XBR_RESET_CHECK: reset_check(); break;
    case XBR_FAULT_HANDLERS: install_fault_handlers(); break;
    case XBR_AV_START: av_title_starting(); break;
    case XBR_AV_HANDOVER: av_hand_over(); break;
    case XBR_AV_PERSIST: av_persist((void *)a[0], a[1], a[2]); break;
    case XBR_GUEST_CALL: {
        /* Current d3d8.c callbacks are both void cdecl(one argument).
         * Fail explicitly if a future renderer adds a different signature. */
        if (a[1] != CONV_CDECL || a[2] != 1)
            fatal("native renderer: unsupported callback convention/count %u/%u", a[1], a[2]);
        const uint32_t *args = (const uint32_t *)a[3];
        ((void (CDECLAPI *)(uint32_t))a[0])(args[0]);
        a[4] = a[5] = 0;
        break;
    }
    default: fatal("native renderer: unknown host service %u", op);
    }
}
static void load_backend(void)
{
    const char *path = getenv("XBCOMPAT_RENDERER_LIBRARY");
    if (!path || !*path) path = "libxbcompat_renderer.so.1";
    void *lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!lib) fatal("native renderer: cannot load %s: %s", path, dlerror());
#define LOAD(name) do { *(void **)(&backend_##name) = dlsym(lib, "xbr_" #name); \
    if (!backend_##name) fatal("native renderer: missing xbr_" #name); } while (0)
    LOAD(init); LOAD(name); LOAD(invoke); LOAD(control);
#undef LOAD
    int rc = backend_init(XBR_ABI, host_service);
    if (rc) fatal("native renderer: incompatible backend/adapter table (%d)", rc);
    uint32_t i;
    for (i = 0; d3d8_funcs[i].name; i++) {
        const char *name = backend_name(i);
        if (!name || strcmp(name, d3d8_funcs[i].name))
            fatal("native renderer: export mismatch at %u", i);
    }
    if (backend_name(i)) fatal("native renderer: backend has extra exports");
    xlog("D3D: native renderer bridge ABI %u, %u exports, %s", XBR_ABI, i, path);
    atomic_store_explicit(&backend_ready, true, memory_order_release);
}
static void ensure_backend(void)
{
    if (!atomic_load_explicit(&backend_ready, memory_order_acquire))
        pthread_once(&once, load_backend);
}
struct xbr_result { uint32_t lo, hi, fret_lo, fret_hi, fp, pop_bytes; };
_Static_assert(sizeof(struct xbr_result) == 24, "assembly result layout");
_Static_assert(offsetof(struct xbr_result, fp) == 16, "assembly fp offset");
_Static_assert(offsetof(struct xbr_result, pop_bytes) == 20, "assembly pop offset");
void xbr_do_call(uint32_t index, const uint32_t *stack, uint32_t ecx, uint32_t edx,
                 struct xbr_result *out)
{
    ensure_backend();
    struct xbr_call call = {.stack = (uint32_t)stack, .ecx = ecx, .edx = edx};
    backend_invoke(index, &call);
    *out = (struct xbr_result){call.lo, call.hi, call.fret_lo, call.fret_hi, call.fp, call.pop_bytes};
}
/* The kernel owns an x86 function pointer; never publish an ARM address.
 * No bridge lock is held: vblank can call the guest and reenter D3D. */
static void native_vblank(void)
{
    backend_control(XBR_VBLANK, NULL);
}
void d3d_bind_globals(void)
{
    ensure_backend();
    struct xbr_config config = {g_trace, g_screenshot_frame, g_exit_after_frames,
                                (uint32_t)g_screenshot_path};
    backend_control(XBR_CONFIG, (uint32_t *)&config);
    backend_control(XBR_BIND_GLOBALS, NULL);
    __atomic_store_n(&g_vblank_hook, native_vblank, __ATOMIC_RELEASE);
}
ULONG d3d_frame_count(void)
{
    ensure_backend();
    uint32_t a[1] = {0}; backend_control(XBR_FRAME_COUNT, a); return a[0];
}
