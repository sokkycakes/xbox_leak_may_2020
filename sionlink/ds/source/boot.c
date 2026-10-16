// ARM9 half of booting a received program (see common/slboot.h).

#include <string.h>

#include <nds.h>

#include "boot.h"
#include "slboot.h"

#define RAM_START   0x02000000u
#define IWRAM7      0x037F8000u // shared WRAM (all given to the ARM7) + ARM7 RAM
#define IWRAM7_END  0x03810000u
#define HEADER_ADDR 0x02FFFE00u
#define LCDC_C      0x06840000u // VRAM C then D, as the ARM9 sees them unmapped

static u32 Rd32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24);
}

typedef struct {
    u32 entry9, dest9, size9, src9;
    u32 entry7, dest7, size7, src7;
    bool iwram7;
} layout;

static void Layout(const uint8_t *img, layout *l)
{
    l->entry9 = Rd32(img + 0x24);
    l->dest9 = Rd32(img + 0x28);
    l->size9 = Rd32(img + 0x2C);
    l->entry7 = Rd32(img + 0x34);
    l->dest7 = Rd32(img + 0x38);
    l->size7 = Rd32(img + 0x3C);
    l->src9 = (u32)img + 0x200;
    l->src7 = l->src9 + ((l->size9 + 3) & ~3u);
    l->iwram7 = l->dest7 >= IWRAM7 && l->dest7 < IWRAM7_END;
}

static bool Overlap(u32 a, u32 alen, u32 b, u32 blen)
{
    return a < b + blen && b < a + alen;
}

const char *BootCheck(const uint8_t *img, uint32_t size)
{
    if (size < 0x200)
        return "too small to be a program";
    layout l;
    Layout(img, &l);

    if (0x200 + ((l.size9 + 3) & ~3u) + l.size7 != size)
        return "image size doesn't match its header";
    if (l.dest9 < RAM_START || l.dest9 + l.size9 > SLBOOT_STAGE_END)
        return "ARM9 binary loads outside main RAM";
    if (l.entry9 < l.dest9 || l.entry9 >= l.dest9 + l.size9)
        return "ARM9 entry outside its binary";
    // The ARM9 binary is moved front to back, so it must not load above
    // where it sits (it never does; staging is above the usual 0x02000000).
    if (l.dest9 > l.src9)
        return "ARM9 binary loads too high";

    if (l.iwram7)
    {
        if (l.dest7 + l.size7 > IWRAM7_END)
            return "ARM7 binary too big for ARM7 RAM";
        if (l.size7 > SLBOOT_VRAM7_MAX)
            return "ARM7 binary too big";
    }
    else
    {
        if (l.dest7 < RAM_START || l.dest7 + l.size7 > SLBOOT_STAGE_END)
            return "ARM7 binary loads somewhere unusable";
        if (Overlap(l.dest7, l.size7, l.dest9, l.size9))
            return "ARM7 and ARM9 binaries overlap";
    }
    if (l.entry7 < l.dest7 || l.entry7 >= l.dest7 + l.size7)
        return "ARM7 entry outside its binary";
    return NULL;
}

#define SYNC_IN() (REG_IPC_SYNC & 0xF)

// Runs from ITCM: everything in main RAM, this program included, is about
// to be overwritten. The stack is in DTCM.
ITCM_CODE ARM_CODE __attribute__((noinline, noreturn))
static void Boot9(u32 src9, u32 dest9, u32 words9, u32 entry9,
                  u32 src7, u32 dest7, u32 words7)
{
    while (SYNC_IN() != SLBOOT_SYNC_ARM7_READY)
        ;

    volatile u32 *s = (volatile u32 *)src9;
    volatile u32 *d = (volatile u32 *)dest9;
    for (u32 i = 0; i < words9; i++) // dest9 <= src9: front to back is safe
        d[i] = s[i];

    if (words7)
    {
        s = (volatile u32 *)src7;
        d = (volatile u32 *)dest7;
        if (dest7 <= src7)
            for (u32 i = 0; i < words7; i++)
                d[i] = s[i];
        else
            for (u32 i = words7; i-- > 0;)
                d[i] = s[i];
    }

    // Clean and invalidate the data cache (4 segments x 32 lines), drain
    // the write buffer, invalidate the instruction cache, then turn the
    // MPU and both caches off; the new program's start-up sets them again.
    for (u32 seg = 0; seg < 4; seg++)
        for (u32 line = 0; line < 1024; line += 32)
            asm volatile("mcr p15, 0, %0, c7, c14, 2" :: "r"((seg << 30) | line) : "memory");
    asm volatile("mcr p15, 0, %0, c7, c10, 4" :: "r"(0) : "memory");
    asm volatile("mcr p15, 0, %0, c7, c5, 0" :: "r"(0) : "memory");
    u32 ctl;
    asm volatile("mrc p15, 0, %0, c1, c0, 0" : "=r"(ctl));
    ctl &= ~((1 << 0) | (1 << 2) | (1 << 12));
    asm volatile("mcr p15, 0, %0, c1, c0, 0" :: "r"(ctl) : "memory");

    REG_IPC_SYNC = SLBOOT_SYNC_ARM9_COPIED << 8;
    while (SYNC_IN() != 0) // the ARM7 has jumped
        ;
    REG_IPC_SYNC = 0;

    ((void (*)(void))entry9)();
    for (;;)
        ;
}

static sl_boot boot_params;

void BootNds(const uint8_t *img, uint32_t size)
{
    (void)size;
    layout l;
    Layout(img, &l);

    // The header, as a real boot leaves it, with no argv
    memcpy((void *)HEADER_ADDR, img, 0x170);
    *(volatile u32 *)(HEADER_ADDR + 0x70) = 0;

    boot_params.dest7 = l.dest7;
    boot_params.size7 = l.size7;
    boot_params.entry7 = l.entry7;
    boot_params.src7 = 0;
    if (l.iwram7)
    {
        // Through VRAM C+D: the ARM7 can't see main RAM's copy safely
        // while the ARM9 is moving things around, and its own RAM is
        // where the binary goes.
        VRAM_C_CR = VRAM_ENABLE | VRAM_C_LCD;
        VRAM_D_CR = VRAM_ENABLE | VRAM_D_LCD;
        memcpy((void *)LCDC_C, (const void *)l.src7, l.size7);
        VRAM_C_CR = VRAM_ENABLE | VRAM_C_ARM7_0x06000000;
        VRAM_D_CR = VRAM_ENABLE | VRAM_D_ARM7_0x06020000;
        boot_params.src7 = SLBOOT_VRAM7;
    }
    WRAM_CR = 3; // all shared WRAM to the ARM7, as after a real boot

    REG_IPC_SYNC = 0;
    DC_FlushAll();
    fifoSendValue32(FIFO_USER_01, (u32)&boot_params);

    REG_IME = 0;
    REG_IE = 0;
    REG_IF = ~0;
    for (int i = 0; i < 4; i++)
    {
        DMA_CR(i) = 0;
        TIMER_CR(i) = 0;
    }

    Boot9(l.src9, l.dest9, (l.size9 + 3) / 4, l.entry9,
          l.src7, l.dest7, l.iwram7 ? 0 : (l.size7 + 3) / 4);
}
