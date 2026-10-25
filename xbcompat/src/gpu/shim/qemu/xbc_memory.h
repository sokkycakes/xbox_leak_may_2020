/* QEMU MemoryRegion, reduced to what the NV2A code reads (xbcompat-gpu).
   Guest RAM is xbcompat's contiguous window; "dirty" answers come from
   nv2a_host.c's tracking. */
#ifndef XBC_QEMU_MEMORY_H
#define XBC_QEMU_MEMORY_H
#include <stdint.h>
#include <stdbool.h>

typedef struct MemoryRegion {
    uint8_t *ptr;
    uint64_t size;
} MemoryRegion;

enum { DIRTY_MEMORY_VGA = 0, DIRTY_MEMORY_CODE = 1, DIRTY_MEMORY_MIGRATION = 2,
       DIRTY_MEMORY_NV2A = 3, DIRTY_MEMORY_NV2A_TEX = 4, DIRTY_MEMORY_NUM = 5 };

static inline uint64_t memory_region_size(MemoryRegion *mr) { return mr->size; }
static inline void *memory_region_get_ram_ptr(MemoryRegion *mr) { return mr->ptr; }
bool memory_region_test_and_clear_dirty(MemoryRegion *mr, uint64_t addr, uint64_t size, unsigned client);
void memory_region_set_client_dirty(MemoryRegion *mr, uint64_t addr, uint64_t size, unsigned client);
void memory_region_set_dirty(MemoryRegion *mr, uint64_t addr, uint64_t size);
static inline void memory_region_set_log(MemoryRegion *mr, bool log, unsigned client) { (void)mr; (void)log; (void)client; }

#endif

#ifndef XBC_TARGET_PAGE
#define XBC_TARGET_PAGE
#define TARGET_PAGE_BITS 12
#define TARGET_PAGE_SIZE (1u << TARGET_PAGE_BITS)
#define TARGET_PAGE_MASK ((uint64_t)-1 << TARGET_PAGE_BITS)
#define TARGET_PAGE_ALIGN(a) (((a) + TARGET_PAGE_SIZE - 1) & TARGET_PAGE_MASK)
/* Only used with the TCG, which xbcompat-gpu never has. */
typedef struct MemAccessCallback MemAccessCallback;
#define qemu_get_cpu(n) NULL
#define mem_access_callback_insert(cpu, mr, addr, size, cb, opaque) NULL
#define mem_access_callback_remove_by_ref(cpu, cb) ((void)0)
#endif
