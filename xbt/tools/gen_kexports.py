#!/usr/bin/env python3
"""
Generate src/kexports.h: the xboxkrnl.exe export table (ordinal, name,
argument count, calling convention, variable or function) used by the HLE
kernel to call handlers and to clean up the guest stack.

Ordinals come from the kernel's export definition file; argument counts and
calling conventions are parsed from the kernel headers (functions without an
explicit convention are __stdcall, as the NT kernel is built with /Gz).

usage: gen_kexports.py <xbox source root> > src/kexports.h
"""
import os
import re
import sys

root = sys.argv[1]
deffile = os.path.join(root, "private/ntos/init/console/obj/i386/xboxkrnl.def")
exports = {}
for line in open(deffile, errors="replace"):
    m = re.match(r"\s+(\w+)\s+@(\d+)\s+NONAME(\s+CONSTANT|\s+DATA)?", line)
    if m:
        exports[m.group(1)] = (int(m.group(2)), bool(m.group(3)))

hdr_dirs = ["private/ntos/inc", "private/inc", "public/ddk/inc", "private/ntos/xapi/inc",
            "private/ntos/rtl", "public/sdk/inc"]
texts = []
for d in hdr_dirs:
    p = os.path.join(root, d)
    for dp, dn, fn in os.walk(p):
        for f in fn:
            if f.lower().endswith(".h"):
                try:
                    t = open(os.path.join(dp, f), errors="replace").read()
                except OSError:
                    continue
                t = re.sub(r"/\*.*?\*/", " ", t, flags=re.S)
                t = re.sub(r"//[^\n]*", " ", t)
                texts.append(t)
alltext = "\n".join(texts)

CONV = {"NTAPI": "S", "__stdcall": "S", "WINAPI": "S", "STDCALL": "S", "FASTCALL": "F",
        "__fastcall": "F", "FASTCALL_": "F", "__cdecl": "C", "CDECL": "C", "_cdecl": "C"}


def find_proto(name):
    best = None
    for m in re.finditer(r"\b%s\s*\(" % re.escape(name), alltext):
        pre = alltext[max(0, m.start() - 200):m.start()]
        # must look like a declaration: a type keyword shortly before, not a call
        tail = pre.split(";")[-1].split("}")[-1].split("{")[-1]
        if not re.search(r"\b(NTSTATUS|VOID|BOOLEAN|ULONG|LONG|PVOID|KIRQL|DECLSPEC_NORETURN|NTKERNELAPI|"
                         r"NTSYSAPI|NTSYSCALLAPI|NTHALAPI|USHORT|UCHAR|LONGLONG|ULONGLONG|LARGE_INTEGER|"
                         r"SIZE_T|HANDLE|PCHAR|CHAR|PLIST_ENTRY|PSINGLE_LIST_ENTRY|KPRIORITY|PKTHREAD|"
                         r"WCHAR|BOOL|DWORD|int|ULONG_PTR|LONG_PTR|PSLIST_ENTRY|PIRP|PDEVICE_OBJECT|"
                         r"PFILE_OBJECT|PUCHAR|PKPCR|PMMPTE|PHYSICAL_ADDRESS|PSTR)\b", tail):
            continue
        if re.search(r"(#define|return|=|\bif\b|\bwhile\b)", tail):
            continue
        # parameter list
        i = m.end()
        depth = 1
        j = i
        while j < len(alltext) and depth:
            if alltext[j] == "(":
                depth += 1
            elif alltext[j] == ")":
                depth -= 1
            j += 1
        params = alltext[i:j - 1]
        after = alltext[j:j + 40].lstrip()
        if not (after.startswith(";") or after.startswith("{") or re.match(r"^(DECLSPEC|\w+\s*;)", after)):
            pass
        conv = "S"
        for tok in re.findall(r"\w+", tail):
            if tok in CONV:
                conv = CONV[tok]
        params = re.sub(r"\b(IN|OUT|OPTIONAL|CONST|const|volatile|UNALIGNED)\b", " ", params).strip()
        if params in ("", "VOID", "void"):
            n = 0
        else:
            parts = [p for p in params.split(",")]
            n = 0
            for p in parts:
                p = p.strip()
                if p == "...":
                    conv = "C"
                    continue
                # 64-bit by-value parameters take two stack slots
                if re.match(r"^(LARGE_INTEGER|ULARGE_INTEGER|LONGLONG|ULONGLONG|PHYSICAL_ADDRESS|__int64)\b", p):
                    n += 2
                else:
                    n += 1
        cand = (n, conv, params)
        if best is None:
            best = cand
        elif best != cand and conv != "S" and best[1] == "S":
            best = cand
    return best


# Exports whose prototypes are not in the headers (macros, private headers)
OVERRIDE = {
    "AvSetDisplayMode": (6, "S"), "ExFreePool": (1, "S"), "ExRaiseException": (1, "S"),
    "ExfInterlockedInsertHeadList": (2, "F"), "ExfInterlockedInsertTailList": (2, "F"),
    "ExfInterlockedRemoveHeadList": (1, "F"), "FscSetCacheSize": (1, "S"),
    "HalClearSoftwareInterrupt": (1, "F"), "HalGetInterruptVector": (2, "S"),
    "HalWriteSMBusValue": (4, "S"), "InterlockedPopEntrySList": (1, "F"), "IoStartNextPacket": (1, "S"),
    "KeBugCheck": (1, "S"), "KeGetCurrentThread": (0, "S"), "KeInsertQueue": (2, "S"),
    "KePulseEvent": (3, "S"), "KeReleaseMutant": (4, "S"), "KfLowerIrql": (1, "F"),
    "KiUnlockDispatcherDatabase": (1, "F"), "NtSuspendThread": (2, "S"), "PhyInitialize": (2, "S"),
    "RtlAssert": (4, "S"), "RtlCompareMemoryUlong": (3, "S"), "RtlEnterCriticalSectionAndRegion": (1, "S"),
    "RtlMultiByteToUnicodeN": (5, "S"), "RtlUnicodeStringToInteger": (3, "S"), "XcSHAInit": (1, "S"),
    "XcSHAUpdate": (3, "S"), "XcSHAFinal": (2, "S"), "XcRC4Key": (3, "S"), "XcRC4Crypt": (3, "S"),
    "XcHMAC": (7, "S"), "XcDESKeyParity": (2, "S"), "XcKeyTable": (3, "S"), "XcBlockCrypt": (5, "S"),
    "XcBlockCryptCBC": (7, "S"), "XcUpdateCrypto": (2, "S"), "RtlRip": (3, "S"), "MmDbgWriteCheck": (2, "S"),
}

print("/* generated by tools/gen_kexports.py from the kernel export definition and headers */")
print("/* ordinal, name, number of 32-bit argument slots, convention (S stdcall, F fastcall, C cdecl, V variable) */")
missing = []
for name, (ordn, var) in sorted(exports.items(), key=lambda x: x[1][0]):
    if var:
        print('KEXP(%d, %s, 0, V)' % (ordn, name))
        continue
    p = OVERRIDE.get(name) or find_proto(name)
    if p is None:
        missing.append(name)
        print('KEXP(%d, %s, -1, S)' % (ordn, name))
    else:
        print('KEXP(%d, %s, %d, %s)' % (ordn, name, p[0], p[1]))
sys.stderr.write("%d exports, %d without prototype: %s\n" % (len(exports), len(missing), " ".join(missing)))
