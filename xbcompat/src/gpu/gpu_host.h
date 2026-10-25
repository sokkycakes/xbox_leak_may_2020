/* xbcompat-gpu: the interface between xbcompat (gpu_glue.c) and the hosted
   xemu NV2A (nv2a_host.c).  Plain C types only, so neither side needs the
   other's headers. */
#ifndef XBC_GPU_HOST_H
#define XBC_GPU_HOST_H
#include <stdbool.h>
#include <stdint.h>

#define GPU_MMIO_BASE   0xFD000000u
#define GPU_MMIO_SIZE   0x01000000u
#define GPU_PRAMIN_OFF  0x00700000u
#define GPU_PRAMIN_SIZE 0x00100000u

struct gpu_config {
    bool trace_mmio;
};
extern struct gpu_config gpu_cfg;

/* nv2a_host.c */
void gpu_host_init(uint8_t *ram, uint64_t ram_size, uint8_t *ramin, uint64_t ramin_size);
uint32_t gpu_mmio_read(uint32_t off, unsigned size);
void gpu_mmio_write(uint32_t off, uint32_t val, unsigned size);
bool gpu_irq_asserted(void);
void gpu_set_display_mode(int width, int height, uint32_t pitch, int bpp, uint32_t framebuffer);
uint32_t gpu_crtc_start(void);
uint32_t gpu_crtc_start_writes(void);
void gpu_display_init(int width, int height, bool fullscreen);
void gpu_vblank(void);
bool gpu_display_frame(uint8_t *pixels, int *pw, int *ph);
bool gpu_poll_events(void);

/* gpu_glue.c */
void gpu_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void gpu_fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
void gpu_irq_changed(bool level);
void gpu_thread_started(const char *name);

#endif
