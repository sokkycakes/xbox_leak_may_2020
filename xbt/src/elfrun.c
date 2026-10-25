/*
 * elfrun.c - run a static i386 Linux ELF (int 0x80 write/exit) through the
 * translator. Used to benchmark the same binaries under xbt, box86 and the
 * native CPU; not part of the Xbox front end.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "xbt.h"

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

int elf_main(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        XBT_LOG("cannot open %s\n", path);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc((size_t)len);
    if (fread(d, 1, (size_t)len, f) != (size_t)len)
        return 1;
    fclose(f);
    if (len < 52 || memcmp(d, "\177ELF\1\1", 6) || rd16(d + 18) != 3) {
        XBT_LOG("%s is not an i386 ELF\n", path);
        return 1;
    }
    if (mem_init(XBOX_RAM_SIZE_RETAIL) || xbt_runtime_init())
        return 1;
    uint32_t entry = rd32(d + 24), phoff = rd32(d + 28);
    uint16_t phnum = rd16(d + 44), phent = rd16(d + 42);
    for (int i = 0; i < phnum; i++) {
        const uint8_t *ph = d + phoff + (uint32_t)i * phent;
        if (rd32(ph) != 1)          /* PT_LOAD */
            continue;
        uint32_t off = rd32(ph + 4), va = rd32(ph + 8), fsz = rd32(ph + 16), msz = rd32(ph + 20);
        if (va < XBOX_LOW_BASE || va + msz > mem_ram_size()) {
            XBT_LOG("segment %08x+%x outside guest RAM\n", va, msz);
            return 1;
        }
        memcpy(mem_host(va), d + off, fsz);
        memset((uint8_t *)mem_host(va) + fsz, 0, msz - fsz);
    }
    static xbt_cpu cpu;
    xbt_cpu_init(&cpu);
    uint32_t sp = 0x03f00000u;
    /* argc, argv[0], NULL, envp NULL, auxv AT_NULL */
    sp -= 32;
    memset(mem_host(sp), 0, 32);
    *(uint32_t *)mem_host(sp) = 1;
    *(uint32_t *)mem_host(sp + 4) = sp + 24;
    cpu.r[R_ESP] = sp;
    cpu.eip = entry;
    for (;;) {
        int r = xbt_run(&cpu);
        if (r == EXIT_PENDING)
            continue;
        if (r == EXIT_INT && cpu.exit_info == 0x80) {
            uint32_t n = cpu.r[R_EAX];
            if (n == 1 || n == 252) {
                fflush(stdout);
                if (xbt_opt.stats)
                    xbt_print_stats();
                return (int)cpu.r[R_EBX];
            }
            if (n == 4) {
                ssize_t w = write((int)cpu.r[R_EBX], mem_host(cpu.r[R_ECX]), cpu.r[R_EDX]);
                cpu.r[R_EAX] = (uint32_t)w;
                continue;
            }
            cpu.r[R_EAX] = (uint32_t)-38;   /* ENOSYS */
            continue;
        }
        XBT_LOG("stopped at %08x (reason %d)\n", cpu.eip, r);
        return 1;
    }
}
