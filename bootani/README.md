# bootani: the Xbox boot animation, portable

A rebuild of the original Xbox startup animation (green blob, pipe
chamber, fog, the X and the wordmark, plus the boot sound) that builds with
CMake and runs on Linux, Windows and macOS, in a window or headless.

The animation code is the original Pipeworks source from
`xbox trunk/xbox/private/ntos/ani2`, kept almost line for line. What changed
is everything underneath it: Direct3D 8, the Xbox kernel, the NV2A shader
microcode and the hardware audio are replaced by a thin portable layer.

## Build

Requirements: CMake 3.16+, a C++11 compiler, and either SDL2 (windowed mode
and audio playback, any platform) or EGL (headless on Linux). Both can be
enabled at once.

**Linux (Debian/Ubuntu)**

    sudo apt install build-essential cmake libsdl2-dev libegl-dev libgl-dev
    cmake -S bootani -B build
    cmake --build build -j

**macOS**

    brew install cmake sdl2
    cmake -S bootani -B build
    cmake --build build -j

macOS runs OpenGL 3.3 core through its (deprecated but present) GL driver;
headless mode there uses a hidden SDL window.

**Windows (Visual Studio)**

    vcpkg install sdl2
    cmake -S bootani -B build -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake
    cmake --build build --config Release

Any SDL2 that CMake can find (`SDL2Config.cmake` or pkg-config) works.

**Windows, cross-compiled from Linux (MinGW-w64)**

    sudo apt install mingw-w64
    # unpack SDL2-devel-2.x-mingw.tar.gz from github.com/libsdl-org/SDL/releases
    S=$PWD/SDL2-2.30.9/x86_64-w64-mingw32
    cmake -S bootani -B build-win -DCMAKE_TOOLCHAIN_FILE=bootani/cmake/mingw-w64-x86_64.cmake \
          -DSDL2_MINGW_ROOT=$S -DSDL2_DIR=$S/lib/cmake/SDL2 -DCMAKE_EXE_LINKER_FLAGS=-static
    cmake --build build-win -j

Ship `bootani.exe` with `$S/bin/SDL2.dll`. This build has been run under Wine.

Options: `-DBOOTANI_WITH_SDL2=OFF` or `-DBOOTANI_WITH_EGL=OFF` drop a
backend. Configuration fails if neither is available.

## Run

    ./build/bootani                      # window, with sound
    ./build/bootani --fullscreen --loop  # kiosk mode; Esc or Q quits
    ./build/bootani --headless --frames-dir out --capture-at 1,4,7.9 --wav out/boot.wav

| Option | Meaning |
| --- | --- |
| `--headless` | Render off-screen (EGL surfaceless on Linux, hidden SDL window elsewhere) |
| `--fullscreen`, `--size WxH` | Window size (default 1280x960); the 640x480 image is letterboxed |
| `--no-vsync`, `--no-audio` | |
| `--loop` | Replay until closed (windowed) |
| `--short` | The short variant the Xbox plays on warm boots |
| `--msaa N` | Back buffer multisampling (default 4, as on the Xbox) |
| `--fps F` | Headless: animation frames per second (default 60) |
| `--frames-dir DIR`, `--every N`, `--capture-at T,...` | Save frames as PNG |
| `--wav FILE` | Headless: write the boot sound, in sync with the frames |
| `--verbose` | Print backend, GL renderer and debug output |

Headless mode runs on a virtual clock, so captures are deterministic and
independent of how fast the machine renders (Mesa llvmpipe works).

`build/src/sound/bootsound_wav out.wav` renders just the boot sound.

## What the original depended on

| Xbox dependency | Where it was used | Replacement |
| --- | --- | --- |
| Direct3D 8 (Xbox flavour: `D3DDevice_*`, push buffers, swizzled textures, `LIN_*` formats, MSAA render targets, shadow-map depth textures) | All rendering | `src/compat/d3d8_compat.h` declares the subset used; `src/gfx/d3d8_gl.cpp` implements it on OpenGL 3.3 core |
| NV2A vertex programs (`.xvu`) and register combiner pixel shaders (`.xpu`), compiled blobs linked into the kernel | Blob, bloblets, scene phong/bump/depth, green fog, X interior | Hand-written GLSL in `src/gfx/shaders_glsl.cpp`, checked against a decoder of the original blobs (`tools/shaderdecode`) |
| Fixed-function texture stages, alpha test, fog | Logo, wordmark, fog passes | Emulated in a generated fixed-function GLSL program |
| Kernel: `KeQueryTickCount`/`NtGetTickCount`, `HalWriteSMBusValue` (SMC LEDs), `AnipRunAnimation`, the D3DK allocator, `MmAllocateContiguousMemory` | Timing, entry point, memory | `src/main.cpp` provides the clock and entry point; memory is `calloc`; the SMC writes are dropped |
| MCPX APU + DirectSound "bootsound" sequencer (`BootSound_Start`, APU voice processing, reverb) | Boot sound | `src/sound/`: the original sequencer and samples on a software voice mixer (`dsound_soft.c`), 48 kHz stereo |
| x86 inline assembly (`fsincos`, `cvttss2si`, the glow texture generator, PRNG) | Math, texture generation | Portable C, bit-exact where the result is integer |
| XDK headers (`xtl.h`, `d3d8types.h`, `xgraphics.h`) | Everywhere | `src/compat/xbox_compat.h` (Win32 types) and `d3d8_compat.h` |

Not ported: `mslogo` (a kernel-only Microsoft logo overlay), the shield and
blob-renderer variants that the kernel build compiled out (`_SHIELD`), and
the placement debugging tool.

## Layout

    src/anim/        original animation code (lightly patched, see below)
    src/compat/      Win32/XTL types and the Direct3D 8 interface subset
    src/gfx/         Direct3D 8 on OpenGL 3.3, GLSL shaders, GL loader
    src/platform/    context, window, clock and audio (SDL2, EGL)
    src/sound/       boot sound sequencer and software mixer (C)
    src/io/          PNG and WAV writers
    src/main.cpp     command line, clock, capture
    tools/           bootsound_wav, and the NV2A shader decoder
    third_party/     Khronos GL headers

The animation calls exactly four host services: `Bootani_GetTickCount`,
`Bootani_AudioStart/Stop/Restart`, and it presents through the D3D8 layer,
which hands each finished frame to a hook in `main.cpp`. Porting to another
host means implementing `src/platform/platform.h`.

## Changes to the original code

The files in `src/anim` are the originals with these edits:

- inline assembly replaced by C (same results);
- kernel, SMC and profiling calls removed; the entry point is `Bootani_RunAnimation`;
- shader blobs replaced by name tags that select the GLSL translations;
- MSVC-isms fixed (`for` scope, `_alloca`, type punning);
- two latent bugs made harmless: `VBlob::destroy` is called twice at shutdown
  (now a no-op the second time), and the normal-map pass of
  `CreateIntensityTexture` reads one row past its buffer (now padded with zeros).

## Known differences

- The boot sound's reverb is an approximation of the APU's, and one filter
  coefficient in the noise track is clamped (see `voice_frame_params` in
  `src/sound/dsound_soft.c`).
- Rendering matches the structure of the Xbox pipeline (register combiner
  clamping, shadow compare direction, texel-addressed linear textures), but
  it has not been compared pixel for pixel with hardware captures.
