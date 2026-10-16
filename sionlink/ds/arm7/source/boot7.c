// ARM7 half of booting a received .nds (see common/slboot.h). Runs from
// IWRAM, so copying the new ARM7 binary into main RAM can't overwrite it.

#include <nds.h>

#include "slboot.h"
#include "boot7.h"

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

static int SyncIn(void)
{
    return REG_IPC_SYNC & 0xF;
}

void Boot7Poll(void)
{
    if (!boot_request)
        return;

    const sl_boot *b = (const sl_boot *)boot_request;
    uint32_t src = b->src7, dest = b->dest7, size = b->size7, entry = b->entry7;

    REG_IME = 0;
    REG_IE = 0;
    REG_IF = ~0;
    for (int i = 0; i < 4; i++)
    {
        DMA_CR(i) = 0;
        TIMER_CR(i) = 0;
    }
    REG_SOUNDCNT = 0;

    volatile uint32_t *s = (volatile uint32_t *)src;
    volatile uint32_t *d = (volatile uint32_t *)dest;
    for (uint32_t i = 0; i < (size + 3) / 4; i++)
        d[i] = s[i];

    REG_IPC_SYNC = SLBOOT_SYNC_ARM7_COPIED << 8;
    while (SyncIn() != SLBOOT_SYNC_ARM9_COPIED)
        ;
    REG_IPC_SYNC = 0;
    while (SyncIn() != 0)
        ;

    ((void (*)(void))entry)();
    for (;;)
        ;
}
