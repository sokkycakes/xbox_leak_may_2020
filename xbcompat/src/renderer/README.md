# Native renderer for Box86

This optional build keeps the XBE and xbcompat runtime in the existing i386
process, but runs the D3D implementation and shader preparation in a native
32-bit ARM shared library. SDL and OpenGL calls made by the renderer stay
native. The built-in renderer remains the default.

## Build

From xbcompat/ on a Linux build machine with i386 and armhf development
libraries and cross compilers:

    make RENDERER=native CC=i686-linux-gnu-gcc
    make -f tools/native-renderer.mk TARGET=armhf

The runtime build retains the existing kernel export-generation prerequisite
(see the main Makefile's LEAK setting). Outputs:

- build-native-client/xbcompat: i386 runtime and renderer entry stubs
- build-renderer-armhf/libxbcompat_renderer.so.1: ARM hard-float renderer

The backend is built with VFP, signed char, and the existing 32-bit Xbox
structure definitions. It deliberately does not enable NEON: guest pointers
and stack data may only be four-byte aligned. Guest allocation remains owned
by the x86 runtime, preserving its low-address pool and contiguous window.

On the current Pi image, OpenGL is supplied without GLX. Build its backend
with a separate output directory:

    make -f tools/native-renderer.mk TARGET=armhf GL=opengl BUILD=build-renderer-pi

Use BOX86_LIBGL=libOpenGL.so.0 when launching the x86 client or smoke test on
that image. The x86 client can retain its default GL=gl linkage: Box86 wraps
that import using the native OpenGL library selected by BOX86_LIBGL.

Stock Box86 does not know this custom library. Add its wrapper to a separate
Box86 checkout and build that copy:

    python3 tools/box86-renderer/install.py /path/to/box86
    cmake -S /path/to/box86 -B /path/to/box86-build \
      -DRPI3=1 -DARM_DYNAREC=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_C_COMPILER=arm-linux-gnueabihf-gcc \
      -DCMAKE_ASM_COMPILER=arm-linux-gnueabihf-gcc
    cmake --build /path/to/box86-build -j4

The wrapper was developed against upstream Box86 commit
7dec0819b5f212910cc7ff030f5161742add121c. Its installer is idempotent and
refuses unexpected registration anchors.

Put the ARM library in a native dynamic-library search directory. For an
isolated test, use LD_LIBRARY_PATH and the library's standard soname:

    LD_LIBRARY_PATH=/path/to/arm-renderer \
      /path/to/patched-box86 /path/to/build-native-client/xbcompat [existing arguments]

XBCOMPAT_RENDERER_LIBRARY may override the dlopen name/path. Box86's native
wrapper must still be able to find libxbcompat_renderer.so.1 through the
native loader. Do not point the Pi at the i386 test backend.

Use the original xbcompat executable with the original Box86 to compare or
roll back. No installed executable, launch script, or system library needs
replacement for these tests.

## Boundary and ownership

The native library compiles the existing d3d8.c, vsh.c, vsh_interp.c and psh.c.
It does not carry a fork of their rendering implementation.

Generated x86 stubs preserve ecx/edx and expose the original argument stack.
The existing gen_adapters.py adapters decode stdcall, fastcall and cdecl,
then report the precise callee stack cleanup. The entry assembly preserves
callee-saved registers, realigns for host C, and supports edx:eax and x87
return values. The wire structure uses only 32-bit words; doubles cross via
memcpy, not assumptions about ARM double alignment.

Initialization verifies the ABI version and every HLE export name/order
before accepting calls. All 303 exports in the initial source revision are
bridged, including decorated aliases and LTCG targets.

The native renderer owns its state and caches. Guest-visible state arrays
remain shared: inline writes from Xbox code are read at their original
addresses. The existing runtime handles guest allocations, symbol lookup,
logging, fault handlers, frame timing and display handover through a single
service callback. The Box86 wrapper converts that callback to a native
function; callbacks execute back in x86 on the same thread, supporting
reentrant renderer calls.

The current D3D callback sites use void cdecl(one argument). The client
rejects other signatures explicitly; extend that dispatch if future D3D
code introduces different callback conventions. Raw ARM function pointers
must never be invoked as Xbox functions.

## Validation

    make -f tools/native-renderer.mk TARGET=i386 check
    make -f tools/native-renderer.mk TARGET=i386 check-render

The first target checks the real entry assembly and real renderer API:
stack cleanup, fastcall register arguments, integer/x87 returns, shared
state, guest allocation, and reentrant callbacks. The second creates a GL
context, draws a triangle, checks its center pixel and presents a frame.

The same i386 smoke executable can test the ARM backend on the Pi:

    LD_LIBRARY_PATH=/path/to/arm-renderer \
      /path/to/patched-box86 build-renderer-i386/renderer-smoke

That test needs no display and does not launch or restart the dashboard.
For a display test on a separate X server, append --render. Do not run a
second KMSDRM renderer while the dashboard owns the display.

These checks establish bridge functionality, not an FPS improvement. Measure
the dashboard on the same scene/build/settings, with 1x and 4x MSAA, and
separate CPU setup time from GPU/presentation time. MSAA and the fullscreen
filter keep their GPU cost.

## Initial validation record (2026-10-10 UTC)

- i386 client, original built-in runtime, and armhf/i386 renderer libraries built.
- Entry ABI test: 60,000 calls passed.
- Native i386 renderer: shared state, fastcall, allocation and 64 reentrant callbacks passed.
- Mesa llvmpipe/Xvfb: triangle pixel check and presentation passed.
- ARM backend through patched Box86 under QEMU: integration test passed.
- Raspberry Pi 3: the same display-free Box86/ARM integration test passed with
  the libOpenGL backend. The running dashboard was not restarted.

Dashboard rendering on VC4 and before/after FPS remain unmeasured.
