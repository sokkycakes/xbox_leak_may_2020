// Just enough of libnds for the DS app's screens (mirror/ds/source/nitroui.c) to build on
// Linux: its two bitmap backgrounds are two arrays here, which the RG DS app scales onto
// its panels; the registers it sets go nowhere.
#ifndef SHIM_NDS_H
#define SHIM_NDS_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef volatile uint8_t vu8;
typedef volatile uint16_t vu16;
typedef volatile uint32_t vu32;

#define BIT(n) (1u << (n))
#define RGB15(r, g, b) ((u16)((r) | ((g) << 5) | ((b) << 10)))

// the two screens (256x256 16-bit bitmaps; 192 lines shown)
extern u16 shimTop[256 * 256], shimBottom[256 * 256];
#define BG_GFX shimTop
#define BG_GFX_SUB shimBottom

extern vu32 shimReg;
#define SHIM_REG (*(vu32*)&shimReg)
#define REG_MASTER_BRIGHT SHIM_REG
#define REG_MASTER_BRIGHT_SUB SHIM_REG
#define REG_BLDCNT SHIM_REG
#define REG_BLDCNT_SUB SHIM_REG
#define REG_MOSAIC SHIM_REG
#define REG_MOSAIC_SUB SHIM_REG
#define REG_DISPCAPCNT SHIM_REG
#define REG_POWERCNT SHIM_REG
#define REG_DISPCNT SHIM_REG
#define REG_DISPCNT_SUB SHIM_REG
#define REG_BG3CNT SHIM_REG
#define REG_BG3CNT_SUB SHIM_REG
#define REG_BG3PA SHIM_REG
#define REG_BG3PB SHIM_REG
#define REG_BG3PC SHIM_REG
#define REG_BG3PD SHIM_REG
#define REG_BG3X SHIM_REG
#define REG_BG3Y SHIM_REG
#define REG_BG3PA_SUB SHIM_REG
#define REG_BG3PB_SUB SHIM_REG
#define REG_BG3PC_SUB SHIM_REG
#define REG_BG3PD_SUB SHIM_REG
#define REG_BG3X_SUB SHIM_REG
#define REG_BG3Y_SUB SHIM_REG
#define MODE_5_2D 0
#define DISPLAY_BG3_ACTIVE 0
#define BG_BMP16_256x256 0
#define BG_BMP_BASE(n) 0
#define BG_PRIORITY(n) 0
#define IRQ_HBLANK 0
static inline void irqDisable(int i) { (void)i; }
static inline void DC_FlushRange(const void* p, u32 n) { (void)p; (void)n; }
static inline void dmaCopyWords(int ch, const void* s, void* d, u32 n) { (void)ch; memcpy(d, s, n); }
static inline void dmaFillHalfWords(u16 v, void* d, u32 n)
{
    u16* p = (u16*)d;
    for (u32 i = 0; i < n / 2; i++) p[i] = v;
}

#endif
