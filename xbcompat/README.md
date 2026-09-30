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

The ATG graphics tutorials run: CreateDevice, Vertices, Matrices, Lights and
Textures (with swizzled and DXT textures).

Not done yet:

- DirectSound
- XInput
- Vertex and pixel shaders (only FVF/fixed-function is supported)
- Push buffers
- Retail titles, which would need signatures for more XDK versions

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
