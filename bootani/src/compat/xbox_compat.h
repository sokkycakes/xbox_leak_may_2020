//
//  xbox_compat.h
//
//  Minimal Win32/XTL type layer so the original Pipeworks boot animation
//  sources compile on any platform. Only what the animation uses is here.
//
#ifndef BOOTANI_XBOX_COMPAT_H
#define BOOTANI_XBOX_COMPAT_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#if defined(_MSC_VER)
#include <malloc.h>
#else
#include <alloca.h>
#ifndef _alloca
#define _alloca alloca
#endif
#endif

typedef uint32_t        DWORD;
typedef uint16_t        WORD;
typedef uint8_t         BYTE;
typedef int32_t         BOOL;
typedef int32_t         LONG;
typedef uint32_t        ULONG;
typedef unsigned int    UINT;
typedef int             INT;
typedef float           FLOAT;
typedef char            CHAR;
typedef char            TCHAR;
typedef int32_t         HRESULT;
typedef BYTE*           PBYTE;
typedef void*           PVOID;
typedef void            VOID;
typedef const char*     LPCSTR;
typedef DWORD           COLORREF;

#ifndef CONST
#define CONST const
#endif
#ifndef TRUE
#define TRUE  1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#ifndef NULL
#define NULL 0
#endif

#define S_OK            ((HRESULT)0)
#define E_FAIL          ((HRESULT)0x80004005L)
#define SUCCEEDED(hr)   (((HRESULT)(hr)) >= 0)
#define FAILED(hr)      (((HRESULT)(hr)) < 0)

#define ZeroMemory(p, n)    memset((p), 0, (n))

#ifdef __cplusplus
#include <type_traits>
// The originals rely on the Windows min/max macros, often with mixed operand
// types (int vs DWORD, float literals). Templates keep that working without
// defining macros that would break the C++ standard headers.
// (Return by value: decltype of the conditional would be a reference to a
// parameter.)
template <typename A, typename B>
inline typename std::common_type<A, B>::type max(A a, B b) { return (a > b) ? a : b; }
template <typename A, typename B>
inline typename std::common_type<A, B>::type min(A a, B b) { return (a < b) ? a : b; }
#endif

// Truncating float->int conversion, the portable spelling of the
// "cvttss2si" inline assembly sprinkled through the originals.
#define XBS_FTOI(f) ((int)(f))

void OutputDebugString(const char* s);

#endif // BOOTANI_XBOX_COMPAT_H
