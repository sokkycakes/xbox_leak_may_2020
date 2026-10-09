/* Rtl*: strings, memory, critical sections, time fields, SEH and printf. */
#define _GNU_SOURCE
#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "../xbcompat.h"
#include "../cpu.h"

/* ---- memory ----------------------------------------------------------- */

SIZE_T NTAPI RtlCompareMemory(const void *a, const void *b, SIZE_T n)
{
    const UCHAR *x = a, *y = b;
    SIZE_T i = 0;
    while (i < n && x[i] == y[i]) i++;
    return i;
}

SIZE_T NTAPI RtlCompareMemoryUlong(const ULONG *p, SIZE_T n, ULONG v)
{
    SIZE_T i = 0;
    while (i + 4 <= n && p[i / 4] == v) i += 4;
    return i;
}

void NTAPI RtlFillMemoryUlong(ULONG *p, SIZE_T n, ULONG v)
{
    for (SIZE_T i = 0; i < n / 4; i++) p[i] = v;
}

void NTAPI RtlZeroMemory(void *p, SIZE_T n) { memset(p, 0, n); }
void NTAPI RtlFillMemory(void *p, SIZE_T n, UCHAR v) { memset(p, v, n); }
void NTAPI RtlMoveMemory(void *d, const void *s, SIZE_T n) { memmove(d, s, n); }

ULONG FASTCALL RtlUlongByteSwap(ULONG v) { return __builtin_bswap32(v); }
USHORT FASTCALL RtlUshortByteSwap(USHORT v) { return __builtin_bswap16(v); }

/* ---- strings ---------------------------------------------------------- */

void NTAPI RtlInitAnsiString(ANSI_STRING *s, const char *src)
{
    s->Buffer = (char *)src;
    s->Length = src ? strlen(src) : 0;
    s->MaximumLength = src ? s->Length + 1 : 0;
}

void NTAPI RtlInitUnicodeString(UNICODE_STRING *s, const WCHAR *src)
{
    size_t n = 0;
    if (src) while (src[n]) n++;
    s->Buffer = (WCHAR *)src;
    s->Length = n * 2;
    s->MaximumLength = src ? s->Length + 2 : 0;
}

BOOLEAN NTAPI RtlEqualString(const ANSI_STRING *a, const ANSI_STRING *b, BOOLEAN CaseInsensitive)
{
    if (a->Length != b->Length) return 0;
    return CaseInsensitive ? !strncasecmp(a->Buffer, b->Buffer, a->Length)
                           : !memcmp(a->Buffer, b->Buffer, a->Length);
}

LONG NTAPI RtlCompareString(const ANSI_STRING *a, const ANSI_STRING *b, BOOLEAN CaseInsensitive)
{
    USHORT n = a->Length < b->Length ? a->Length : b->Length;
    for (USHORT i = 0; i < n; i++) {
        int x = (UCHAR)a->Buffer[i], y = (UCHAR)b->Buffer[i];
        if (CaseInsensitive) { x = toupper(x); y = toupper(y); }
        if (x != y) return x - y;
    }
    return (LONG)a->Length - (LONG)b->Length;
}

BOOLEAN NTAPI RtlEqualUnicodeString(const UNICODE_STRING *a, const UNICODE_STRING *b, BOOLEAN ci)
{
    if (a->Length != b->Length) return 0;
    for (USHORT i = 0; i < a->Length / 2; i++) {
        WCHAR x = a->Buffer[i], y = b->Buffer[i];
        if (ci && x < 128 && y < 128) { x = toupper(x); y = toupper(y); }
        if (x != y) return 0;
    }
    return 1;
}

CHAR NTAPI RtlUpperChar(CHAR c) { return (CHAR)toupper((UCHAR)c); }
CHAR NTAPI RtlLowerChar(CHAR c) { return (CHAR)tolower((UCHAR)c); }
WCHAR NTAPI RtlUpcaseUnicodeChar(WCHAR c) { return c < 128 ? (WCHAR)toupper(c) : c; }
WCHAR NTAPI RtlDowncaseUnicodeChar(WCHAR c) { return c < 128 ? (WCHAR)tolower(c) : c; }

void NTAPI RtlUpperString(ANSI_STRING *d, const ANSI_STRING *s)
{
    USHORT n = s->Length < d->MaximumLength ? s->Length : d->MaximumLength;
    for (USHORT i = 0; i < n; i++) d->Buffer[i] = (char)toupper((UCHAR)s->Buffer[i]);
    d->Length = n;
}

void NTAPI RtlCopyString(ANSI_STRING *d, const ANSI_STRING *s)
{
    if (!s) { d->Length = 0; return; }
    USHORT n = s->Length < d->MaximumLength ? s->Length : d->MaximumLength;
    memcpy(d->Buffer, s->Buffer, n);
    d->Length = n;
}

NTSTATUS NTAPI RtlUnicodeStringToAnsiString(ANSI_STRING *d, const UNICODE_STRING *s, BOOLEAN Allocate)
{
    USHORT n = s->Length / 2;
    if (Allocate) {
        d->Buffer = pool_alloc(n + 1);
        d->MaximumLength = n + 1;
    } else if (n >= d->MaximumLength) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    for (USHORT i = 0; i < n; i++) d->Buffer[i] = s->Buffer[i] < 256 ? (char)s->Buffer[i] : '?';
    d->Buffer[n] = 0;
    d->Length = n;
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI RtlAnsiStringToUnicodeString(UNICODE_STRING *d, const ANSI_STRING *s, BOOLEAN Allocate)
{
    USHORT n = s->Length;
    if (Allocate) {
        d->Buffer = pool_alloc((n + 1) * 2);
        d->MaximumLength = (n + 1) * 2;
    } else if ((n + 1) * 2 > d->MaximumLength) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    for (USHORT i = 0; i < n; i++) d->Buffer[i] = (UCHAR)s->Buffer[i];
    d->Buffer[n] = 0;
    d->Length = n * 2;
    return STATUS_SUCCESS;
}

void NTAPI RtlFreeAnsiString(ANSI_STRING *s) { pool_free(s->Buffer); s->Buffer = NULL; }
void NTAPI RtlFreeUnicodeString(UNICODE_STRING *s) { pool_free(s->Buffer); s->Buffer = NULL; }

NTSTATUS NTAPI RtlMultiByteToUnicodeN(WCHAR *d, ULONG dmax, ULONG *written, const char *s, ULONG slen)
{
    ULONG n = slen < dmax / 2 ? slen : dmax / 2;
    for (ULONG i = 0; i < n; i++) d[i] = (UCHAR)s[i];
    if (written) *written = n * 2;
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI RtlUnicodeToMultiByteN(char *d, ULONG dmax, ULONG *written, const WCHAR *s, ULONG slen)
{
    ULONG n = slen / 2 < dmax ? slen / 2 : dmax;
    for (ULONG i = 0; i < n; i++) d[i] = s[i] < 256 ? (char)s[i] : '?';
    if (written) *written = n;
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI RtlMultiByteToUnicodeSize(ULONG *size, const char *s, ULONG n) { (void)s; *size = n * 2; return 0; }
NTSTATUS NTAPI RtlUnicodeToMultiByteSize(ULONG *size, const WCHAR *s, ULONG n) { (void)s; *size = n / 2; return 0; }

NTSTATUS NTAPI RtlCharToInteger(const char *s, ULONG base, ULONG *value)
{
    *value = strtoul(s, NULL, base);
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI RtlIntegerToChar(ULONG value, ULONG base, LONG len, char *out)
{
    char buf[40];
    const char *digits = "0123456789ABCDEF";
    int i = 0;
    if (!base) base = 10;
    do { buf[i++] = digits[value % base]; value /= base; } while (value);
    if (len < i + 1) return STATUS_BUFFER_TOO_SMALL;
    for (int j = 0; j < i; j++) out[j] = buf[i - 1 - j];
    out[i] = 0;
    return STATUS_SUCCESS;
}

/* ---- critical sections ----------------------------------------------- */

typedef struct {
    DISPATCHER_HEADER Event;
    LONG LockCount;
    LONG RecursionCount;
    HANDLE OwningThread;
} RTL_CRITICAL_SECTION;

extern void NTAPI KeInitializeEvent(KEVENT *, ULONG, BOOLEAN);
extern LONG NTAPI KeSetEvent(KEVENT *, LONG, BOOLEAN);
extern NTSTATUS NTAPI KeWaitForSingleObject(PVOID, ULONG, KPROCESSOR_MODE, BOOLEAN, LARGE_INTEGER *);

void NTAPI RtlInitializeCriticalSection(RTL_CRITICAL_SECTION *cs)
{
    KeInitializeEvent((KEVENT *)&cs->Event, EventSynchronizationObject, 0);
    cs->LockCount = -1;
    cs->RecursionCount = 0;
    cs->OwningThread = NULL;
}

void NTAPI RtlEnterCriticalSection(RTL_CRITICAL_SECTION *cs)
{
    HANDLE self = thread_current();
    if (__sync_add_and_fetch(&cs->LockCount, 1) == 0) {
        cs->OwningThread = self;
        cs->RecursionCount = 1;
    } else if (cs->OwningThread == self) {
        cs->RecursionCount++;
    } else {
        KeWaitForSingleObject(&cs->Event, 0, 0, 0, NULL);
        cs->OwningThread = self;
        cs->RecursionCount = 1;
    }
}

void NTAPI RtlLeaveCriticalSection(RTL_CRITICAL_SECTION *cs)
{
    if (--cs->RecursionCount != 0) {
        __sync_sub_and_fetch(&cs->LockCount, 1);
        return;
    }
    cs->OwningThread = NULL;
    if (__sync_sub_and_fetch(&cs->LockCount, 1) >= 0)
        KeSetEvent((KEVENT *)&cs->Event, 0, 0);
}

BOOLEAN NTAPI RtlTryEnterCriticalSection(RTL_CRITICAL_SECTION *cs)
{
    HANDLE self = thread_current();
    if (__sync_bool_compare_and_swap(&cs->LockCount, -1, 0)) {
        cs->OwningThread = self;
        cs->RecursionCount = 1;
        return 1;
    }
    if (cs->OwningThread == self) {
        __sync_add_and_fetch(&cs->LockCount, 1);
        cs->RecursionCount++;
        return 1;
    }
    return 0;
}

/* ---- time fields ------------------------------------------------------ */

typedef struct {
    SHORT Year, Month, Day, Hour, Minute, Second, Milliseconds, Weekday;
} TIME_FIELDS;

static int days_before_year(int y) { y--; return y * 365 + y / 4 - y / 100 + y / 400; }
static bool leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
static const int mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

void NTAPI RtlTimeToTimeFields(const LARGE_INTEGER *t, TIME_FIELDS *tf)
{
    ULONGLONG v = t->QuadPart;
    ULONGLONG ms = v / 10000;
    ULONG days = ms / 86400000ULL;
    ULONG rem = ms % 86400000ULL;
    tf->Milliseconds = rem % 1000; rem /= 1000;
    tf->Second = rem % 60; rem /= 60;
    tf->Minute = rem % 60;
    tf->Hour = rem / 60;
    tf->Weekday = (days + 1) % 7;   /* 1601-01-01 was a Monday */
    int y = 1601;
    int base = days_before_year(1601);
    while (days_before_year(y + 1) - base <= (int)days) y++;
    days -= days_before_year(y) - base;
    int m = 0;
    for (; m < 12; m++) {
        int md = mdays[m] + (m == 1 && leap(y));
        if ((int)days < md) break;
        days -= md;
    }
    tf->Year = y;
    tf->Month = m + 1;
    tf->Day = days + 1;
}

BOOLEAN NTAPI RtlTimeFieldsToTime(const TIME_FIELDS *tf, LARGE_INTEGER *t)
{
    if (tf->Month < 1 || tf->Month > 12 || tf->Year < 1601) return 0;
    LONGLONG days = days_before_year(tf->Year) - days_before_year(1601);
    for (int m = 0; m < tf->Month - 1; m++) days += mdays[m] + (m == 1 && leap(tf->Year));
    days += tf->Day - 1;
    LONGLONG ms = ((days * 24 + tf->Hour) * 60 + tf->Minute) * 60000LL + tf->Second * 1000LL + tf->Milliseconds;
    t->QuadPart = ms * 10000;
    return 1;
}

/* ---- large integer helpers ------------------------------------------- */

LONGLONG NTAPI RtlExtendedIntegerMultiply(LONGLONG a, LONG b) { return a * b; }

LONGLONG NTAPI RtlExtendedLargeIntegerDivide(ULONGLONG a, ULONG b, ULONG *rem)
{
    if (rem) *rem = a % b;
    return a / b;
}

LONGLONG NTAPI RtlExtendedMagicDivide(LONGLONG Dividend, ULONGLONG MagicDivisor, CCHAR ShiftCount)
{
    bool neg = Dividend < 0;
    ULONGLONG a = (ULONGLONG)(neg ? -Dividend : Dividend), b = MagicDivisor;
    /* High 64 bits of the 128-bit product, from 32-bit halves. */
    ULONGLONG al = (ULONG)a, ah = a >> 32, bl = (ULONG)b, bh = b >> 32;
    ULONGLONG mid = (al * bl >> 32) + (ULONG)(ah * bl) + (ULONG)(al * bh);
    ULONGLONG high = ah * bh + (ah * bl >> 32) + (al * bh >> 32) + (mid >> 32);
    LONGLONG q = (LONGLONG)(high >> ShiftCount);
    return neg ? -q : q;
}

/* ---- status codes ----------------------------------------------------- */

ULONG NTAPI RtlNtStatusToDosError(NTSTATUS st)
{
    switch ((ULONG)st) {
    case 0: return 0;
    case STATUS_TIMEOUT: return 1460;               /* ERROR_TIMEOUT */
    case STATUS_PENDING: return 997;                /* ERROR_IO_PENDING */
    case STATUS_OBJECT_NAME_EXISTS: return 183;     /* ERROR_ALREADY_EXISTS */
    case STATUS_NO_MORE_FILES: return 18;
    case STATUS_INVALID_HANDLE: return 6;
    case STATUS_INVALID_PARAMETER: return 87;
    case STATUS_NO_SUCH_FILE:
    case STATUS_OBJECT_NAME_NOT_FOUND: return 2;    /* ERROR_FILE_NOT_FOUND */
    case STATUS_OBJECT_PATH_NOT_FOUND: return 3;    /* ERROR_PATH_NOT_FOUND */
    case STATUS_END_OF_FILE: return 38;             /* ERROR_HANDLE_EOF */
    case STATUS_NO_MEMORY:
    case STATUS_INSUFFICIENT_RESOURCES: return 8;
    case STATUS_ACCESS_DENIED: return 5;
    case STATUS_BUFFER_TOO_SMALL: return 122;
    case STATUS_OBJECT_NAME_COLLISION: return 183;
    case STATUS_FILE_IS_A_DIRECTORY: return 5;
    case STATUS_NOT_A_DIRECTORY: return 267;
    case STATUS_DIRECTORY_NOT_EMPTY: return 145;
    case STATUS_NOT_IMPLEMENTED: return 1;
    case STATUS_UNRECOGNIZED_VOLUME: return 1005;
    case 0xC0000010: return 1;                      /* STATUS_INVALID_DEVICE_REQUEST: ERROR_INVALID_FUNCTION */
    case 0xC0000013: return 21;                     /* STATUS_NO_MEDIA_IN_DEVICE: ERROR_NOT_READY */
    case 0xC0000014: return 1785;                   /* STATUS_UNRECOGNIZED_MEDIA: ERROR_UNRECOGNIZED_MEDIA */
    case 0xC00000A2: return 19;                     /* STATUS_MEDIA_WRITE_PROTECTED: ERROR_WRITE_PROTECT */
    default: return 317;                            /* ERROR_MR_MID_NOT_FOUND */
    }
}

/* ---- structured exception handling ----------------------------------- */

typedef struct EXCEPTION_RECORD {
    NTSTATUS ExceptionCode;
    ULONG ExceptionFlags;
    struct EXCEPTION_RECORD *ExceptionRecord;
    PVOID ExceptionAddress;
    ULONG NumberParameters;
    ULONG_PTR ExceptionInformation[15];
} EXCEPTION_RECORD;

typedef struct REGISTRATION {
    struct REGISTRATION *Next;
    ULONG (CDECLAPI *Handler)(EXCEPTION_RECORD *, struct REGISTRATION *, PVOID Context, PVOID Dispatcher);
} REGISTRATION;

#define EXCEPTION_NONCONTINUABLE 0x01
#define EXCEPTION_UNWINDING      0x02
#define EXCEPTION_EXIT_UNWIND    0x04
#define END_OF_CHAIN             ((REGISTRATION *)-1)

#ifdef XBC_NATIVE
static REGISTRATION *seh_head(void)
{
    REGISTRATION *r;
    __asm__ volatile("movl %%fs:0, %0" : "=r"(r));
    return r;
}

static void seh_set_head(REGISTRATION *r)
{
    __asm__ volatile("movl %0, %%fs:0" : : "r"(r));
}

#define CALL_HANDLER(r, rec, ctx, disp) (r)->Handler(rec, r, ctx, disp)
#else
static REGISTRATION *seh_head(void)
{
    return (REGISTRATION *)thread_current()->pcr->NtTib.ExceptionList;
}

static void seh_set_head(REGISTRATION *r)
{
    thread_current()->pcr->NtTib.ExceptionList = (PVOID)r;
}

#define CALL_HANDLER(r, rec, ctx, disp) \
    (ULONG)CPU_CALL((r)->Handler, CONV_CDECL, (uint32_t)(rec), (uint32_t)(r), (uint32_t)(ctx), (uint32_t)(disp))
#endif

/* An i386 CONTEXT is 0x2CC bytes; handlers only look at a few registers. */
typedef struct { ULONG raw[0x2CC / 4]; } CONTEXT;

void NTAPI RtlRaiseException(EXCEPTION_RECORD *rec)
{
    CONTEXT ctx;
    memset(&ctx, 0, sizeof(ctx));
    if (!rec->ExceptionAddress) rec->ExceptionAddress = __builtin_return_address(0);
    xlog("exception %#x at %p (%u params)", rec->ExceptionCode, rec->ExceptionAddress,
         rec->NumberParameters);
    for (REGISTRATION *r = seh_head(); r && r != END_OF_CHAIN; r = r->Next) {
        PVOID dispatcher = NULL;
        ULONG disp = CALL_HANDLER(r, rec, &ctx, &dispatcher);
        if (disp == 0 /* ExceptionContinueExecution */) {
            if (rec->ExceptionFlags & EXCEPTION_NONCONTINUABLE)
                fatal("handler tried to continue a noncontinuable exception %#x", rec->ExceptionCode);
            return;
        }
        /* ExceptionContinueSearch: keep walking. */
    }
    fatal("unhandled guest exception %#x at %p", rec->ExceptionCode, rec->ExceptionAddress);
}

void NTAPI RtlRaiseStatus(NTSTATUS st)
{
    EXCEPTION_RECORD rec = { .ExceptionCode = st, .ExceptionFlags = EXCEPTION_NONCONTINUABLE };
    RtlRaiseException(&rec);
}

void NTAPI RtlUnwind(REGISTRATION *TargetFrame, PVOID TargetIp, EXCEPTION_RECORD *rec, PVOID ReturnValue)
{
    (void)TargetIp; (void)ReturnValue;
    EXCEPTION_RECORD local = { .ExceptionCode = 0xC0000027 /* STATUS_UNWIND */ };
    if (!rec) rec = &local;
    rec->ExceptionFlags |= EXCEPTION_UNWINDING | (TargetFrame ? 0 : EXCEPTION_EXIT_UNWIND);
    CONTEXT ctx;
    memset(&ctx, 0, sizeof(ctx));
    REGISTRATION *r = seh_head();
    while (r && r != END_OF_CHAIN && r != TargetFrame) {
        PVOID dispatcher = NULL;
        CALL_HANDLER(r, rec, &ctx, &dispatcher);
        r = r->Next;
        seh_set_head(r);
    }
}

void NTAPI RtlAssert(const char *Assertion, const char *File, ULONG Line, const char *Message)
{
    xlog("guest assertion failed: %s at %s:%u %s", Assertion, File, Line, Message ? Message : "");
}

void NTAPI RtlRip(const char *ApiName, const char *Expression, const char *Message)
{
    xlog("guest RIP: %s %s %s", ApiName ? ApiName : "", Expression ? Expression : "",
         Message ? Message : "");
}

PVOID NTAPI RtlGetCallersAddress(PVOID *CallersAddress, PVOID *CallersCaller)
{
    *CallersAddress = *CallersCaller = NULL;
    return NULL;
}

ULONG NTAPI RtlWalkFrameChain(PVOID *Callers, ULONG Count, ULONG Flags)
{
    (void)Callers; (void)Count; (void)Flags;
    return 0;
}

void NTAPI RtlCaptureContext(CONTEXT *ctx) { memset(ctx, 0, sizeof(*ctx)); }

/* ---- printf with Windows format extensions --------------------------- */

/*
 * Translate MSVC printf formats to glibc: %ws / %S / %ls take 16-bit wide
 * strings, %Z takes an ANSI_STRING*, %wZ a UNICODE_STRING*, and %I64 is the
 * 64-bit length modifier.
 */
int xvsnprintf(char *out, size_t size, const char *fmt, va_list ap)
{
    size_t o = 0;
#define PUT(c) do { if (o + 1 < size) out[o] = (c); o++; } while (0)
    while (*fmt) {
        if (*fmt != '%') { PUT(*fmt++); continue; }
        const char *start = fmt++;
        if (*fmt == '%') { PUT('%'); fmt++; continue; }
        char spec[32];
        size_t sl = 0;
        spec[sl++] = '%';
        while (strchr("-+ #0", *fmt) && sl < 20) spec[sl++] = *fmt++;
        if (*fmt == '*') { sl += snprintf(spec + sl, 12, "%d", va_arg(ap, int)); fmt++; }
        while (isdigit((UCHAR)*fmt) && sl < 24) spec[sl++] = *fmt++;
        if (*fmt == '.') {
            spec[sl++] = *fmt++;
            if (*fmt == '*') { sl += snprintf(spec + sl, 12, "%d", va_arg(ap, int)); fmt++; }
            while (isdigit((UCHAR)*fmt) && sl < 28) spec[sl++] = *fmt++;
        }
        int wide = 0, i64 = 0, lng = 0, shrt = 0;
        for (;;) {
            if (*fmt == 'w') { wide = 1; fmt++; }
            else if (*fmt == 'l') { lng++; fmt++; }
            else if (*fmt == 'h') { shrt = 1; fmt++; }
            else if (!strncmp(fmt, "I64", 3)) { i64 = 1; fmt += 3; }
            else if (!strncmp(fmt, "I32", 3)) { fmt += 3; }
            else break;
        }
        char conv = *fmt++;
        char tmp[512];
        spec[sl] = 0;
        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o':
            if (i64 || lng >= 2) {
                snprintf(spec + sl, 4, "ll%c", conv);
                snprintf(tmp, sizeof(tmp), spec, va_arg(ap, long long));
            } else {
                snprintf(spec + sl, 3, "%s%c", shrt ? "h" : "", conv);
                snprintf(tmp, sizeof(tmp), spec, va_arg(ap, int));
            }
            break;
        case 'c': case 'C':
            snprintf(spec + sl, 2, "c");
            snprintf(tmp, sizeof(tmp), spec, (char)va_arg(ap, int));
            break;
        case 'p':
            snprintf(tmp, sizeof(tmp), "%08X", va_arg(ap, unsigned));
            break;
        case 'e': case 'E': case 'f': case 'g': case 'G':
            snprintf(spec + sl, 2, "%c", conv);
            snprintf(tmp, sizeof(tmp), spec, va_arg(ap, double));
            break;
        case 's': case 'S': {
            bool w = wide || lng || conv == 'S';
            char s8[480];
            if (w) {
                const WCHAR *ws = va_arg(ap, const WCHAR *);
                size_t i = 0;
                if (!ws) { snprintf(s8, sizeof(s8), "(null)"); }
                else { for (; ws[i] && i + 1 < sizeof(s8); i++) s8[i] = ws[i] < 128 ? (char)ws[i] : '?'; s8[i] = 0; }
            } else {
                const char *s = va_arg(ap, const char *);
                snprintf(s8, sizeof(s8), "%s", s ? s : "(null)");
            }
            snprintf(spec + sl, 2, "s");
            snprintf(tmp, sizeof(tmp), spec, s8);
            break;
        }
        case 'Z': {
            char s8[480];
            if (wide) {
                const UNICODE_STRING *u = va_arg(ap, const UNICODE_STRING *);
                size_t i = 0;
                for (; u && i < u->Length / 2 && i + 1 < sizeof(s8); i++) s8[i] = (char)u->Buffer[i];
                s8[i] = 0;
            } else {
                const ANSI_STRING *a = va_arg(ap, const ANSI_STRING *);
                snprintf(s8, sizeof(s8), "%.*s", a ? a->Length : 0, a ? a->Buffer : "");
            }
            snprintf(tmp, sizeof(tmp), "%s", s8);
            break;
        }
        case 'n':
            *va_arg(ap, int *) = (int)o;
            tmp[0] = 0;
            break;
        default:
            snprintf(tmp, sizeof(tmp), "%.*s", (int)(fmt - start), start);
        }
        for (char *t = tmp; *t; t++) PUT(*t);
    }
    if (size) out[o < size ? o : size - 1] = 0;
    return (int)o;
#undef PUT
}

int CDECLAPI RtlSprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = xvsnprintf(buf, 1 << 20, fmt, ap);
    va_end(ap);
    return n;
}

int CDECLAPI RtlSnprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = xvsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return (size_t)n >= size ? -1 : n;
}

#ifdef XBC_TRANSLATED
static uint64_t xa_RtlSprintf_varargs(struct xa_frame *f)
{
    return (uint32_t)xa_format((char *)(uintptr_t)f->stack[0], 1 << 20, (const char *)(uintptr_t)f->stack[1],
                               f->stack + 2);
}

static uint64_t xa_RtlSnprintf_varargs(struct xa_frame *f)
{
    size_t size = f->stack[1];
    char tmp[4096];
    int n = xa_format(tmp, sizeof(tmp), (const char *)(uintptr_t)f->stack[2], f->stack + 3);
    if (size) {
        size_t c = (size_t)n < size ? (size_t)n + 1 : size;
        memcpy((char *)(uintptr_t)f->stack[0], tmp, c);
        if ((size_t)n >= size) ((char *)(uintptr_t)f->stack[0])[size - 1] = 0;
    }
    return (uint32_t)((size_t)n >= size ? -1 : n);
}

static const struct xa_entry sprintf_entries[] = {
    { (void *)RtlSprintf, xa_RtlSprintf_varargs, CONV_CDECL, "RtlSprintf" },
    { (void *)RtlSnprintf, xa_RtlSnprintf_varargs, CONV_CDECL, "RtlSnprintf" },
};
__attribute__((constructor)) static void sprintf_register(void) { xa_register(sprintf_entries, 2); }
#endif

int CDECLAPI RtlVsprintf(char *buf, const char *fmt, va_list ap)
{
    return xvsnprintf(buf, 1 << 20, fmt, ap);
}

int CDECLAPI RtlVsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    int n = xvsnprintf(buf, size, fmt, ap);
    return (size_t)n >= size ? -1 : n;
}
