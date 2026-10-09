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
/* Leave the process: runs the xbc_at_exit hooks and ends without exit()'s
   C library teardown, which races the guest threads still running (and
   crashes under box86). */
void xbc_at_exit(void (*fn)(void));
void xbc_exit(int code) __attribute__((noreturn));
#define TRACE(...) do { if (g_trace) xlog(__VA_ARGS__); } while (0)

/* ---- memory ----------------------------------------------------------- */

/*
 * Guest address space layout inside the 32-bit host process:
 *   0x00010000 .. 0x04000000  XBE image (headers at 0x10000)
 *   0x10000000 .. 0x50000000  NtAllocateVirtualMemory arena, thread stacks, pool
 *                             (0x40000000 on ARM)
 *   0x80000000 .. 0x88000000  "physical" contiguous memory, VA = 0x80000000 | PA
 * Xbox code tests the top address bit to tell contiguous memory apart, so
 * nothing the guest can see is allowed to live at or above 0x80000000 except
 * the contiguous window.
 */
#define IMAGE_REGION_END   0x04000000u
#define ARENA_BASE         0x10000000u
#if defined(__i386__) && !defined(XBC_SMALL_ARENA)
#define ARENA_END          0x50000000u
#else
/* 32-bit ARM: QEMU's user-mode emulator (used for testing) maps the
   program's libraries from 0x40000000; 768 MB is plenty for a 64 MB console. */
#define ARENA_END          0x40000000u
#endif
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
    struct xapc *apc_head, *apc_tail;   /* queued APCs, under g_disp_lock */
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
/* Call with g_disp_lock held after changing an object's SignalState: wakes
   the threads waiting on it (disp_signal_all: every waiting thread). */
void disp_signal(void *object);
void disp_signal_all(void);
/* A user-mode alertable wait (mode 1) runs the thread's queued APCs and
   returns STATUS_USER_APC. */
NTSTATUS wait_objects(ULONG count, PVOID objects[], int wait_any, KPROCESSOR_MODE mode,
                      BOOLEAN alertable, LARGE_INTEGER *timeout);
/* Queue an APC to t: routine(ctx, arg1, arg2), stdcall.  kapc, when set, is
   the guest's KAPC (KeInsertQueueApc); its KernelRoutine runs first. */
void apc_queue(xthread *t, PVOID routine, PVOID ctx, PVOID arg1, PVOID arg2, KAPC *kapc);
ULONG NTAPI RtlNtStatusToDosError(NTSTATUS st);
void timers_init(void);
extern void (*g_vblank_hook)(void);   /* run 60 times a second on the DPC thread */
void ke_frame_presented(void);   /* XBCOMPAT_FIXED_FPS clock step */
ULONGLONG system_time_now(void);   /* 100 ns units since 1601 */
ULONGLONG ke_guest_tsc(void);      /* what a guest rdtsc reads */
extern volatile unsigned g_guest_traps;   /* faults answered for the guest (rdtsc etc.), for XBCOMPAT_LOG_FPS */
void thread_trap_tsc(void);        /* make this thread's rdtsc fault into ke_guest_tsc */
void thread_untrap_tsc(void);      /* back to the host's counter, before exec */

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

void fs_init(const char *xbe_path, const char *hdd_root, const char *dvd_root, const char *dvd_drive);
NTSTATUS fs_host_path(const char *xpath, char *host, size_t hostlen);
const char *fs_hdd_root(void);
void fs_set_card(const char *dir);
void install_fault_handlers(void);

/* ---- the DVD drive and its tray (kernel/dvd.c) ------------------------ */

typedef struct dvd_node dvd_node;
struct stat;
void dvd_init(const char *src, const char *drive);
bool dvd_is_media(void);
const char *dvd_host_dir(void);
ULONG dvd_tray_state(void);
bool dvd_tray_empty(void);
NTSTATUS dvd_check_verify(void);
NTSTATUS dvd_lookup(const char *rest, dvd_node **out);
bool dvd_node_is_dir(const dvd_node *n);
const char *dvd_node_name(const dvd_node *n);
void dvd_stat(const dvd_node *n, struct stat *sb);
dvd_node *dvd_child(dvd_node *n, int i);
ssize_t dvd_read(dvd_node *n, void *buf, size_t len, uint64_t off);
ssize_t dvd_read_volume(void *buf, size_t len, uint64_t off);
bool dvd_extract(dvd_node *n, char *host, size_t hostlen);
void dvd_title_started(const char *xbe_host_path);
NTSTATUS dvd_scsi_pass_through(void *in, ULONG inlen);

/* ---- title launches (XLaunchNewImage) --------------------------------- */

void launch_init(int argc, char **argv, const char *d_path, const char *launch_data_file, const char *xbe_rel);
NTSTATUS fs_translate(const OBJECT_ATTRIBUTES *oa, char *host, size_t hostlen, int *is_device);

/* ---- the picture across title switches (kernel/av.c) ------------------ */

void av_title_starting(void);
void av_persist(const unsigned char *px, unsigned w, unsigned h);
void av_hand_over(void);

/* ---- soft reset to the dashboard (kernel/reset.c) --------------------- */

void reset_init(ULONG title_id);
bool reset_enabled(void);
void reset_request(const char *why);
void reset_check(void);
void xinput_check_reset_combo(void);

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
