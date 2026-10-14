/*
 * main.c - command line front end.
 *
 *   xbt [options] game.xbe          run an Xbox executable (HLE kernel)
 *   xbt --test cases.bin out.bin    run differential test cases (see tests/)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xbt.h"
#include "jit.h"

/* test harness memory layout (shared with tests/runner32.c) */
#define TEST_CODE   0x00400000u
#define TEST_STOP   0x00410000u
#define TEST_DATA   0x00500000u
#define TEST_DATA_SIZE 0x1000u
#define TEST_SLOT   0x00510000u
#define TEST_STACK_TOP 0x0060f000u
#define TEST_STACK_DUMP 512u

static void *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    void *p = malloc((size_t)n + 16);
    if (fread(p, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(p);
        return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return p;
}

static uint32_t lcg_fill(uint8_t *p, uint32_t n, uint32_t seed)
{
    uint32_t x = seed;
    for (uint32_t i = 0; i < n; i++) {
        x = x * 1103515245u + 12345u;
        p[i] = (uint8_t)(x >> 16);
    }
    return x;
}

typedef struct test_hdr {
    uint32_t code_len;
    uint32_t seed;
    uint32_t regs[8];
    uint32_t eflags;
    uint32_t flags_mask;
    uint8_t xmm[8][16];
} test_hdr;

typedef struct test_res {
    uint32_t regs[8];
    uint32_t eflags;
    uint32_t status;
    uint8_t xmm[8][16];
    uint8_t data[TEST_DATA_SIZE];
    uint8_t stack[TEST_STACK_DUMP];
} test_res;

static int run_tests(const char *in, const char *out, int only)
{
    size_t len;
    uint8_t *buf = read_file(in, &len);
    if (!buf) {
        fprintf(stderr, "cannot read %s\n", in);
        return 1;
    }
    if (memcmp(buf, "XTC1", 4) != 0) {
        fprintf(stderr, "bad test file\n");
        return 1;
    }
    uint32_t count;
    memcpy(&count, buf + 4, 4);
    FILE *fo = fopen(out, "wb");
    fwrite("XTR1", 1, 4, fo);
    fwrite(&count, 4, 1, fo);

    if (mem_init(XBOX_RAM_SIZE_RETAIL) || xbt_runtime_init())
        return 1;
    static xbt_cpu cpu;
    size_t pos = 8;
    for (uint32_t t = 0; t < count; t++) {
        test_hdr h;
        memcpy(&h, buf + pos, sizeof h);
        pos += sizeof h;
        const uint8_t *code = buf + pos;
        pos += (h.code_len + 3) & ~3u;

        test_res r;
        memset(&r, 0, sizeof r);
        if (only >= 0 && (int)t != only) {
            fwrite(&r, sizeof r, 1, fo);
            continue;
        }
        if (getenv("XBT_TEST_VERBOSE"))
            fprintf(stderr, "test %u\n", t);
        xbt_flush("new test");
        uint8_t *cp = (uint8_t *)(uintptr_t)TEST_CODE;
        memset(cp, 0xcc, 0x1000);
        memcpy(cp, code, h.code_len);
        static const uint8_t tail[6] = { 0xff, 0x25, 0x00, 0x00, 0x51, 0x00 }; /* jmp [TEST_SLOT] */
        memcpy(cp + h.code_len, tail, 6);
        uint32_t x = lcg_fill((uint8_t *)(uintptr_t)TEST_DATA, TEST_DATA_SIZE, h.seed);
        lcg_fill((uint8_t *)(uintptr_t)(TEST_STACK_TOP - TEST_STACK_DUMP), TEST_STACK_DUMP, x);
        *(uint32_t *)(uintptr_t)TEST_SLOT = TEST_STOP;

        xbt_cpu_init(&cpu);
        memcpy(cpu.r, h.regs, sizeof cpu.r);
        cpu.r[R_ESP] = TEST_STACK_TOP;
        cpu.eflags = (h.eflags & 0x00000ed5u) | 0x202;
        cpu.eip = TEST_CODE;
        cpu.stop_eip = TEST_STOP;
        memcpy(cpu.xmm, h.xmm, sizeof cpu.xmm);
        int reason;
        do {
            reason = xbt_run(&cpu);
        } while (reason == EXIT_PENDING);
        r.status = reason == EXIT_STOP ? 0 : (uint32_t)reason;
        memcpy(r.regs, cpu.r, sizeof r.regs);
        r.eflags = xbt_flags(&cpu);
        memcpy(r.xmm, cpu.xmm, sizeof r.xmm);
        memcpy(r.data, (void *)(uintptr_t)TEST_DATA, TEST_DATA_SIZE);
        memcpy(r.stack, (void *)(uintptr_t)(TEST_STACK_TOP - TEST_STACK_DUMP), TEST_STACK_DUMP);
        fwrite(&r, sizeof r, 1, fo);
    }
    fclose(fo);
    if (xbt_opt.stats)
        xbt_print_stats();
    return 0;
}

extern int xbe_main(const char *path, int argc, char **argv);

static void usage(void)
{
    fprintf(stderr,
            "usage: xbt [options] game.xbe\n"
            "       xbt [options] --test cases.bin results.bin [--only N]\n"
            "options:\n"
            "  --no-link        do not link translated blocks to each other\n"
            "  --no-fusion      do not pass flags through host NZCV\n"
            "  --no-flag-elim   compute flags for every instruction\n"
            "  --strict-abi     do not assume flags are dead across call/ret\n"
            "  --max-insns N    guest instructions per block (default 200)\n"
            "  --trace          log translated blocks\n"
            "  --stats          print translation statistics on exit\n");
}

int main(int argc, char **argv)
{
    const char *test_in = NULL, *test_out = NULL, *xbe = NULL;
    int only = -1;
    int i;
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--no-link"))
            xbt_opt.link_blocks = 0;
        else if (!strcmp(a, "--no-fusion"))
            xbt_opt.flag_fusion = 0;
        else if (!strcmp(a, "--no-flag-elim"))
            xbt_opt.flag_elim = 0;
        else if (!strcmp(a, "--strict-abi"))
            xbt_opt.abi_flags = 0;
        else if (!strcmp(a, "--trace"))
            xbt_opt.trace = 1;
        else if (!strcmp(a, "--dump") && i + 1 < argc)
            xbt_opt.dump_dir = argv[++i];
        else if (!strcmp(a, "--stats"))
            xbt_opt.stats = 1;
        else if (!strcmp(a, "--max-insns") && i + 1 < argc)
            xbt_opt.max_insns = atoi(argv[++i]);
        else if (!strcmp(a, "--only") && i + 1 < argc)
            only = atoi(argv[++i]);
        else if (!strcmp(a, "--test") && i + 2 < argc) {
            test_in = argv[++i];
            test_out = argv[++i];
        } else if (!strcmp(a, "-h") || !strcmp(a, "--help")) {
            usage();
            return 0;
        } else if (a[0] != '-') {
            xbe = a;
            break;
        } else {
            usage();
            return 1;
        }
    }
    if (test_in)
        return run_tests(test_in, test_out, only);
    if (xbe)
        return xbe_main(xbe, argc - i, argv + i);
    usage();
    return 1;
}
