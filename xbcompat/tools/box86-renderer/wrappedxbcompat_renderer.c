/* Box86 native-library wrapper for xbcompat's 32-bit renderer. */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include "wrappedlibs.h"
#include "debug.h"
#include "wrapper.h"
#include "bridge.h"
#include "librarian/library_private.h"
#include "x86emu.h"
#include "callback.h"
const char *xbcompat_rendererName = "libxbcompat_renderer.so.1";
#define LIBNAME xbcompat_renderer
#include "generated/wrappedxbcompat_renderertypes.h"
#include "wrappercallback.h"
static uintptr_t guest_host_service;
static void native_host_service(uint32_t op, uint32_t *args)
{
    RunFunctionFmt(guest_host_service, "up", op, args);
}
EXPORT int my_xbr_init(x86emu_t *emu, uint32_t abi, void *callback)
{
    (void)emu;
    if (!my->xbr_init || !callback || (guest_host_service && guest_host_service != (uintptr_t)callback))
        return -1;
    guest_host_service = (uintptr_t)callback;
    return my->xbr_init(abi, native_host_service);
}
#define CUSTOM_INIT getMy(lib);
#define CUSTOM_FINI freeMy(); guest_host_service = 0;
#include "wrappedlib_init.h"
