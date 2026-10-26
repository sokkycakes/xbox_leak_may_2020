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
/* Per-frame noise (drawing, pads, memory): left out when g_trace is 2
   (XBCOMPAT_TRACE=files), which keeps file and thread calls only. */
#define TRACE_ALL(...) do { if (g_trace == 1) xlog(__VA_ARGS__); } while (0)

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
    bool started;               /* past its creation suspend, under start_lock */
    int parked;                 /* stopped in thread_suspend's signal handler */
    int in_wait;                /* blocked in a kernel wait (thread_wait_end clears) */
    ULONG suspend_ip;           /* host code the last suspend signal found it in */
    int resume_gen;             /* counts resumes that brought SuspendCount to 0 */
} xthread;

void thread_init_main(void);
xthread *thread_current(void);
xthread *thread_create(SIZE_T stack_size, SIZE_T tls_size, PVOID system_routine,
                       PVOID start_routine, PVOID start_context, bool suspended);
/* Attach a host thread (e.g. the DPC thread) to a guest KPCR so guest code can run on it. */
xthread *thread_adopt_host(const char *name);
ULONG thread_suspend(xthread *t);
void thread_wait_end(xthread *t);
void thread_exit(NTSTATUS status) __attribute__((noreturn));

/* ---- dispatcher objects ---------------------------------------------- */

extern pthread_mutex_t g_disp_lock;
/* Condition variables 8-byte aligned, as the ARM C library expects: under
   box86 (Raspberry Pi) they go to it as they are, and i386 only aligns
   them to 4, which a 64-bit kernel answers with SIGBUS. */
#define XBC_COND_ALIGN __attribute__((aligned(8)))
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
/* Uniprocessor IRQL semantics once networking is up (ke.c). */
void ke_dispatch_exclusive(void);
void ke_dispatch_enter(KIRQL irql);
void ke_dispatch_leave(void);
bool ke_dispatch_release_for_wait(void);
void ke_dispatch_reacquire(bool was_held);
/* The console identity in the hard disk directory (misc.c). */
/* probe.c: XBCOMPAT_PROBE address tracing */
void probe_init(void);
bool probe_trap(void *ucontext);
void identity_init(const char *hdd);
const UCHAR *identity_mac(void);
/* The emulated Ethernet controller (nic.c). */
extern bool g_nic_enabled;
void nic_interrupt_init(PVOID Interrupt, PVOID ServiceRoutine, PVOID ServiceContext, ULONG Vector);
bool nic_interrupt_connect(PVOID Interrupt, bool connect);
extern void (*g_vblank_hook)(void);   /* run 60 times a second on the DPC thread */
extern void (*g_render_drain)(void);  /* before freeing guest memory queued drawing may still read */
/* hle/d3d8.c: the GL context goes between the title's threads that draw.
   The thread holding it lets it go at kernel calls while another waits for
   it (gl_yield_point), and when it exits.  Weak: builds whose renderer is a
   library of its own have none of this. */
bool xbc_gl_yield_due(void) __attribute__((weak));
void xbc_gl_yield(void) __attribute__((weak));
void xbc_gl_thread_exit(void) __attribute__((weak));
static inline void gl_yield_point(void) { if (xbc_gl_yield) xbc_gl_yield(); }
void ke_wake_waiters(void);   /* every thread in a kernel wait looks again */
void ke_frame_presented(void);   /* XBCOMPAT_FIXED_FPS clock step */
ULONGLONG system_time_now(void);   /* 100 ns units since 1601 */
ULONGLONG ke_guest_tsc(void);      /* what a guest rdtsc reads */
extern volatile unsigned g_guest_traps;   /* faults answered for the guest (rdtsc etc.), for XBCOMPAT_LOG_FPS */
void thread_trap_tsc(void);        /* make this thread's rdtsc fault into ke_guest_tsc */
void thread_untrap_tsc(void);      /* back to the host's counter, before exec */

/* ---- handles ---------------------------------------------------------- */

enum obj_kind { OBJ_NONE, OBJ_EVENT, OBJ_SEMAPHORE, OBJ_MUTANT, OBJ_TIMER, OBJ_THREAD,
                OBJ_FILE, OBJ_SYMLINK, OBJ_DEVICE, OBJ_IOCOMPLETION,
                OBJ_OB };   /* made by ObCreateObject: body behind an OBJECT_HEADER (kernel/driver.h) */

typedef struct xobject {
    enum obj_kind kind;
    int refs;
    /* Where a FILE_OBJECT keeps FsContext (+8), since a file's xobject is
       the file object titles get: 5xxx XAPI's XGetFilePhysicalSortKey reads
       the file system's FCB through it (FATX's FirstCluster at +0x1c,
       GDFX's FirstSector at +0).  Files point it at a small stand-in FCB. */
    PVOID fs_context;
    char *name;
    union {
        KEVENT event;
        KSEMAPHORE semaphore;
        KMUTANT mutant;
        KTIMER timer;
        KQUEUE queue;           /* an I/O completion port */
    } u;                       /* dispatcher body lives here for NtCreate* objects */
    xthread *thread;
    struct xfile *file;
    char *link_target;
    PVOID body;                /* OBJ_OB: the object body */
    PVOID ob_base;             /* OBJ_OB: the allocation holding header and body */
    struct xobject *ob_next;   /* OBJ_OB: every live one, under ob.c's lock */
} xobject;

HANDLE handle_insert(xobject *obj);
xobject *handle_lookup(HANDLE h);
NTSTATUS handle_close(HANDLE h);
xobject *object_new(enum obj_kind kind);
void object_release(xobject *obj);
/* Queue a completion packet on an I/O completion port (ob.c). */
NTSTATUS iocp_post(xobject *port, PVOID key, PVOID apc_context, NTSTATUS status, ULONG_PTR info);

/* ---- files ------------------------------------------------------------ */

void fs_init(const char *xbe_path, const char *hdd_root, const char *dvd_root, const char *dvd_drive);
NTSTATUS fs_host_path(const char *xpath, char *host, size_t hostlen);
const char *fs_hdd_root(void);
void fs_set_card(const char *dir);
void install_fault_handlers(void);
void prof_init(void);   /* XBCOMPAT_PROFILE=FILE, see prof.c */
void prof_thread_start(void);
/* XBCOMPAT_PROFILE_WORK: time a thread spent blocked in a kernel wait or
   delay that began at start (prof_wait_begin), charged to the title code that
   made the call; logged every 5 seconds with who signalled the objects
   (prof_signal).  kind: the first object's dispatcher type, or -1 for a
   delay; obj: the first object, or NULL. */
uint64_t prof_wait_begin(void);
void prof_wait_end(uint64_t start, int kind, const void *obj);
void prof_signal(const void *obj);

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
/* The Guide overlay (src/guide): xinput_guide_read() gives the pads' combined
   state as GUIDE_* bits plus the port that pressed last; xinput_guide_mask()
   hides the pads from the title while the Guide is open (and until every
   button is let go after it closes). */
#define GUIDE_A 0x100
#define GUIDE_B 0x200
#define GUIDE_X 0x400
#define GUIDE_Y 0x800
#define GUIDE_LT 0x1000
#define GUIDE_RT 0x2000
#define GUIDE_HOME 0x4000
unsigned xinput_guide_read(int *port);
void xinput_guide_mask(bool on);
void ds_set_duck(float gain);
void ds_set_volume(float gain);
void ds_play_ui(const float *pcm, int frames);
unsigned xinput_guide_ports(void);
void xinput_guide_vibration(bool on);
void reset_request_always(const char *why);

/* ---- loader ----------------------------------------------------------- */

typedef struct {
    ULONG entry;
    ULONG kernel_thunk;
    XBE_HEADER *header;
    const char *path;
} xbe_image;

void xbe_load(const char *path, xbe_image *img);
void wincart_exec(const char *xbe);          /* kernel/wincart.c */
ULONG wincart_title_id(const char *id);
ULONG xbe_title_id(void);
const char *xbe_title_name(void);
const char *xbe_file_path(void);
bool xbe_image_read(const char *xbe, const char *section, uint8_t **bgra, int *w, int *h);
bool xpr_image_read(const char *path, uint8_t **bgra, int *w, int *h);
/* The Guide's Quick Launch (misc.c, io.c) */
void guide_launch(const char *nt_dir, const char *xbe);
void guide_launch_pending(ULONG title_id);
bool fs_card_dir(char *out, size_t outlen);
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
