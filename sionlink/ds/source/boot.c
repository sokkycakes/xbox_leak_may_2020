// ARM9 half of booting a received .nds (see common/slboot.h).

#include <string.h>

#include <nds.h>

#include "boot.h"
#include "slboot.h"

#define RAM_START   0x02000000u
#define ARM7_AREA   0x02380000u // where the ARM7 loads; the ARM9 binary must end below it
#define ARM7_END    0x023FE000u // keep clear of the header and system area above
#define HEADER_ADDR 0x02FFFE00u

static u32 Rd32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((u32)p[3] << 24);
}

const char *BootCheck(const uint8_t *file, uint32_t size)
{
    if (size < 0x200)
        return "too small to be a .nds";
    u32 off9 = Rd32(file + 0x20), entry9 = Rd32(file + 0x24);
    u32 dest9 = Rd32(file + 0x28), size9 = Rd32(file + 0x2C);
    u32 off7 = Rd32(file + 0x30), entry7 = Rd32(file + 0x34);
    u32 dest7 = Rd32(file + 0x38), size7 = Rd32(file + 0x3C);

    if (off9 > size || size9 > size - off9 || off7 > size || size7 > size - off7)
        return "binaries run past the file";
    if (dest9 < RAM_START || dest9 + size9 > ARM7_AREA)
        return "ARM9 binary loads somewhere else";
    if (entry9 < dest9 || entry9 >= dest9 + size9)
        return "ARM9 entry outside its binary";
    if (dest7 < ARM7_AREA || dest7 + size7 > ARM7_END)
        return "ARM7 binary loads somewhere else";
    if (entry7 < dest7 || entry7 >= dest7 + size7)
        return "ARM7 entry outside its binary";
    // The ARM7 copies first, into ARM7_AREA and up; the ARM9's source must
    // not be there, and the copies run front to back so the ARM9's source
    // must not sit below its destination.
    if ((u32)file + size > ARM7_AREA || (u32)file + off9 < dest9)
        return "staging area in the wrong place";
    return NULL;
}

#define SYNC_IN() (REG_IPC_SYNC & 0xF)

// Runs from ITCM: everything in main RAM, this program included, is about
// to be overwritten. The stack is in DTCM.
ITCM_CODE ARM_CODE __attribute__((noinline, noreturn))
static void Boot9(u32 src, u32 dest, u32 words, u32 entry)
{
    while (SYNC_IN() != SLBOOT_SYNC_ARM7_COPIED)
        ;

    volatile u32 *s = (volatile u32 *)src;
    volatile u32 *d = (volatile u32 *)dest;
    for (u32 i = 0; i < words; i++)
        d[i] = s[i];

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
    while (SYNC_IN() != 0)
        ;
    REG_IPC_SYNC = 0;

    ((void (*)(void))entry)();
    for (;;)
        ;
}

static sl_boot boot_params;

void BootNds(const uint8_t *file, uint32_t size)
{
    (void)size;
    u32 off9 = Rd32(file + 0x20), entry9 = Rd32(file + 0x24);
    u32 dest9 = Rd32(file + 0x28), size9 = Rd32(file + 0x2C);

    boot_params.src7 = (u32)file + Rd32(file + 0x30);
    boot_params.entry7 = Rd32(file + 0x34);
    boot_params.dest7 = Rd32(file + 0x38);
    boot_params.size7 = Rd32(file + 0x3C);

    // The cartridge header, as a real boot leaves it, with no argv
    memcpy((void *)HEADER_ADDR, file, 0x170);
    *(volatile u32 *)(HEADER_ADDR + 0x70) = 0;

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

    Boot9((u32)file + off9, dest9, (size9 + 3) / 4, entry9);
}
