/* A title's own picture: the XPR texture an XBE carries in its $$XTIMAGE
   section (the dashboard's game icon), decoded to 8-bit BGRA rows for the
   Guide.  Read from the file, not the mapped image: the section is not
   preloaded, so the title may never load it or may reuse its memory. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xbcompat.h"
#pragma GCC diagnostic ignored "-Wunused-function"  /* the volume decoder */
#include "hle/dxt_decode.h"

/* XPR0: magic, total size, header size, then one D3D resource header
   (Common, Data, Lock, Format, Size); the pixels start at the header size.
   Format packs the texel format in bits 8-15 and log2 width and height in
   bits 20-23 and 24-27. */
static ULONG rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (ULONG)p[3] << 24; }

static void expand(ULONG fmt, const uint8_t *s, uint8_t *bgra)
{
    unsigned v = s[0] | s[1] << 8, r, g, b, a = 255;
    switch (fmt) {
    case 0x05: case 0x11:  /* R5G6B5 */
        r = v >> 11; g = (v >> 5) & 63; b = v & 31;
        bgra[0] = b << 3 | b >> 2; bgra[1] = g << 2 | g >> 4; bgra[2] = r << 3 | r >> 2; bgra[3] = 255;
        return;
    case 0x02: case 0x03: case 0x10: case 0x1c:  /* A1R5G5B5, X1R5G5B5 */
        r = (v >> 10) & 31; g = (v >> 5) & 31; b = v & 31;
        if ((fmt == 0x02 || fmt == 0x10) && !(v & 0x8000)) a = 0;
        bgra[0] = b << 3 | b >> 2; bgra[1] = g << 3 | g >> 2; bgra[2] = r << 3 | r >> 2; bgra[3] = a;
        return;
    case 0x04: case 0x1d:  /* A4R4G4B4 */
        bgra[0] = (v & 15) * 17; bgra[1] = ((v >> 4) & 15) * 17; bgra[2] = ((v >> 8) & 15) * 17;
        bgra[3] = (v >> 12) * 17;
        return;
    default:  /* A8R8G8B8, X8R8G8B8 */
        memcpy(bgra, s, 4);
        if (fmt == 0x07 || fmt == 0x1e) bgra[3] = 255;
    }
}

bool xbe_image_read(const char *xbe, const char *section, uint8_t **out, int *ow, int *oh)
{
    FILE *f = xbe ? fopen(xbe, "rb") : NULL;
    if (!f) return false;
    uint8_t hdr[0x1000];
    size_t n = fread(hdr, 1, sizeof hdr, f);
    uint8_t *xpr = NULL;
    ULONG size = 0;
    if (n >= 0x178 && rd32(hdr) == 0x48454258) {
        ULONG base = rd32(hdr + 0x104), nsec = rd32(hdr + 0x11c), sh = rd32(hdr + 0x120) - base;
        for (ULONG i = 0; i < nsec && sh + i * 56 + 56 <= n; i++) {
            const uint8_t *s = hdr + sh + i * 56;
            ULONG name = rd32(s + 20) - base;
            if (name >= n || strncmp((const char *)hdr + name, section, n - name)) continue;
            size = rd32(s + 16);
            if (size < 20 + 12 || size > (1u << 24)) break;
            xpr = malloc(size);
            if (fseek(f, rd32(s + 12), SEEK_SET) || fread(xpr, 1, size, f) != size) { free(xpr); xpr = NULL; }
            break;
        }
    }
    fclose(f);
    if (!xpr) return false;
    bool ok = false;
    ULONG start = rd32(xpr + 8), format = rd32(xpr + 24);
    ULONG fmt = (format >> 8) & 0xff, w = 1u << ((format >> 20) & 15), h = 1u << ((format >> 24) & 15);
    bool dxt = fmt == 0x0c || fmt == 0x0e || fmt == 0x0f;
    bool lin = fmt == 0x10 || fmt == 0x11 || fmt == 0x12 || fmt == 0x1c || fmt == 0x1d || fmt == 0x1e;
    unsigned bpp = fmt == 0x06 || fmt == 0x07 || fmt == 0x12 || fmt == 0x1e ? 4 : 2;
    bool known = dxt || lin || (fmt >= 0x02 && fmt <= 0x07);
    size_t need = dxt ? (size_t)((w + 3) / 4) * ((h + 3) / 4) * (fmt == 0x0c ? 8 : 16) : (size_t)w * h * bpp;
    if (rd32(xpr) == 0x30525058 && known && w <= 1024 && h <= 1024 && start <= size && size - start >= need) {
        const uint8_t *src = xpr + start;
        uint8_t *px = malloc((size_t)w * h * 4);
        if (dxt) {
            for (ULONG by = 0; by < h; by += 4)
                for (ULONG bx = 0; bx < w; bx += 4, src += fmt == 0x0c ? 8 : 16) {
                    uint8_t rgba[16][4];
                    dxt_decode_block(fmt, src, rgba);
                    for (int k = 0; k < 16; k++) {
                        ULONG x = bx + (k & 3), y = by + (k >> 2);
                        if (x >= w || y >= h) continue;
                        uint8_t *d = px + (y * w + x) * 4;
                        d[0] = rgba[k][2]; d[1] = rgba[k][1]; d[2] = rgba[k][0]; d[3] = rgba[k][3];
                    }
                }
        } else {
            /* Swizzled formats are in Morton order: x and y bits interleaved,
               x first, up to the smaller side. */
            ULONG xmask = 0, ymask = 0;
            for (ULONG bit = 1, i = 1; i < w || i < h; i <<= 1) {
                if (i < w) { xmask |= bit; bit <<= 1; }
                if (i < h) { ymask |= bit; bit <<= 1; }
            }
            ULONG sy = 0;
            for (ULONG y = 0; y < h; y++) {
                ULONG sx = 0;
                for (ULONG x = 0; x < w; x++) {
                    ULONG at = lin ? y * w + x : (sx | sy);
                    expand(fmt, src + at * bpp, px + (y * w + x) * 4);
                    sx = (sx - xmask) & xmask;
                }
                sy = (sy - ymask) & ymask;
            }
        }
        *out = px; *ow = (int)w; *oh = (int)h;
        ok = true;
    }
    free(xpr);
    return ok;
}
