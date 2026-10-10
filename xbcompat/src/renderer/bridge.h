/* Same-process, 32-bit wire ABI shared by the x86 client and native renderer. */
#ifndef XBC_RENDERER_BRIDGE_H
#define XBC_RENDERER_BRIDGE_H
#include <stdint.h>
#include <stddef.h>
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error The renderer bridge requires a little-endian host
#endif
#define XBR_ABI 2u
#define XBR_PUBLIC __attribute__((visibility("default")))
_Static_assert(sizeof(void *) == 4, "renderer bridge requires 32-bit pointers");
struct xbr_call {
    uint32_t stack, ecx, edx;
    uint32_t lo, hi, fp, fret_lo, fret_hi, pop_bytes;
};
_Static_assert(sizeof(struct xbr_call) == 36, "xbr_call wire layout");
_Static_assert(offsetof(struct xbr_call, pop_bytes) == 32, "xbr_call field layout");
enum xbr_control {
    XBR_CONFIG, XBR_BIND_GLOBALS, XBR_FRAME_COUNT, XBR_VBLANK
};
struct xbr_config { int32_t trace, shot_frame, exit_frames; uint32_t shot_path; };
enum xbr_service {
    XBR_LOG, XBR_FATAL, XBR_EXIT, XBR_LOOKUP, XBR_LOOKUP_PREFIX,
    XBR_POOL_ALLOC, XBR_POOL_FREE, XBR_POOL_OWNS, XBR_ARENA_ALLOC,
    XBR_CONTIG_ALLOC, XBR_CONTIG_FREE, XBR_ALLOC_SIZE,
    XBR_FRAME_PRESENTED, XBR_FAULT_HANDLERS, XBR_AV_START, XBR_AV_HANDOVER,
    XBR_AV_PERSIST, XBR_GUEST_CALL, XBR_SET_EVENT, XBR_RESET_COMBO, XBR_RESET_CHECK
};
typedef void (*xbr_host_fn)(uint32_t service, uint32_t *args);
XBR_PUBLIC int xbr_init(uint32_t abi, xbr_host_fn host);
XBR_PUBLIC const char *xbr_name(uint32_t index);
XBR_PUBLIC void xbr_invoke(uint32_t index, struct xbr_call *call);
XBR_PUBLIC void xbr_control(uint32_t op, uint32_t *args);
#endif
