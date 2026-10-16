// ARM7 half of booting a received program (see common/slboot.h).

#include <string.h>

#include <nds.h>

#include "slboot.h"
#include "boot7.h"

extern const u8 boot7_stub[], boot7_stub_end[];

static volatile uint32_t boot_request;

static void BootRequest(u32 value, void *userdata)
{
    (void)userdata;
    boot_request = value;
}

void Boot7Install(void)
{
    fifoSetValue32Handler(FIFO_USER_01, BootRequest, NULL);
}

void Boot7Poll(void)
{
    if (!boot_request)
        return;

    const sl_boot *b = (const sl_boot *)boot_request;
    uint32_t src = b->src7, dest = b->dest7, size = (b->size7 + 3) & ~3u;
    uint32_t entry = b->entry7;

    REG_IME = 0;
    REG_IE = 0;
    REG_IF = ~0;
    for (int i = 0; i < 4; i++)
    {
        DMA_CR(i) = 0;
        TIMER_CR(i) = 0;
    }
    REG_SOUNDCNT = 0;

    if (src)
    {
        // The binary goes into ARM7 RAM, where this code runs: copy and
        // jump from a stub in VRAM instead.
        memcpy((void *)SLBOOT_STUB7, boot7_stub, boot7_stub_end - boot7_stub);
        ((void (*)(u32, u32, u32, u32))SLBOOT_STUB7)(src, dest, size, entry);
    }

    // The ARM9 puts our binary in main RAM; wait for it, then go.
    REG_IPC_SYNC = SLBOOT_SYNC_ARM7_READY << 8;
    while ((REG_IPC_SYNC & 0xF) != SLBOOT_SYNC_ARM9_COPIED)
        ;
    REG_IPC_SYNC = 0;
    ((void (*)(void))entry)();
    for (;;)
        ;
}
