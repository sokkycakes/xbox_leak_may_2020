# NV2A pixel behavior audit

Behavior-only checkpoint for claude/project-thread-zvdmzt. Restricted reference examined: xemu 478b4f49. No restricted code or pseudocode is included.

“Matches” means the inspected behavior agrees with available documentation/reference; it does not mean hardware verification. “Partly” includes unverified edge cases. Several requested features are also incomplete in the pinned reference, so it cannot serve as a complete hardware oracle.

## Sources

- [XDK pixel shader format documentation](https://github.com/sokkycakes/xbox_leak_may_2020/blob/claude/project-thread-zvdmzt/xbox_leak_may_2020/xbox%20trunk/xbox/public/xdk/inc/d3d8types.h): PS_TEXTUREMODES, PS_DOTMAPPING, PS_INPUTTEXTURE, PS_COMPAREMODE, combiner fields.
- [XDK public API header](https://github.com/sokkycakes/xbox_leak_may_2020/blob/claude/project-thread-zvdmzt/xbox_leak_may_2020/xbox%20trunk/xbox/public/xdk/inc/d3d8.h).
- [Pinned restricted reference, read only](https://github.com/xemu-project/xemu/blob/478b4f49/hw/xbox/nv2a/pgraph/glsl/psh.c).
- [NVIDIA texture-shader specification](https://registry.khronos.org/OpenGL/extensions/NV/NV_texture_shader.txt): corroborating related hardware behavior, not automatically authoritative for Xbox differences.
- [NVIDIA register-combiner specification](https://registry.khronos.org/OpenGL/extensions/NV/NV_register_combiners.txt): same limitation.

## Priority findings

1. **Missing:** SPECULARENABLE in the synthesized final combiner.
2. **Missing:** texture-stage ALPHAKILL and COLORKEYOP in the programmable pixel path.
3. **Partly:** shadow texture mode handling; PROJECT2D and PROJECT3D have distinct depth-reference behavior.
4. **Missing:** DOT_RFLCT_SPEC_CONST; reflection math is documented, but the API/state source for its constant eye vector remains unverified.
5. **No established fix:** dependent AR/GB coordinate scaling. Swizzled destinations agree. Pinned xemu rejects linear destinations rather than defining them.
6. The owner's goggles symptom—garbled graphics and a frozen frame—also requires render-target, texture feedback, and framebuffer-copy investigation. No evidence establishes dependent-coordinate scaling as the cause.

## Texture-stage behavior

Hardware fields: PS_TEXTUREMODES, PS_INPUTTEXTURE, PS_DOTMAPPING, PS_COMPAREMODE.

Let a stage receive texture coordinates (s,t,r,q), and let its selected preceding texture result be (R,G,B,A).

| Mode | Status | Required behavior / limitation |
|---|---|---|
| NONE | Matches | Inactive texture result has zero RGB and alpha one. |
| PROJECT2D | Partly | Sample at (s/q,t/q). Ordinary 2D behavior matches. Shadow behavior differs; see below. Other resource-dimensionality combinations need independent tests. |
| PROJECT3D | Partly | Sample at (s/q,t/q,r/q). Ordinary volume behavior matches. A 2D depth resource still requires the projected depth-reference semantics described below. |
| CUBEMAP | Matches for ordinary cube resources | Sample with direction (s,t,r). Non-cube resources used with this mode are not fully audited. |
| PASSTHRU | Matches corroborating NVIDIA specification | Convert coordinates to RGBA with component clamping to [0,1]. Pinned xemu omits this clamp; that disagreement does **not** justify removing xbcompat's clamp. |
| CLIPPLANE | Matches | Each component independently rejects the fragment according to its PS_COMPAREMODE bit: zero selects rejection below zero; one selects rejection at or above zero. |
| BUMPENVMAP | Partly | Add the selected source's two signed perturbations, transformed by the bump matrix, to the stage's base 2D coordinates, then sample. Host upload channel order must be considered; xemu and xbcompat use different channel representations. |
| BUMPENVMAP_LUM | Partly | Apply bump addressing, then multiply fetched RGBA by luminance × scale + offset. Existing xbcompat saturates the product. Signed texture filtering and representation remain risks. |
| BRDF | Partly / unverified | Documented lookup coordinates are eye polar angle, light polar angle, and eye azimuth minus light azimuth. xbcompat implements a packed interpretation; pinned xemu leaves this mode incomplete. |
| DOT_ST | Matches ordinary 2D case | Use the preceding dot result as the first coordinate and the current dot result as the second. |
| DOT_ZW | Partly | Replace depth with preceding dot result divided by current dot result. xbcompat implements a fixed depth-range interpretation; other depth formats and clipping require tests. Pinned xemu does not implement the replacement. |
| DOT_RFLCT_DIFF | Matches ordinary cube case | Assemble a normal from preceding, current, and following stage dot results, then sample the cube with that normal. |
| DOT_RFLCT_SPEC | Matches ordinary nondegenerate cube case | Assemble the normal from three dot results. Assemble the eye vector from the three participating stages' q coordinates. Reflect using the equation below. A zero-length normal is unresolved. |
| DOT_STR_3D | Matches ordinary volume case | Three consecutive dot results provide the three lookup coordinates. |
| DOT_STR_CUBE | Matches ordinary cube case | Three consecutive dot results provide the cube direction. |
| DPNDNT_AR | Matches swizzled destination; linear unresolved | Selected source alpha supplies the first coordinate and red the second. |
| DPNDNT_GB | Matches swizzled destination; linear unresolved | Selected source green supplies the first coordinate and blue the second. |
| DOTPRODUCT | Matches as an intermediate dot producer | Produce a dot result for subsequent stages. Color-result details should not be inferred from its purpose alone. |
| DOT_RFLCT_SPEC_CONST | Missing | Same reflection construction as specular reflection, using the separately supplied constant eye vector. |

Reflection requirement, from XDK PS_TEXTUREMODES documentation:

\[
\text{direction}=\frac{2(\mathbf n\cdot\mathbf e)}{\mathbf n\cdot\mathbf n}\mathbf n-\mathbf e.
\]

For DOT_RFLCT_SPEC_CONST, the documentation calls the state-setting operation SetEyeVector(), while the inspected public API header does not expose that operation. The claim that pixel-shader constant zero supplies this state has **not been independently established**. Do not connect arbitrary packed combiner constants to this vector.

### Dependent-read coordinate scaling

For a normal swizzled destination, AR/GB channels directly represent normalized destination coordinates.

Ordinary linear texture sampling uses pixel-space coordinates, requiring conversion by logical texture dimensions when the host sampler expects normalized coordinates. That statement does **not** establish how AR/GB should behave with a linear destination: the pinned reference rejects that configuration.

A useful test matrix separates:

- Linear versus swizzled **source** textures.
- Linear versus swizzled **destination** textures.
- AR versus GB.
- Point versus linear filtering.
- Render-target-backed versus uploaded source textures.

Changing scaling based solely on the source texture's layout would be unjustified.

### Stage selection and comparisons

PS_INPUTTEXTURE selects the preceding result for dependent/bump/dot operations. Stage 1 selects stage 0; stage 2 can select 0 or 1; stage 3 can select 0, 1, or 2. Existing valid-case decoding matches.

PS_COMPAREMODE affects clipping comparisons, not arbitrary texture depth tests. Existing decoding matches.

## Dot mappings

Hardware field: PS_DOTMAPPING. Status: **matches documented ordinary mappings; HILO filtering and out-of-domain details partly verified**.

For an unsigned byte value b:

| Mapping | Conversion |
|---|---|
| ZERO_TO_ONE | b/255 |
| MINUS1_TO_1_D3D | (b-128)/127 |
| MINUS1_TO_1_GL | For b<128, (b+0.5)/127.5; otherwise (b-255.5)/127.5 |
| MINUS1_TO_1 | Interpret the byte as signed two's-complement, then divide by 127 |

The D3D and ordinary signed variants deliberately include an endpoint below −1. Do not silently clamp that endpoint. In particular, byte zero mapping to −128/127 in MINUS1_TO_1_D3D is explicitly documented, not a suspected bug.

HILO_1 converts unsigned 16-bit H and L to (H/65535,L/65535,1).

The hemisphere variants construct:

\[
(H,L,\sqrt{1-H^2-L^2}).
\]

Their signed 16-bit mappings differ:

- D3D: signed value divided by 32768.
- GL: signed value mapped with (2v+1)/65535.
- Ordinary: signed value divided by 32767.

XDK documentation supplies these mappings. Pinned xemu does not correctly implement the hemisphere variants. xbcompat does more here than the reference.

Unresolved: exact treatment when H²+L²>1, and whether reconstruction from filtered packed components preserves hardware quantization. Existing xbcompat prevents a negative square-root argument; do not present that choice as hardware-proven.

## General combiners

Hardware fields: PSRGBInputs, PSAlphaInputs, PSRGBOutputs, PSAlphaOutputs, PSCombinerCount.

### Input mapping — matches

For source component x:

| Mapping | Behavior |
|---|---|
| Unsigned identity | max(x,0) |
| Unsigned invert | 1-clamp(x,0,1) in both inspected implementations |
| Expand normal | 2max(x,0)-1 |
| Expand negate | 1-2max(x,0) |
| Half-bias normal | max(x,0)-0.5 |
| Half-bias negate | 0.5-max(x,0) |
| Signed identity | x |
| Signed negate | -x |

Unsigned identity is not a blanket upper clamp.

RGB inputs select RGB or replicated alpha. Alpha inputs select blue or alpha.

### Products, sum, output mapping — matches

AB and CD independently perform component products or RGB dot products. A dot product is replicated across the RGB result.

The sum result is formed from **unmapped** AB and CD results; it must not sum already scaled/clamped destination values.

Output mappings include identity, subtract one-half, multiply by two, subtract one-half then multiply by two, multiply by four, and divide by two. Each written result is clamped to [-1,1] after its output mapping.

RGB and alpha halves observe the same pre-stage register state. Writes from one half must not become inputs to the other half within that stage.

Blue-to-alpha flags copy the mapped RGB result's blue component.

### Mux — partly

- MSB mode selects CD when the pre-stage R0 alpha is at least 0.5; otherwise AB. Existing behavior matches.
- LSB mode selects according to the low bit of the stored eight-bit alpha.

Pinned xemu truncates a floating reconstruction while xbcompat rounds. That difference alone does not prove xbcompat wrong, because actual hardware register quantization is the missing fact. Test values on either side of byte boundaries before changing it.

### Constants — matches

The unique-C0 and unique-C1 flags independently select per-stage constants. With a flag clear, that constant bank uses the first stage's constant throughout. Final-combiner constants are separate.

## Final combiner

Hardware fields: PSFinalCombinerInputsABCD, PSFinalCombinerInputsEFG, PS_FINALCOMBINERSETTING_*.

Explicit final color:

\[
RGB=D+A\,B+(1-A)\,C,\qquad \alpha=G.
\]

Status: **matches ordinary explicit final operation**.

EF_PROD is the product of the final E and F RGB inputs. V1R0_SUM combines the designated V1/R0 values. Their alpha components are zero. Both special inputs are valid only for A–D.

CLAMP_SUM constrains the sum to [0,1] before its later final-input use.

**Complement edge case unresolved:** xbcompat applies unsigned-invert-style clamping before complementing V1/R0; the pinned reference directly subtracts them from one. The XDK describes these flags as unsigned invert. Do not change this solely to imitate xemu.

### Synthesized final combiner — missing specular

When the API leaves the final combiner implicit:

- With specular disabled, use R0 RGB.
- With SPECULARENABLE, add V1 RGB and clamp the resulting color to [0,1].
- When fog is active, combine that color with fog color using the fog factor.
- Preserve R0 alpha.

The translation cache must distinguish specular-enabled and disabled implicit states. Explicit final-combiner behavior must remain under the application's control.

Suggested regression: two otherwise identical draws differing only in SPECULARENABLE; repeat with fog and verify alpha remains unchanged.

## Shadow textures

Hardware fields: PS_TEXTUREMODES, SHADOWFUNC, texture depth format. Status: **partly**.

The comparison's operand order is:

\[
\text{stored texture depth}\;\text{SHADOWFUNC}\;\text{reference depth}.
\]

Existing xbcompat reverses the host comparison function to compensate for the host sampler's opposite operand ordering; that part matches.

Pinned reference behavior:

- PROJECT2D: reference depth is zero.
- PROJECT3D: reference depth is r/q, clamped to the representable native depth interval.
- D16 fixed depth uses 65535 as maximum.
- D24 fixed depth uses 16777215.
- Float-depth cases are not sufficiently established for a complete implementation oracle.

**Confidence:** the PROJECT2D/PROJECT3D distinction is directly established by the pinned reference, but was not independently corroborated by an Xbox hardware test in this audit.

Current xbcompat exposes shadow comparison through PROJECT2D, always using projected r/q. Its texture-mode adjustment can also collapse the distinction between the two modes for 2D depth resources.

Suggested regression: one depth texture, identical coordinates, both modes, all comparison functions, both fixed depth formats. Include fractional and out-of-range reference values.

## Alpha kill, alpha test, and color key

### Alpha kill — missing

Hardware field: texture ALPHAKILL.

Pinned reference rejects a fragment when a texture-fetch stage's completed alpha equals exactly zero, before general combiners. It applies to stages that actually fetch texture data, not merely any stage producing a result.

For BUMPENVMAP_LUM specifically, the luminance multiplication occurs before this check. The check observes the completed stage result, not the unmodulated fetched alpha.

### Color key — missing

Hardware fields: texture COLORKEYMODE / API COLORKEYOP, COLORKEYCOLOR.

Pinned reference compares the completed texture-stage color against a packed ARGB key after conversion to nearest eight-bit components. For X1R5G5B5 and X8R8G8B8 representations, alpha is ignored.

Matching-key actions:

| Mode | Result |
|---|---|
| Disabled | Preserve fetched result |
| Alpha | Set alpha to zero |
| RGBA | Set all components to zero |
| Kill | Reject the fragment |

Reference ordering is completion of the texture stage (including BUMPENVMAP_LUM modulation), then alpha kill, then key matching/substitution.

**Confidence limitation:** this describes the examined reference's behavior after sampling/filtering. It does not establish exact hardware treatment before filtering, nor all formats lacking RGB or alpha. Preserve this caveat in tests and change notes. Pinned reference does not clamp the luminance product, whereas xbcompat currently does; this difference does not establish which implementation matches Xbox hardware.

Suggested regression: matching and nonmatching point-filtered texels for every action, ignored alpha in X formats, and separate tests documenting alpha-kill/key ordering and luminance modulation.

### Alpha test — partly

Hardware fields: alpha-test enable, alpha function, alpha reference.

The reference compares an eight-bit quantized final alpha against an integer reference after final combination. xbcompat delegates this to legacy GL alpha testing. Common comparisons are present; equality and near-byte-boundary behavior require validation.

## Remaining pixel checklist

| Area | Status | Audit conclusion |
|---|---|---|
| Border color | Matches ordinary constant border | Existing host sampler receives the border color. Texture-supplied borders need separate work. |
| Texture-supplied border | Missing | Reference supports a four-texel border around the logical swizzled image, with coordinate adjustment for physical storage. Linear/cube cases are not established there. |
| Fog | Partly | See vertex spec. Applying a nonlinear fog function after interpolation differs from generating/interpolating the factor at vertices. |
| Point sprites | Partly | Stage 3 receives sprite coordinates. Existing fixed-function path configures this, but programmable combinations need a render regression. |
| W-buffer | Partly | See vertex spec: perspective-correct interpolation of original W, not a universally recoverable projection-specific Z conversion. |
| Clip range | Partly | Select rejection outside the clip interval or clamping to it; explicit fragment depth must observe the same rule. |
| D16/D24 quantization | Partly | Native integer depth truncates downward. Host normalized depth conversion can round differently. |
| F16/F24S8 | Missing complete fidelity | Pinned reference also acknowledges incomplete floating-depth handling. |
| Flat shading | Partly / unverified | A host shade-model state exists; programmable front/back color interpolation needs explicit tests. |
| Two-sided color | Partly / unverified | Do not assume selecting legacy lighting state reproduces programmable front/back color selection. |

No implementation files were modified by this spec-writing agent.
