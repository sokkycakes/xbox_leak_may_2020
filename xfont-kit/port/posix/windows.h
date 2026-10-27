/* Just enough of the Win32 API for XFONT to build on Linux/macOS.
   Windows builds use the real <windows.h> and never see this file. */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

#define __stdcall
#define __fastcall
#define __cdecl
#define _declspec(x)
#define __declspec(x)
#ifndef __forceinline
#define __forceinline inline __attribute__((always_inline))
#endif

#define CONST const
#define VOID void
#define NOERROR 0
#define TRUE 1
#define FALSE 0
#ifndef MAX_PATH
#define MAX_PATH 260
#endif
#define _MAX_PATH 260

typedef int32_t BOOL;
typedef uint8_t BYTE;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef int32_t LONG;
typedef uint32_t ULONG;
typedef uint32_t UINT;
typedef int32_t INT;
typedef int32_t HRESULT;
typedef char CHAR;
typedef char *PCHAR;
typedef char *LPSTR;
typedef const char *LPCSTR;
/* Xbox/Windows WCHAR is 16 bits; wchar_t here is 32. */
typedef uint16_t WCHAR;
typedef WCHAR *LPWSTR;
typedef const WCHAR *LPCWSTR;
typedef void *LPVOID;
typedef const void *LPCVOID;
typedef void *PVOID;
typedef BYTE *PBYTE;
typedef DWORD *LPDWORD;
typedef void *HANDLE;
typedef uint16_t USHORT;
typedef int16_t SHORT;
typedef uint8_t UCHAR;

#define ZeroMemory(d, n) memset((d), 0, (n))
#define CopyMemory(d, s, n) memcpy((d), (s), (n))

#define S_OK ((HRESULT)0)
#define S_FALSE ((HRESULT)1)
#define E_FAIL ((HRESULT)0x80004005)
#define E_OUTOFMEMORY ((HRESULT)0x8007000E)
#define E_INVALIDARG ((HRESULT)0x80070057)
#define E_NOTIMPL ((HRESULT)0x80004001)
#define E_UNEXPECTED ((HRESULT)0x8000FFFF)
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#define FAILED(hr) (((HRESULT)(hr)) < 0)
#define HRESULT_FROM_WIN32(x) ((HRESULT)(x) <= 0 ? ((HRESULT)(x)) : \
    ((HRESULT)(((x) & 0x0000FFFF) | (7 << 16) | 0x80000000)))

/* Files: a HANDLE is a FILE*. */
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define INVALID_SET_FILE_POINTER ((DWORD)-1)
#define GENERIC_READ 0x80000000u
#define FILE_SHARE_READ 1
#define OPEN_EXISTING 3
#define FILE_ATTRIBUTE_NORMAL 0x80
#define FILE_BEGIN 0
#define FILE_CURRENT 1
#define FILE_END 2
#define ERROR_FILE_NOT_FOUND 2
#define ERROR_READ_FAULT 30

typedef struct _BY_HANDLE_FILE_INFORMATION {
    DWORD dwFileAttributes;
    DWORD nFileSizeHigh;
    DWORD nFileSizeLow;
} BY_HANDLE_FILE_INFORMATION;

DWORD GetLastError(void);
HANDLE CreateFileA(LPCSTR name, DWORD access, DWORD share, void *sec,
                   DWORD disposition, DWORD flags, HANDLE templ);
BOOL ReadFile(HANDLE h, void *buf, DWORD cb, DWORD *read, void *ov);
DWORD SetFilePointer(HANDLE h, LONG dist, LONG *high, DWORD method);
BOOL GetFileInformationByHandle(HANDLE h, BY_HANDLE_FILE_INFORMATION *info);
BOOL CloseHandle(HANDLE h);

#define CP_ACP 0
int WideCharToMultiByte(UINT cp, DWORD flags, LPCWSTR src, int cchSrc,
                        LPSTR dst, int cbDst, LPCSTR def, BOOL *usedDef);

void OutputDebugStringA(LPCSTR s);
#define _vsnprintf vsnprintf

#ifdef __cplusplus
}
#endif
