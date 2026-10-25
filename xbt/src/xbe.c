/*
 * xbe.c - XBE loader and the Xbox front end (memory, kernel, devices).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>
#include <time.h>
#include "kernel.h"

#define XBE_KEY_ENTRY_RETAIL 0xA8FC57ABu
#define XBE_KEY_ENTRY_DEBUG  0x94859D4Bu
#define XBE_KEY_THUNK_RETAIL 0x5B6D40B6u
#define XBE_KEY_THUNK_DEBUG  0xEFB1F152u

extern uint32_t kernel_thunk_target(uint32_t ord);
extern int hle_strict, hle_sample;
extern double hle_run_seconds;
int kernel_run(uint32_t entry, uint32_t stack_size);

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

typedef struct xbe_info {
    uint32_t base, size_headers, size_image, entry, thunk, stack;
    int debug;
    char title[41];
} xbe_info;

static int xbe_load_file(const char *path, xbe_info *xi)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        XBT_LOG("cannot open %s\n", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)len);
    if (fread(d, 1, (size_t)len, f) != (size_t)len) {
        fclose(f);
        return -1;
    }
    fclose(f);
    if (len < 0x180 || memcmp(d, "XBEH", 4)) {
        XBT_LOG("%s is not an XBE\n", path);
        return -1;
    }
    xi->base = rd32(d + 0x104);
    xi->size_headers = rd32(d + 0x108);
    xi->size_image = rd32(d + 0x10c);
    uint32_t cert = rd32(d + 0x118), nsec = rd32(d + 0x11c), sechdr = rd32(d + 0x120);
    uint32_t ent = rd32(d + 0x128), thk = rd32(d + 0x158);
    xi->stack = rd32(d + 0x130);
    uint32_t lo = xi->base, hi = xi->base + xi->size_image;
    if (xi->base != 0x10000 || xi->size_image > mem_ram_size() - 0x20000) {
        XBT_LOG("unexpected image base/size %08x/%08x\n", xi->base, xi->size_image);
        return -1;
    }
    uint32_t er = ent ^ XBE_KEY_ENTRY_RETAIL, ed = ent ^ XBE_KEY_ENTRY_DEBUG;
    uint32_t tr = thk ^ XBE_KEY_THUNK_RETAIL, td = thk ^ XBE_KEY_THUNK_DEBUG;
    if (er >= lo && er < hi && tr >= lo && tr < hi) {
        xi->entry = er;
        xi->thunk = tr;
        xi->debug = 0;
    } else if (ed >= lo && ed < hi && td >= lo && td < hi) {
        xi->entry = ed;
        xi->thunk = td;
        xi->debug = 1;
    } else {
        XBT_LOG("cannot decode the entry point / kernel thunk addresses\n");
        return -1;
    }

    /* headers are mapped at the base address, sections at their addresses */
    memcpy(mem_host(xi->base), d, xi->size_headers);
    for (uint32_t i = 0; i < nsec; i++) {
        const uint8_t *s = d + (sechdr - xi->base) + 56 * i;
        uint32_t va = rd32(s + 4), vs = rd32(s + 8), raw = rd32(s + 12), rs = rd32(s + 16);
        if (va < lo || va + vs > hi || raw + rs > (uint32_t)len) {
            XBT_LOG("bad section %u\n", i);
            return -1;
        }
        memcpy(mem_host(va), d + raw, rs);
        if (vs > rs)
            memset(mem_host(va + rs), 0, vs - rs);
    }
    phys_reserve(xi->base, xi->size_image);
    memset(xi->title, 0, sizeof xi->title);
    if (cert >= lo && cert < hi)
        for (int i = 0; i < 40; i++)
            xi->title[i] = (char)d[cert - xi->base + 12 + 2 * i];

    /* resolve kernel imports */
    int n = 0;
    for (uint32_t p = xi->thunk; ; p += 4, n++) {
        uint32_t v = G32(p);
        if (!v)
            break;
        G32(p) = kernel_thunk_target(v & 0x7fffffffu);
    }
    XBT_LOG("loaded \"%s\" (%s key): entry %08x, %d kernel imports, image %u KB\n",
            xi->title, xi->debug ? "debug" : "retail", xi->entry, n, xi->size_image >> 10);
    free(d);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Devices: trapped MMIO                                               */
/* ------------------------------------------------------------------ */

#define REGS_HASH 4096
typedef struct reg { uint32_t addr, val; struct reg *next; } reg;
static reg *regs[REGS_HASH];
static int mmio_log;
static uint64_t mmio_reads, mmio_writes;

static reg *reg_get(uint32_t a, int create)
{
    uint32_t h = (a >> 2) & (REGS_HASH - 1);
    for (reg *r = regs[h]; r; r = r->next)
        if (r->addr == a)
            return r;
    if (!create)
        return NULL;
    reg *r = calloc(1, sizeof *r);
    r->addr = a;
    r->next = regs[h];
    regs[h] = r;
    return r;
}

static const char *dev_name(uint32_t a)
{
    if (a >= 0xfd000000u && a < 0xfe000000u) return "nv2a";
    if (a >= 0xfe800000u && a < 0xfe880000u) return "apu";
    if (a >= 0xfec00000u && a < 0xfec01000u) return "aci";
    if (a >= 0xfed00000u && a < 0xfed01000u) return "usb";
    if (a >= 0xfef00000u && a < 0xfef00400u) return "nic";
    if (a >= 0xff000000u) return "flash";
    return "?";
}

static int mmio(uint32_t addr, int size, int is_write, uint64_t *value)
{
    uint32_t a = addr & ~3u;
    if (is_write) {
        mmio_writes++;
        reg *r = reg_get(a, 1);
        if (size == 4)
            r->val = (uint32_t)*value;
        else {
            uint32_t sh = (addr & 3) * 8, m = (size == 1 ? 0xffu : 0xffffu) << sh;
            r->val = (r->val & ~m) | ((uint32_t)*value << sh & m);
        }
        /* NV2A user DMA channel: treat the push buffer as consumed instantly */
        if (a >= 0xfd800000u && a < 0xfda00000u && (a & 0xffff) == 0x40)
            reg_get(a + 4, 1)->val = r->val;
        if (mmio_log)
            XBT_LOG("mmio %s write %08x <- %08x (%d)\n", dev_name(addr), addr, (uint32_t)*value, size);
        return 1;
    }
    mmio_reads++;
    uint32_t v = 0;
    reg *r = reg_get(a, 0);
    if (r)
        v = r->val;
    if (a == 0xfd000000u)
        v = 0x02a000a1;                       /* PMC_BOOT_0: NV2A A1 */
    else if (a == 0xfd009400u || a == 0xfd009410u) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
        v = a == 0xfd009400u ? (uint32_t)(ns << 5) : (uint32_t)(ns >> 27);
    } else if (a == 0xfd10020cu)
        v = mem_ram_size();                   /* PFB_CSTATUS */
    else if (a == 0xfd100410u)
        v &= ~0x00010000u;                    /* PFB_WBC: write-back flush completes at once */
    else if (a == 0xfd002400u)
        v = 0x10;                             /* PFIFO_RUNOUT_STATUS: low mark empty */
    else if (a == 0xfd003214u)
        v = 0x10;                             /* PFIFO_CACHE1_STATUS: empty */
    else if (a == 0xfd003220u)
        v &= ~0x100u;                         /* PFIFO_CACHE1_DMA_PUSH: not busy */
    else if (a == 0xfd400700u)
        v = 0;                                /* PGRAPH_STATUS: idle */
    else if (a >= 0xff000000u)
        v = 0xffffffffu;
    if (size < 4)
        v = (v >> ((addr & 3) * 8)) & (size == 1 ? 0xffu : 0xffffu);
    *value = v;
    if (mmio_log)
        XBT_LOG("mmio %s read  %08x -> %08x (%d)\n", dev_name(addr), addr, v, size);
    return 1;
}

/* port I/O: ACPI PM timer, the rest is logged */
static uint32_t pio(uint16_t port, int size, int is_write, uint32_t value)
{
    if (!is_write && port == 0x8008) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t ns = (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
        return (uint32_t)(ns * 3375000ull / 1000000000ull) & 0xffffff;
    }
    if (mmio_log)
        XBT_LOG("port %s %04x %08x (%d)\n", is_write ? "out" : "in ", port, value, size);
    return 0;
}

/* ------------------------------------------------------------------ */

int xbe_main(const char *path, int argc, char **argv)
{
    uint32_t ram = XBOX_RAM_SIZE_RETAIL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--hle-trace"))
            hle_trace++;
        else if (!strcmp(argv[i], "--hle-strict"))
            hle_strict = 1;
        else if (!strcmp(argv[i], "--mmio-log"))
            mmio_log = 1;
        else if (!strcmp(argv[i], "--sample"))
            hle_sample = 1;
        else if (!strcmp(argv[i], "--run-seconds") && i + 1 < argc)
            hle_run_seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--devkit"))
            ram = XBOX_RAM_SIZE_DEVKIT;
    }
    if (mem_init(ram) || xbt_runtime_init())
        return 1;
    char *dup = strdup(path);
    char *dir = dirname(dup);
    kernel_init(ram, dir);
    xbe_info xi;
    if (xbe_load_file(path, &xi))
        return 1;
    xbt_set_mmio_handler(mmio, 0xfd000000u, 0xffffffffu);
    xbt_set_pio_handler(pio);
    int rc = kernel_run(xi.entry, xi.stack ? xi.stack : 0x10000);
    if (xbt_opt.stats) {
        xbt_print_stats();
        fprintf(stderr, "mmio accesses         %llu reads, %llu writes\n",
                (unsigned long long)mmio_reads, (unsigned long long)mmio_writes);
    }
    return rc;
}
