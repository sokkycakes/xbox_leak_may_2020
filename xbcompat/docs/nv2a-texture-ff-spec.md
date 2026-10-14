# NV2A texture and fixed-function behavior audit

## Scope and provenance

Reference examined: xemu commit `478b4f49`. xbcompat examined: `claude/project-thread-zvdmzt`, `src/hle/d3d8.c`. This document contains behavioral observations, mathematical relationships and test requirements; it contains no implementation excerpts.

**Merged-branch update:** consolidated commit `56d801b` adds authored mipmap support. The parent independently verified the merged mipmap render test with an authored chain under none, point and linear mip filtering. Accordingly, the premerge level-zero-only finding below is historical, not an outstanding merged-branch defect.

**XDK alias update:** the parent independently verified in the XDK `SDK/inc/d3d8types.h` that `X8L8V8U8 = 0x07` and `LIN_X8L8V8U8 = 0x1E`. They alias the corresponding ordinary X8R8G8B8 formats.

“Matches” means the examined logic agrees for the stated subset, not that hardware equivalence was tested. “Partly” includes behavior delegated to host GL whose NV2A edge cases remain unverified. Some reference behaviors are explicitly uncertain; those must not become asserted hardware requirements.

## Texture checklist

| Item | Status | Finding |
|---|---|---|
| Common uncompressed color layouts | Partly | xbcompat handles many 8-, 16- and 32-bit layouts. Exhaustive channel/boundary render tests have not been performed. |
| A8 and AL8 channel expansion | Matches, inspected subset | A8 supplies white RGB plus stored alpha. AL8 supplies the stored byte to RGB and alpha. |
| X1R5G5B5 and X8R8G8B8 alpha | Matches, inspected subset | Alpha is opaque regardless of unused stored bits. |
| V8U8 and L6V5U5 bump data | Partly | xbcompat preserves signed-component information through its private upload representation. L6V5U5 intermediate scaling differs from the reference and needs a numerical test. |
| X8L8V8U8 bump interpretation | Partly; high priority | Confirmed alias shares the ordinary X8R8G8B8 encoding. Ordinary RGB interpretation and bump U/V/luminance interpretation require different component selection. |
| Per-channel signed texture flags | Missing/uncertain | The examined xbcompat path does not establish general signed-texture semantics. The pinned xemu GL renderer itself explicitly does not implement the individual signed-channel flags. |
| P8 palettes | Partly | Palette lookup and cache identity exist. Smaller palette behavior and out-of-range indices require tests. |
| YUY2/UYVY | Partly | Packed conversion exists. The reference itself questions behavior when colorspace conversion is disabled. |
| DXT1/3/5, 2D/cube | Matches, inspected storage subset | Linear 4×4 block upload and minimum block footprint agree. Pixel-level decoding is delegated to GL and untested here. |
| Compressed volume textures | Missing in premerge branch | The examined upload path explicitly excludes compressed 3D textures. |
| Swizzled 2D and 3D | Partly | Both conversion paths exist. Non-square and strongly unequal dimensions need verified address-pattern tests. |
| Cube face allocation | Matches, inspected subset | Six whole mip chains use 128-byte face alignment. |
| Authored mip levels | Matches, merged regression subset | Parent verified the merged authored-chain render test under none, point and linear mip filtering. The premerge branch's level-zero-only upload is historical. |
| Linear pitch | Matches, ordinary 2D subset | Upload advances rows by byte pitch, not by visible width. |
| Floating depth formats | Partly | The examined xbcompat explicitly treats float depth storage as integer depth during upload. |
| Depth sampled as color | Partly/unknown | Existing depth-format textures use comparison sampling; arbitrary reinterpretation as color needs separate validation. |
| Texture-sourced borders | Missing in examined branch | Constant sampler border color exists. Physical border texels require different layout and coordinate handling. |

## Texture behavior specification

### Format identity and channel meaning

Relevant hardware fields:

- `NV097_SET_TEXTURE_FORMAT.COLOR`
- `NV097_SET_TEXTURE_FILTER.ASIGNED`
- `NV097_SET_TEXTURE_FILTER.RSIGNED`
- `NV097_SET_TEXTURE_FILTER.GSIGNED`
- `NV097_SET_TEXTURE_FILTER.BSIGNED`
- `NV097_SET_SHADER_STAGE_PROGRAM`
- `NV097_SET_SHADER_OTHER_STAGE_INPUT`

Storage layout, signedness and texture-stage use are distinct pieces of state. A host texture representation may differ from the hardware layout, but every consumer must account for that difference. A component rearrangement suitable for bump mapping must not silently change ordinary color sampling.

For `X8L8V8U8` (format `0x07`) and `LIN_X8L8V8U8` (format `0x1E`), a little-endian texel has low-to-high bytes **U, V, L, unused**. Under the ordinary X8R8G8B8 color interpretation, these same bytes are **B, G, R, unused**. Consequently:

- Ordinary color reads require logical RGB = L, V, U.
- Bump coordinate displacement requires U and V.
- Bump luminance requires L.
- Unused high-byte contents must not affect alpha.
- Changing the format’s global upload ordering is insufficient because the alias cannot be distinguished from the format number alone.
- If the host upload exposes RGB = L, V, U, the bump consumer must select B and G for displacement and R for luminance.
- Existing specialized bump uploads that already expose RGB = U, V, L must retain that interpretation.

**Signedness uncertainty:** preserving the byte representation and selecting the correct components is separate from reproducing signed filtering. The pinned xemu GL renderer explicitly leaves individual signed-channel flags unsupported. It cannot establish whether converting interpolated unsigned samples after filtering is equivalent to hardware signed filtering.

Suggested tests:

1. A 2×2 texture with independently varying U, V and L; choose values that make all three channels distinguishable.
2. Nearest-filtered bump displacement for zero, positive and negative U/V.
3. Luminance-only variation with displacement held zero.
4. Reuse the same bytes as ordinary X8R8G8B8 in a separate draw.
5. Use the same texture simultaneously as color and bump input on different stages.
6. Linear-filtered interpolation across the sign boundary, marked unresolved until a hardware or trustworthy reference oracle exists.

### Swizzled and linear addressing

Relevant fields:

- `NV097_SET_TEXTURE_FORMAT.BASE_SIZE_U/V/P`
- `NV097_SET_TEXTURE_FORMAT.DIMENSIONALITY`
- `NV097_SET_TEXTURE_IMAGE_RECT`
- `NV097_SET_TEXTURE_CONTROL1.IMAGE_PITCH`

For swizzled power-of-two images, coordinate bits contribute to the texel address in repeated X, Y, Z order, omitting an axis after all bits required for that dimension have been consumed. The resulting address counts texels; multiply by bytes per texel for a byte address. Two-dimensional textures omit Z entirely. Rectangular textures therefore must not use an indefinitely alternating two-axis pattern.

For linear 2D images, dimensions come from the rectangle fields and adjacent rows begin one byte pitch apart. Padding after the visible row is not image content. Linear textures do not acquire a mip chain merely because a mipmapped filter is requested.

Suggested tests: 8×2, 2×8, 8×4×2 and 2×4×8 coordinate-colored images; padded linear rows filled with conspicuous sentinel values.

### Mips, cubes and compressed storage

Relevant fields:

- `NV097_SET_TEXTURE_FORMAT.MIPMAP_LEVELS`
- `NV097_SET_TEXTURE_FORMAT.CUBEMAP_ENABLE`
- `NV097_SET_TEXTURE_CONTROL0.MIN_LOD_CLAMP/MAX_LOD_CLAMP`
- `NV097_SET_TEXTURE_FILTER.MIN/MAG/MIPMAP_LOD_BIAS`

Each successive mip dimension halves, remaining at least one texel. All authored levels belonging to a cube face precede the next face; each face begins on a 128-byte boundary.

DXT storage uses 4×4 blocks. DXT1 consumes eight bytes per block; DXT3 and DXT5 consume sixteen. A sub-4-texel mip still occupies complete blocks. For a 2D mip, the byte footprint equals ceiling(W/4) × ceiling(H/4) × block size. Volume handling must also account for slices and the changing depth of each mip.

The reference contains caveats around volume mip limits; do not copy those limits into a hardware specification without corroboration.

Suggested tests: a distinct solid color per authored mip, six distinct cube faces, small final DXT levels, noncubic volume dimensions, and LOD clamp/bias changes.

### Palettes, video and depth

Relevant fields:

- `NV097_SET_TEXTURE_PALETTE.LENGTH/OFFSET`
- `NV097_SET_TEXTURE_FORMAT.COLOR`
- `NV097_SET_TEXTURE_CONTROL0`
- `NV097_SET_SHADOW_ZSLOPE_THRESHOLD`
- `NV097_SET_SHADOW_DEPTH_FUNC`

P8 uses byte indices into ARGB palette entries. Palette changes must invalidate a texture’s expanded color representation even when its indexed texels are unchanged. Hardware palette lengths include 32, 64, 128 and 256 entries; behavior of an index outside the selected range remains unverified here.

YUY2/UYVY store two luma samples with shared chroma. Their byte ordering differs. Tests must distinguish byte ordering from the selected YUV-to-RGB transfer behavior. The pinned reference does not settle conversion-disabled behavior.

Floating depth storage requires interpreting its encoded numeric representation, not merely dividing its raw bits by an integer maximum. Sampling as depth, comparing depth and reinterpreting the same memory as color are separate uses; they must not be conflated. Exact F16/F24 conversion and comparison rules belong in the dedicated depth specification.

## Fixed-function checklist

| Item | Status | Finding |
|---|---|---|
| Directional lighting | Partly | Ordinary GL lighting path exists; local-eye and NV2A edge behavior remain unverified. |
| Point lighting | Partly | Distance attenuation exists; stored light range is not enforced by the examined rendering path. |
| Spot lighting | Missing cone behavior | Examined xbcompat treats nondirectional lights as point lights and disables the spot cone. Cone/falloff state is not retained. |
| Local viewer | Missing | No corresponding viewer-selection behavior found in examined path. |
| Diffuse/specular/emissive material | Partly | Basic GL material terms exist; shininess is capped to GL’s 128 limit. |
| Material color sources | Partly | Vertex color is routed to diffuse; independent material/diffuse/specular source selection is not implemented generally. |
| Two-sided lighting | Partly | Front/back GL materials exist. The pinned xemu FF reference itself lacks complete two-sided lighting, so it is not a sufficient oracle. |
| Texgen modes | Partly | Standard position, normal, reflection, object and sphere generation exist through GL. Independent component modes and viewer semantics need verification. |
| Texture matrices/projected coordinates | Partly | Existing transforms and q selection cover common cases; all component counts need tests. |
| Fog source and distance generation | Partly/missing | Fog state exists, but complete FF radial/planar/absolute-planar/explicit/specular-alpha source behavior is not established. |
| Point attenuation | Partly | Host point attenuation exists; NV2A limits and distance definition need explicit handling. |
| Vertex blending/skinning | Missing | Blend state is recorded, but no fixed-function weighted matrix path was found. |
| Normal normalization | Partly | Existing GL normalization is conditional on the lighting path; texgen-only cases require verification. |
| FF pipeline owned by xbcompat GLSL | Missing | Examined path still delegates vertex work to legacy host GL. |

## Fixed-function behavior specification

### Transform and skinning

Relevant registers:

- `NV097_SET_SKIN_MODE`
- `NV097_SET_MODEL_VIEW_MATRIX`
- `NV097_SET_INVERSE_MODEL_VIEW_MATRIX`
- `NV097_SET_COMPOSITE_MATRIX`
- `NV097_SET_NORMALIZATION_ENABLE`

Modes support an unblended transform, or two, three or four matrices. Generated-weight modes supply the final weight as one minus the sum of the preceding weights; explicit-weight modes use all supplied weights. Do not normalize explicit weights automatically.

Position is the weighted sum of the individually transformed positions. Normals use the corresponding normal transforms with no translation; blend first, then normalize if normalization is enabled. Projection follows the blended position. Avoid applying the model-view transform twice when a composite transform already incorporates it.

Suggested tests: weights summing to one, weights not summing to one, negative generated final weight, distinct translations for each matrix, and nonuniform scaling with normalization both enabled and disabled.

### Lighting

Relevant registers:

- `NV097_SET_LIGHT_ENABLE_MASK`
- `NV097_SET_LIGHT_LOCAL_POSITION`
- `NV097_SET_LIGHT_LOCAL_RANGE`
- `NV097_SET_LIGHT_LOCAL_ATTENUATION`
- `NV097_SET_LIGHT_INFINITE_DIRECTION`
- `NV097_SET_LIGHT_INFINITE_HALF_VECTOR`
- `NV097_SET_LIGHT_SPOT_DIRECTION`
- `NV097_SET_LIGHT_SPOT_FALLOFF`
- `NV097_SET_LIGHT_CONTROL.LOCALEYE`
- `NV097_SET_COLOR_MATERIAL`
- `NV097_SET_MATERIAL_EMISSION`
- `NV097_SET_MATERIAL_ALPHA`

Point-light attenuation uses 1 / (a0 + a1×d + a2×d²), with d measured between the Cartesian eye-space vertex and light. Range limits whether the light contributes. The reference treats range equality as included but explicitly questions that boundary; exact equality is therefore a pending test.

Diffuse intensity is bounded below by zero. Specular is suppressed when the light lies behind the surface; its exponent is not fundamentally limited by host GL’s 128 shininess cap.

Local-viewer behavior derives a view direction for each vertex. Infinite-viewer behavior uses its specified fixed information; they are not interchangeable.

Material emissive, ambient, diffuse and specular terms have independently selectable sources. Alpha must follow the applicable source rules instead of inheriting an arbitrary host GL convention.

**Reference limitations:** the pinned reference approximates spotlight falloff and does not fully implement two-sided lighting. These areas require XDK/API documentation and hardware tests before an exact hardware specification can be claimed.

Suggested tests: point-light range crossing; constant/linear/quadratic attenuation separately; spotlight inner, transition and outer regions; moving the eye while geometry stays fixed; each material source independently; shininess above 128; front/back materials on opposite triangle windings.

### Texgen and matrices

Relevant registers:

- `NV097_SET_TEXGEN_S/T/R/Q`
- `NV097_SET_TEXGEN_PLANE_S/T/R/Q`
- `NV097_SET_TEXGEN_VIEW_MODEL`
- `NV097_SET_TEXTURE_MATRIX_ENABLE`
- `NV097_SET_TEXTURE_MATRIX`

Disabled texgen preserves the input component. Object-linear generation takes a plane dot product with object position. Eye-linear generation takes a plane dot product with transformed position. Normal mapping uses transformed normal components. Reflection mapping uses the incident/view direction and transformed normal. Sphere mapping derives S and T from the reflection direction, including its shifted Z normalization.

Generate coordinates before applying the texture matrix. Projected coordinates must preserve the selected divisor through interpolation; premature division in the vertex stage changes perspective behavior.

The reference explicitly leaves texgen viewer-model behavior unresolved and questions normalization details. These remain test targets rather than settled rules.

### Fog and point size

Relevant registers:

- `NV097_SET_FOG_GEN_MODE`
- `NV097_SET_FOG_PLANE`
- `NV097_SET_FOG_MODE`
- `NV097_SET_FOG_PARAMS`
- `NV097_SET_POINT_PARAMS_ENABLE`
- `NV097_SET_POINT_PARAMS`
- `NV097_SET_POINT_SIZE`

Fog distance sources include specular alpha, explicit fog input, radial transformed-position distance, a fog-plane evaluation and the absolute value of that plane evaluation. Source selection is independent of the fog table/function. Use off-axis geometry to distinguish radial and planar fog; testing only the viewing axis cannot distinguish them.

Point attenuation includes a reciprocal square root of a quadratic distance expression. The hardware parameterization also supplies offsets/scales and lower/upper size bounds. The pinned reference caps the hardware-size bounds at 63.875 pixels. Its distance calculation includes the homogeneous position component; verify this against hardware before changing xbcompat’s distance convention.

Suggested tests: off-axis fog, negative planar distance, explicit fog input, specular-alpha fog, zero-width fog interval, point distances near zero, point bounds, and homogeneous positions whose W differs from one.

## GLSL migration boundaries

The migration should preserve a clear sequence:

1. Fetch and decode vertex attributes.
2. Apply weighted position/normal transforms.
3. Generate front/back diffuse/specular colors.
4. Generate fog, point size and texture coordinates.
5. Apply texture matrices and retain projection divisors.
6. Apply xbcompat’s shared NV2A-to-host position/depth conversion.
7. Pass outputs through the existing pixel-shader interface.

Make feature selection part of the shader key and numeric state uniforms. Keep render-target orientation, clipping, depth mapping and output conventions shared with programmable vertex shaders. Otherwise the FF migration will recreate existing discrepancies in another path.

A complete migration is larger than the confirmed bump-channel fix and should follow shader and texture correctness work.

## Source index

- [xemu texture behavior reference](https://github.com/xemu-project/xemu/blob/478b4f49/hw/xbox/nv2a/pgraph/texture.c)
- [xemu swizzled addressing reference](https://github.com/xemu-project/xemu/blob/478b4f49/hw/xbox/nv2a/pgraph/swizzle.c)
- [xemu compressed texture reference](https://github.com/xemu-project/xemu/blob/478b4f49/hw/xbox/nv2a/pgraph/s3tc.c)
- [xemu GL texture behavior and unresolved signed flags](https://github.com/xemu-project/xemu/blob/478b4f49/hw/xbox/nv2a/pgraph/gl/texture.c)
- [xemu FF behavior reference and explicit limitations](https://github.com/xemu-project/xemu/blob/478b4f49/hw/xbox/nv2a/pgraph/glsl/vsh-ff.c)
- [Hardware register/field names examined](https://github.com/xemu-project/xemu/blob/478b4f49/hw/xbox/nv2a/nv2a_regs.h)
- [Audited xbcompat premerge D3D implementation](https://github.com/sokkycakes/xbox_leak_may_2020/blob/claude/project-thread-zvdmzt/xbcompat/src/hle/d3d8.c)

No rendering tests were executed by the spec writer. The merged mipmap validation and XDK alias confirmation above were independently reported by the parent implementer.

## Compressed volume follow-up

The pinned reference decoder and GL upload path agree that compressed volumes use depth slabs of up to four slices. Within a slab, XY 4×4 tiles are in row-major order; the blocks for one tile across that slab’s slices are consecutive. The final slab contains only its remaining slices, without padding to four. Each mip occupies ceil(W/4) × ceil(H/4) × D × block bytes, followed immediately by the next mip. This is reference-level behavioral evidence; independent XDK or hardware confirmation of the volume ordering was not established. An 8×8×8 fixture distinguishes tile-major slabs from complete sequential Z images. No reference decoder code was supplied to the implementer.
