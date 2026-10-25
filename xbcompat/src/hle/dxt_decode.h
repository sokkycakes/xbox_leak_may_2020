/*
 * DXT1/3/5 decoding for NV2A volume texture uploads.
 *
 * Block formats: Khronos EXT_texture_compression_s3tc, Appendix.
 * https://registry.khronos.org/OpenGL/extensions/EXT/EXT_texture_compression_s3tc.txt
 * Written from the format description, without reference decoder source.
 *
 * Volume block ordering follows the sanitized texture specification's
 * reference-model behavior: XY tiles within slabs of up to four Z slices,
 * with each tile's slice blocks adjacent. It is not a 3D Morton swizzle.
 */
#ifndef XBCOMPAT_DXT_DECODE_H
#define XBCOMPAT_DXT_DECODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool dxt_multiply_size(size_t a, size_t b, size_t *result)
{
    if (a && b > SIZE_MAX / a) return false;
    *result = a * b;
    return true;
}

/* Sizes describe one mip level. No source or destination memory is accessed. */
static bool dxt_volume_sizes(unsigned format, unsigned width, unsigned height,
                             unsigned depth, size_t *packed, size_t *rgba)
{
    if ((format != 0x0c && format != 0x0e && format != 0x0f) ||
        !width || !height || !depth) return false;
    size_t columns = width / 4u + (width % 4u != 0);
    size_t rows = height / 4u + (height % 4u != 0);
    size_t blocks, pixels;
    return dxt_multiply_size(columns, rows, &blocks) &&
           dxt_multiply_size(blocks, depth, &blocks) &&
           dxt_multiply_size(blocks, format == 0x0c ? 8 : 16, packed) &&
           dxt_multiply_size(width, height, &pixels) &&
           dxt_multiply_size(pixels, depth, &pixels) &&
           dxt_multiply_size(pixels, 4, rgba);
}

static void dxt_decode_block(unsigned format, const uint8_t *src, uint8_t rgba[16][4])
{
    const uint8_t *rgb = src + (format == 0x0c ? 0 : 8);
    unsigned endpoint[2] = {rgb[0] | (rgb[1] << 8), rgb[2] | (rgb[3] << 8)};
    uint8_t colors[4][4] = {{0}};
    for (int i = 0; i < 2; i++) {
        unsigned r = endpoint[i] >> 11;
        unsigned g = (endpoint[i] >> 5) & 63;
        unsigned b = endpoint[i] & 31;
        colors[i][0] = (r << 3) | (r >> 2);
        colors[i][1] = (g << 2) | (g >> 4);
        colors[i][2] = (b << 3) | (b >> 2);
        colors[i][3] = 255;
    }
    bool opaque = format != 0x0c || endpoint[0] > endpoint[1];
    for (int channel = 0; channel < 3; channel++) {
        unsigned a = colors[0][channel], b = colors[1][channel];
        colors[2][channel] = opaque ? (2 * a + b) / 3 : (a + b) / 2;
        colors[3][channel] = opaque ? (a + 2 * b) / 3 : 0;
    }
    colors[2][3] = 255;
    colors[3][3] = opaque ? 255 : 0;

    uint8_t alpha[8] = {0};
    uint64_t selectors = 0;
    if (format == 0x0f) {
        alpha[0] = src[0]; alpha[1] = src[1];
        if (alpha[0] > alpha[1]) {
            for (int i = 2; i < 8; i++)
                alpha[i] = ((8 - i) * alpha[0] + (i - 1) * alpha[1]) / 7;
        } else {
            for (int i = 2; i < 6; i++)
                alpha[i] = ((6 - i) * alpha[0] + (i - 1) * alpha[1]) / 5;
            alpha[6] = 0; alpha[7] = 255;
        }
        for (int i = 0; i < 6; i++) selectors |= (uint64_t)src[2 + i] << (8 * i);
    }
    for (unsigned pixel = 0; pixel < 16; pixel++) {
        unsigned color = (rgb[4 + pixel / 4] >> (2 * (pixel % 4))) & 3;
        memcpy(rgba[pixel], colors[color], 4);
        if (format == 0x0e)
            rgba[pixel][3] = ((src[pixel / 2] >> (4 * (pixel % 2))) & 15) * 17;
        else if (format == 0x0f)
            rgba[pixel][3] = alpha[(selectors >> (3 * pixel)) & 7];
    }
}

/* Source blocks are packed; destination is tightly packed RGBA8, X then Y
   then Z. Validate all size arithmetic and buffers before writing anything. */
static bool dxt_decode_volume(unsigned format, unsigned width, unsigned height,
                              unsigned depth, const uint8_t *src, size_t src_size,
                              uint8_t *dst, size_t dst_size)
{
    size_t packed, rgba;
    if (!src || !dst || !dxt_volume_sizes(format, width, height, depth, &packed, &rgba) ||
        src_size < packed || dst_size < rgba) return false;
    unsigned columns = width / 4u + (width % 4u != 0);
    unsigned rows = height / 4u + (height % 4u != 0);
    unsigned block_bytes = format == 0x0c ? 8 : 16;
    for (unsigned base_z = 0; base_z < depth;) {
        unsigned slices = depth - base_z < 4 ? depth - base_z : 4;
        for (unsigned by = 0; by < rows; by++) {
            for (unsigned bx = 0; bx < columns; bx++) {
                for (unsigned slice = 0; slice < slices; slice++) {
                    uint8_t block[16][4];
                    dxt_decode_block(format, src, block);
                    src += block_bytes;
                    unsigned x = bx * 4, y = by * 4;
                    unsigned span = width - x < 4 ? width - x : 4;
                    for (unsigned row = 0; row < 4 && y + row < height; row++) {
                        size_t pixel = ((size_t)(base_z + slice) * height + y + row) * width + x;
                        memcpy(dst + pixel * 4, block[row * 4], span * 4);
                    }
                }
            }
        }
        base_z += slices;
    }
    return true;
}

#endif
