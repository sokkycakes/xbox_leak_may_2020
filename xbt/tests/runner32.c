/*
 * runner32.c - reference runner: executes test cases natively on an x86 CPU
 * in 32-bit mode and records the final machine state. Freestanding (no libc)
 * so it builds without 32-bit system libraries:
 *
 *   clang --target=i386-linux-gnu -ffreestanding -nostdlib -static -O1 \
 *         -msse -fno-pic -o runner32 runner32.c
 *
 * The memory layout and file formats match `xbt --test` (src/main.c).
 */
typedef unsigned int u32;
typedef unsigned char u8;

#define TEST_CODE   0x00400000u
#define TEST_STOP   0x00410000u
#define TEST_DATA   0x00500000u
#define TEST_DATA_SIZE 0x1000u
#define TEST_SLOT   0x00510000u
#define TEST_STACK_TOP 0x0060f000u
#define TEST_STACK_DUMP 512u

static int sys1(int n, u32 a)
{
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a) : "memory");
    return r;
}
static int sys3(int n, u32 a, u32 b, u32 c)
{
    int r;
    __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory");
    return r;
}

void *memcpy(void *d, const void *s, u32 n)
{
    u8 *dd = d;
    const u8 *ss = s;
    while (n--)
        *dd++ = *ss++;
    return d;
}
void *memset(void *d, int c, u32 n)
{
    u8 *dd = d;
    while (n--)
        *dd++ = (u8)c;
    return d;
}

static void *map(u32 addr, u32 len, int fixed)
{
    struct { u32 addr, len, prot, flags, fd, off; } a = {
        addr, len, 7, 0x22 | (fixed ? 0x10 : 0), (u32)-1, 0
    };
    return (void *)sys1(90, (u32)&a);
}

static void die(const char *m)
{
    u32 n = 0;
    while (m[n])
        n++;
    sys3(4, 2, (u32)m, n);
    sys1(1, 1);
}

typedef struct test_hdr {
    u32 code_len;
    u32 seed;
    u32 regs[8];
    u32 eflags;
    u32 flags_mask;
    u8 xmm[8][16];
} test_hdr;

typedef struct test_res {
    u32 regs[8];
    u32 eflags;
    u32 status;
    u8 xmm[8][16];
    u8 data[TEST_DATA_SIZE];
    u8 stack[TEST_STACK_DUMP];
} test_res;

/* state shared with the assembly below */
u32 g_saved_esp;
u32 g_in_regs[8];
u32 g_in_eflags;
u8 g_in_xmm[128] __attribute__((aligned(16)));
u32 g_out_regs[8];
u32 g_out_eflags;
u8 g_out_xmm[128] __attribute__((aligned(16)));
u8 g_cap_stack[4096] __attribute__((aligned(16)));

void run_test(void);
__asm__(
    ".text\n"
    ".globl run_test\n"
    "run_test:\n"
    "  pushal\n"
    "  movl %esp, g_saved_esp\n"
    "  fninit\n"
    "  pushl $0x1f80\n"
    "  ldmxcsr (%esp)\n"
    "  addl $4, %esp\n"
    "  movups g_in_xmm+0, %xmm0\n"
    "  movups g_in_xmm+16, %xmm1\n"
    "  movups g_in_xmm+32, %xmm2\n"
    "  movups g_in_xmm+48, %xmm3\n"
    "  movups g_in_xmm+64, %xmm4\n"
    "  movups g_in_xmm+80, %xmm5\n"
    "  movups g_in_xmm+96, %xmm6\n"
    "  movups g_in_xmm+112, %xmm7\n"
    "  pushl g_in_eflags\n"
    "  popfl\n"
    "  movl g_in_regs+0, %eax\n"
    "  movl g_in_regs+4, %ecx\n"
    "  movl g_in_regs+8, %edx\n"
    "  movl g_in_regs+12, %ebx\n"
    "  movl g_in_regs+20, %ebp\n"
    "  movl g_in_regs+24, %esi\n"
    "  movl g_in_regs+28, %edi\n"
    "  movl $0x0060f000, %esp\n"
    "  jmp *test_code_ptr\n"
    ".globl capture\n"
    "capture:\n"
    "  movl %eax, g_out_regs+0\n"
    "  movl %ecx, g_out_regs+4\n"
    "  movl %edx, g_out_regs+8\n"
    "  movl %ebx, g_out_regs+12\n"
    "  movl %esp, g_out_regs+16\n"
    "  movl %ebp, g_out_regs+20\n"
    "  movl %esi, g_out_regs+24\n"
    "  movl %edi, g_out_regs+28\n"
    "  movl $g_cap_stack+4000, %esp\n"
    "  pushfl\n"
    "  popl g_out_eflags\n"
    "  movups %xmm0, g_out_xmm+0\n"
    "  movups %xmm1, g_out_xmm+16\n"
    "  movups %xmm2, g_out_xmm+32\n"
    "  movups %xmm3, g_out_xmm+48\n"
    "  movups %xmm4, g_out_xmm+64\n"
    "  movups %xmm5, g_out_xmm+80\n"
    "  movups %xmm6, g_out_xmm+96\n"
    "  movups %xmm7, g_out_xmm+112\n"
    "  cld\n"
    "  emms\n"
    "  fninit\n"
    "  movl g_saved_esp, %esp\n"
    "  popal\n"
    "  ret\n"
    ".data\n"
    "test_code_ptr: .long 0x00400000\n"
    ".text\n");
void capture(void);

static u32 lcg_fill(u8 *p, u32 n, u32 seed)
{
    u32 x = seed;
    for (u32 i = 0; i < n; i++) {
        x = x * 1103515245u + 12345u;
        p[i] = (u8)(x >> 16);
    }
    return x;
}

static u8 *g_file;
static test_res g_res;

void _start_c(u32 *sp)
{
    u32 argc = sp[0];
    char **argv = (char **)(sp + 1);
    if (argc < 3)
        die("usage: runner32 cases.bin results.bin\n");
    int fd = sys3(5, (u32)argv[1], 0, 0);
    if (fd < 0)
        die("cannot open input\n");
    g_file = map(0, 256u << 20, 0);
    u32 len = 0;
    for (;;) {
        int n = sys3(3, (u32)fd, (u32)(g_file + len), 1u << 20);
        if (n <= 0)
            break;
        len += (u32)n;
    }
    if (map(TEST_CODE, 0x20000, 1) != (void *)TEST_CODE ||
        map(TEST_DATA, 0x20000, 1) != (void *)TEST_DATA ||
        map(0x00600000, 0x10000, 1) != (void *)0x00600000)
        die("cannot map test memory\n");
    int out = sys3(5, (u32)argv[2], 0x241, 0644);   /* O_WRONLY|O_CREAT|O_TRUNC */
    if (out < 0)
        die("cannot open output\n");
    u32 count = *(u32 *)(g_file + 4);
    sys3(4, (u32)out, (u32)"XTR1", 4);
    sys3(4, (u32)out, (u32)&count, 4);
    u32 pos = 8;
    for (u32 t = 0; t < count; t++) {
        test_hdr *h = (test_hdr *)(g_file + pos);
        pos += sizeof(test_hdr);
        u8 *code = g_file + pos;
        pos += (h->code_len + 3) & ~3u;
        u8 *cp = (u8 *)TEST_CODE;
        memset(cp, 0xcc, 0x1000);
        memcpy(cp, code, h->code_len);
        static const u8 tail[6] = { 0xff, 0x25, 0x00, 0x00, 0x51, 0x00 };
        memcpy(cp + h->code_len, tail, 6);
        u32 x = lcg_fill((u8 *)TEST_DATA, TEST_DATA_SIZE, h->seed);
        lcg_fill((u8 *)(TEST_STACK_TOP - TEST_STACK_DUMP), TEST_STACK_DUMP, x);
        *(u32 *)TEST_SLOT = (u32)capture;
        memcpy(g_in_regs, h->regs, 32);
        g_in_eflags = (h->eflags & 0x00000ed5u) | 2;
        memcpy(g_in_xmm, h->xmm, 128);
        run_test();
        memcpy(g_res.regs, g_out_regs, 32);
        g_res.eflags = g_out_eflags;
        g_res.status = 0;
        memcpy(g_res.xmm, g_out_xmm, 128);
        memcpy(g_res.data, (void *)TEST_DATA, TEST_DATA_SIZE);
        memcpy(g_res.stack, (void *)(TEST_STACK_TOP - TEST_STACK_DUMP), TEST_STACK_DUMP);
        sys3(4, (u32)out, (u32)&g_res, sizeof g_res);
    }
    sys1(1, 0);
}

__asm__(".text\n.globl _start\n_start:\n  movl %esp, %eax\n  pushl %eax\n  call _start_c\n");
