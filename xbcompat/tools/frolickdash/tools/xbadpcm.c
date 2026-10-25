/* xbadpcm: 16-bit PCM WAV (mono or stereo) to an Xbox ADPCM WAV (format
 * 0x69, 64 frames per 36-byte channel block), the 4:1 format the Xbox
 * dashboard engine streams from a file (CFilePump) and xbcompat's mixer
 * decodes (src/hle/dsmix.h, which this encoder mirrors step for step).
 * The output's data chunk comes last and holds whole blocks, so a looping
 * pump wraps at a block boundary.  The tail is padded with silence to the
 * next block.
 *
 *   cc -O2 -o xbadpcm xbadpcm.c && ./xbadpcm in.wav out.wav
 *
 * Not part of Microsoft's dashboard; added for Frolick (tools/frolickdash). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static const int16_t step_tab[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88,
    97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658,
    724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660,
    4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818,
    18500, 20350, 22385, 24623, 27086, 29794, 32767
};
static const int8_t idx_adj[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

/* the decoder's step, as dsmix_adpcm_sample */
static int dec(int n, int *pred, int *idx)
{
    int step = step_tab[*idx], diff = step >> 3;
    if (n & 4) diff += step;
    if (n & 2) diff += step >> 1;
    if (n & 1) diff += step >> 2;
    if (n & 8) diff = -diff;
    int s = *pred + diff;
    if (s < -32768) s = -32768; else if (s > 32767) s = 32767;
    *pred = s;
    *idx += idx_adj[n];
    if (*idx < 0) *idx = 0; else if (*idx > 88) *idx = 88;
    return s;
}

/* the nibble whose decoded sample lands nearest the target (all 16 tried,
   so the encoder never drifts from what the decoder will produce) */
static int enc(int target, int *pred, int *idx)
{
    int best = 0, bestErr = 1 << 30;
    for (int n = 0; n < 16; n++) {
        int p = *pred, i = *idx, s = dec(n, &p, &i), e = abs(s - target);
        if (e < bestErr) { bestErr = e; best = n; }
    }
    dec(best, pred, idx);
    return best;
}

static void put16(FILE *f, int v) { fputc(v & 0xFF, f); fputc((v >> 8) & 0xFF, f); }
static void put32(FILE *f, uint32_t v) { put16(f, v & 0xFFFF); put16(f, v >> 16); }

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: xbadpcm in.wav out.wav\n"); return 2; }
    FILE *in = fopen(argv[1], "rb");
    if (!in) { perror(argv[1]); return 1; }
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, in) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) { fprintf(stderr, "not a WAV\n"); return 1; }
    int ch = 0, rate = 0, bits = 0; uint32_t dataBytes = 0; int16_t *pcm = NULL;
    for (;;) {
        uint8_t c[8];
        if (fread(c, 1, 8, in) != 8) break;
        uint32_t len = c[4] | c[5] << 8 | c[6] << 16 | (uint32_t)c[7] << 24;
        if (!memcmp(c, "fmt ", 4)) {
            uint8_t fm[16]; if (fread(fm, 1, 16, in) != 16) return 1;
            if ((fm[0] | fm[1] << 8) != 1) { fprintf(stderr, "PCM only\n"); return 1; }
            ch = fm[2] | fm[3] << 8; rate = fm[4] | fm[5] << 8 | fm[6] << 16 | fm[7] << 24; bits = fm[14] | fm[15] << 8;
            fseek(in, len - 16 + (len & 1), SEEK_CUR);
        } else if (!memcmp(c, "data", 4)) {
            dataBytes = len; pcm = malloc(len + 4);
            if (fread(pcm, 1, len, in) != len) { fprintf(stderr, "short data\n"); return 1; }
            break;
        } else fseek(in, len + (len & 1), SEEK_CUR);
    }
    fclose(in);
    if (!pcm || bits != 16 || (ch != 1 && ch != 2)) { fprintf(stderr, "16-bit mono or stereo PCM only\n"); return 1; }
    uint32_t frames = dataBytes / (2 * ch), blocks = (frames + 63) / 64;
    int align = 36 * ch; uint32_t outData = blocks * align;

    FILE *out = fopen(argv[2], "wb");
    if (!out) { perror(argv[2]); return 1; }
    fwrite("RIFF", 1, 4, out); put32(out, 4 + 8 + 20 + 8 + outData); fwrite("WAVE", 1, 4, out);
    fwrite("fmt ", 1, 4, out); put32(out, 20);
    put16(out, 0x69); put16(out, ch); put32(out, rate); put32(out, (uint32_t)((uint64_t)rate * align / 64));
    put16(out, align); put16(out, 4); put16(out, 2); put16(out, 64);
    fwrite("data", 1, 4, out); put32(out, outData);

    int pred[2] = { 0, 0 }, idx[2] = { 0, 0 };
    uint8_t blk[72];
    for (uint32_t b = 0; b < blocks; b++) {
        int s[2][64];
        for (int k = 0; k < 64; k++)
            for (int c = 0; c < ch; c++) {
                uint32_t fr = b * 64 + k;
                s[c][k] = fr < frames ? pcm[fr * ch + c] : 0;
            }
        memset(blk, 0, sizeof blk);
        for (int c = 0; c < ch; c++) {
            pred[c] = s[c][0];      /* the header sample is the block's first, exactly */
            blk[4 * c] = pred[c] & 0xFF; blk[4 * c + 1] = (pred[c] >> 8) & 0xFF; blk[4 * c + 2] = (uint8_t)idx[c];
        }
        uint8_t *p = blk + 4 * ch;
        if (ch == 1) {
            for (int k = 1; k < 64; k++) {
                int n = enc(s[0][k], &pred[0], &idx[0]);
                p[(k - 1) >> 1] |= (k & 1) ? n : n << 4;
            }
        } else {
            int k = 1;
            for (int g = 0; g < 8; g++) {
                uint32_t l = 0, r = 0;
                for (int i = 0; i < 8 && k < 64; i++, k++) {
                    l |= (uint32_t)enc(s[0][k], &pred[0], &idx[0]) << (4 * i);
                    r |= (uint32_t)enc(s[1][k], &pred[1], &idx[1]) << (4 * i);
                }
                for (int j = 0; j < 4; j++) { p[g * 8 + j] = (l >> (8 * j)) & 0xFF; p[g * 8 + 4 + j] = (r >> (8 * j)) & 0xFF; }
            }
        }
        fwrite(blk, 1, align, out);
    }
    fclose(out);
    fprintf(stderr, "%u frames, %u blocks, %u bytes of ADPCM\n", frames, blocks, outData);
    return 0;
}
