/*
 * A frame window for auditing the renderer against a reference run: with
 * XBCOMPAT_COMPARE_AUDIT set, the frames from XBCOMPAT_COMPARE_AUDIT_START_FRAME
 * to XBCOMPAT_COMPARE_AUDIT_STOP_FRAME (or the end) are bracketed by calls to
 * xbcompat_compare_audit_window(1) and (0), a hook a test harness preloads.
 * Without the hook, or the variables, this does nothing.  Included by d3d8.c.
 */
#include <dlfcn.h>
#include <limits.h>
#include <stdlib.h>
static void cpu_compare_audit_frame(uint32_t frame)
{
    static bool initialized;
    static unsigned long start, stop;
    static void (*control)(unsigned);
    if (!initialized) {
        initialized = true;
        const char *value = getenv("XBCOMPAT_COMPARE_AUDIT");
        const char *first = getenv("XBCOMPAT_COMPARE_AUDIT_START_FRAME");
        const char *last = getenv("XBCOMPAT_COMPARE_AUDIT_STOP_FRAME");
        if (value && value[0] && value[0] != '0' && first && first[0]) {
            char *end;
            start = strtoul(first, &end, 10);
            if (!start || *end) return;
            stop = last && last[0] ? strtoul(last, &end, 10) : ULONG_MAX;
            if (*end || stop <= start) return;
            control = (void (*)(unsigned))dlsym(RTLD_DEFAULT, "xbcompat_compare_audit_window");
            xlog("COMPARE_AUDIT control %s window %lu..%lu", control ? "resolved" : "missing", start, stop);
        }
    }
    if (!control) return;
    if (frame == start) {
        xlog("COMPARE_AUDIT start frame %u", frame);
        control(1);
    } else if (frame == stop) {
        control(0);
        xlog("COMPARE_AUDIT stop frame %u", frame);
    }
}
