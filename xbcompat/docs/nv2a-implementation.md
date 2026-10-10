# NV2A clean-room implementation and validation

Base selected by the owner: `claude/project-thread-zvdmzt` at `11ceddce`. Consolidated `claude/project-thread-p7e5s4` at `643b4c6` was merged locally as `29ece29`, retaining the W-buffer workaround and consolidated runtime/mipmap changes.

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
| Fixed-function GLSL lighting subset | Owned ordinary point/directional lighting, range, attenuation, local viewer, material/primary and absent-color selection, normalization, and powers above 128. | Fifteen direct lighting checks and nine D3D draws, including a fixed fragment stage. Unsupported state combinations deliberately retain the existing path. |

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

Recorded passing shader checks: 35 pixel-output assertions, 48 texture-mode assertions, seven vertex execution fixtures, 15 lighting assertions, 37 synthetic pixel translation/link checks, and seven synthetic vertex programs. `psh_test --synthetic` explicitly reports that sample XPU files were not tested; missing sample files are not counted as passes.

The original ATG executable/shader corpus and Splinter Cell game assets were not present in the examined worktree/container. The approximately 113-sample regression and the in-game goggles/depth regression could therefore not be run. The supplied goggles symptom is garbled graphics plus a still frame during the effect; framebuffer freshness is a plausible contributor, not a demonstrated diagnosis or confirmed game fix.

## Remaining gaps and limits

This change is not a complete replacement of NV2A rendering. The audits preserve the full requested checklist and distinguish matches, partial implementations, missing features and unresolved evidence.

- Full W-depth interpolation, post-interpolation clip-range behavior, float-depth encoding, and zero/negative-W edge behavior remain open. The known workaround is preserved and regression-tested.
- Dependent-read scaling for linear destinations and signed-filter boundary behavior remain unchanged because the pinned reference does not establish a trustworthy rule.
- Exceptional LOG/LIT/reciprocal and constant-address behavior, point-output enable selection, fog interpolation/source details, flat/two-sided edge cases, and zero-width fog intervals need stronger oracles.
- The fixed-function migration covers the stated lighting subset. Skinning, spot cones/falloff, supplied secondary-color material sources, two-sided lighting, texgen, fog and point attenuation remain on the legacy path.
- GPU CopyRects readback/update is exact for BGRA8 format views (06/07/12/1E). Packed/depth/compressed copies, format conversion and rendering into nonzero mip attachments remain incomplete. Source/destination allocation failures are checked.
- Compressed-volume uploads use complete mip storage. Arbitrary nonzero compressed-volume LockBox sub-box offsets remain unverified and are not fixed by this change.
- Exhaustive color/palette/YUV/depth-format and physical texture-border tests are still outstanding. Authored mipmap support was already supplied by the consolidated branch and is covered by its renderer regression.
- FocusBlur's black box and actual Splinter Cell night/thermal vision remain unverified without their assets.

Reference-modeled shadow and key-order rules are documented as such; passing them against hand-written expected values is not independent hardware confirmation.
