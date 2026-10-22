// Packed bundles on the live link (MIRROR_FLAG_LZ). The DS takes data far slower
// than the Sion makes it when a game loads textures, and VRAM and 3D commands pack
// to a third or less. After the header, the body is:
//   u32 rawSize     the unpacked bundle's size, header included
//   u32 rawSum      mirrorLzSeal: sum of the unpacked body's words (after the header),
//                   mixed with the header's frame and flags
//   u32 packedLen   bytes of packed data that follow (then zeros to a multiple of 4)
// The packed data is the LZ4 block format (lz4.org): sequences of a token (high
// nibble literal count, low nibble match length - 4; 15 = more bytes follow, each
// added, until one isn't 255), the literals, a 2-byte little-endian offset back
// into the output, the extra match length bytes. The last sequence has literals only.
#ifndef MIRROR_LZ_H
#define MIRROR_LZ_H

#include <stdint.h>
#include <string.h>

#define MIRROR_LZ_HEAD 12   // rawSize, rawSum, packedLen

// Unpacks exactly dn bytes; 0 if the data is damaged (it never reads or writes out of bounds)
static inline int mirrorLzUnpack(const uint8_t* s, uint32_t sn, uint8_t* d, uint32_t dn)
{
    const uint8_t* se = s + sn;
    uint8_t* const d0 = d;
    uint8_t* const de = d + dn;
    while (s < se)
    {
        uint32_t t = *s++;
        uint32_t lit = t >> 4, m, b;
        if (lit == 15)
            do
            {
                if (s >= se) return 0;
                b = *s++;
                lit += b;
            } while (b == 255);
        if (lit > (uint32_t)(se - s) || lit > (uint32_t)(de - d)) return 0;
        memcpy(d, s, lit);
        d += lit;
        s += lit;
        if (s == se)
            break;  // the last sequence: literals only
        if (se - s < 2) return 0;
        uint32_t off = s[0] | (uint32_t)s[1] << 8;
        s += 2;
        if (off == 0 || off > (uint32_t)(d - d0)) return 0;
        m = (t & 15) + 4;
        if ((t & 15) == 15)
            do
            {
                if (s >= se) return 0;
                b = *s++;
                m += b;
            } while (b == 255);
        if (m > (uint32_t)(de - d)) return 0;
        const uint8_t* r = d - off;
        if (off >= 4 && !(((uintptr_t)d | (uintptr_t)r) & 3))
        {
            // aligned and at least a word apart: whole words (VRAM and 3D data mostly are)
            for (; m >= 4; m -= 4, d += 4, r += 4)
                *(uint32_t*)d = *(const uint32_t*)r;
        }
        while (m--)
            *d++ = *r++;
    }
    return d == de;
}

static inline uint32_t mirrorLzSum(const uint32_t* w, uint32_t words)
{
    uint32_t sum = 0;
    for (uint32_t i = 0; i < words; i++)
        sum += w[i] ^ (sum >> 7);
    return sum;
}

// the checksum a packed bundle carries: its body and its header's frame and flags
// (the header's size is checked against packedLen before the body is read)
static inline uint32_t mirrorLzSeal(uint32_t bodySum, uint32_t frame, uint32_t flags)
{
    return bodySum ^ (frame * 0x9E3779B1u) ^ (flags * 0x85EBCA6Bu);
}

// the size a packed bundle's header must give
static inline uint32_t mirrorLzSize(uint32_t hdrSize, uint32_t packedLen)
{
    return (hdrSize + MIRROR_LZ_HEAD + packedLen + 3) & ~3u;
}

#endif
