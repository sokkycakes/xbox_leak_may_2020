#define _GNU_SOURCE
/*
 * XBCOMPAT_PROBE=ADDR[,ADDR...]: log the registers and the top of the stack
 * every time guest code reaches one of these addresses (hex, from the title's
 * .map), for following a title through code that has no traces of its own.
 * Each address gets an int 3; on the trap the original byte goes back, the
 * instruction runs single-stepped, and the int 3 is put back after it.
 */
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ucontext.h>

#include "xbcompat.h"
#include "cpu.h"

#ifdef XBC_NATIVE
#define MAX_PROBES 32

static struct { uint8_t *addr; uint8_t orig; } probes[MAX_PROBES];
static int nprobes;
static __thread uint8_t *rearm;

void probe_init(void)
{
    const char *env = getenv("XBCOMPAT_PROBE");
    if (!env) return;
    char *list = strdup(env), *save = NULL;
    for (char *t = strtok_r(list, ",", &save); t && nprobes < MAX_PROBES; t = strtok_r(NULL, ",", &save)) {
        uint8_t *a = (uint8_t *)(uintptr_t)strtoul(t, NULL, 16);
        probes[nprobes].addr = a;
        probes[nprobes].orig = *a;
        *a = 0xCC;
        nprobes++;
        xlog("probe: %p", (void *)a);
    }
    free(list);
}

bool probe_trap(void *ucontext)
{
    greg_t *r = ((ucontext_t *)ucontext)->uc_mcontext.gregs;
    if (rearm) {
        *rearm = 0xCC;
        rearm = NULL;
        r[REG_EFL] &= ~0x100;
        return true;
    }
    uint8_t *at = (uint8_t *)(uintptr_t)r[REG_EIP] - 1;
    for (int i = 0; i < nprobes; i++) {
        if (probes[i].addr != at) continue;
        const uint32_t *sp = (const uint32_t *)(uintptr_t)r[REG_ESP];
        xlog("probe %08x: eax=%08x ecx=%08x edx=%08x ebx=%08x esi=%08x edi=%08x ebp=%08x | %08x %08x %08x %08x %08x",
             (uint32_t)(uintptr_t)at, r[REG_EAX], r[REG_ECX], r[REG_EDX], r[REG_EBX], r[REG_ESI], r[REG_EDI],
             r[REG_EBP], sp[0], sp[1], sp[2], sp[3], sp[4]);
        /* XBCOMPAT_PROBE_MEM=REG also dumps 32 bytes at that register */
        static const char *const rn[] = { "eax", "ecx", "edx", "ebx", "esi", "edi", "ebp" };
        static const int ri[] = { REG_EAX, REG_ECX, REG_EDX, REG_EBX, REG_ESI, REG_EDI, REG_EBP };
        const char *mem = getenv("XBCOMPAT_PROBE_MEM");
        for (int k = 0; mem && k < 7; k++) {
            uint32_t a = (uint32_t)r[ri[k]];
            if (strcmp(mem, rn[k]) || a < 0x10000 || a >= 0xc0000000) continue;
            const uint32_t *m = (const uint32_t *)(uintptr_t)a;
            xlog("probe %08x: [%s] %08x %08x %08x %08x %08x %08x %08x %08x", (uint32_t)(uintptr_t)at, mem,
                 m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7]);
        }
        *at = probes[i].orig;
        r[REG_EIP] = (greg_t)(uintptr_t)at;
        r[REG_EFL] |= 0x100;
        rearm = at;
        return true;
    }
    return false;
}
#else
void probe_init(void) {}
bool probe_trap(void *ucontext) { (void)ucontext; return false; }
#endif
