/* Internal interfaces shared by the loader, the kernel shims and the HLE libraries. */
#ifndef XBCOMPAT_H
#define XBCOMPAT_H

#include <pthread.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdio.h>

#include "xbox.h"

/* ---- logging ---------------------------------------------------------- */

extern int g_trace;          /* --trace: log every kernel call */
extern FILE *g_log;
extern volatile ULONG *g_apu_sample_counter;   /* the APU's 48 kHz counter, or NULL */

void xlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
#define TRACE(...) do { if (g_trace) xlog(__VA_ARGS__); } while (0)

/* ---- memory ----------------------------------------------------------- */

/*
 * Guest address space layout inside the 32-bit host process:
 *   0x00010000 .. 0x04000000  XBE image (headers at 0x10000)
 *   0x10000000 .. 0x50000000  NtAllocateVirtualMemory arena, thread stacks, pool
 *   0x80000000 .. 0x88000000  "physical" contiguous memory, VA = 0x80000000 | PA
 * Xbox code tests the top address bit to tell contiguous memory apart, so
 * nothing the guest can see is allowed to live at or above 0x80000000 except
 * the contiguous window.
 */
#define IMAGE_REGION_END   0x04000000u
#define ARENA_BASE         0x10000000u
#define ARENA_END          0x50000000u
#define CONTIG_BASE        0x80000000u
#define CONTIG_SIZE        0x08000000u   /* 128 MB, devkit sized */

void mem_init(void);
void *arena_alloc(size_t size, ULONG top_down);  /* reserve+commit, 64 KB granular */
void arena_free(void *p);
void *pool_alloc(size_t size);
void pool_free(void *p);
bool pool_owns(const void *p);
size_t pool_size(void *p);

NTSTATUS NTAPI NtAllocateVirtualMemory(PVOID *BaseAddress, ULONG_PTR ZeroBits,
                                       SIZE_T *RegionSize, ULONG AllocationType, ULONG Protect);
PVOID NTAPI MmAllocateContiguousMemoryEx(SIZE_T NumberOfBytes, ULONG_PTR Lowest,
                                         ULONG_PTR Highest, ULONG_PTR Alignment, ULONG Protect);
void NTAPI MmFreeContiguousMemory(PVOID BaseAddress);

/* ---- threads ---------------------------------------------------------- */

typedef struct xthread {
    ETHREAD ethread;            /* must be first: guest sees &ethread */
    KPCR *pcr;
    pthread_t host;
    int ldt_index;
    void *tls_block;
    SIZE_T tls_size;
    SIZE_T stack_size;
    PVOID system_routine, start_routine, start_context;
    jmp_buf exit_jmp;
} xthread;

void thread_init_main(void);
xthread *thread_current(void);
xthread *thread_create(SIZE_T stack_size, SIZE_T tls_size, PVOID system_routine,
                       PVOID start_routine, PVOID start_context, bool suspended);
/* Attach a host thread (e.g. the DPC thread) to a guest KPCR so guest code can run on it. */
xthread *thread_adopt_host(const char *name);
void thread_exit(NTSTATUS status) __attribute__((noreturn));

/* ---- dispatcher objects ---------------------------------------------- */

extern pthread_mutex_t g_disp_lock;
extern pthread_cond_t g_disp_cond;
void disp_signal_all(void);  /* call with g_disp_lock held after changing a SignalState */
NTSTATUS wait_objects(ULONG count, PVOID objects[], int wait_any,
                      BOOLEAN alertable, LARGE_INTEGER *timeout);
void timers_init(void);
ULONGLONG system_time_now(void);   /* 100 ns units since 1601 */

/* ---- handles ---------------------------------------------------------- */

enum obj_kind { OBJ_NONE, OBJ_EVENT, OBJ_SEMAPHORE, OBJ_MUTANT, OBJ_TIMER, OBJ_THREAD,
                OBJ_FILE, OBJ_SYMLINK, OBJ_DEVICE };

typedef struct xobject {
    enum obj_kind kind;
    int refs;
    char *name;
    union {
        KEVENT event;
        KSEMAPHORE semaphore;
        KMUTANT mutant;
        KTIMER timer;
    } u;                        /* dispatcher body lives here for NtCreate* objects */
    xthread *thread;
    struct xfile *file;
    char *link_target;
} xobject;

HANDLE handle_insert(xobject *obj);
xobject *handle_lookup(HANDLE h);
NTSTATUS handle_close(HANDLE h);
xobject *object_new(enum obj_kind kind);
void object_release(xobject *obj);

/* ---- files ------------------------------------------------------------ */

void fs_init(const char *xbe_path, const char *hdd_root, const char *dvd_root);
NTSTATUS fs_host_path(const char *xpath, char *host, size_t hostlen);
const char *fs_hdd_root(void);
void fs_set_card(const char *dir);

/* ---- title launches (XLaunchNewImage) --------------------------------- */

void launch_init(int argc, char **argv, const char *d_path, const char *launch_data_file, const char *xbe_rel);
NTSTATUS fs_translate(const OBJECT_ATTRIBUTES *oa, char *host, size_t hostlen, int *is_device);

/* ---- loader ----------------------------------------------------------- */

typedef struct {
    ULONG entry;
    ULONG kernel_thunk;
    XBE_HEADER *header;
    const char *path;
} xbe_image;

void xbe_load(const char *path, xbe_image *img);
void xbe_reload_section(ULONG va, ULONG raw_offset, ULONG raw_size, ULONG virtual_size);
void kernel_resolve_imports(xbe_image *img);
void kernel_init(void);
const char *kernel_export_name(unsigned ordinal);

/* ---- HLE -------------------------------------------------------------- */

void hle_patch(xbe_image *img, const char *sigdir);
ULONG d3d_frame_count(void);   /* frames presented so far (hle/d3d8.c) */
void video_init(int width, int height);
void video_present(void);

#endif
