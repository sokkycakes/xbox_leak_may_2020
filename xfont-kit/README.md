# xfont-kit

XFONT is the Xbox SDK's text library: a GDI-style `TextOut` API on top of
pre-built bitmap fonts and Microsoft's TrueType scaler, the same rasterizer
family Windows uses. This kit takes it out of the May 2020 leak and builds it
as a plain static library on Linux, macOS and Windows, without the Xbox SDK.

Source: `xbox trunk/xbox/private/windows/directx/dxg/xfont` plus the public
headers `xfont.h`/`xfont-pc.h`. The code is Microsoft's and Apple's (the
scaler carries their copyright notices). It is not open source, so check
that before you put it in another project.

## Layout

| Path | What it is |
|---|---|
| `include/xfont_portable.h` | The one header to include. |
| `include/xfont-pc.h`, `xfontformat-pc.h` | The PC flavour of the API, used by this build. |
| `include/xfont.h`, `xfontformat.h` | The Xbox XDK headers, for reference. |
| `src/library/` | XFONT itself: the API and cache (`xfont.c`), bitmap fonts (`bitmap.cpp`), TrueType glue (`truetype.cpp`), the pixel writer (`painttext.cpp`), and the built-in 24 px Arial (`default.h`). |
| `src/scaler/` | The TrueType scaler: outline loading, hinting interpreter, scan converter, grey and subpixel output. `clientif.doc` is its original client interface spec. |
| `port/` | Stand-ins for the Xbox D3D types (`common/`) and for the bits of Win32 that XFONT uses (`posix/`, only used off Windows). |
| `tools/demo/` | `xfont_demo`: draws some lines into a 32-bit buffer and writes a PPM. |
| `tools/makefont/` | The XDK's MakeFont: TrueType to XFONT bitmap font. Windows only. |

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/xfont_demo out.ppm /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf
```

In another CMake project, use `add_subdirectory(xfont-kit)` and
`target_link_libraries(yourapp xfont)`. Turn off the demo with
`-DXFONT_BUILD_DEMO=OFF`. Without CMake, compile `src/scaler/*.c` and
`src/library/*` (and `port/posix/win32_shim.c` off Windows) with
`include`, `port/common`, `src/scaler` and `src/library` on the include path,
adding `port/posix` off Windows, and define `UNICODE` and `XFONT_TRUETYPE`.

Tested here: gcc 13 on x86-64 Linux (runs, output checked), and MinGW-w64
cross builds for Windows x64 (compiles and links; not run). MSVC should work
as is, since this is MSVC code, but it has not been tried.

## Using it

```c
#include "xfont_portable.h"

XFONT *font;
XFONT_OpenTrueTypeFont(L"C:\\fonts\\arial.ttf", 64 * 1024, &font); /* UTF-16 path */
XFONT_SetTextHeight(font, 24);
XFONT_SetTextAntialiasLevel(font, 4);       /* 0, 2 or 4 bits of coverage */
XFONT_SetTextColor(font, 0xFFFFFFFF);
XFONT_SetBkMode(font, XFONT_TRANSPARENT);   /* or XFONT_OPAQUE + SetBkColor */
XFONT_SetTextAlignment(font, XFONT_LEFT | XFONT_TOP);
XFONT_TextOutToMemory(font, pixels, pitch, width, height,
                      D3DFMT_LIN_A8R8G8B8, text, -1, x, y);
XFONT_Release(font);
```

* Strings are UTF-16 (`WCHAR`, 16 bits on every platform here). On Linux
  `L"..."` is 32-bit, so convert first, as the demo does.
* `D3DFORMAT` values are the Xbox ones, not PC Direct3D's. The common targets
  are `D3DFMT_LIN_A8R8G8B8`, `D3DFMT_LIN_X8R8G8B8`, `D3DFMT_LIN_R5G6B5`,
  `D3DFMT_LIN_A1R5G5B5`, `D3DFMT_LIN_A4R4G4B4` and `D3DFMT_LIN_A8`. The
  non-`LIN_` names mean the same pixel layout to XFONT, since it writes
  rows linearly either way.
* `XFONT_OpenDefaultFont` gives the built-in 24 px Arial (Western characters
  only). `XFONT_OpenBitmapFont`/`FromMemory` load `.xft` files from MakeFont.
* `XFONT_TextOut` (draw onto an `IDirect3DSurface8`) is Xbox only. Lock your
  own surface and call `TextOutToMemory` instead.

## Changes from the leaked source

Kept small so the code still reads like the original:

* `scaler/fscdefs.h`: `int32`/`uint32`/`Fixed`/`Fract` use `<stdint.h>`, so
  they stay 32-bit where `long` is 64-bit (Linux, macOS).
* `scaler/*`: the remaining raw `long`s in code (not comments) became
  `int32`/`uint32`, and `LONG_MAX`/`LONG_MIN`/`ULONG_MAX` became the 32-bit
  limits, for the same reason.
* `scaler/fsconfig.h`: `nt.h`/`ntrtl.h` replaced by `memset`/`memcpy` and
  `uintptr_t`.
* `library/truetype.cpp`: the font read callback takes a pointer-sized
  cookie, as the scaler expects. Before, a 64-bit file handle was cut to 32 bits.
* `include/xfont*.h`: `#endif` labels turned into comments.
* `tools/makefont/xboxverp.h`: a version string stand-in.

Checked with `xfont_demo` on 64-bit Linux, using DejaVu Sans and the hinted
Liberation Serif: the default font, hinted TrueType glyphs, 4-bit
antialiasing, bold simulation, opaque background and right alignment all
render correctly. There is no comparison against a real Xbox yet.
