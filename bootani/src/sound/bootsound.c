/*
 *  bootsound.c - host glue replacing the kernel's bootsound.cpp.
 *
 *  The original armed a KTIMER/KDPC that fired every 5 ms and did
 *      system_clock_music++; sos_main();
 *  Here that tick is driven by the amount of audio rendered: one tick at the
 *  start of every 240-frame (5 ms @ 48 kHz) block.
 */
#include "bootsound.h"
#include "dsound_soft.h"
#include "sos.h"
#include "protos.h"

extern short system_clock_music;
extern int   dev_cleanup(void);

static int g_running;
static int g_tick_pos;      /* frames rendered since the last tick */

void BootSound_Start(void)
{
    if (g_running)
        BootSound_Stop();
    do_sos_init_return();   /* dev_init() + put_fifo(1) + process queue */
    g_tick_pos = 0;         /* dueTime 0: first tick happens immediately */
    g_running = 1;
}

void BootSound_Stop(void)
{
    if (!g_running)
        return;
    g_running = 0;
    dev_cleanup();
}

void BootSound_Restart(void)
{
    if (g_running)
        put_fifo(1);
}

void BootSound_Render(short *out, int frames)
{
    enum { BLOCK = BOOTSOUND_TICK_FRAMES };
    float tmp[2 * BLOCK];
    int done = 0;

    if (!out || frames <= 0)
        return;

    while (done < frames) {
        int n, i;

        if (g_running && g_tick_pos == 0) {
            system_clock_music++;
            sos_main();
        }

        n = BLOCK - g_tick_pos;
        if (n > frames - done)
            n = frames - done;

        dsoft_Render(tmp, n);
        for (i = 0; i < 2 * n; i++) {
            float v = tmp[i];
            long s = (long)(v >= 0.0f ? v + 0.5f : v - 0.5f);
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            out[2 * done + i] = (short)s;
        }

        done += n;
        g_tick_pos += n;
        if (g_tick_pos >= BLOCK)
            g_tick_pos = 0;
    }
}
