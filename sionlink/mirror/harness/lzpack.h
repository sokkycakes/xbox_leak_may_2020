// Packs a bundle for the live link (MIRROR_FLAG_LZ, see common/mirror_lz.h):
// a plain greedy LZ4 block encoder, enough for VRAM and 3D command data.
#ifndef LZPACK_H
#define LZPACK_H

#include <cstdint>
#include <cstring>
#include <vector>
#include "../common/mirror_format.h"
#include "../common/mirror_lz.h"

static void LzLength(std::vector<uint8_t>& out, size_t n)
{
    // what doesn't fit the token's nibble (15 already counted)
    for (; n >= 255; n -= 255) out.push_back(255);
    out.push_back((uint8_t)n);
}

static void LzSequence(std::vector<uint8_t>& out, const uint8_t* lit, size_t nLit, size_t off, size_t match)
{
    size_t ml = match ? match - 4 : 0;
    out.push_back((uint8_t)((nLit < 15 ? nLit : 15) << 4 | (ml < 15 ? ml : 15)));
    if (nLit >= 15) LzLength(out, nLit - 15);
    out.insert(out.end(), lit, lit + nLit);
    if (!match) return;
    out.push_back((uint8_t)off);
    out.push_back((uint8_t)(off >> 8));
    if (ml >= 15) LzLength(out, ml - 15);
}

static void LzPackBytes(const uint8_t* src, size_t n, std::vector<uint8_t>& out)
{
    const int bits = 15;
    std::vector<uint32_t> table(1u << bits, 0xFFFFFFFFu);
    auto load = [&](size_t i) { uint32_t v; memcpy(&v, src + i, 4); return v; };
    size_t anchor = 0, i = 0;
    // LZ4's end rules: the last 5 bytes are literals, no match starts in the last 12
    size_t limit = n > 12 ? n - 12 : 0;
    while (i < limit)
    {
        uint32_t v = load(i);
        uint32_t h = (v * 2654435761u) >> (32 - bits);
        uint32_t c = table[h];
        table[h] = (uint32_t)i;
        if (c == 0xFFFFFFFFu || i - c > 65535 || load(c) != v)
        {
            i++;
            continue;
        }
        size_t m = 4, maxM = n - 5 - i;
        while (m < maxM && src[c + m] == src[i + m]) m++;
        LzSequence(out, src + anchor, i - anchor, i - c, m);
        for (size_t k = i + 1; k < i + m && k < limit; k += 2)   // keep the table useful inside long matches
        {
            uint32_t vk = load(k);
            table[(vk * 2654435761u) >> (32 - bits)] = (uint32_t)k;
        }
        i += m;
        anchor = i;
    }
    LzSequence(out, src + anchor, n - anchor, 0, 0);
}

// The packed form of bundle b, or b itself when packing doesn't make it smaller
static std::vector<uint8_t> LzPackBundle(const std::vector<uint8_t>& b)
{
    const mirror_hdr* h = (const mirror_hdr*)b.data();
    size_t body = b.size() - sizeof(mirror_hdr);
    std::vector<uint8_t> out(sizeof(mirror_hdr) + MIRROR_LZ_HEAD);
    LzPackBytes(b.data() + sizeof(mirror_hdr), body, out);
    uint32_t packed = (uint32_t)(out.size() - sizeof(mirror_hdr) - MIRROR_LZ_HEAD);
    out.resize((out.size() + 3) & ~size_t(3), 0);
    if (out.size() >= b.size()) return b;
    mirror_hdr* o = (mirror_hdr*)out.data();
    *o = *h;
    o->size = (uint32_t)out.size();
    o->flags |= MIRROR_FLAG_LZ;
    uint32_t* w = (uint32_t*)(o + 1);
    w[0] = (uint32_t)b.size();
    w[1] = mirrorLzSum((const uint32_t*)(h + 1), (uint32_t)(body / 4));
    w[2] = packed;
    return out;
}

#endif
