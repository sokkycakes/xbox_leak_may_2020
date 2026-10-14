/*
 * memory.c - Xbox address space.
 *
 * The Xbox has one flat physical RAM (64 MB retail, 128 MB devkit). Titles see
 * it at its identity virtual addresses (the XBE is loaded at 0x10000) and the
 * kernel exposes the same physical pages at 0x80000000 (contiguous memory)
 * and 0xF0000000 (write-combined). We reproduce exactly that with one memfd
 * mapped three times, at the *same* host addresses. Guest address == host
 * address, so translated code needs no address translation at all: a guest
 * register holding a pointer is used directly as an AArch64 base register.
 *
 * The rest of the low 4 GB is reserved PROT_NONE so wild guest accesses fault
 * instead of corrupting the host, and so host libraries never land there.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "xbt.h"

static int ram_fd = -1;
static uint32_t ram_size;
static uint8_t code_pages[(XBOX_RAM_SIZE_DEVKIT >> 12) / 8];
static long host_page;

uint32_t mem_ram_size(void) { return ram_size; }

static int map_fixed(uint32_t addr, uint32_t size, int prot, int flags, int fd, off_t off)
{
    void *p = mmap((void *)(uintptr_t)addr, size, prot, flags | MAP_FIXED, fd, off);
    if (p == MAP_FAILED || (uintptr_t)p != addr) {
        XBT_LOG("mmap at 0x%08x size 0x%x failed: %s\n", addr, size, strerror(errno));
        return -1;
    }
    return 0;
}

int mem_init(uint32_t size)
{
    host_page = sysconf(_SC_PAGESIZE);
    ram_size = size;

    /* Reserve guest address space [0x10000, 4GB + 64KB) so nothing else lives there. */
    uint64_t resv_len = 0x100010000ull - XBOX_LOW_BASE;
    void *r = mmap((void *)(uintptr_t)XBOX_LOW_BASE, resv_len, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (r == MAP_FAILED || (uintptr_t)r != XBOX_LOW_BASE) {
        XBT_LOG("cannot reserve the low 4GB of address space (%s); "
                "identity mapping requires a 64-bit host with a free low 4GB\n", strerror(errno));
        return -1;
    }

    ram_fd = memfd_create("xbox-ram", 0);
    if (ram_fd < 0 || ftruncate(ram_fd, size) != 0) {
        XBT_LOG("memfd_create failed: %s\n", strerror(errno));
        return -1;
    }
    int rw = PROT_READ | PROT_WRITE;
    /* identity view: guest virtual == physical for the low region */
    if (map_fixed(XBOX_LOW_BASE, size - XBOX_LOW_BASE, rw, MAP_SHARED, ram_fd, XBOX_LOW_BASE))
        return -1;
    if (map_fixed(XBOX_CONTIG_BASE, size, rw, MAP_SHARED, ram_fd, 0))
        return -1;
    if (map_fixed(XBOX_WC_BASE, size, rw, MAP_SHARED, ram_fd, 0))
        return -1;
    /* HLE kernel area: thunk call targets and kernel data visible to the title */
    if (map_fixed(XBOX_HLE_BASE, XBOX_HLE_SIZE, rw, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0))
        return -1;
    /* code cache and lookup table */
    if (map_fixed(XBT_CODE_BASE, XBT_CODE_SIZE, rw | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0))
        return -1;
    if (map_fixed(XBT_JTAB_BASE, XBT_JTAB_RANGE * 4u, rw,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0))
        return -1;
    return 0;
}

void *mem_host(uint32_t guest) { return (void *)(uintptr_t)guest; }

int mem_map_anon(uint32_t addr, uint32_t size)
{
    return map_fixed(addr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

/* Return the physical offset for a guest address in one of the RAM views, or -1. */
static int64_t guest_to_phys(uint32_t a)
{
    if (a >= XBOX_LOW_BASE && a < ram_size)
        return a;
    if (a >= XBOX_CONTIG_BASE && a - XBOX_CONTIG_BASE < ram_size)
        return a - XBOX_CONTIG_BASE;
    if (a >= XBOX_WC_BASE && a - XBOX_WC_BASE < ram_size)
        return a - XBOX_WC_BASE;
    return -1;
}

int mem_readable(uint32_t addr, uint32_t len)
{
    if (guest_to_phys(addr) >= 0 && guest_to_phys(addr + len - 1) >= 0)
        return 1;
    if (addr >= XBOX_HLE_BASE && addr + len <= XBOX_HLE_BASE + XBOX_HLE_SIZE)
        return 1;
    return 0;
}

static void protect_views(uint32_t phys_page, int prot)
{
    uint32_t pa = phys_page << 12;
    uint32_t len = (uint32_t)(host_page > 4096 ? host_page : 4096);
    uint32_t start = pa & ~(len - 1);
    if (start >= XBOX_LOW_BASE)
        mprotect((void *)(uintptr_t)start, len, prot);
    mprotect((void *)(uintptr_t)(XBOX_CONTIG_BASE + start), len, prot);
    mprotect((void *)(uintptr_t)(XBOX_WC_BASE + start), len, prot);
}

/* Write-protect a page that contains translated code (self-modifying code detection). */
void mem_protect_code_page(uint32_t guest_page_addr)
{
    int64_t phys = guest_to_phys(guest_page_addr);
    if (phys < 0)
        return;
    uint32_t pg = (uint32_t)phys >> 12;
    if (code_pages[pg >> 3] & (1u << (pg & 7)))
        return;
    /* mark every 4K page covered by the host page (16K/64K page hosts) */
    uint32_t per = host_page > 4096 ? (uint32_t)(host_page >> 12) : 1;
    uint32_t first = pg & ~(per - 1);
    for (uint32_t i = first; i < first + per; i++)
        code_pages[i >> 3] |= (uint8_t)(1u << (i & 7));
    protect_views(pg, PROT_READ);
}

void mem_unprotect_all_code(void)
{
    uint32_t npages = ram_size >> 12;
    for (uint32_t pg = 0; pg < npages; pg++) {
        if (code_pages[pg >> 3] & (1u << (pg & 7))) {
            code_pages[pg >> 3] &= (uint8_t)~(1u << (pg & 7));
            protect_views(pg, PROT_READ | PROT_WRITE);
        }
    }
}

/* Called from the SIGSEGV handler: is this a write to a protected code page? */
int mem_is_code_page_fault(uint32_t addr)
{
    int64_t phys = guest_to_phys(addr);
    if (phys < 0)
        return 0;
    uint32_t pg = (uint32_t)phys >> 12;
    return (code_pages[pg >> 3] >> (pg & 7)) & 1;
}
