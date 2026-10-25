/* The VGA register file the NV2A aliases through PRMCIO/PRMVIO, without
   QEMU's VGA emulation (xbcompat-gpu).  The display size comes from the CRTC
   registers, or from xbcompat's AvSetDisplayMode when it set one. */
#ifndef XBC_VGA_INT_H
#define XBC_VGA_INT_H
#include "qemu/osdep.h"
#include "hw/display/vga_regs.h"

typedef struct VGADisplayParams {
    uint32_t line_offset;
    uint32_t start_addr;
    uint32_t line_compare;
} VGADisplayParams;

typedef struct VGACommonState {
    uint8_t sr_index, sr[256];
    uint8_t gr_index, gr[256];
    uint8_t ar_index, ar[21];
    int ar_flip_flop;
    uint8_t cr_index, cr[256];
    uint8_t msr, fcr, st00, st01;
    uint8_t dac_state, dac_sub_index, dac_read_index, dac_write_index, dac_cache[3];
    uint8_t palette[768];
    uint32_t vram_size_mb;
    int mode_width, mode_height;   /* set by AvSetDisplayMode */
    int (*get_bpp)(struct VGACommonState *s);
    void (*get_params)(struct VGACommonState *s, VGADisplayParams *params);
    void (*get_resolution)(struct VGACommonState *s, int *pwidth, int *pheight);
} VGACommonState;

uint32_t vga_ioport_read(void *opaque, uint32_t addr);
void vga_ioport_write(void *opaque, uint32_t addr, uint32_t val);
void vga_common_reset(VGACommonState *s);
static inline void vga_invalidate_scanlines(VGACommonState *s, int y1, int y2) { (void)s; (void)y1; (void)y2; }

#endif
