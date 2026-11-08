/*
 * sion-groovy: mirror the Sion's screen to a MiSTer running the Groovy core.
 *
 * The Groovy core is a "network GPU": it takes frames over UDP and puts them
 * out at native 15 kHz on the MiSTer's analog port (RGB, or composite and
 * S-video with a YC cable). This tool reads whatever the Sion is scanning out
 * right now (the active KMS plane), shrinks it to 720x480 on the GPU, and
 * streams it as 480i. It never touches the Sion's own display.
 *
 * Protocol written from the Groovy core's documented behaviour (UDP 32100):
 *   INIT      [2][lz4][audio rate][audio chans][rgb mode]        -> 13-byte ACK
 *   SWITCHRES [3][f64 pclock MHz][u16 hact hbeg hend htot vact vbeg vend vtot][u8 interlace]
 *   BLIT      [7][u32 frame][u8 field][u16 vsync line][u32 lz4 size] then data
 *             in MTU-sized datagrams; pixels are B,G,R bytes.
 *   CLOSE     [1]
 *
 * Usage: sion-groovy --host 192.168.1.x [--test] [--raw] [--rgb565] [--fps N] [--frames N]
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include "lz4.h"

#define W 720
#define H 480
#define MTU 1472

int capture_init(int verbose);                 /* capture.c */
int capture_frame(uint8_t *bgr, int verbose);  /* 0 = new frame in bgr */
void capture_wait_vblank(void);
int capture_selftest(const uint8_t *rgb2880, uint8_t *bgr);

static volatile int stop;
static void on_sig(int s) { (void)s; stop = 1; }

static int sock;
static struct sockaddr_in dst;

static void send_buf(const void *p, size_t n) {
    sendto(sock, p, n, 0, (struct sockaddr *)&dst, sizeof dst);
}

static int wait_ack(int ms, uint8_t ack[13]) {
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    return recv(sock, ack, 13, 0) == 13;
}

static void drain_acks(void) {
    uint8_t b[64];
    while (recv(sock, b, sizeof b, MSG_DONTWAIT) > 0) {}
}

static void test_pattern(uint8_t *bgr, unsigned n) {
    static const uint8_t bars[8][3] = { {191,191,191},{0,191,191},{191,191,0},{0,191,0},
                                        {191,0,191},{0,0,191},{191,0,0},{0,0,0} }; /* B,G,R */
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint8_t *p = bgr + (y * W + x) * 3;
            memcpy(p, bars[x / 90], 3);
            int bx = (n * 4) % (W - 80), by = 200;
            if (x >= bx && x < bx + 80 && y >= by && y < by + 80) p[0] = p[1] = p[2] = 255;
            if (y == 0 || y == H - 1 || x == 0 || x == W - 1) p[0] = p[1] = p[2] = 255;
        }
}

int main(int argc, char **argv) {
    const char *host = NULL;
    int port = 32100, test = 0, raw = 0, verbose = 0, rgb565 = 0;
    double fps = 0;
    long frames = -1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--host") && i + 1 < argc) host = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--test")) test = 1;
        else if (!strcmp(argv[i], "--raw")) raw = 1;
        else if (!strcmp(argv[i], "--rgb565")) rgb565 = 1;
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc) fps = atof(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atol(argv[++i]);
        else if (!strcmp(argv[i], "-v")) verbose++;
        else if (!strcmp(argv[i], "--selftest-gl") && i + 2 < argc) {
            /* read a raw 2880x480 RGB file, shrink it on the GPU, write raw 720x480 BGR */
            uint8_t *in = malloc(2880 * H * 3), *o = malloc(W * H * 3);
            FILE *f = fopen(argv[i + 1], "rb");
            if (!f || fread(in, 1, 2880 * H * 3, f) != 2880 * H * 3) { fprintf(stderr, "bad input\n"); return 1; }
            fclose(f);
            if (capture_selftest(in, o)) return 1;
            f = fopen(argv[i + 2], "wb"); fwrite(o, 1, W * H * 3, f); fclose(f);
            return 0;
        }
        else { fprintf(stderr, "usage: %s --host IP [--port 32100] [--test] [--raw] [--rgb565] [--fps N] [--frames N] [-v]\n", argv[0]); return 2; }
    }
    if (!host) { fprintf(stderr, "need --host (the MiSTer's IP)\n"); return 2; }
    signal(SIGINT, on_sig); signal(SIGTERM, on_sig);

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    int sb = 4 << 20;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sb, sizeof sb);
    memset(&dst, 0, sizeof dst);
    dst.sin_family = AF_INET;
    dst.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &dst.sin_addr) != 1) { fprintf(stderr, "bad IP %s\n", host); return 2; }
    connect(sock, (struct sockaddr *)&dst, sizeof dst);

    if (!test && capture_init(verbose)) {
        fprintf(stderr, "screen capture failed; try --test to check the link alone\n");
        return 1;
    }

    uint8_t ack[13];
    int ok = 0;
    for (int tries = 0; tries < 10 && !ok; tries++) {
        uint8_t init[5] = { 2, (uint8_t)(raw ? 0 : 1), 0, 0, (uint8_t)(rgb565 ? 2 : 0) };   /* no audio */
        send_buf(init, 5);
        ok = wait_ack(200, ack);
    }
    if (!ok) { fprintf(stderr, "no answer from %s:%d - is the Groovy core running?\n", host, port); return 1; }
    fprintf(stderr, "Groovy core answered\n");

    /* CEA 720x480i: 13.5 MHz, 720 739 801 858 / 480 488 494 525.
       interlace 2 = interlaced output from a full progressive frame buffer */
    uint8_t sw[26];
    double pclk = 13.5;
    uint16_t m[8] = { 720, 739, 801, 858, 480, 488, 494, 525 };
    sw[0] = 3;
    memcpy(sw + 1, &pclk, 8);
    memcpy(sw + 9, m, 16);
    sw[25] = 2;
    send_buf(sw, 26);
    usleep(200000);

    uint8_t *bgr = malloc(W * H * 3);
    int cap = LZ4_compressBound(W * H * 3);
    uint8_t *lz = malloc(cap);
    memset(bgr, 0, W * H * 3);
    uint32_t frame = 1;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    double bytes = 0;
    long sent = 0;
    while (!stop && (frames < 0 || sent < frames)) {
        if (test) {
            test_pattern(bgr, frame);
        } else {
            capture_wait_vblank();
            if (capture_frame(bgr, verbose) < 0) break;
        }
        uint8_t hdr[12];
        uint32_t n = W * H * 3;
        if (rgb565) {   /* pack in place: B,G,R bytes -> little-endian 5:6:5 */
            uint16_t *o = (uint16_t *)bgr;
            for (int i = 0; i < W * H; i++) {
                uint8_t b = bgr[i * 3], g = bgr[i * 3 + 1], r = bgr[i * 3 + 2];
                o[i] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            }
            n = W * H * 2;
        }
        const uint8_t *payload = bgr;
        size_t hlen = 8;
        uint16_t vs = 525 / 2;
        hdr[0] = 7;
        memcpy(hdr + 1, &frame, 4);
        hdr[5] = 0;
        memcpy(hdr + 6, &vs, 2);
        if (!raw) {
            int c = LZ4_compress_default((const char *)bgr, (char *)lz, (int)n, cap);
            if (c > 0) {
                memcpy(hdr + 8, &c, 4); hlen = 12; payload = lz; n = c;
            }
        }
        send_buf(hdr, hlen);
        for (uint32_t off = 0; off < n; off += MTU)
            send_buf(payload + off, n - off < MTU ? n - off : MTU);
        bytes += n; sent++; frame++;
        drain_acks();
        if (test || fps > 0) {
            double f = fps > 0 ? fps : 60.0;
            struct timespec d = { 0, (long)(1e9 / f) };
            nanosleep(&d, NULL);
        }
        if (sent % 120 == 0) {
            clock_gettime(CLOCK_MONOTONIC, &t1);
            double dt = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
            fprintf(stderr, "%ld frames, %.1f fps, %.1f Mbit/s\n", sent, 120 / dt, bytes * 8 / dt / 1e6);
            t0 = t1; bytes = 0;
        }
    }
    uint8_t cl = 1;
    send_buf(&cl, 1);
    fprintf(stderr, "closed after %ld frames\n", sent);
    return 0;
}
