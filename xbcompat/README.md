# xbcompat

A compatibility layer that runs original Xbox `.xbe` executables on x86 Linux
without emulating the console. Xbox code is 32-bit x86, so the XBE is mapped at
its native address (0x10000) inside a 32-bit Linux process and executed directly.
What differs from a PC is replaced underneath it:

- **Kernel**: all 375 `xboxkrnl.exe` exports, generated from the leak's
  `private/ntos/init/ntoskrnl.src`. Threads, events, waits, timers/DPCs, virtual
  and contiguous memory, the object manager, file I/O and Rtl are implemented on
  pthreads and the host filesystem (`src/kernel/`). Each guest thread gets a
  KPCR at `fs:0` through an LDT entry.
- **Direct3D 8**: the XDK statically links D3D8 into every title. `tools/mksigs.py`
  builds signatures from the leak's own `d3d8.lib`/`d3d8d.lib`, and
  `tools/findsigs.py` finds those functions in the XBE. `src/hle/` patches
  each API entry with a jump into an SDL2 + OpenGL implementation. Anything
  that isn't implemented yet is patched with a trap that names the function.

## Status

106 of the 113 prebuilt ATG samples in the leak run 60 frames without a trap
or crash. The other seven are a Windows tool, three debug-monitor samples,
and three whose media or DSP image is missing from the leak. What works:

- **Direct3D 8**: fixed function with lighting, specular, fog and texture
  stages (all texture ops but bump env mapping); texture coordinate generation and texture matrices; NV2A vertex
  programs and register combiners translated to GLSL; vertex state shaders run
  on the CPU; push buffers (recorded and BeginPush); point sprites; swizzled,
  linear, DXT, cube, volume and bump-map textures; render targets, including
  cube map faces; visibility tests; state blocks; rect and tri patches
  tessellated on the CPU; back buffer and depth buffer reads from memory.
- **DirectSound**: buffers, streams, submixes, 3D positioning and packet
  completion, mixed in software to SDL2 audio.
- **Input**: XInput on SDL2 game controllers, with rumble, plus a keyboard pad
  when no controller is attached (see below).
- **Kernel**: SHA-1, HMAC, RC4 and big-number crypto are real, so save
  signatures work. There is no Ethernet: the Xbox Live samples start and
  report that the console is offline.

Not done yet:

- DirectSound effects that run on the audio DSP (the GlobalFX sample) are
  not emulated.
- The debug-monitor samples import `xbdm.dll`, which `pe2xbe.py` does not
  handle.
- Retail titles need signatures for their XDK versions.

## Keyboard pad

| Key | Button |
| --- | --- |
| Space or Enter | A |
| B, X, Y | B, X, Y |
| 1, 2 | White, Black |
| Q, E | Left and right triggers |
| Arrow keys | D-pad |
| Backspace | Start |
| Escape | Back |
| W A S D | Left stick |
| I J K L | Right stick |
| 3, 4 | Left and right stick clicks |

## Build

Needs a 32-bit toolchain and 32-bit SDL2/GL:

    sudo dpkg --add-architecture i386 && sudo apt update
    sudo apt install gcc-multilib libsdl2-dev:i386 libgl-dev:i386 libgl1-mesa-dri:i386
    make

The Makefile expects the leak tree at `../xbox_leak_may_2020/xbox trunk/xbox`;
override that with `make LEAK=/path/to/xbox`.

## Run

    python3 tools/xbrun.py path/to/Sample.exe        # PE from the XDK build tree
    build/xbcompat --hle game.map path/to/default.xbe

`xbrun.py` converts a PE to an XBE with `pe2xbe.py`, copies its media, and
builds the HLE map (cached in `~/.cache/xbcompat`) before launching.

Options:

| Option | Meaning |
| --- | --- |
| `--hdd DIR` | Directory backing the hard disk. The default is `~/.local/share/xbcompat/hdd`. |
| `--trace` | Log every kernel call. |
| `--log FILE` | Write the log to a file. |
| `--screenshot FILE --shot-frame N` | Save frame N as a BMP. |
| `--frames N` | Exit after N frames. |

Environment variables:

| Variable | Meaning |
| --- | --- |
| `XBCOMPAT_NO_KBD_PAD=1` | Do not offer the keyboard pad. |
| `XBCOMPAT_VIRTUAL_PAD=1` | Attach an SDL virtual controller (for testing). |
| `XBCOMPAT_DUMP_TEXTURES=DIR` | Write every texture upload to DIR; `tools/texdump.py` turns one into a PNG. |
| `XBCOMPAT_DUMP_DRAWS=DIR` | Save the frame after each draw of the first frame. |
| `XBCOMPAT_PSH_DUMP=1` | Log each translated pixel shader. |
| `XBCOMPAT_VSH_DUMP=DIR` | Write each vertex program and its GLSL to DIR. |
| `XBCOMPAT_FIXED_FPS=N` | Advance the guest clock 1/N s per frame instead of following the wall clock, so frame K shows the moment K/N s. Audio still plays in real time. |
