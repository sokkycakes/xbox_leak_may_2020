# xbcompat-gpu (test build)

`make GPU=1` builds `build-gpu/xbcompat-gpu`, an alternate xbcompat that
emulates the Xbox GPU instead of replacing Direct3D. Titles run their own
statically linked Direct3D, which programs an emulated NV2A through its
registers and push buffers, the way xemu does it. Everything else (kernel,
DirectSound, input, the native CPU) is the normal xbcompat.

The NV2A model is xemu's (`xemu/`, from xemu-project/xemu at 25e1ee2f, plus
`vsh_cpu/` from xemu-project/nv2a_vsh_cpu). Those sources are GPL/LGPL, so
this executable is GPL and is kept apart from the main xbcompat binary,
which links none of it.

- `shim/`: the QEMU services the xemu sources expect (threads, timers, the
  big lock, MemoryRegion and dirty tracking, VGA registers, tracing), cut
  down to what the NV2A code uses.
- `nv2a_host.c`: the device instance, register dispatch, vblank and the
  window that shows the CRTC's framebuffer.
- `gpu_glue.c`: the xbcompat side: the trapped register window at
  0xFD000000 (each access is decoded and executed against the device), the
  PRAMIN aperture, the GPU interrupt, AvSetDisplayMode, the display loop.

Changes to the xemu sources are marked `xbcompat-gpu`: `nv2a.c` and
`nv2a_int.h` lose the PCI/QEMU device plumbing, `pbus.c` answers the PCI
IDs itself, `gl/vertex.c` always refetches vertex data, gloffscreen uses
SDL2.

Environment:

- `XBCOMPAT_GPU_COHERENT=1`: xemu's KVM-style surface handling (download
  after every draw, re-upload the displayed surface each frame). Slower,
  but CPU writes into render targets are seen. Default is the lazy
  TCG-style handling.
- `XBCOMPAT_GPU_TRACE_MMIO=1`: log every GPU register access.
- `XBCOMPAT_GPU_HZ`: vblank rate (default 60).

Needs OpenGL 4.0 core (Mesa on the Sion's Intel GPU is fine; the Pi 3's
VC4 is not). Build deps on top of xbcompat's: `libglib2.0-dev:i386
libepoxy-dev:i386 libxxhash-dev:i386`.
