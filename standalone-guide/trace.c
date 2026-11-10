/* Diagnostic API tracer for the converted runtime (enabled with --trace).
 * Redirects the original runtime's IAT entries, and functions it resolves
 * through GetProcAddress, to small thunks that log caller, arguments, return
 * value and last error. The host's own imports are untouched, so logging never
 * re-enters the tracer. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>

void trace_log(const char *text);

typedef struct { const char *name; void *target; BYTE flags; } trace_entry;
enum { TRACE_NO_RETURN = 1 };
#define TRACE_MAX_ENTRIES 4096
static trace_entry entries[TRACE_MAX_ENTRIES];
static LONG entry_count;
static BYTE *thunk_pool;
static volatile LONG tracing;

/* Per-thread shadow stack of displaced return addresses. */
typedef struct { DWORD thread; DWORD depth; DWORD ret[512]; DWORD entry[512]; } shadow_stack;
static shadow_stack stacks[64];

static shadow_stack *thread_stack(void) {
    DWORD id = GetCurrentThreadId();
    for (int i = 0; i < 64; ++i) if (stacks[i].thread == id) return &stacks[i];
    for (int i = 0; i < 64; ++i)
        if (!stacks[i].thread && !InterlockedCompareExchange((LONG *)&stacks[i].thread, id, 0))
            return &stacks[i];
    return 0;
}

static BOOL readable(const void *p, DWORD bytes) {
    MEMORY_BASIC_INFORMATION m;
    if ((ULONG_PTR)p < 0x10000 || !VirtualQuery(p, &m, sizeof(m)) || m.State != MEM_COMMIT) return FALSE;
    if (m.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return FALSE;
    return (BYTE *)p + bytes <= (BYTE *)m.BaseAddress + m.RegionSize;
}

static void append_string(char *line, const void *p, BOOL wide) {
    char text[200]; int n = 0;
    if (!readable(p, 2)) return;
    if (wide) {
        const WCHAR *s = p;
        while (n < 190 && readable(s + n, 2) && s[n]) { text[n] = s[n] < 128 ? (char)s[n] : '?'; ++n; }
    } else {
        const char *s = p;
        while (n < 190 && readable(s + n, 1) && s[n]) { text[n] = s[n] >= 32 && s[n] < 127 ? s[n] : '?'; ++n; }
    }
    text[n] = 0;
    if (n > 1) { lstrcatA(line, " \""); lstrcatA(line, text); lstrcatA(line, "\""); }
}

static BOOL wide_name(const char *name) { int n = lstrlenA(name); return n && name[n-1] == 'W'; }

/* Called with frame[0] = entry index, frame[1] = return address, frame[2..] = arguments.
 * Returns the address to jump to. */
void *__cdecl trace_enter(DWORD *frame) {
    DWORD error = GetLastError();
    trace_entry *e = &entries[frame[0]];
    if (tracing) {
        char line[1024];
        wsprintfA(line, "> %s from %08lX (%08lX %08lX %08lX %08lX)", e->name, frame[1],
                  frame[2], frame[3], frame[4], frame[5]);
        BOOL wide = wide_name(e->name);
        for (int i = 2; i < 6; ++i) append_string(line, (void *)frame[i], wide);
        trace_log(line);
        shadow_stack *s = e->flags & TRACE_NO_RETURN ? 0 : thread_stack();
        if (s && s->depth < 512) {
            extern void trace_return_stub(void);
            s->ret[s->depth] = frame[1]; s->entry[s->depth] = frame[0]; ++s->depth;
            frame[1] = (DWORD)trace_return_stub;
        }
    }
    SetLastError(error);
    return e->target;
}

/* Returns the displaced return address. */
DWORD __cdecl trace_leave(DWORD result) {
    DWORD error = GetLastError();
    shadow_stack *s = thread_stack();
    --s->depth;
    char line[256];
    wsprintfA(line, "< %s = %08lX (error %lu)", entries[s->entry[s->depth]].name, result, error);
    trace_log(line);
    SetLastError(error);
    return s->ret[s->depth];
}

__asm__(
    ".text\n"
    ".globl _trace_common\n"
    "_trace_common:\n"
    "  pushfl\n"
    "  pushal\n"
    "  leal 36(%esp), %eax\n"
    "  pushl %eax\n"
    "  call _trace_enter\n"
    "  addl $4, %esp\n"
    "  movl %eax, 36(%esp)\n"
    "  popal\n"
    "  popfl\n"
    "  ret\n"
    ".globl _trace_return_stub\n"
    "_trace_return_stub:\n"
    "  subl $4, %esp\n"
    "  pushfl\n"
    "  pushal\n"
    "  pushl %eax\n"
    "  call _trace_leave\n"
    "  addl $4, %esp\n"
    "  movl %eax, 36(%esp)\n"
    "  popal\n"
    "  popfl\n"
    "  ret\n");
extern void trace_common(void);

static const char *const quiet[] = {
    "GetLastError", "SetLastError", "TlsGetValue", "TlsSetValue", "EnterCriticalSection",
    "LeaveCriticalSection", "TryEnterCriticalSection", "InterlockedIncrement", "InterlockedDecrement",
    "InterlockedExchange", "InterlockedCompareExchange", "InterlockedExchangeAdd", "HeapAlloc",
    "HeapFree", "HeapReAlloc", "HeapSize", "GetCurrentThreadId", "GetCurrentThread",
    "GetCurrentProcess", "GetCurrentProcessId", "GetTickCount", "QueryPerformanceCounter",
    "lstrlenW", "lstrlenA", "MultiByteToWideChar", "WideCharToMultiByte", "GetProcessHeap",
    "LocalAlloc", "LocalFree", "GlobalAlloc", "GlobalFree", "SysAllocString", "SysFreeString",
    "VariantInit", "VariantClear", "GetSystemTimeAsFileTime", 0 };
static const char *const no_return[] = {
    "ExitProcess", "ExitThread", "RaiseException", "TerminateProcess", "longjmp", "_CxxThrowException",
    "FreeLibraryAndExitThread", "_setjmp3", 0 };

static BOOL listed(const char *name, const char *const *list) {
    for (int i = 0; list[i]; ++i) if (!lstrcmpA(name, list[i])) return TRUE;
    return FALSE;
}

/* Builds "push index; jmp trace_common" and returns it. */
void *trace_wrap(const char *name, void *target) {
    if (!thunk_pool || !target || listed(name, quiet)) return target;
    LONG index = InterlockedIncrement(&entry_count) - 1;
    if (index >= TRACE_MAX_ENTRIES) return target;
    char *copy = HeapAlloc(GetProcessHeap(), 0, lstrlenA(name) + 1);
    lstrcpyA(copy, name);
    entries[index].name = copy; entries[index].target = target;
    entries[index].flags = listed(name, no_return) ? TRACE_NO_RETURN : 0;
    BYTE *thunk = thunk_pool + index * 10;
    thunk[0] = 0x68; *(DWORD *)(thunk + 1) = index;
    thunk[5] = 0xE9; *(DWORD *)(thunk + 6) = (DWORD)trace_common - (DWORD)(thunk + 10);
    return thunk;
}

void trace_set(BOOL on) { InterlockedExchange(&tracing, on); }

/* Wraps every IAT entry that belongs to the original runtime (below host_limit). */
void trace_install(HMODULE image, DWORD host_limit) {
    thunk_pool = VirtualAlloc(0, TRACE_MAX_ENTRIES * 10, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    BYTE *base = (BYTE *)image;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR *d = (void *)(base + dir.VirtualAddress); d->Name; ++d) {
        if (d->FirstThunk >= host_limit || !d->OriginalFirstThunk) continue;
        if (!lstrcmpiA((char *)base + d->Name, "MSVCR80.dll")) continue;
        DWORD *names = (DWORD *)(base + d->OriginalFirstThunk);
        void **iat = (void **)(base + d->FirstThunk);
        for (; *names; ++names, ++iat) {
            if (*names & 0x80000000) continue;
            const char *name = (char *)base + *names + 2;
            void *wrapped = trace_wrap(name, *iat);
            if (wrapped == *iat) continue;
            DWORD old, ignored;
            if (VirtualProtect(iat, sizeof(*iat), PAGE_READWRITE, &old)) {
                *iat = wrapped;
                VirtualProtect(iat, sizeof(*iat), old, &ignored);
            }
        }
    }
}
