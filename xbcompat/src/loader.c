/* Mapping an XBE at its fixed base, the way the kernel's XBE loader does. */
#define _GNU_SOURCE
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "xbcompat.h"
#include "cpu.h"

/* Entry point and kernel thunk pointers are XOR-scrambled with a key that
   depends on how the image was signed. */
static const struct { const char *kind; ULONG ep, kt; } keys[] = {
    { "retail",  0xA8FC57AB, 0x5B6D40B6 },
    { "debug",   0x94859D4B, 0xEFB1F152 },
    { "chihiro", 0x40B5C16E, 0x2290059D },
};

static char *xbe_path;
static ULONG title_id;

ULONG xbe_title_id(void) { return title_id; }

/* XeLoadSection on a section nobody holds: the console reads it from disk
   again, so whatever the title changed in place since (fixed-up resource
   headers, say) is back as shipped. */
void xbe_reload_section(ULONG va, ULONG raw_offset, ULONG raw_size, ULONG virtual_size)
{
    FILE *f = xbe_path ? fopen(xbe_path, "rb") : NULL;
    if (!f) return;
    if (fseek(f, raw_offset, SEEK_SET) == 0 && fread((void *)va, 1, raw_size, f) == raw_size &&
        virtual_size > raw_size)
        memset((uint8_t *)va + raw_size, 0, virtual_size - raw_size);
    fclose(f);
    cpu_code_changed(va, virtual_size);
}

void xbe_load(const char *path, xbe_image *img)
{
    FILE *f = fopen(path, "rb");
    if (!f) fatal("cannot open %s: %s", path, strerror(errno));
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *file = malloc(size);
    if (fread(file, 1, size, f) != (size_t)size) fatal("short read on %s", path);
    fclose(f);

    XBE_HEADER *h = (XBE_HEADER *)file;
    if (h->Signature != 0x48454258) fatal("%s is not an XBE", path);
    if (h->BaseAddress != XBE_BASE) fatal("unexpected XBE base %#x", h->BaseAddress);
    if (XBE_BASE + h->SizeOfImage > IMAGE_REGION_END) fatal("XBE image is too large");

    void *base = mmap((void *)XBE_BASE, IMAGE_REGION_END - XBE_BASE,
                      PROT_READ | PROT_WRITE | PROT_EXEC,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (base != (void *)XBE_BASE)
        fatal("cannot map the XBE at %#x: %s (check vm.mmap_min_addr <= 65536)", XBE_BASE,
              strerror(errno));

    memcpy(base, file, h->SizeOfHeaders);
    h = (XBE_HEADER *)base;
    XBE_SECTION *sec = (XBE_SECTION *)h->SectionHeaders;
    for (ULONG i = 0; i < h->NumberOfSections; i++) {
        if (sec[i].PointerToRawData + sec[i].SizeOfRawData > (ULONG)size)
            fatal("section %u runs past the end of the file", i);
        memcpy((void *)sec[i].VirtualAddress, file + sec[i].PointerToRawData, sec[i].SizeOfRawData);
        /* Preloaded sections are in use from the start; the rest count
           XeLoadSection calls, as on the console. */
        if (sec[i].SectionFlags & XBE_SECTION_PRELOAD) sec[i].SectionReferenceCount = 1;
        xlog("section %-8s %#010x +%#08x", (char *)sec[i].SectionName, sec[i].VirtualAddress,
             sec[i].VirtualSize);
    }
    free(file);
    xbe_path = strdup(path);

    const char *kind = NULL;
    for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        ULONG ep = h->AddressOfEntryPoint ^ keys[i].ep;
        if (ep >= XBE_BASE && ep < XBE_BASE + h->SizeOfImage) {
            h->AddressOfEntryPoint = ep;
            h->XboxKernelThunkData ^= keys[i].kt;
            kind = keys[i].kind;
            break;
        }
    }
    if (!kind) fatal("cannot decode the XBE entry point");

    XBE_CERTIFICATE *cert = (XBE_CERTIFICATE *)h->Certificate;
    title_id = cert->TitleID;
    char title[41];
    for (int i = 0; i < 40; i++) title[i] = (char)cert->TitleName[i];
    title[40] = 0;
    xlog("loaded \"%s\" (title %08X, %s key), entry %#x", title, cert->TitleID, kind,
         h->AddressOfEntryPoint);
    /* XBCOMPAT_TRACE=1 traces every title; =game every title but the
       dashboard (for a console whose dashboard starts the game); =files is
       =game without the per-frame drawing, pad and memory calls. */
    const char *tr = getenv("XBCOMPAT_TRACE");
    bool files = tr && !strcmp(tr, "files");
    if (tr && ((strcmp(tr, "game") && !files) || cert->TitleID != 0xFFFE0000)) g_trace = files ? 2 : 1;

    img->header = h;
    img->entry = h->AddressOfEntryPoint;
    img->kernel_thunk = h->XboxKernelThunkData;
    img->path = path;
}
