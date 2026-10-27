/* Win32 file and string calls used by XFONT, on top of stdio. */
#include "windows.h"

static DWORD s_lastError;

DWORD GetLastError(void) { return s_lastError; }

HANDLE CreateFileA(LPCSTR name, DWORD access, DWORD share, void *sec,
                   DWORD disposition, DWORD flags, HANDLE templ)
{
    FILE *f;
    (void)access; (void)share; (void)sec; (void)disposition; (void)flags; (void)templ;
    f = fopen(name, "rb");
    if (!f) {
        s_lastError = ERROR_FILE_NOT_FOUND;
        return INVALID_HANDLE_VALUE;
    }
    return (HANDLE)f;
}

BOOL ReadFile(HANDLE h, void *buf, DWORD cb, DWORD *read, void *ov)
{
    size_t n;
    (void)ov;
    n = fread(buf, 1, cb, (FILE *)h);
    if (read)
        *read = (DWORD)n;
    if (n != cb && ferror((FILE *)h)) {
        s_lastError = ERROR_READ_FAULT;
        return FALSE;
    }
    return TRUE;
}

DWORD SetFilePointer(HANDLE h, LONG dist, LONG *high, DWORD method)
{
    static const int whence[] = { SEEK_SET, SEEK_CUR, SEEK_END };
    (void)high;
    if (method > FILE_END || fseek((FILE *)h, dist, whence[method]) != 0) {
        s_lastError = ERROR_READ_FAULT;
        return INVALID_SET_FILE_POINTER;
    }
    return (DWORD)ftell((FILE *)h);
}

BOOL GetFileInformationByHandle(HANDLE h, BY_HANDLE_FILE_INFORMATION *info)
{
    FILE *f = (FILE *)h;
    long pos = ftell(f), size;
    if (fseek(f, 0, SEEK_END) != 0) {
        s_lastError = ERROR_READ_FAULT;
        return FALSE;
    }
    size = ftell(f);
    fseek(f, pos, SEEK_SET);
    memset(info, 0, sizeof(*info));
    info->nFileSizeLow = (DWORD)size;
    return TRUE;
}

BOOL CloseHandle(HANDLE h)
{
    return fclose((FILE *)h) == 0;
}

/* UTF-16 to UTF-8; font paths are the only caller. */
int WideCharToMultiByte(UINT cp, DWORD flags, LPCWSTR src, int cchSrc,
                        LPSTR dst, int cbDst, LPCSTR def, BOOL *usedDef)
{
    int i = 0, o = 0;
    (void)cp; (void)flags; (void)def;
    if (usedDef)
        *usedDef = FALSE;
    for (; cchSrc < 0 ? 1 : i < cchSrc; i++) {
        uint32_t c = src[i];
        char tmp[4];
        int n, k;
        if (c >= 0xD800 && c < 0xDC00 && src[i + 1] >= 0xDC00 && src[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (src[i + 1] - 0xDC00);
            i++;
        }
        if (c < 0x80) { tmp[0] = (char)c; n = 1; }
        else if (c < 0x800) { tmp[0] = (char)(0xC0 | c >> 6); tmp[1] = (char)(0x80 | (c & 0x3F)); n = 2; }
        else if (c < 0x10000) { tmp[0] = (char)(0xE0 | c >> 12); tmp[1] = (char)(0x80 | (c >> 6 & 0x3F)); tmp[2] = (char)(0x80 | (c & 0x3F)); n = 3; }
        else { tmp[0] = (char)(0xF0 | c >> 18); tmp[1] = (char)(0x80 | (c >> 12 & 0x3F)); tmp[2] = (char)(0x80 | (c >> 6 & 0x3F)); tmp[3] = (char)(0x80 | (c & 0x3F)); n = 4; }
        if (o + n > cbDst)
            return 0;
        for (k = 0; k < n; k++)
            dst[o++] = tmp[k];
        if (c == 0)
            break;
    }
    return o;
}

void OutputDebugStringA(LPCSTR s)
{
    fputs(s, stderr);
}
