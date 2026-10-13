/*
 * xbprof PID [SECONDS] [HZ]: where a running process's CPU time goes, every
 * thread of it, for a Raspberry Pi with no perf.  Samples each thread's own
 * CPU clock (the kernel's software task clock, HZ times a second of CPU,
 * default 1000) for SECONDS (default 20), then prints per thread how much
 * of a core it used and the busiest files and functions: native libraries
 * resolve against their ELF symbols, kernel addresses against
 * /proc/kallsyms, and code that box86 generated at run time (anonymous
 * executable memory) shows as [jit].
 *
 * Built static so it runs on any Pi image:
 *   arm-linux-gnueabihf-gcc -O2 -static -o xbprof xbprof.c
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <elf.h>
#include <signal.h>
#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/perf_event.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define RING_PAGES 16
#define MAX_THREADS 256

typedef struct { uint32_t tid; uint64_t ip; } sample_t;
static sample_t *samples;
static size_t nsamples, cap_samples;
static uint64_t lost;

typedef struct {
    uint32_t tid;
    int fd;
    void *ring;
    char comm[32];
} watched_t;
static watched_t threads[MAX_THREADS];
static int nthreads;
static size_t page;

static void add_sample(uint32_t tid, uint64_t ip)
{
    if (nsamples == cap_samples) {
        cap_samples = cap_samples ? cap_samples * 2 : 65536;
        samples = realloc(samples, cap_samples * sizeof *samples);
        if (!samples) { perror("realloc"); exit(1); }
    }
    samples[nsamples].tid = tid;
    samples[nsamples].ip = ip;
    nsamples++;
}

static void read_comm(int pid, uint32_t tid, char *out, size_t size)
{
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/task/%u/comm", pid, tid);
    FILE *f = fopen(path, "r");
    if (!f) return;
    if (fgets(out, (int)size, f)) out[strcspn(out, "\n")] = 0;
    fclose(f);
}

static void watch(int pid, uint32_t tid, uint64_t period_ns)
{
    for (int i = 0; i < nthreads; i++)
        if (threads[i].tid == tid) return;
    if (nthreads == MAX_THREADS) return;
    struct perf_event_attr a;
    memset(&a, 0, sizeof a);
    a.size = sizeof a;
    a.type = PERF_TYPE_SOFTWARE;
    a.config = PERF_COUNT_SW_TASK_CLOCK;
    a.sample_period = period_ns;
    a.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_TID;
    a.exclude_hv = 1;
    a.wakeup_events = 1;
    int fd = (int)syscall(__NR_perf_event_open, &a, (int)tid, -1, -1, 0);
    if (fd < 0) {
        if (errno != ESRCH) fprintf(stderr, "xbprof: thread %u: perf_event_open: %s\n", tid, strerror(errno));
        return;
    }
    void *ring = mmap(NULL, (1 + RING_PAGES) * page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ring == MAP_FAILED) {
        fprintf(stderr, "xbprof: thread %u: mmap: %s\n", tid, strerror(errno));
        close(fd);
        return;
    }
    watched_t *t = &threads[nthreads++];
    t->tid = tid;
    t->fd = fd;
    t->ring = ring;
    strcpy(t->comm, "?");
    read_comm(pid, tid, t->comm, sizeof t->comm);
}

static void scan_threads(int pid, uint64_t period_ns)
{
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/task", pid);
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)))
        if (e->d_name[0] >= '0' && e->d_name[0] <= '9') watch(pid, (uint32_t)strtoul(e->d_name, NULL, 10), period_ns);
    closedir(d);
}

static void drain(watched_t *t)
{
    struct perf_event_mmap_page *meta = t->ring;
    uint8_t *data = (uint8_t *)t->ring + page;
    uint64_t size = (uint64_t)RING_PAGES * page;
    uint64_t head = __atomic_load_n(&meta->data_head, __ATOMIC_ACQUIRE);
    uint64_t tail = meta->data_tail;
    while (tail < head) {
        struct perf_event_header h;
        uint8_t rec[256];
        for (size_t i = 0; i < sizeof h; i++) ((uint8_t *)&h)[i] = data[(tail + i) % size];
        if (h.size < sizeof h || h.size > sizeof rec) break;
        for (size_t i = 0; i < h.size; i++) rec[i] = data[(tail + i) % size];
        if (h.type == PERF_RECORD_SAMPLE && h.size >= sizeof h + 16) {
            uint64_t ip;
            uint32_t pid_tid[2];
            memcpy(&ip, rec + sizeof h, 8);
            memcpy(pid_tid, rec + sizeof h + 8, 8);
            add_sample(pid_tid[1], ip);
        } else if (h.type == PERF_RECORD_LOST && h.size >= sizeof h + 16) {
            uint64_t n;
            memcpy(&n, rec + sizeof h + 8, 8);
            lost += n;
        }
        tail += h.size;
    }
    __atomic_store_n(&meta->data_tail, tail, __ATOMIC_RELEASE);
}

/* ---- where an address is ------------------------------------------------- */

typedef struct { uint64_t addr, size; const char *name; } sym_t;

typedef struct {
    uint32_t start, end, offset;
    bool exec;
    char path[256];
    /* symbols, loaded on first use */
    bool loaded;
    sym_t *syms;
    size_t nsyms;
    char *strtab_copy;
    int64_t bias;          /* file vaddr = address - start + offset + bias */
} map_t;
static map_t maps[1024];
static int nmaps;

static sym_t *ksyms;
static size_t nksyms;

static int by_addr(const void *a, const void *b)
{
    const sym_t *x = a, *y = b;
    return x->addr < y->addr ? -1 : x->addr > y->addr;
}

static void read_maps(int pid)
{
    char path[64], line[512];
    snprintf(path, sizeof path, "/proc/%d/maps", pid);
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f) && nmaps < (int)(sizeof maps / sizeof maps[0])) {
        unsigned long s, e, off;
        char perms[8], file[256] = "";
        if (sscanf(line, "%lx-%lx %7s %lx %*s %*s %255[^\n]", &s, &e, perms, &off, file) < 4) continue;
        map_t *m = &maps[nmaps++];
        memset(m, 0, sizeof *m);
        m->start = (uint32_t)s;
        m->end = (uint32_t)e;
        m->offset = (uint32_t)off;
        m->exec = perms[2] == 'x';
        snprintf(m->path, sizeof m->path, "%s", file);
    }
    fclose(f);
}

/* ELF symbols (the symbol table, else the dynamic one) of a mapped file. */
static void load_syms(map_t *m)
{
    m->loaded = true;
    if (m->path[0] != '/') return;
    int fd = open(m->path, O_RDONLY);
    if (fd < 0) return;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size < (off_t)sizeof(Elf32_Ehdr)) { close(fd); return; }
    uint8_t *f = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (f == MAP_FAILED) return;
    Elf32_Ehdr *eh = (Elf32_Ehdr *)f;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) || eh->e_ident[EI_CLASS] != ELFCLASS32) goto out;
    /* The load segment holding this mapping's file offset gives the bias
       from file offsets to symbol addresses. */
    Elf32_Phdr *ph = (Elf32_Phdr *)(f + eh->e_phoff);
    for (int i = 0; i < eh->e_phnum; i++)
        if (ph[i].p_type == PT_LOAD && m->offset >= ph[i].p_offset &&
            m->offset < ph[i].p_offset + (ph[i].p_filesz ? ph[i].p_filesz : 1)) {
            m->bias = (int64_t)ph[i].p_vaddr - ph[i].p_offset;
            break;
        }
    Elf32_Shdr *sh = (Elf32_Shdr *)(f + eh->e_shoff);
    if (!eh->e_shoff || eh->e_shoff + (uint64_t)eh->e_shnum * sizeof *sh > (uint64_t)st.st_size) goto out;
    int want = -1;
    for (int i = 0; i < eh->e_shnum; i++)
        if (sh[i].sh_type == SHT_SYMTAB) want = i;
    if (want < 0)
        for (int i = 0; i < eh->e_shnum; i++)
            if (sh[i].sh_type == SHT_DYNSYM) want = i;
    if (want < 0) goto out;
    Elf32_Shdr *ss = &sh[want], *strs = &sh[ss->sh_link];
    size_t n = ss->sh_size / sizeof(Elf32_Sym);
    Elf32_Sym *sym = (Elf32_Sym *)(f + ss->sh_offset);
    m->strtab_copy = malloc(strs->sh_size + 1);
    memcpy(m->strtab_copy, f + strs->sh_offset, strs->sh_size);
    m->strtab_copy[strs->sh_size] = 0;
    m->syms = calloc(n ? n : 1, sizeof(sym_t));
    for (size_t i = 0; i < n; i++) {
        int type = ELF32_ST_TYPE(sym[i].st_info);
        if ((type != STT_FUNC && type != STT_NOTYPE) || !sym[i].st_value || sym[i].st_shndx == SHN_UNDEF) continue;
        if (type == STT_NOTYPE && !sym[i].st_size) continue;
        const char *name = m->strtab_copy + sym[i].st_name;
        if (!name[0] || name[0] == '$') continue;      /* ARM mapping symbols */
        sym_t *s = &m->syms[m->nsyms++];
        s->addr = sym[i].st_value & ~1u;                 /* Thumb bit */
        s->size = sym[i].st_size;
        s->name = name;
    }
    qsort(m->syms, m->nsyms, sizeof(sym_t), by_addr);
out:
    munmap(f, st.st_size);
}

static const sym_t *find_sym(const sym_t *s, size_t n, uint64_t addr)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (s[mid].addr <= addr) lo = mid + 1;
        else hi = mid;
    }
    if (!lo) return NULL;
    const sym_t *c = &s[lo - 1];
    if (c->size && addr >= c->addr + c->size) return NULL;
    return c;
}

static void load_kallsyms(void)
{
    FILE *f = fopen("/proc/kallsyms", "r");
    if (!f) return;
    char line[256];
    size_t cap = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long long a;
        char type, name[128];
        if (sscanf(line, "%llx %c %127s", &a, &type, name) != 3 || !a) continue;
        if (type != 't' && type != 'T') continue;
        if (nksyms == cap) {
            cap = cap ? cap * 2 : 65536;
            ksyms = realloc(ksyms, cap * sizeof *ksyms);
        }
        ksyms[nksyms].addr = a;
        ksyms[nksyms].size = 0;
        ksyms[nksyms].name = strdup(name);
        nksyms++;
    }
    fclose(f);
    qsort(ksyms, nksyms, sizeof(sym_t), by_addr);
}

/* "file" and "function" for an address; each is valid until the next call. */
static void place(uint64_t ip, char *file, size_t fsize, char *func, size_t nsize)
{
    if (ip > 0xffffffffull) {
        snprintf(file, fsize, "[kernel]");
        const sym_t *s = nksyms ? find_sym(ksyms, nksyms, ip) : NULL;
        snprintf(func, nsize, "%s", s ? s->name : "?");
        return;
    }
    for (int i = 0; i < nmaps; i++) {
        map_t *m = &maps[i];
        if (ip < m->start || ip >= m->end) continue;
        if (!m->path[0] || m->path[0] == '[') {
            snprintf(file, fsize, "%s", m->exec ? "[jit]" : (m->path[0] ? m->path : "[anon]"));
            snprintf(func, nsize, "-");
            return;
        }
        const char *base = strrchr(m->path, '/');
        snprintf(file, fsize, "%s", base ? base + 1 : m->path);
        if (!m->loaded) load_syms(m);
        uint64_t vaddr = ip - m->start + m->offset + m->bias;
        const sym_t *s = m->nsyms ? find_sym(m->syms, m->nsyms, vaddr) : NULL;
        if (s) snprintf(func, nsize, "%s", s->name);
        else snprintf(func, nsize, "+%#llx (1 KB block)", (unsigned long long)(vaddr & ~0x3ffull));
        return;
    }
    snprintf(file, fsize, "[unmapped]");
    snprintf(func, nsize, "-");
}

/* ---- report -------------------------------------------------------------- */

typedef struct { char key[200]; uint32_t n; } tally_t;

static void tally(tally_t **t, size_t *n, size_t *cap, const char *key)
{
    for (size_t i = 0; i < *n; i++)
        if (!strcmp((*t)[i].key, key)) { (*t)[i].n++; return; }
    if (*n == *cap) {
        *cap = *cap ? *cap * 2 : 256;
        *t = realloc(*t, *cap * sizeof **t);
    }
    snprintf((*t)[*n].key, sizeof (*t)[*n].key, "%s", key);
    (*t)[*n].n = 1;
    (*n)++;
}

static int by_n(const void *a, const void *b)
{
    const tally_t *x = a, *y = b;
    return x->n < y->n ? 1 : x->n > y->n ? -1 : 0;
}

static int by_ip(const void *a, const void *b)
{
    const sample_t *x = a, *y = b;
    if (x->tid != y->tid) return x->tid < y->tid ? -1 : 1;
    return x->ip < y->ip ? -1 : x->ip > y->ip;
}

static void report(int pid, double secs, unsigned hz, int top)
{
    printf("xbprof: pid %d, %.1f s at %u Hz of each thread's CPU time, %zu samples, %llu lost\n",
           pid, secs, hz, nsamples, (unsigned long long)lost);
    printf("(\"of a core\" is the thread's CPU time over the run; [jit] is code box86 generated)\n");
    qsort(samples, nsamples, sizeof *samples, by_ip);
    /* Threads, busiest first. */
    uint32_t order[MAX_THREADS];
    uint32_t counts[MAX_THREADS] = { 0 };
    for (size_t i = 0; i < nsamples; i++)
        for (int t = 0; t < nthreads; t++)
            if (threads[t].tid == samples[i].tid) { counts[t]++; break; }
    for (int t = 0; t < nthreads; t++) order[t] = (uint32_t)t;
    for (int a = 0; a < nthreads; a++)
        for (int b = a + 1; b < nthreads; b++)
            if (counts[order[b]] > counts[order[a]]) { uint32_t x = order[a]; order[a] = order[b]; order[b] = x; }
    double per_core = secs * hz;
    for (int oi = 0; oi < nthreads; oi++) {
        watched_t *t = &threads[order[oi]];
        uint32_t n = counts[order[oi]];
        if (n < per_core * 0.01) continue;           /* under 1% of a core */
        printf("\n== thread %u %s: %.1f%% of a core\n", t->tid, t->comm, 100.0 * n / per_core);
        tally_t *files = NULL, *funcs = NULL;
        size_t nf = 0, cf = 0, nfn = 0, cfn = 0;
        for (size_t i = 0; i < nsamples; i++) {
            if (samples[i].tid != t->tid) continue;
            char file[128], func[160], key[300];
            place(samples[i].ip, file, sizeof file, func, sizeof func);
            tally(&files, &nf, &cf, file);
            snprintf(key, sizeof key, "%s  %s", file, func);
            tally(&funcs, &nfn, &cfn, key);
        }
        qsort(files, nf, sizeof *files, by_n);
        qsort(funcs, nfn, sizeof *funcs, by_n);
        printf("   by file:");
        for (size_t i = 0; i < nf && i < 8; i++) printf("%s %.1f%% %s", i ? "," : "", 100.0 * files[i].n / n, files[i].key);
        printf("\n");
        for (size_t i = 0; i < nfn && (int)i < top; i++) {
            if (funcs[i].n * 1000 < n * 3) break;     /* under 0.3% of the thread */
            printf("   %5.1f%%  %s\n", 100.0 * funcs[i].n / n, funcs[i].key);
        }
        free(files);
        free(funcs);
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: xbprof PID [SECONDS] [HZ] [TOP]\n");
        return 2;
    }
    int pid = atoi(argv[1]);
    double secs = argc > 2 ? atof(argv[2]) : 20;
    unsigned hz = argc > 3 ? (unsigned)atoi(argv[3]) : 1000;
    int top = argc > 4 ? atoi(argv[4]) : 30;
    if (pid <= 0 || secs <= 0 || hz == 0 || hz > 20000) {
        fprintf(stderr, "xbprof: bad arguments\n");
        return 2;
    }
    page = (size_t)sysconf(_SC_PAGESIZE);
    uint64_t period = 1000000000ull / hz;
    scan_threads(pid, period);
    if (!nthreads) {
        fprintf(stderr, "xbprof: no threads to watch in pid %d\n", pid);
        return 1;
    }
    struct timespec t0, now;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int rescan = 0;
    for (;;) {
        struct timespec nap = { 0, 20 * 1000000 };
        nanosleep(&nap, NULL);
        for (int i = 0; i < nthreads; i++) drain(&threads[i]);
        if (++rescan == 25) { rescan = 0; scan_threads(pid, period); }
        clock_gettime(CLOCK_MONOTONIC, &now);
        double el = (now.tv_sec - t0.tv_sec) + (now.tv_nsec - t0.tv_nsec) / 1e9;
        if (el >= secs) { secs = el; break; }
        if (kill(pid, 0) != 0) { secs = el; break; }
    }
    for (int i = 0; i < nthreads; i++) {
        ioctl(threads[i].fd, PERF_EVENT_IOC_DISABLE, 0);
        drain(&threads[i]);
        read_comm(pid, threads[i].tid, threads[i].comm, sizeof threads[i].comm);
    }
    read_maps(pid);
    load_kallsyms();
    report(pid, secs, hz, top);
    return 0;
}
