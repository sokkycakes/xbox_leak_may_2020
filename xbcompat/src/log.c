#include <stdarg.h>
#include <stdlib.h>
#include <time.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "xbcompat.h"

int g_trace;
FILE *g_log;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

static void vlog(const char *prefix, const char *fmt, va_list ap)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    pthread_mutex_lock(&log_lock);
    FILE *out = g_log ? g_log : stderr;
    fprintf(out, "[%5ld.%03ld] %s", (long)ts.tv_sec % 100000, ts.tv_nsec / 1000000, prefix);
    vfprintf(out, fmt, ap);
    fputc('\n', out);
    fflush(out);
    pthread_mutex_unlock(&log_lock);
}

void xlog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog("", fmt, ap);
    va_end(ap);
}

void fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vlog("fatal: ", fmt, ap);
    va_end(ap);
    xbc_exit(1);
}

static void (*exit_hooks[4])(void);
static int n_exit_hooks;

void xbc_at_exit(void (*fn)(void))
{
    if (n_exit_hooks < 4) exit_hooks[n_exit_hooks++] = fn;
}

void xbc_exit(int code)
{
    static int exiting;
    if (!__sync_lock_test_and_set(&exiting, 1))
        for (int i = n_exit_hooks - 1; i >= 0; i--) exit_hooks[i]();
    fflush(NULL);
    /* The raw syscall: box86's _exit wrapper frees its own state first. */
    syscall(SYS_exit_group, code);
    _exit(code);
}
