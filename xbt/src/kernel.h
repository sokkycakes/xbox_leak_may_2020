/*
 * kernel.h - high-level emulation of the Xbox kernel (xboxkrnl.exe).
 */
#ifndef XBT_KERNEL_H
#define XBT_KERNEL_H

#include <stdint.h>
#include "xbt.h"

#define G8(a)  (*(volatile uint8_t *)(uintptr_t)(uint32_t)(a))
#define G16(a) (*(volatile uint16_t *)(uintptr_t)(uint32_t)(a))
#define G32(a) (*(volatile uint32_t *)(uintptr_t)(uint32_t)(a))
#define G64(a) (*(volatile uint64_t *)(uintptr_t)(uint32_t)(a))
#define GPTR(a) ((void *)(uintptr_t)(uint32_t)(a))

/* special trap addresses inside the HLE thunk range */
#define TRAP_THREAD_EXIT  (XBOX_HLE_BASE + 0xf00)
#define TRAP_GUEST_RET    (XBOX_HLE_BASE + 0xf04)

#define STATUS_SUCCESS              0x00000000u
#define STATUS_WAIT_0               0x00000000u
#define STATUS_ABANDONED_WAIT_0     0x00000080u
#define STATUS_TIMEOUT              0x00000102u
#define STATUS_PENDING              0x00000103u
#define STATUS_UNSUCCESSFUL         0xC0000001u
#define STATUS_NOT_IMPLEMENTED      0xC0000002u
#define STATUS_INVALID_HANDLE       0xC0000008u
#define STATUS_INVALID_PARAMETER    0xC000000Du
#define STATUS_NO_SUCH_FILE         0xC000000Fu
#define STATUS_END_OF_FILE          0xC0000011u
#define STATUS_NO_MEMORY            0xC0000017u
#define STATUS_CONFLICTING_ADDRESSES 0xC0000018u
#define STATUS_ACCESS_DENIED        0xC0000022u
#define STATUS_BUFFER_TOO_SMALL     0xC0000023u
#define STATUS_OBJECT_TYPE_MISMATCH 0xC0000024u
#define STATUS_OBJECT_NAME_INVALID  0xC0000033u
#define STATUS_OBJECT_NAME_NOT_FOUND 0xC0000034u
#define STATUS_OBJECT_NAME_COLLISION 0xC0000035u
#define STATUS_OBJECT_PATH_NOT_FOUND 0xC000003Au
#define STATUS_MUTANT_NOT_OWNED     0xC0000046u
#define STATUS_SEMAPHORE_LIMIT_EXCEEDED 0xC0000047u
#define STATUS_INVALID_DEVICE_REQUEST 0xC0000010u
#define STATUS_NO_MORE_FILES        0x80000006u
#define STATUS_FILE_IS_A_DIRECTORY  0xC00000BAu
#define STATUS_NOT_A_DIRECTORY      0xC0000103u

enum { TS_READY, TS_WAIT, TS_DONE };

typedef struct hthread {
    xbt_cpu cpu;
    uint32_t ethread;          /* guest ETHREAD (KTHREAD at offset 0) */
    uint32_t kpcr;             /* guest KPCR used as this thread's FS base */
    uint32_t id;
    uint32_t stack_lo, stack_hi;
    int state;
    int pending_ord;           /* HLE call to complete when the wait ends */
    int in_guest_call;
    /* wait state */
    uint32_t wait_obj[64];
    int nwait;
    int wait_all;
    int wait_delay;            /* KeDelayExecutionThread */
    uint64_t deadline;         /* host ns, 0 = infinite */
    int is_dpc;
    uint32_t wait_cs;          /* critical section to own when the wait ends */
    struct hthread *next;
} hthread;


int  kernel_init(uint32_t ram_size, const char *game_dir);
int  kernel_run(uint32_t entry, uint32_t stack_size);
uint32_t phys_alloc(uint32_t size, uint32_t align, uint32_t lo, uint32_t hi);
void phys_reserve(uint32_t pa, uint32_t size);
extern int hle_trace;

#endif
