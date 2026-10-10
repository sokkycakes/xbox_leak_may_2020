# NV2A clean-room implementation and validation

Base selected by the owner: `claude/project-thread-zvdmzt` at `11ceddce`. Consolidated `claude/project-thread-p7e5s4` at `643b4c6` was merged locally as `29ece29`, retaining the W-buffer workaround and consolidated runtime/mipmap changes.

Continuation requested from `16927d20853636403c0976698ae1453bd83c01e1` on the same branch. The 2026-10-10 continuation uses GitHub-hosted CI because the interactive executor could not provision its sandbox. No synced project references were modified.

## Provenance

Separate reference-only agents inspected xemu commit `478b4f49` and produced the plain-language specifications below. Implementation agents and the integrating author used those specifications, existing xbcompat code, XDK declarations, and permitted primary documentation. They did not open xemu or Cxbx implementation sources. No reference-emulator source, generated shader, GPL-linked test harness, or binary was added. No GPL differential harness was used.

- [Pixel and depth behavior audit](nv2a-pixel-spec.md)
- [Vertex behavior audit](nv2a-vertex-spec.md)
- [Texture and fixed-function behavior audit](nv2a-texture-ff-spec.md)

Audit statuses describe the examined baseline or explicitly identified merged subset. The implementation table below records what changed after that audit. A passing synthetic test demonstrates the stated fixture, not complete hardware equivalence.

## Implemented changes

| Change | Behavior | Regression |
|---|---|---|
| Paired ARL/ILU | Both units read instruction-entry A0 before ARL publishes its result. | Three literal transform-feedback cases in `vsh_exec --selftest`; all three fail with the merged baseline translator. |
| W-buffer preservation | Retains the accepted projection-derived depth workaround from 11ceddce. | Four literal clip-position checks at W=2 and W=4, enabled/disabled. These do not assert complete NV2A W-depth semantics. |
| Implicit final combiner | SPECULARENABLE includes secondary RGB, saturating the sum before fog while retaining R0 alpha. Explicit final combiners remain authoritative. | `psh_render` off/on, saturation, fog, alpha and explicit-final cases. |
| Constant-eye reflection | DOT_RFLCT_SPEC_CONST uses the supplied eye vector and normal-length correction. Raw NV097 eye-vector methods feed a uniform; push-buffer/state-block snapshots preserve it. Capture drains pending hardware state. | Non-unit normal and eye-vector update cases in `psh_render`. Raw-method/state-block round-trip has not been independently rendered. No unproven constant-0 alias is introduced. |
| Sample alpha and key controls | Alpha kill, key operations, XRGB alpha exclusion, sampled-stage selection and reference-modeled operation order. | `psh_render` numerical cases; nine HLE draws in `renderer_smoke` verify disabled/enabled/disabled cache selection and uniform-only key-color changes. |
| Bump alias channels | X8L8V8U8 shares X8R8G8B8 storage; bump consumers select B/G for U/V and R for luminance while ordinary color upload stays intact. | Independent U/L values and zero-luminance alpha kill in `psh_render`. |
| Projected shadow reads | Preserves PROJECT3D on a 2D depth texture, supplies projected native-depth reference, and distinguishes PROJECT2D's reference-modeled zero reference. | D16/D24 high/low projected-reference pixel checks. |
| Point sprites | Synchronizes stage-three coordinate replacement on pixel-shader draws, including disabling after an enabled draw. | Pixel-shader point quadrants and fixed UVs in renderer_smoke. |
| Framebuffer copies | Refreshes texture-backed GPU source images, linearizes/swizzles copy storage, preserves partial destination contents, updates existing cached BGRA8 subresources without deleting siblings/attachments, and initializes new standalone targets from copied memory. | Successive GPU colors into linear/swizzled textures, partial backbuffer preservation, active/rebound/first-bound standalone targets, and cube sibling preservation in `renderer_smoke`. The initial successive-color test fails on merged baseline. |
| Compressed volume textures | Decodes DXT1/3/5 blocks independently from the Khronos format description and uploads ordinary RGBA volumes. Volume storage uses the sanitized reference-level four-slice slab ordering. | Pure palette, crop, slab, bounds and truncation fixtures; D3D draws distinguish XY tiles, Z slabs and authored mip offsets for all three formats. |
| Fixed-function GLSL lighting subset | Owned ordinary point/directional lighting, range, attenuation, local viewer, material/primary and absent-color selection, normalization, and powers above 128. | Twenty-two direct lighting checks and additional D3D draws, including a fixed fragment stage. Unsupported state combinations deliberately retain the existing path. |

### Continuation from 16927d2

The behavior specifications above remain separate, unchanged reference inputs. This continuation read those sanitized specifications, xbcompat implementation/tests and XDK header declarations. It did not inspect reference-emulator implementation source. Fixtures are independently written literal inputs and expected results derived from the specifications; they are not hardware captures.

| Change | Implemented scope | Regression evidence |
|---|---|---|
| Constant-mode uploads | Honor NORESERVEDCONSTANTS during 96-mode draw uploads as well as viewport updates. | Render logical endpoints -96/95, application constant 0 and reserved -38 in all three API modes; verify reserved viewport updates after clearing the flag. |
| Programmable point size | Enable oPts only with point parameters enabled; restore fixed size on disable and when leaving the programmable path. | HLE enabled/disabled draws with programmable and compatibility fragment stages, then FVF return. |
| Vertex fog placement | Paired programmable stages evaluate finite EXP/EXP2/LINEAR fog at vertices, interpolate unclamped factors, then clamp in the fragment shader. | Nonlinear endpoint interpolation, masked scalar oFog write, out-of-range linear interpolation and fog disable. Mixed stages retain legacy distance behavior. |
| Front/back outputs | Synchronize programmable two-sided color selection with render state. | Opposite windings with distinct front/back colors; disable restores front color. A flat triangle checks the current last-vertex convention, not all Xbox primitive rules. |
| Linear texture coordinates | Avoid dividing fixed-function vertex coordinates by texture dimensions when the pixel shader already performs that scaling. | Padded linear rows and packed copy tests sample distinct texels; the initial new packed-copy fixture exposed the double scaling. Uncertain dependent-read scaling is unchanged. |
| Color copies | Native transfer layouts for nine packed/RGBA color families; refresh GPU source and update cached destination image in place. | GPU-written source copied partially into cached destinations, preserving untouched texels across all nine families. Same-format copies only; no conversion claim. |
| Integer depth/stencil copies | Native D16 and D24S8 transfer types. | D16 and D24S8 partial copies preserve untouched depth and source sibling levels; D24S8 also checks stencil. |
| Compressed copies | Same-format DXT1/3/5 block copies, aligned origins and block-aligned or edge ends; complete small mips; snapshot for overlap. | Cached destination sampling, overlapping self-copy and 2x2 final-mip storage. Unsupported rectangles are not treated as decoded color conversions. |
| Mip attachments | Attach the actual 2D/cube color subresource and integer depth level; use surface-view dimensions for viewport. | Independent base/level-one clears, cube and 2D copy-out, D16/D24S8 sibling preservation. |
| FF skinning | All six API modes; weighted position/normal transforms; generated final weights; no explicit-weight normalization. | Both FVF and declaration input, two/three/four matrices, nonsumming weights, negative final weight, nonuniform scale and normalization after blending. |
| Secondary material colors | Preserve four-component COLOR2 separately from legacy GL secondary RGB. | Direct diffuse-alpha and emissive fixtures; HLE FVF/declaration RGBA and COLORVERTEX toggles. |
| Known texgen modes | Owned normal, eye-position and object-position API modes before texture matrices; normalize texgen normals when requested even without lighting. | HLE component values, matrix ordering and unlit normalization. Raw independent component planes and reflection/sphere viewer rules remain outside this subset. |
| Texture-format/address coverage | Literal packed color, luminance, alpha, palette and YUV fixtures; rectangular 2D/3D swizzle and padded linear rows. | Sixteen formats in both linear and swizzled layouts; four P8 palette sizes and palette-only changes; YUY2/UYVY black/white ordering; 8x2, 2x8, 8x4x2 and 2x4x8 addresses. |

Detailed pixel notes: [nv2a-pixel-changes.md](nv2a-pixel-changes.md). Lighting eligibility: [nv2a-ff-lighting-change.md](nv2a-ff-lighting-change.md). Volume details: [nv2a-dxt-volume-changes.md](nv2a-dxt-volume-changes.md).

## Validation environment and commands

Tests run on 32-bit i386 Linux in Debian Bookworm, SDL2, Xvfb and Mesa 22.3.6 llvmpipe (OpenGL compatibility profile). The full native xbcompat executable and separate native-renderer bridge build successfully. This is software-driver validation; Raspberry Pi hardware was not exercised.

From `xbcompat/`:

```sh
make -f tools/nv2a-check.mk -j4 CC=i686-linux-gnu-gcc \
  PKGCFG='env PKG_CONFIG_LIBDIR=/usr/lib/i386-linux-gnu/pkgconfig pkg-config' check
make -f tools/native-renderer.mk TARGET=i386 BUILD=build-nv2a -j4 check check-render
make -j8 TARGET=i386 CC=i686-linux-gnu-gcc BUILD=build-native-nv2a
```

The full build requires generated kernel exports from the existing XDK source, as in the repository's normal build. The standalone NV2A target generates its own `glslcheck` and needs no game assets.

The decoder fixtures and all D3D renderer fixtures also pass, including 60,000 native bridge ABI calls.

Recorded passing shader checks: 35 pixel-output assertions, 48 texture-mode assertions, seven vertex execution fixtures, 22 lighting assertions, 37 synthetic pixel translation/link checks, and seven synthetic vertex programs. `psh_test --synthetic` explicitly reports that sample XPU files were not tested; missing sample files are not counted as passes.

The original ATG executable/shader corpus and Splinter Cell game assets were not present in the examined worktree/container. The approximately 113-sample regression and the in-game goggles/depth regression could therefore not be run. The supplied goggles symptom is garbled graphics plus a still frame during the effect; framebuffer freshness is a plausible contributor, not a demonstrated diagnosis or confirmed game fix.

## Remaining gaps and limits

This change is not a complete replacement of NV2A rendering. The audits preserve the full requested checklist and distinguish matches, partial implementations, missing features and unresolved evidence.

- Full W-depth interpolation, post-interpolation clip-range behavior, float-depth encoding, and zero/negative-W edge behavior remain open. The known workaround is preserved and regression-tested.
- Dependent-read scaling for linear destinations and signed-filter boundary behavior remain unchanged because the pinned reference does not establish a trustworthy rule.
- Exceptional LOG/LIT/reciprocal, constant-address edge behavior still need dedicated validation. Endpoint mapping and ordinary constant-mode/reserved-viewport transitions are now covered. Point-output selection is now tested; raw eighth-pixel register limits are not. Ordinary paired-program fog interpolation is covered; ABS/NaN/infinity, mixed/FF sources and zero-width intervals remain open. Flat strips/fans, unwritten back outputs and NaN colors remain unverified.
- The fixed-function migration now includes skinning, supplied secondary material colors and normal/eye/object API texgen. Spot cones/falloff, FF two-sided lighting, reflection/sphere texgen, fog and point attenuation remain on the legacy path. Raw independent texgen component planes, viewer semantics and singular normal transforms are not resolved.
- CopyRects now covers the same-format packed, integer-depth and aligned compressed subsets above, including nonzero mip attachments. Cross-format conversion, arbitrary unaligned compressed rectangles, floating-depth images and standalone depth-surface readback are not established. Source/destination allocation failures are checked.
- Compressed-volume uploads use complete mip storage. Arbitrary nonzero compressed-volume LockBox sub-box offsets remain unverified and are not fixed by this change.
- Common color/luminance/alpha, all palette lengths, YUV byte order, pitch and rectangular swizzle now have literal fixtures. Signed filtering, L6V5U5 scaling, out-of-range palette indices, YUV conversion-disabled behavior, floating-depth encodings, physical texture borders and complete LOD clamp/bias behavior remain outstanding. Authored mipmap support was supplied by the consolidated branch and remains regression-tested.
- FocusBlur's black box and actual Splinter Cell night/thermal vision remain unverified without their assets.

Reference-modeled shadow and key-order rules are documented as such; passing them against hand-written expected values is not independent hardware confirmation.

## Evidence needed to finish the full checklist

The checklist is not fully complete. Remaining work must not be marked done solely because the software-driver suite passes.

| Remaining area | Required next input or validation |
|---|---|
| W-depth, raster clip bounds, integer quantization boundaries, F16/F24 and reinterpretation | Independent numeric/hardware oracle covering interpolation and encoded depth values; current workaround and projected-shadow fixtures are narrower. |
| Exceptional shader arithmetic, fog ABS/NaN/infinity, constant-address edges | Hardware outputs for the conflicting or explicitly unresolved cases in the vertex audit; ordinary GLSL behavior is not proof. |
| Spotlight falloff, FF two-sided lighting, reflection/sphere viewer behavior, raw point bounds | Complete behavioral specification from permitted API/register documentation and hardware tests; the sanitized audit identifies reference limitations. |
| Signed filtering, dependent linear reads, physical borders and unresolved video/depth formats | Independent sampling fixtures with expected hardware results. |
| Fixed-function interactions with constant modes, raw reflection-eye state round-trip, flat strips/fans, texture projection counts, LOD controls and compressed-volume sub-box locks | Additional integration fixtures and implementation follow-up; these remain test/implementation backlog, not all hardware-blocked. |
| Approximately 113 ATG samples, FocusBlur and Splinter Cell night/thermal vision | Executable/shader/game fixtures accessible to the test environment, with reproducible launch paths and expected captures. Synthetic coverage does not replace this regression. |
| Raspberry Pi/ARM renderer | Build and rendering validation on the target driver/device; current CI exercises i386 Mesa software rendering. |

Cloud CI is defined in `.github/workflows/nv2a.yml`. It runs the shader/texture suite, native renderer bridge ABI/render suite, and full i386 executable build. The generated kernel export build input is the normal repository input; it is not reference-emulator code.
