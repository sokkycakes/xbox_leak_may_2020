/* The renderer worker's interface to d3d8.c: state and draw calls captured
 * on the guest's thread as self-contained packets, to run later on the
 * thread that owns the GL context.  Defined in state_packet.h and
 * hud_packet.h, which d3d8.c includes.
 */
#ifndef XBCOMPAT_DRAW_PACKET_H
#define XBCOMPAT_DRAW_PACKET_H
#include <stddef.h>
#include <stdint.h>
struct xa_frame; struct xbr_draw_front; struct xbr_draw_packet; struct xbr_state_packet;
struct xbr_draw_front *d3d_state_front_create(void *);
struct xbr_state_packet *d3d_state_packet_capture(struct xbr_draw_front *, void *, struct xa_frame *);
void d3d_state_packet_commit(struct xbr_draw_front *, const struct xbr_state_packet *);
void d3d_state_packet_run(const struct xbr_state_packet *);
void d3d_state_packet_free(struct xbr_state_packet *);
size_t d3d_state_packet_size(const struct xbr_state_packet *);
unsigned d3d_state_packet_category(const struct xbr_state_packet *);

/* A front is NULL when the state can't be captured; the call then runs in order on the worker. */
struct xbr_draw_front *d3d_draw_front_create(void *);
void d3d_draw_front_free(struct xbr_draw_front *);
struct xbr_draw_packet *d3d_draw_packet_capture(const struct xbr_draw_front *, void *,
                                                struct xa_frame *);
size_t d3d_draw_packet_size(const struct xbr_draw_packet *);
void d3d_draw_packet_run(struct xbr_draw_packet *);
void d3d_draw_packet_free(struct xbr_draw_packet *);
void d3d_renderer_barrier(void);
void d3d_backend_read_pixels(uint32_t *);
void d3d_backend_stats(uint32_t *);
void d3d_backend_shutdown(void);
#endif
