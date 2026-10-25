/*
 * Guest printf-style formatting for the translated builds (cpu.h): the
 * arguments are the guest's stack words, laid out as MSVC passes varargs.
 */
#include <stdio.h>
#include <string.h>

#include "cpu.h"

int xa_format(char *out, size_t size, const char *fmt, const uint32_t *args)
{
    size_t o = 0;
    unsigned a = 0;
    char spec[32], piece[512];
    if (!size) return 0;
    out[0] = 0;
    for (const char *p = fmt; *p;) {
        if (*p != '%') {
            if (o + 1 < size) out[o++] = *p;
            p++;
            continue;
        }
        const char *start = p++;
        if (*p == '%') { if (o + 1 < size) out[o++] = '%'; p++; continue; }
        while (*p && strchr("-+ #0", *p)) p++;
        if (*p == '*') { p++; } else while (*p >= '0' && *p <= '9') p++;
        if (*p == '.') { p++; if (*p == '*') p++; else while (*p >= '0' && *p <= '9') p++; }
        int wide = 0, big = 0;
        for (;;) {
            if (*p == 'l' && p[1] == 'l') { big = 1; p += 2; }
            else if (*p == 'I' && p[1] == '6' && p[2] == '4') { big = 1; p += 3; }
            else if (*p == 'h' || *p == 'l' || *p == 'w' || *p == 'L') { if (*p == 'w' || *p == 'l') wide = 1; p++; }
            else break;
        }
        char conv = *p ? *p++ : 0;
        size_t len = (size_t)(p - start);
        if (len >= sizeof(spec)) len = sizeof(spec) - 1;
        memcpy(spec, start, len);
        spec[len] = 0;
        /* Rebuild the spec without the size prefixes, then format one value. */
        char clean[32];
        size_t c = 0;
        for (size_t i = 0; i < len - 1 && c < sizeof(clean) - 4; i++)
            if (!strchr("hlwLI", spec[i]) && !(spec[i] == '6' && i && spec[i - 1] == 'I') &&
                !(spec[i] == '4' && i > 1 && spec[i - 2] == 'I'))
                clean[c++] = spec[i];
        int star = strchr(clean, '*') != NULL;
        int sw = star ? (int)args[a++] : 0;
        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c':
            if (big) {
                uint64_t v = args[a] | (uint64_t)args[a + 1] << 32;
                a += 2;
                clean[c++] = 'l'; clean[c++] = 'l'; clean[c++] = conv; clean[c] = 0;
                if (star) snprintf(piece, sizeof(piece), clean, sw, (long long)v);
                else snprintf(piece, sizeof(piece), clean, (long long)v);
            } else {
                clean[c++] = conv; clean[c] = 0;
                if (star) snprintf(piece, sizeof(piece), clean, sw, args[a++]);
                else snprintf(piece, sizeof(piece), clean, args[a++]);
            }
            break;
        case 'p':
            snprintf(piece, sizeof(piece), "%08X", args[a++]);
            break;
        case 'f': case 'g': case 'e': case 'G': case 'E': {
            double d;
            memcpy(&d, &args[a], 8);
            a += 2;
            clean[c++] = conv; clean[c] = 0;
            if (star) snprintf(piece, sizeof(piece), clean, sw, d);
            else snprintf(piece, sizeof(piece), clean, d);
            break;
        }
        case 's': case 'S': case 'Z': {
            const void *s = (const void *)(uintptr_t)args[a++];
            if (!s) { snprintf(piece, sizeof(piece), "(null)"); break; }
            if (wide || conv == 'S') {
                const uint16_t *w = s;
                size_t k = 0;
                while (w[k] && k < sizeof(piece) - 1) { piece[k] = w[k] < 0x80 ? (char)w[k] : '?'; k++; }
                piece[k] = 0;
            } else {
                clean[c++] = 's'; clean[c] = 0;
                if (star) snprintf(piece, sizeof(piece), clean, sw, (const char *)s);
                else snprintf(piece, sizeof(piece), clean, (const char *)s);
            }
            break;
        }
        default:
            snprintf(piece, sizeof(piece), "%s", spec);
        }
        for (const char *q = piece; *q; q++)
            if (o + 1 < size) out[o++] = *q;
    }
    out[o] = 0;
    return (int)o;
}
