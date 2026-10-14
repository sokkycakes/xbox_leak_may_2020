/*
 * bootsound_wav - render the Xbox boot sound to a 48 kHz stereo 16-bit WAV.
 *
 *   bootsound_wav out.wav [seconds]      (default 8.5 s)
 */
#include <stdio.h>
#include <stdlib.h>
#include "bootsound.h"

static void put_u32(FILE *f, unsigned long v)
{
    fputc((int)(v & 0xff), f);
    fputc((int)((v >> 8) & 0xff), f);
    fputc((int)((v >> 16) & 0xff), f);
    fputc((int)((v >> 24) & 0xff), f);
}

static void put_u16(FILE *f, unsigned v)
{
    fputc((int)(v & 0xff), f);
    fputc((int)((v >> 8) & 0xff), f);
}

int main(int argc, char **argv)
{
    double seconds = 8.5;
    long total, done = 0;
    unsigned long bytes;
    short buf[2 * 1024];
    FILE *f;

    if (argc < 2) {
        fprintf(stderr, "usage: %s out.wav [seconds]\n", argv[0]);
        return 1;
    }
    if (argc > 2)
        seconds = atof(argv[2]);
    total = (long)(seconds * BOOTSOUND_SAMPLE_RATE);
    bytes = (unsigned long)total * 4u;

    f = fopen(argv[1], "wb");
    if (!f) {
        perror(argv[1]);
        return 1;
    }
    fwrite("RIFF", 1, 4, f);
    put_u32(f, 36 + bytes);
    fwrite("WAVEfmt ", 1, 8, f);
    put_u32(f, 16);
    put_u16(f, 1);                              /* PCM */
    put_u16(f, 2);                              /* stereo */
    put_u32(f, BOOTSOUND_SAMPLE_RATE);
    put_u32(f, BOOTSOUND_SAMPLE_RATE * 4);
    put_u16(f, 4);
    put_u16(f, 16);
    fwrite("data", 1, 4, f);
    put_u32(f, bytes);

    BootSound_Start();
    while (done < total) {
        int n = (int)((total - done) > 1024 ? 1024 : (total - done));
        int i;
        /* odd block size on purpose: exercises tick scheduling across calls */
        if (n > 1000)
            n = 1000;
        BootSound_Render(buf, n);
        for (i = 0; i < 2 * n; i++)
            put_u16(f, (unsigned)(unsigned short)buf[i]);
        done += n;
    }
    BootSound_Stop();

    fclose(f);
    printf("wrote %s: %ld frames (%.2f s)\n", argv[1], total, (double)total / BOOTSOUND_SAMPLE_RATE);
    return 0;
}
