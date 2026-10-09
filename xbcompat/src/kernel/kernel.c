/* Resolving an XBE's kernel imports against our export table. */
#define _GNU_SOURCE
#include <string.h>
#include <sys/mman.h>

#include "../xbcompat.h"
#include "exports.h"
#include "../cpu.h"

static const struct kexport *find_export(unsigned ordinal)
{
    for (const struct kexport *e = kernel_exports; e->name; e++)
        if (e->ordinal == ordinal) return e;
    return NULL;
}

const char *kernel_export_name(unsigned ordinal)
{
    const struct kexport *e = find_export(ordinal);
    return e ? e->name : "?";
}

static void CDECLAPI unimplemented(unsigned ordinal, void *return_address)
{
    /* Guest stack words that look like code addresses, for finding the caller. */
    ULONG *sp = (ULONG *)&return_address + 1;
    for (int i = 0; i < 64; i++)
        if (sp[i] >= 0x10000 && sp[i] < 0x01000000)
            xlog("  stack[%d] = 0x%lx", i, (unsigned long)sp[i]);
    fatal("guest called unimplemented kernel export %u (%s) from %p", ordinal,
          kernel_export_name(ordinal), return_address);
}

#ifdef XBC_TRANSLATED
static void *make_stub(unsigned ordinal)
{
    char *msg;
    if (asprintf(&msg, "unimplemented kernel export %u (%s)", ordinal, kernel_export_name(ordinal)) < 0)
        fatal("out of memory");
    return (void *)(uintptr_t)cpu_guest_trap(msg);
}
#else
/* Each missing export gets a tiny trampoline that reports which one it was:
     push [esp]; push ordinal; mov eax, unimplemented; call eax */
static void *make_stub(unsigned ordinal)
{
    static uint8_t *page, *cur;
    if (!page || cur + 16 > page + 4096) {
        page = cur = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (page == MAP_FAILED) fatal("mmap stub page");
    }
    uint8_t *s = cur;
    uint32_t handler = (uint32_t)unimplemented;
    uint8_t code[] = { 0xFF, 0x34, 0x24,                       /* push [esp] */
                       0x68, 0, 0, 0, 0,                       /* push ordinal */
                       0xB8, 0, 0, 0, 0,                       /* mov eax, handler */
                       0xFF, 0xD0 };                           /* call eax */
    memcpy(code + 4, &ordinal, 4);
    memcpy(code + 9, &handler, 4);
    memcpy(s, code, sizeof(code));
    cur += 16;
    return s;
}
#endif

void kernel_resolve_imports(xbe_image *img)
{
    ULONG *thunk = (ULONG *)img->kernel_thunk;
    unsigned total = 0, missing = 0;
    for (; *thunk; thunk++, total++) {
        unsigned ordinal = *thunk & 0x7FFFFFFF;
        const struct kexport *e = find_export(ordinal);
        if (e && e->address) {
            /* Functions are called through their guest entry; data exports
               are used where they are. */
            *thunk = e->is_data ? (ULONG)(uintptr_t)e->address : cpu_guest_entry(e->address);
        } else {
            missing++;
            xlog("kernel import %u (%s) is not implemented", ordinal, e ? e->name : "unknown");
            *thunk = (ULONG)make_stub(ordinal);
        }
    }
    xlog("resolved %u kernel imports, %u missing", total, missing);
}
