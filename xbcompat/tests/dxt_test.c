/*
 * DXT volume decoding tests: literal blocks and expected texels.
 * Build: cc -std=gnu11 -Wall -I src/hle tests/dxt_test.c -o dxt_test
 * No display or GPU required.
 */
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "dxt_decode.h"

static void expect(const uint8_t *p, int r, int g, int b, int a)
{
    if (p[0] != r || p[1] != g || p[2] != b || p[3] != a) {
        fprintf(stderr,"got %u %u %u %u, expected %d %d %d %d\n",
                p[0],p[1],p[2],p[3],r,g,b,a);
        assert(0);
    }
}

static void block_tests(void)
{
    /* Red then green, selector row [0,1,2,3]. */
    uint8_t dxt1[8] = {0x00,0xf8, 0xe0,0x07, 0xe4,0xe4,0xe4,0xe4};
    uint8_t decoded[16][4];
    dxt_decode_block(0x0c,dxt1,decoded);
    for (int row = 0; row < 4; row++) {
        expect(decoded[row*4],255,0,0,255);
        expect(decoded[row*4+1],0,255,0,255);
        expect(decoded[row*4+2],170,85,0,255);
        expect(decoded[row*4+3],85,170,0,255);
    }

    /* Reverse endpoints: half blend and transparent-black selector. */
    const uint8_t transparent[8] = {0xe0,0x07, 0x00,0xf8, 0xe4,0xe4,0xe4,0xe4};
    dxt_decode_block(0x0c,transparent,decoded);
    expect(decoded[0],0,255,0,255);
    expect(decoded[1],255,0,0,255);
    expect(decoded[2],127,127,0,255);
    expect(decoded[3],0,0,0,0);

    /* DXT3 color interpolation stays four-color even with reversed endpoints.
       Alpha nibbles encode 0..15, including both halves of every source byte. */
    uint8_t dxt3[16] = {0x10,0x32,0x54,0x76,0x98,0xba,0xdc,0xfe};
    memcpy(dxt3+8,transparent,8);
    dxt_decode_block(0x0e,dxt3,decoded);
    for (int i = 0; i < 16; i++) assert(decoded[i][3] == i * 17);
    expect(decoded[2],85,170,0,34);
    expect(decoded[3],170,85,0,51);

    /* Alpha selector sequence 0..7 repeated: crosses byte boundaries. */
    uint8_t dxt5[16] = {210,0, 0x88,0xc6,0xfa,0x88,0xc6,0xfa};
    memcpy(dxt5+8,transparent,8);
    static const int seven_step[8] = {210,0,180,150,120,90,60,30};
    dxt_decode_block(0x0f,dxt5,decoded);
    for (int i = 0; i < 16; i++) assert(decoded[i][3] == seven_step[i % 8]);
    expect(decoded[2],85,170,0,180);
    expect(decoded[3],170,85,0,150);
    dxt5[0] = 0; dxt5[1] = 200;
    static const int five_step[8] = {0,200,40,80,120,160,0,255};
    dxt_decode_block(0x0f,dxt5,decoded);
    for (int i = 0; i < 16; i++) assert(decoded[i][3] == five_step[i % 8]);
}

static void volume_layout_tests(void)
{
    /* Generic 8x8x5 fixture distinguishes per-tile slab order from linear
       complete slices, and checks the short final slab without padding.
       Actual Xbox texture dimensions are powers of two. */
    uint8_t packed[20*8] = {0}, rgba[8*8*5*4];
    for (int i = 0; i < 20; i++) {
        unsigned endpoint = (i + 1) << 11;
        packed[i*8] = endpoint & 255;
        packed[i*8+1] = endpoint >> 8;
    }
    static const int red[5][4] = {
        {8,41,74,107}, {16,49,82,115}, {24,57,90,123},
        {33,66,99,132}, {140,148,156,165}
    };
    assert(dxt_decode_volume(0x0c,8,8,5,packed,sizeof packed,rgba,sizeof rgba));
    for (int z = 0; z < 5; z++)
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++)
                expect(rgba+((z*8+y)*8+x)*4,red[z][(y/4)*2+x/4],0,0,255);

    /* Partial XY blocks crop the logical image, but every compressed block
       is consumed. Source order: first tile's red/green slices, then blue/white. */
    const uint8_t edge[32] = {
        0,0xf8, 0,0, 0,0,0,0, 0xe0,7, 0,0, 0,0,0,0,
        0x1f,0, 0,0, 0,0,0,0, 0xff,0xff, 0,0, 0,0,0,0
    };
    uint8_t guarded[8+5*3*2*4+8];
    memset(guarded,0xa5,sizeof guarded);
    uint8_t *out = guarded + 8;
    assert(dxt_decode_volume(0x0c,5,3,2,edge,sizeof edge,out,5*3*2*4));
    for (int z = 0; z < 2; z++)
        for (int y = 0; y < 3; y++)
            for (int x = 0; x < 5; x++) {
                int right = x == 4;
                expect(out+((z*3+y)*5+x)*4,
                       z ? (right ? 255 : 0) : (right ? 0 : 255),
                       z ? 255 : 0,right ? 255 : 0,255);
            }
    for (int i = 0; i < 8; i++) {
        assert(guarded[i] == 0xa5);
        assert(guarded[sizeof guarded-1-i] == 0xa5);
    }
}

static void validation_tests(void)
{
    size_t packed, rgba;
    assert(dxt_volume_sizes(0x0c,8,8,8,&packed,&rgba));
    assert(packed == 256 && rgba == 2048);
    assert(dxt_volume_sizes(0x0f,2,1,2,&packed,&rgba));
    assert(packed == 32 && rgba == 16);
    assert(!dxt_volume_sizes(6,4,4,4,&packed,&rgba));
    assert(!dxt_volume_sizes(0x0c,0,4,4,&packed,&rgba));
    assert(!dxt_volume_sizes(0x0c,UINT_MAX,UINT_MAX,UINT_MAX,&packed,&rgba));

    uint8_t source[8] = {0}, dest[64];
    memset(dest,0xa5,sizeof dest);
    assert(!dxt_decode_volume(0x0c,4,4,1,source,7,dest,sizeof dest));
    assert(!dxt_decode_volume(0x0c,4,4,1,source,8,dest,sizeof dest-1));
    assert(!dxt_decode_volume(0x0c,4,4,1,NULL,8,dest,sizeof dest));
    for (int i = 0; i < 64; i++) assert(dest[i] == 0xa5);
    assert(dxt_decode_volume(0x0c,1,1,1,source,8,dest,4));
    expect(dest,0,0,0,255);
}

int main(void)
{
    block_tests();
    volume_layout_tests();
    validation_tests();
    puts("DXT: color/alpha palettes, slab layout, cropped edges and size checks passed");
    return 0;
}
