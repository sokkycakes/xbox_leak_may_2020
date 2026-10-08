#include <stdarg.h>
#include <stdlib.h>
#include <time.h>

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
    exit(1);
}
