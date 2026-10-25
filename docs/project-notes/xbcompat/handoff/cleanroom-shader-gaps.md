# Handoff: fill xbcompat's NV2A rendering gaps by clean-room reimplementation

## Goal

xbcompat runs original Xbox games natively on Linux. It is a Proton-style compatibility layer: no CPU emulation and no VM. It replaces each game's Direct3D library with its own code, and that code renders through OpenGL. The rendering bugs found so far mostly come from NV2A GPU behaviors that xbcompat models wrong or not at all. Splinter Cell's W-buffered depth was one; its broken night/thermal goggles are still open.

xemu (an Xbox emulator) already implements most of those behaviors correctly. **The owner wants to keep xbcompat's license open: nothing GPL may enter xbcompat.** The job is to learn what the hardware does from xemu and reimplement it in xbcompat without copying xemu's code.

## Repository

- GitHub: `sokkycakes/xbox_leak_may_2020`, directory `xbcompat/`.
- Latest shader work is on branch `claude/project-thread-zvdmzt` at `11ceddce` (the W-buffer depth fix).
  - The consolidated main line is `claude/project-thread-p7e5s4` (PR #2). As of the last check it did not yet have the three 2026-10-10 Splinter Cell commits from zvdmzt (78f39a73, 53363941, 11ceddce).
  - Ask the owner which branch to base on. If unsure, start from zvdmzt and merge p7e5s4 in.
- Don't push to any branch the owner hasn't named.

### Files that matter

| File | What it is |
|---|---|
| `src/hle/psh.c` | Pixel shader translator: NV2A register combiners plus the four texture-stage modes, emitted as GLSL 1.20. |
| `src/hle/vsh.c` | Vertex program translator: NV2A vertex microcode to GLSL 1.20. The epilogue maps the program's screen-space oPos back to GL clip space. The W-buffer fix lives there (uniform `wdepth`). |
| `src/hle/vsh_interp.c` | CPU interpreter for vertex programs (reference for tests). |
| `src/hle/d3d8.c` | The replacement D3D8: render state to GL state, fixed-function pipeline (currently legacy GL fixed function, not GLSL), textures, render targets, Present. |
| `src/hle/nv2a_shaders.h` | Shared definitions for the translators. |
| `tests/psh_test.c`, `tests/psh_render.c`, `tests/psh_modes.c` | Pixel shader translation and render tests. |
| `tests/vsh_exec.c`, `tests/vsh_test.c`, `tests/vsh_synth.c` | Vertex program tests; vsh_exec compares translated GLSL against the interpreter. |
| `tools/xbrun.py` | Runs an XBE headless (see "Testing"). |

## License rules (important)

xemu is at https://github.com/xemu-project/xemu. Its GPU code is in `hw/xbox/nv2a/`; the version examined was commit `478b4f49`.

- **GPL:** `pgraph/glsl/psh.c`, `pgraph/glsl/vsh-prog.c`, `pgraph/glsl/vsh-prog.h`, `pgraph/vsh_regs.h`. **Never copy, paste or closely paraphrase these.**
- **LGPL:** almost everything else in `hw/xbox/nv2a/`, including `pgraph/glsl/vsh-ff.c`, `pgraph/texture.c`, `pgraph/swizzle.c` and the gl renderer. Treat these the same way: read for behavior, don't copy. (LGPL code could only enter as a separately linked, swappable library, and nobody has asked for that.)
- **Public domain:** `nv2a_vsh_cpu` (github.com/xemu-project/nv2a_vsh_cpu), a CPU vertex-shader emulator. Free to use.
- **Cxbx-Reloaded** is also GPL: reference only.
- **Fine to use:** the leaked XDK headers and docs in this repo (they describe hardware formats; they are already the basis of xbcompat), envytools' NV2A register database (rnndb, MIT), XbSymbolDatabase (MIT; already used by `tools/xbsymmap.py`).

### Clean-room procedure

Use two separate agents or passes:

1. **Spec writer.** Reads xemu's generators and writes a behavior spec in plain language: inputs (which registers and bits), the math, edge cases, and how they interact. **No code, no pseudo-code lifted from xemu, no identifier names beyond the hardware's register and field names.** Cite the hardware register or field, not the xemu line.
2. **Implementer.** Works only from the spec, the XDK headers and xbcompat's existing code, and never opens xemu. Writes the change in xbcompat's own style.
3. **Differential test (optional, private).** A test tool that runs the same input through xemu's generator and xbcompat's and compares rendered pixels. It may link GPL code **only if it is never committed to the repo or shipped**: keep it outside the repo, or in a clearly separate, uncommitted directory.

## What to cover

These are areas xemu's generators handle and that need checking in xbcompat. They are a checklist to investigate, not confirmed bugs. The spec writer should mark each one "xbcompat already matches", "partly" or "missing".

### Pixel side (vs xbcompat `psh.c`)

- Every texture mode in PS_TEXTUREMODES, including the dot-product family, the dependent reads (AR/GB), BUMPENVMAP and BUMPENVMAP_LUM, CLIPPLANE, DOT_ZW (depth replace) and DOT_RFLCT_SPEC_CONST. xbcompat currently returns "unsupported" for DOT_RFLCT_SPEC_CONST.
- How dependent-read coordinates are scaled for linear (non-swizzled) versus swizzled textures.
- PS_DOTMAPPING signed/unsigned conversions, PS_INPUTTEXTURE and PS_COMPAREMODE.
- Combiner input mappings (unsigned, invert, expand, half-bias, negate), output scale/bias, mux and the MSB/LSB selection flag, combiner count flags (unique C0/C1 per stage), and when register values are clamped.
- The final combiner, including the special inputs (V1R0 sum, EF product), the clamp-sum flag, and specular enable.
- Shadow-buffer depth compares: which compare function, D16 versus D24 scaling, linear versus swizzled.
- Alpha kill and alpha test, color key, texture border color.
- Fog modes and fog sources (including fog from the vertex program's oFog).
- Point sprite texture coordinates.
- **Depth:** the NV2A's W-buffer mode (z_perspective), depth clipping/clamping against the clip range, and float depth formats (F16, F24S8). xbcompat has a partial W-buffer fix for vertex programs only: it rebuilds z/w from the projection transform. The hardware behavior should be specified fully.
- Flat shading and two-sided color (front/back colors).

### Vertex side (vs xbcompat `vsh.c`)

- Every opcode's exact semantics, especially RCP/RCC clamps, LOG/EXP/LIT, DPH, ARL rounding, and out-of-range constant addressing.
- The 96/192-constant modes and the reserved constants (viewport scale/offset).
- The oPos epilogue: screen space to clip space, W-buffer, clip range, and what happens when w is zero or negative.
- oFog, oPts, front/back colors, texture-coordinate outputs.

### Fixed-function vertex pipeline (xemu `vsh-ff.c`, LGPL; vs xbcompat's legacy GL fixed function in `d3d8.c`)

- Lighting: directional/point/spot, local viewer, specular, emissive, material color sources, two-sided lighting.
- Texgen modes, texture matrices, projected coordinates.
- Fog generation (radial versus plane, the fog table), point size attenuation.
- Vertex blending/skinning (weights, matrix count), normalization.
- This would mean moving xbcompat's fixed function from legacy GL to its own GLSL. It's the largest item; do it last.

### Textures (xemu `pgraph/texture.c`, `swizzle.c`, `s3tc.c`; vs `d3d8.c` upload code)

- Every color format, including compressed (DXT1/3/5), palettized, YUV, signed, and depth formats sampled as color.
- Swizzled versus linear, cube maps and volume textures, mip levels, and pitch rules for linear textures.

## Known open bugs to use as test cases

- **Splinter Cell (XDK 4928):** night/thermal vision goggles render wrong (exact symptom not yet described by the owner). All 31 of its pixel shaders translate. One (texture modes 0x7c021: stages 2/3 are dependent GB/AR reads off stage 0) looks like the vision color lookup.
- **Splinter Cell depth:** fixed in 11ceddce. Use it as a regression test.
- Known xbcompat gaps from earlier notes: SPECULARENABLE with a synthesized final combiner, DOT_RFLCT_SPEC_CONST eye vector, X8L8V8U8 bump format, fog end == start, FocusBlur ATG sample draws a black box.

## Building and testing

- **Cloud build (32-bit):**

  ```
  cd xbcompat
  make -j8 BUILD=build GL=opengl PKGCFG=true \
    CFLAGS="-O2 -I/usr/include/SDL2 -I<sdl-config-dir> -D_REENTRANT -I/usr/include" \
    LDLIBS="-L<i386-lib-dir> -lSDL2 -lOpenGL -lpthread -lm -ldl"
  ```

  This needs 32-bit SDL2 and libOpenGL. The repo's `tools/sion` and the `bundle.sh` scripts show how the Sion build gets them.
- **Shader tests:** see each file's header comment in `tests/`. vsh_exec and psh_render need a display, so run them under Xvfb.
- **Running a game headless:**

  ```
  DISPLAY=:N SDL_AUDIODRIVER=dummy python3 tools/xbrun.py <default.xbe> --dvd <dir> --hdd <dir> --log <file> --screenshot "out/s%d.bmp" --shot-frame 100
  ```

  Useful environment variables:
  - `XBCOMPAT_INPUT_SCRIPT="700:a/10 1300:a/10"` presses buttons at given frames. A `t60:a` entry fires at a given number of seconds; a `w8:a` entry fires after 8 seconds with no new frame.
  - `XBCOMPAT_TRACE_FRAME=n` traces one frame.
  - `XBCOMPAT_VSH_DUMP=<dir>` dumps translated vertex programs.
  - `XBCOMPAT_PSH_DUMP=1` logs pixel shader source.
  - `XBCOMPAT_PSH_CATALOG=1` logs every pixel shader definition a game creates.
- The ATG samples in the leak are the regression suite (about 106–109 of 113 run). Re-run them after any translator change.

## Deliverables

1. The behavior spec (markdown, no xemu code), with each item marked matches / partly / missing.
2. xbcompat changes implementing the "missing" and "partly" items, pixel and vertex shaders first, each with a test in `tests/` where practical.
3. A short note per change saying which hardware behavior it implements and which test covers it.
