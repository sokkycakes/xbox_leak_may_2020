/* Draws a few lines with XFONT into a 32-bit buffer and writes a PPM.
   usage: xfont_demo out.ppm [font.ttf] */
#ifndef XFONT_TRUETYPE
#define XFONT_TRUETYPE
#endif
#include "xfont_portable.h"
#include <stdio.h>
#include <stdlib.h>

#define W 640
#define H 240

/* XFONT takes UTF-16 strings; turn an ASCII literal into one. */
static const WCHAR *u16(const char *s)
{
    static WCHAR buf[256];
    int i;
    for (i = 0; s[i] && i < 255; i++)
        buf[i] = (WCHAR)(unsigned char)s[i];
    buf[i] = 0;
    return buf;
}

static void line(XFONT *f, DWORD *px, D3DCOLOR color, long x, long y, const char *s)
{
    XFONT_SetTextColor(f, color);
    XFONT_TextOutToMemory(f, px, W * 4, W, H, D3DFMT_LIN_A8R8G8B8, u16(s), (unsigned)-1, x, y);
}

int main(int argc, char **argv)
{
    DWORD *px = calloc(W * H, 4);
    XFONT *def = NULL, *tt = NULL;
    unsigned width = 0;
    FILE *out;
    int i;

    for (i = 0; i < W * H; i++)
        px[i] = 0xFF202830; /* slate background */

    if (FAILED(XFONT_OpenDefaultFont(&def))) {
        fprintf(stderr, "XFONT_OpenDefaultFont failed\n");
        return 1;
    }
    XFONT_SetBkMode(def, XFONT_TRANSPARENT);
    line(def, px, 0xFFFFFFFF, 16, 12, "XFONT default bitmap font (24 px Arial)");
    XFONT_GetTextExtent(def, u16("measured"), (unsigned)-1, &width);
    printf("default font: \"measured\" is %u px wide\n", width);

    if (argc > 2) {
        WCHAR path[512];
        int k;
        for (k = 0; argv[2][k] && k < 511; k++)
            path[k] = (WCHAR)(unsigned char)argv[2][k];
        path[k] = 0;
        if (FAILED(XFONT_OpenTrueTypeFont(path, 64 * 1024, &tt))) {
            fprintf(stderr, "XFONT_OpenTrueTypeFont(%s) failed\n", argv[2]);
            return 1;
        }
        XFONT_SetBkMode(tt, XFONT_TRANSPARENT);
        XFONT_SetTextAntialiasLevel(tt, 4);
        XFONT_SetTextHeight(tt, 20);
        line(tt, px, 0xFFB0E040, 16, 56, "TrueType 20 px, antialias 4: The quick brown fox");
        XFONT_SetTextHeight(tt, 36);
        line(tt, px, 0xFFFFFFFF, 16, 90, "TrueType 36 px Hamburgefonstiv");
        XFONT_SetTextStyle(tt, XFONT_BOLD);
        XFONT_SetTextHeight(tt, 28);
        line(tt, px, 0xFFFFC040, 16, 140, "Bold style 28 px");
        XFONT_SetTextStyle(tt, XFONT_NORMAL);
        XFONT_SetBkMode(tt, XFONT_OPAQUE);
        XFONT_SetBkColor(tt, 0xFF4060A0);
        XFONT_SetTextAlignment(tt, XFONT_RIGHT | XFONT_TOP);
        line(tt, px, 0xFFFFFFFF, W - 16, 190, "Opaque, right aligned");
        XFONT_Release(tt);
    }
    XFONT_Release(def);

    out = fopen(argv[1], "wb");
    if (!out)
        return 1;
    fprintf(out, "P6\n%d %d\n255\n", W, H);
    for (i = 0; i < W * H; i++) {
        unsigned char rgb[3] = { (unsigned char)(px[i] >> 16), (unsigned char)(px[i] >> 8), (unsigned char)px[i] };
        fwrite(rgb, 1, 3, out);
    }
    fclose(out);
    return 0;
}
