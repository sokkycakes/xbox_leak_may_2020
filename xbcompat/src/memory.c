/*
 * Guest memory: the virtual memory arena behind NtAllocateVirtualMemory, the
 * contiguous "physical" memory window, and the kernel pool.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <string.h>
#include <sys/mman.h>

#include "xbcompat.h"

#define PAGE_SIZE      0x1000u
#define GRANULE        0x10000u
#define ARENA_PAGES    ((ARENA_END - ARENA_BASE) / PAGE_SIZE)
#define CONTIG_PAGES   (CONTIG_SIZE / PAGE_SIZE)

#define MEM_COMMIT     0x1000
#define MEM_RESERVE    0x2000
#define MEM_DECOMMIT   0x4000
#define MEM_RELEASE    0x8000
#define MEM_FREE       0x10000
#define MEM_PRIVATE    0x20000
#define MEM_TOP_DOWN   0x100000
#define PAGE_NOACCESS  0x01
#define PAGE_READWRITE 0x04

enum { PG_FREE = 0, PG_RESERVED = 1, PG_COMMITTED = 2 };

static pthread_mutex_t mem_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t arena_state[ARENA_PAGES];
static uint8_t arena_protect[ARENA_PAGES];
static uint32_t arena_alloc_base[ARENA_PAGES];   /* allocation base of each page */
static uint32_t contig_len[CONTIG_PAGES];        /* pages in the allocation starting here */
static uint8_t contig_used[CONTIG_PAGES];

static void reserve_fixed(uint32_t base, uint32_t size, int prot)
{
    void *p = mmap((void *)base, size, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE
                   | MAP_NORESERVE, -1, 0);
    if (p != (void *)base)
        fatal("cannot reserve guest range %#x-%#x: %s (is this a 32-bit build?)",
              base, base + size, strerror(errno));
}

volatile ULONG *g_apu_sample_counter;

void mem_init(void)
{
    /* The image region is mapped by the loader; reserve the rest up front so
       host allocations (malloc, SDL, GL) can never land inside them. */
    reserve_fixed(ARENA_BASE, ARENA_END - ARENA_BASE, PROT_NONE);
    reserve_fixed(CONTIG_BASE, CONTIG_SIZE, PROT_READ | PROT_WRITE | PROT_EXEC);
    /* The network library pokes the MCP NIC's registers at 0xFEF00000
       directly.  Back them with plain memory: every register reads zero,
       which the driver sees as a NIC with no link. */
    void *nic = mmap((void *)0xFEF00000u, 0x10000, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (nic != (void *)0xFEF00000u) xlog("NIC register window unavailable: %s", strerror(errno));
    /* Later XDKs read the APU's sample counter (0xFE80200C) directly for
       DirectSoundGetSampleTime; the DPC thread keeps it counting at 48 kHz. */
    void *apu = mmap((void *)0xFE800000u, 0x80000, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (apu != (void *)0xFE800000u) xlog("APU register window unavailable: %s", strerror(errno));
    else g_apu_sample_counter = (volatile ULONG *)0xFE80200Cu;
    /* Physical page 0 holds the kernel on a real console; never hand it out. */
    for (unsigned i = 0; i < 16; i++)
        contig_used[i] = 1;
}

/* ---- arena ------------------------------------------------------------ */

static long arena_find(uint32_t pages, int top_down)
{
    uint32_t gpages = GRANULE / PAGE_SIZE;
    pages = (pages + gpages - 1) & ~(gpages - 1);
    if (top_down) {
        for (long start = ARENA_PAGES - pages; start >= 0; start -= gpages) {
            uint32_t i = 0;
            while (i < pages && arena_state[start + i] == PG_FREE) i++;
            if (i == pages) return start;
        }
    } else {
        for (uint32_t start = 0; start + pages <= ARENA_PAGES; start += gpages) {
            uint32_t i = 0;
            while (i < pages && arena_state[start + i] == PG_FREE) i++;
            if (i == pages) return start;
            start += i & ~(gpages - 1);
        }
    }
    return -1;
}

static void arena_set(uint32_t first, uint32_t pages, uint8_t state, uint32_t base)
{
    for (uint32_t i = 0; i < pages; i++) {
        arena_state[first + i] = state;
        arena_alloc_base[first + i] = base;
    }
}

static inline bool in_arena(uint32_t a) { return a >= ARENA_BASE && a < ARENA_END; }
static inline uint32_t page_of(uint32_t a) { return (a - ARENA_BASE) / PAGE_SIZE; }

NTSTATUS NTAPI NtAllocateVirtualMemory(PVOID *BaseAddress, ULONG_PTR ZeroBits,
                                       SIZE_T *RegionSize, ULONG AllocationType, ULONG Protect)
{
    (void)ZeroBits;
    uint32_t base = (uint32_t)*BaseAddress, size = *RegionSize;
    NTSTATUS st = STATUS_SUCCESS;
    TRACE("NtAllocateVirtualMemory(%#x, %#x, type %#x, prot %#x)", base, size, AllocationType, Protect);
    if (size == 0)
        return STATUS_INVALID_PARAMETER;

    pthread_mutex_lock(&mem_lock);
    if (base == 0) {
        long first = arena_find((size + PAGE_SIZE - 1) / PAGE_SIZE, AllocationType & MEM_TOP_DOWN);
        if (first < 0) { st = STATUS_NO_MEMORY; goto out; }
        base = ARENA_BASE + first * PAGE_SIZE;
        size = (size + GRANULE - 1) & ~(GRANULE - 1);
        arena_set(first, size / PAGE_SIZE, PG_RESERVED, base);
    } else {
        uint32_t end = (base + size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        if (!in_arena(base) || end > ARENA_END) { st = STATUS_INVALID_PARAMETER; goto out; }
        if (AllocationType & MEM_RESERVE) {
            base &= ~(GRANULE - 1);
            for (uint32_t a = base; a < end; a += PAGE_SIZE)
                if (arena_state[page_of(a)] != PG_FREE) { st = STATUS_UNSUCCESSFUL; goto out; }
            arena_set(page_of(base), (end - base) / PAGE_SIZE, PG_RESERVED, base);
        } else {
            base &= ~(PAGE_SIZE - 1);
            for (uint32_t a = base; a < end; a += PAGE_SIZE)
                if (arena_state[page_of(a)] == PG_FREE) { st = STATUS_UNSUCCESSFUL; goto out; }
        }
        size = end - base;
    }
    if (AllocationType & MEM_COMMIT) {
        if (mprotect((void *)base, size, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
            fatal("mprotect commit %#x+%#x: %s", base, size, strerror(errno));
        for (uint32_t a = base; a < base + size; a += PAGE_SIZE) {
            arena_state[page_of(a)] = PG_COMMITTED;
            arena_protect[page_of(a)] = Protect ? Protect : PAGE_READWRITE;
        }
    }
    *BaseAddress = (PVOID)base;
    *RegionSize = size;
out:
    pthread_mutex_unlock(&mem_lock);
    return st;
}

NTSTATUS NTAPI NtFreeVirtualMemory(PVOID *BaseAddress, SIZE_T *RegionSize, ULONG FreeType)
{
    uint32_t base = (uint32_t)*BaseAddress & ~(PAGE_SIZE - 1), size = *RegionSize;
    TRACE("NtFreeVirtualMemory(%#x, %#x, %#x)", base, size, FreeType);
    if (!in_arena(base))
        return STATUS_INVALID_PARAMETER;
    pthread_mutex_lock(&mem_lock);
    uint32_t first = page_of(base);
    if (arena_state[first] == PG_FREE) {
        pthread_mutex_unlock(&mem_lock);
        return STATUS_INVALID_PARAMETER;
    }
    uint32_t pages;
    if (size == 0) {
        /* Whole allocation. */
        uint32_t ab = arena_alloc_base[first];
        first = page_of(ab);
        base = ab;
        pages = 0;
        while (first + pages < ARENA_PAGES && arena_state[first + pages] != PG_FREE &&
               arena_alloc_base[first + pages] == ab)
            pages++;
    } else {
        pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    }
    mprotect((void *)base, pages * PAGE_SIZE, PROT_NONE);
    madvise((void *)base, pages * PAGE_SIZE, MADV_DONTNEED);
    for (uint32_t i = 0; i < pages; i++) {
        if (FreeType & MEM_RELEASE)
            arena_state[first + i] = PG_FREE;
        else if (arena_state[first + i] == PG_COMMITTED)
            arena_state[first + i] = PG_RESERVED;
    }
    *BaseAddress = (PVOID)base;
    *RegionSize = pages * PAGE_SIZE;
    pthread_mutex_unlock(&mem_lock);
    return STATUS_SUCCESS;
}

typedef struct {
    PVOID BaseAddress;
    PVOID AllocationBase;
    ULONG AllocationProtect;
    SIZE_T RegionSize;
    ULONG State;
    ULONG Protect;
    ULONG Type;
} MEMORY_BASIC_INFORMATION;

NTSTATUS NTAPI NtQueryVirtualMemory(PVOID BaseAddress, MEMORY_BASIC_INFORMATION *mbi)
{
    uint32_t a = (uint32_t)BaseAddress & ~(PAGE_SIZE - 1);
    memset(mbi, 0, sizeof(*mbi));
    mbi->BaseAddress = (PVOID)a;
    if (!in_arena(a)) {
        /* Image and contiguous memory: report committed, one page at a time. */
        mbi->AllocationBase = (PVOID)a;
        mbi->RegionSize = PAGE_SIZE;
        mbi->State = MEM_COMMIT;
        mbi->Protect = mbi->AllocationProtect = PAGE_READWRITE;
        mbi->Type = MEM_PRIVATE;
        return STATUS_SUCCESS;
    }
    pthread_mutex_lock(&mem_lock);
    uint32_t p = page_of(a), n = 0;
    uint8_t st = arena_state[p];
    while (p + n < ARENA_PAGES && arena_state[p + n] == st &&
           (st == PG_FREE || arena_alloc_base[p + n] == arena_alloc_base[p]))
        n++;
    mbi->RegionSize = n * PAGE_SIZE;
    if (st == PG_FREE) {
        mbi->State = MEM_FREE;
        mbi->Protect = PAGE_NOACCESS;
    } else {
        mbi->AllocationBase = (PVOID)arena_alloc_base[p];
        mbi->AllocationProtect = PAGE_READWRITE;
        mbi->State = st == PG_COMMITTED ? MEM_COMMIT : MEM_RESERVE;
        mbi->Protect = st == PG_COMMITTED ? arena_protect[p] : 0;
        mbi->Type = MEM_PRIVATE;
    }
    pthread_mutex_unlock(&mem_lock);
    return STATUS_SUCCESS;
}

void *arena_alloc(size_t size, ULONG top_down)
{
    PVOID base = NULL;
    SIZE_T sz = size;
    if (!NT_SUCCESS(NtAllocateVirtualMemory(&base, 0, &sz, MEM_RESERVE | MEM_COMMIT |
                                            (top_down ? MEM_TOP_DOWN : 0), PAGE_READWRITE)))
        return NULL;
    return base;
}

void arena_free(void *p)
{
    PVOID base = p;
    SIZE_T sz = 0;
    NtFreeVirtualMemory(&base, &sz, MEM_RELEASE);
}

/* ---- kernel pool: small blocks carved from the arena ---------------- */

/* Power-of-two size classes up to 32 KB carved from 1 MB arena chunks; larger
   blocks get their own arena allocation.  The pool only backs kernel-side
   bookkeeping that the guest occasionally holds pointers to. */
typedef struct pool_hdr { uint32_t size, cls; } pool_hdr;
#define POOL_CLASSES 12            /* 16 .. 32768 bytes */
#define POOL_BIG     0xFFu
static void *pool_free_list[POOL_CLASSES];
static char *pool_chunk, *pool_chunk_end;
static pthread_mutex_t pool_lock = PTHREAD_MUTEX_INITIALIZER;

void *pool_alloc(size_t size)
{
    size_t need = size + sizeof(pool_hdr);
    unsigned cls = 0;
    while (cls < POOL_CLASSES && (16u << cls) < need) cls++;
    pool_hdr *h;
    if (cls == POOL_CLASSES) {
        h = arena_alloc(need, 1);
        if (!h) return NULL;
        h->cls = POOL_BIG;
    } else {
        pthread_mutex_lock(&pool_lock);
        if (pool_free_list[cls]) {
            h = pool_free_list[cls];
            pool_free_list[cls] = *(void **)h;
        } else {
            size_t csize = 16u << cls;
            if (pool_chunk + csize > pool_chunk_end) {
                pool_chunk = arena_alloc(1 << 20, 1);
                if (!pool_chunk) { pthread_mutex_unlock(&pool_lock); return NULL; }
                pool_chunk_end = pool_chunk + (1 << 20);
            }
            h = (pool_hdr *)pool_chunk;
            pool_chunk += csize;
        }
        pthread_mutex_unlock(&pool_lock);
        h->cls = cls;
    }
    h->size = size;
    memset(h + 1, 0, size);
    return h + 1;
}

void pool_free(void *p)
{
    if (!p) return;
    pool_hdr *h = (pool_hdr *)p - 1;
    if (h->cls == POOL_BIG) {
        arena_free(h);
    } else if (h->cls < POOL_CLASSES) {
        pthread_mutex_lock(&pool_lock);
        *(void **)h = pool_free_list[h->cls];
        pool_free_list[h->cls] = h;
        pthread_mutex_unlock(&pool_lock);
    } else {
        fatal("pool_free of a non-pool pointer %p", p);
    }
}

size_t pool_size(void *p)
{
    return ((pool_hdr *)p - 1)->size;
}

PVOID NTAPI ExAllocatePool(SIZE_T n) { return pool_alloc(n); }
PVOID NTAPI ExAllocatePoolWithTag(SIZE_T n, ULONG tag) { (void)tag; return pool_alloc(n); }
void NTAPI ExFreePool(PVOID p) { pool_free(p); }
ULONG NTAPI ExQueryPoolBlockSize(PVOID p) { return pool_size(p); }

/* ---- contiguous memory ----------------------------------------------- */

PVOID NTAPI MmAllocateContiguousMemoryEx(SIZE_T NumberOfBytes, ULONG_PTR Lowest,
                                         ULONG_PTR Highest, ULONG_PTR Alignment, ULONG Protect)
{
    (void)Protect;
    uint32_t pages = (NumberOfBytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint32_t align = Alignment > PAGE_SIZE ? Alignment / PAGE_SIZE : 1;
    uint32_t lo = Lowest / PAGE_SIZE, hi = Highest / PAGE_SIZE;
    if (hi >= CONTIG_PAGES) hi = CONTIG_PAGES - 1;
    PVOID result = NULL;
    if (pages == 0) return NULL;

    pthread_mutex_lock(&mem_lock);
    /* Top-down, like the kernel, so low physical memory stays available for
       callers that ask for it explicitly. */
    long start = (long)(hi + 1 - pages);
    start -= start % align;
    for (; start >= (long)lo; start -= align) {
        uint32_t i = 0;
        while (i < pages && !contig_used[start + i]) i++;
        if (i == pages) {
            memset(&contig_used[start], 1, pages);
            contig_len[start] = pages;
            result = (PVOID)(CONTIG_BASE + start * PAGE_SIZE);
            break;
        }
    }
    pthread_mutex_unlock(&mem_lock);
    if (result)
        memset(result, 0, pages * PAGE_SIZE);
    TRACE("MmAllocateContiguousMemoryEx(%#x, %#x-%#x, align %#x) = %p",
          NumberOfBytes, Lowest, Highest, Alignment, result);
    return result;
}

PVOID NTAPI MmAllocateContiguousMemory(SIZE_T NumberOfBytes)
{
    return MmAllocateContiguousMemoryEx(NumberOfBytes, 0, 0xFFFFFFFF, 0, PAGE_READWRITE);
}

static int contig_page(PVOID p)
{
    uint32_t a = (uint32_t)p;
    if (a < CONTIG_BASE || a >= CONTIG_BASE + CONTIG_SIZE) return -1;
    return (a - CONTIG_BASE) / PAGE_SIZE;
}

void NTAPI MmFreeContiguousMemory(PVOID BaseAddress)
{
    int p = contig_page(BaseAddress);
    TRACE("MmFreeContiguousMemory(%p)", BaseAddress);
    if (p < 0 || !contig_len[p]) return;
    pthread_mutex_lock(&mem_lock);
    memset(&contig_used[p], 0, contig_len[p]);
    contig_len[p] = 0;
    pthread_mutex_unlock(&mem_lock);
}

SIZE_T NTAPI MmQueryAllocationSize(PVOID BaseAddress)
{
    int p = contig_page(BaseAddress);
    if (p >= 0) return contig_len[p] * PAGE_SIZE;
    MEMORY_BASIC_INFORMATION mbi;
    NtQueryVirtualMemory(BaseAddress, &mbi);
    return mbi.RegionSize;
}

void NTAPI MmPersistContiguousMemory(PVOID BaseAddress, SIZE_T NumberOfBytes, BOOLEAN Persist)
{
    (void)BaseAddress; (void)NumberOfBytes; (void)Persist;
}

ULONG_PTR NTAPI MmGetPhysicalAddress(PVOID BaseAddress)
{
    return (ULONG_PTR)BaseAddress & 0x7FFFFFFF;
}

PVOID NTAPI MmMapIoSpace(ULONG_PTR PhysicalAddress, SIZE_T NumberOfBytes, ULONG Protect)
{
    (void)NumberOfBytes; (void)Protect;
    if (PhysicalAddress < CONTIG_SIZE)
        return (PVOID)(CONTIG_BASE | PhysicalAddress);
    fatal("MmMapIoSpace(%#x): device memory is not emulated", PhysicalAddress);
}

void NTAPI MmUnmapIoSpace(PVOID BaseAddress, SIZE_T NumberOfBytes)
{
    (void)BaseAddress; (void)NumberOfBytes;
}

PVOID NTAPI MmAllocateSystemMemory(SIZE_T NumberOfBytes, ULONG Protect)
{
    (void)Protect;
    return arena_alloc(NumberOfBytes, 1);
}

ULONG NTAPI MmFreeSystemMemory(PVOID BaseAddress, SIZE_T NumberOfBytes)
{
    (void)NumberOfBytes;
    arena_free(BaseAddress);
    return 0;
}

PVOID NTAPI MmClaimGpuInstanceMemory(SIZE_T NumberOfBytes, SIZE_T *NumberOfPaddingBytes)
{
    /* The GPU instance memory sits at the top of physical memory. */
    if (NumberOfPaddingBytes) *NumberOfPaddingBytes = 0;
    return (PVOID)(CONTIG_BASE + CONTIG_SIZE - NumberOfBytes);
}

typedef struct {
    ULONG Length;
    ULONG TotalPhysicalPages;
    ULONG AvailablePages;
    ULONG VirtualMemoryBytesCommitted;
    ULONG VirtualMemoryBytesReserved;
    ULONG CachePagesCommitted;
    ULONG PoolPagesCommitted;
    ULONG StackPagesCommitted;
    ULONG ImagePagesCommitted;
} MM_STATISTICS;

NTSTATUS NTAPI MmQueryStatistics(MM_STATISTICS *s)
{
    if (s->Length != sizeof(*s)) return STATUS_INVALID_PARAMETER;
    uint32_t free_pages = 0;
    for (uint32_t i = 0; i < CONTIG_PAGES; i++) free_pages += !contig_used[i];
    memset((char *)s + 4, 0, sizeof(*s) - 4);
    s->TotalPhysicalPages = CONTIG_PAGES;
    s->AvailablePages = free_pages;
    return STATUS_SUCCESS;
}

ULONG NTAPI MmQueryAddressProtect(PVOID VirtualAddress)
{
    (void)VirtualAddress;
    return PAGE_READWRITE;
}

void NTAPI MmSetAddressProtect(PVOID BaseAddress, ULONG NumberOfBytes, ULONG NewProtect)
{
    (void)BaseAddress; (void)NumberOfBytes; (void)NewProtect;
}

BOOLEAN NTAPI MmIsAddressValid(PVOID VirtualAddress)
{
    uint32_t a = (uint32_t)VirtualAddress;
    if (in_arena(a)) return arena_state[page_of(a)] == PG_COMMITTED;
    return a >= XBE_BASE;
}

void NTAPI MmLockUnlockBufferPages(PVOID BaseAddress, SIZE_T NumberOfBytes, BOOLEAN Unlock)
{
    (void)BaseAddress; (void)NumberOfBytes; (void)Unlock;
}

void NTAPI MmLockUnlockPhysicalPage(ULONG_PTR PhysicalAddress, BOOLEAN Unlock)
{
    (void)PhysicalAddress; (void)Unlock;
}

/* Committed guest memory stays readable, writable and executable; report
   the change without making it. */
NTSTATUS NTAPI NtProtectVirtualMemory(PVOID *BaseAddress, SIZE_T *RegionSize, ULONG NewProtect, ULONG *OldProtect)
{
    TRACE("NtProtectVirtualMemory(%p, %#lx, %#x)", *BaseAddress, (unsigned long)*RegionSize, NewProtect);
    if (OldProtect) *OldProtect = 0x04;   /* PAGE_READWRITE */
    return STATUS_SUCCESS;
}
