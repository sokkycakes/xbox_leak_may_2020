# NV2A vertex behavior specification

## Scope and evidence

This document describes behavior, equations, register meaning, implementation gaps, and proposed tests. It contains no copied implementation, pseudocode, or implementation-specific identifiers from GPL/LGPL references.

Audit baseline: xbcompat branch `claude/project-thread-zvdmzt`, including the W-buffer change identified in the handoff as `11ceddce`. Reference behavior was examined at xemu `478b4f49`. Additional evidence came from the public-domain vertex CPU reference at `1115255708c10c4841b65dcd2223262e7a316598`, NVIDIA's published vertex-program specifications, XDK header definitions, and hardware-oriented tests at `abaire/nxdk_pgraph_tests` commit `706a41e61188d3b08e5b1bb2a32b74ad062a883d`.

“Matches” means the inspected xbcompat implementation agrees for the specified domain. It does not establish every hardware rounding or exceptional-value detail. “Partly” includes implemented ordinary behavior with missing, conflicting, or unverified edge behavior. No executable tests were run during this specification pass.

## Priority result: paired address updates

**Status: partly. Confirmed actionable gap in the vertex translator.**

The MAC and ILU operations occupying one instruction consume the register values present when that instruction begins. Neither operation's output becomes an input to the other operation in the same instruction.

This rule includes the address register, **A0.x**. An **ARL** paired with an ILU operation must not change the constant address used by the companion ILU. The new address affects subsequent instructions.

The rule applies independently of temporary-register write masks. An ILU operation writing only an output register still consumes the old address.

Relevant hardware fields:

- MAC operation selector: instruction word 1, bits 21–24.
- ILU operation selector: instruction word 1, bits 25–27.
- Constant index: instruction word 1, bits 13–20.
- Relative-address enable: instruction word 3, bit 1.
- A0.x: scalar address register.

In the inspected xbcompat translator, **ARL** updates A0 before emitting the paired ILU calculation. The CPU interpreter already captures operands before publishing the update, so it provides a useful independent regression oracle.

### Behavioral tests

1. Begin an instruction with A0 = 2. Set physical constant slot 98 to `(0.2, 0.3, 0.4, 0.5)` and slot 101 to `(0.7, 0.8, 0.9, 1)`. Pair ARL of an input value 5 with ILU MOV reading constant base 96 relatively. The ILU must obtain slot 98. A subsequent relative read must obtain slot 101.
2. Repeat with the ILU temporary write mask zero and its output write mask nonzero.
3. Repeat with the ARL source itself read through A0-relative constant addressing. For this variant, change slot 98 X to 5 so ARL obtains the new address from slot 98. Both source reads must use the address from instruction entry: the companion obtains `(5, 0.3, 0.4, 0.5)`, and the subsequent relative read obtains slot 101.
4. Retain existing paired temporary-register tests and R12/oPos alias tests.

No W-buffer or position-epilogue changes are needed for this fix.

## Opcode coverage

Operands below mean values after source selection, component swizzle, and source negation. Scalar ILU instructions select the component named by their scalar input swizzle and replicate their scalar result where applicable.

| Opcode | Behavior for ordinary finite inputs | Baseline status |
|---|---|---|
| NOP | No arithmetic result or destination update. | Matches |
| MOV | Copy selected components. | Matches |
| MUL | Componentwise product. A zero operand produces zero even when its counterpart is infinity or NaN in the inspected GPU reference. | Matches this specified zero-product rule |
| ADD | Componentwise sum. | Matches for finite inputs |
| MAD | Componentwise product followed by addition. The zero-product rule applies before addition. | Matches for finite inputs and specified zero product |
| DP3 | Sum the three component products and replicate the scalar. | Matches for finite inputs |
| DPH | Three-component dot product plus the second operand's W. The first operand's W contributes an implicit positive 1, independently of its source W. | Matches for finite inputs |
| DP4 | Four-component dot product, replicated. | Matches for finite inputs |
| DST | X is 1; Y is the product of operand Y values; Z comes from the first operand; W comes from the second. | Matches for finite inputs |
| MIN/MAX | Select componentwise minimum/maximum. | Matches for ordinary finite inputs; exceptional behavior unresolved |
| SLT/SGE | Componentwise comparison yielding 0 or 1. | Matches for ordinary finite inputs; signed-zero and NaN details unresolved |
| ARL | Load scalar A0 using a floor-like conversion; see rounding qualification below. | Partly: paired publication order is wrong |
| RCP | Reciprocal, replicated. | Matches ordinary inputs; host exceptional behavior needs targeted verification |
| RCC | Reciprocal with magnitude restricted to the interval from 2^-64 through 2^64, preserving sign. | Matches ordinary inputs and intended clamp limits |
| RSQ | Reciprocal square root of absolute input, replicated. | Matches ordinary inputs |
| EXP | X is 2^floor(s); Y is s − floor(s); Z is 2^s; W is 1. | Matches ordinary inputs |
| LOG | For positive magnitude a = abs(s): X is floor(log2(a)), Y is a / 2^X, Z is log2(a), W is 1. | Partly: zero and other exceptional cases require resolution |
| LIT | X/W are 1. Y is nonnegative input X. Z is zero when input X is nonpositive; for positive X and positive Y it is Y^p, with exponent clamped to approximately ±127.99609375. | Partly: zero-base and exceptional cases unresolved |

### RCP, RCC, and RSQ boundaries

NVIDIA's **RCC** specification explicitly distinguishes positive and negative zero:

- RCP of positive zero is positive infinity; RCP of negative zero is negative infinity.
- Reciprocal of positive/negative infinity is positive/negative zero before RCC clamping.
- RCC converts those results to the corresponding signed clamp endpoints.
- RCC of exactly 1 is exactly 1.
- Clamp endpoint bit patterns are `0x1F800000` and `0x5F800000`, with the sign bit added for negative values.

Useful tests include both signed zeros, both infinities, ±1, ±2, and values immediately on either side of each clamp boundary. Do not infer NaN behavior solely from a host clamp operation.

For **RSQ**, the inspected reference explicitly expects positive infinity for either signed zero and positive zero for either signed infinity. Negative finite inputs use their absolute magnitude.

### ARL rounding qualification

Both inspected references and xbcompat apply a small positive bias before flooring. The reference describes this as a workaround for host normalization differences, not a measured exact NV2A threshold.

Therefore:

- Retain the existing rounding policy for the immediate paired-address fix.
- Do not document the bias as an exact hardware rule.
- Exact thresholds near integers, large values, infinities, and NaNs need hardware evidence.

### Conflicting arithmetic evidence: do not silently choose one reference

The GPU reference, public-domain CPU reference, and xbcompat disagree on several exceptional cases:

- **LOG of zero:** the GPU reference uses negative infinity in X/Z; xbcompat uses the most-negative finite float. The CPU reference agrees with infinity but explicitly flags its LOG behavior for hardware validation.
- **LOG of infinity:** CPU and GPU expressions differ in the Y component.
- **LIT with zero Y:** references differ for zero or negative exponents. xbcompat contains its own special case.
- **Finite multiply overflow:** the public-domain CPU model contains finite saturation behavior absent from the inspected GPU model and xbcompat.
- **SLT/SGE signed zero:** the public-domain CPU model distinguishes negative zero from positive zero; the GPU model and xbcompat use ordinary host comparisons.
- **MIN/MAX NaNs:** host operations and CPU selection rules are not equivalent for every operand order.
- **Paired writes targeting R1:** the inspected GPU reference suppresses a MAC R1 write more broadly than xbcompat, while the public-domain reference differs again. Do not broaden suppression without a hardware test.

These remain **partly**, with the unresolved domain explicitly recorded. “Match xemu” is insufficient where xemu's own paths disagree.

## Register files and constant addressing

**Status: partly.**

There are 192 physical constant vectors. XDK logical constant numbering uses the range −96 through 95; the corresponding physical slot is the logical number plus 96. In the ordinary 96-constant API mode, application constants 0 through 95 occupy physical slots 96 through 191.

XDK exposes:

- `D3DSCM_96CONSTANTS`
- `D3DSCM_192CONSTANTS`
- `D3DSCM_192CONSTANTSANDFIXEDPIPELINE`
- `D3DSCM_NORESERVEDCONSTANTS`

Viewport scale/offset occupy reserved constants when `D3DSCM_NORESERVEDCONSTANTS` is clear. The inspected xbcompat viewport update writes physical slots 58 and 59, corresponding to logical −38 and −37, and suppresses that automatic update when the no-reserved flag is set.

The translator supports physical constant indices and a local constant copy for programs that write constants. The CPU state-program path writes constants in place.

### Out-of-range addressing

The inspected xbcompat implementation clamps relative constant indices into the physical range 0–191. Neither inspected reference establishes a reliable hardware result for out-of-range reads: one emits an unchecked address, and the CPU reference accesses the resulting index directly.

**Do not claim endpoint clamping is hardware-correct.** Do not replace it with wrapping or zero without evidence.

### Additional tests

- Logical endpoints −96 and 95 map to physical endpoints 0 and 191.
- Application constants 0 and 95 map to physical slots 96 and 191.
- Positive and negative relative offsets that remain within range.
- Viewport update with reserved constants enabled and disabled.
- Constant write followed by read in a later instruction.
- Constant write paired with a read of the same constant: the read sees the old value.
- Constant-mode transitions and interaction with fixed-function state remain to be audited.

## Position output and raster depth

### Screen-space position

**Status: partly.**

NV2A programmable output **oPos** supplies screen-space X/Y/Z together with W. A host rasterizer using homogeneous clip coordinates must preserve that resulting screen position after its own perspective divide. Reconstructing homogeneous coordinates is a host representation issue; it must not introduce a second effective division of X/Y/Z.

R12 aliases oPos. Reads through that alias participate in the same instruction-entry operand rule.

The inspected GPU reference additionally models X/Y raster precision as increments of 1/16 pixel, with conversion toward zero. This is different from flooring for negative coordinates. The inspected xbcompat epilogue does not implement that quantization.

Useful raster tests:

- Positions separated by less than 1/16 pixel.
- Positive and negative fractional screen coordinates.
- Nondefault viewport origin and extent.
- Render targets with inverted host Y.
- R12 read combined with oPos and temporary output writes.

### Zero and negative W

**Status: partly.**

The inspected GPU reference restricts W magnitude to 2^-64 through 2^64, retaining the distinction between positive and negative zero. The current xbcompat epilogue uses the same magnitude range, but its ordinary nonnegative comparison treats negative zero as nonnegative.

This establishes a reference discrepancy, not a complete hardware primitive-clipping specification. Mixed-sign W triangles, negative W, horizon crossing, and NaN W require rendered tests. Do not change them while making an unrelated opcode fix.

### Z-buffer versus W-buffer

**Status: partly. Preserve the existing Splinter Cell fix.**

Relevant controls:

- `NV097_SET_CONTROL0.Z_PERSPECTIVE_ENABLE`
- `NV_PGRAPH_CONTROL_0.Z_PERSPECTIVE_ENABLE`
- `NV097_SET_ZMIN_MAX_CONTROL.ZCLAMP_EN`
- `NV_PGRAPH_ZCLIPMIN`
- `NV_PGRAPH_ZCLIPMAX`
- `NV097_SET_SURFACE_FORMAT.ZETA`

For a triangle, let λᵢ be screen-space barycentric weights and let Zᵢ, Wᵢ be its original vertex depth coordinates.

With ordinary Z depth, the reference behavior is affine screen-space interpolation:

D_Z = Σᵢ λᵢ Zᵢ.

With perspective/W depth enabled, depth is perspective-correct interpolation of original W:

D_W = 1 / (Σᵢ λᵢ / Wᵢ).

This is equivalent to weighting each W by its perspective-correct barycentric weight. It is not ordinary affine interpolation of W.

The reference treats nonpositive or NaN W-depth as maximum finite depth before applying clip-range handling. Degenerate primitive handling and exact hardware precision need separate validation.

The existing xbcompat W-buffer change derives a normalized projection-depth value from W. It is useful for the observed Splinter Cell regression but is not a complete implementation of native W-depth storage and comparisons. Full replacement requires coordinating raster depth generation, depth range, bias, storage format, and texture sampling.

### Clip-range behavior

Depth clip handling occurs after interpolation and applicable depth offsets.

- In cull mode, fragments below the minimum or above the maximum are rejected.
- Values exactly equal to either bound remain eligible.
- In clamp mode, depth is restricted to the inclusive range instead of rejecting the fragment.

This must not be approximated solely by host clipping of reconstructed vertex positions; that can discard a primitive before the intended per-fragment clamp behavior takes place.

### Depth offsets

Both constant offset and slope-dependent offset affect depth before clip handling. In W mode, the reference's slope adjustment depends on squared interpolated W.

Exact W-slope offset behavior is not fully established by the inspected implementation. Keep this explicitly unresolved rather than adopting an approximation as a hardware requirement.

### Integer and floating depth formats

**Integer D16/D24: partly. Floating F16/F24S8: missing as a complete hardware model.**

The inspected reference quantizes integer depth downward in native depth units before host normalized storage. Native integer maxima are 65,535 and 16,777,215.

XDK headers define the usable depth ranges:

| Format | XDK maximum |
|---|---:|
| D16 | 65,535 |
| D24S8 | 16,777,215 |
| F16 | 511.9375 |
| F24S8 | 10^30, intentionally below the representable extreme |

The inspected xbcompat depth-upload/range helper classifies floating formats alongside integer formats and uses integer maxima. This is insufficient to implement their numeric encodings.

The examined xemu pixel path explicitly marks full floating-depth handling incomplete. It cannot serve as a complete oracle for F16/F24S8 encoding, rounding, denormals, comparisons, or color sampling.

### Depth regression tests

- Preserve the Splinter Cell W-buffer scene from `11ceddce`.
- Triangle with constant Z and varying W: W mode must vary independently of Z.
- Triangle with varying Z and constant W: W mode must remain constant.
- Sample a known interior point and compare harmonic W interpolation with affine W; choose values that separate the results substantially.
- Depth immediately below, equal to, and above each clip bound, in both modes.
- D16/D24 values immediately below and above integer boundaries.
- Preserve correct stencil bits when depth changes.
- Floating-format tests must use an independently established encoding oracle.

## Fog output and fog table

**Status: partly.**

Relevant fields include:

- oFog
- `NV097_SET_FOG_MODE`
- `NV097_SET_FOG_GEN_MODE`
- `NV_PGRAPH_CONTROL_3.FOGENABLE`
- `NV_PGRAPH_CONTROL_3.FOG_MODE`
- `NV_PGRAPH_FOGPARAM0`
- `NV_PGRAPH_FOGPARAM1`

For programmable vertices, the inspected reference uses the scalar oFog result as fog distance. For a masked oFog write, the first enabled component in X/Y/Z/W order supplies the scalar value. xbcompat implements this scalar write selection.

For ordinary finite values and conventional XDK parameter settings:

- Linear factor: (end − d) / (end − start).
- Exponential factor: exp(−ρd).
- Squared-exponential factor: exp(−(ρd)^2).

The reference computes fog factor at vertices, preserves its finite out-of-range values through interpolation, and clamps to [0,1] for fragment use. xbcompat currently interpolates the fog coordinate and evaluates these functions in the fragment shader. These differ for nonlinear modes and exceptional inputs.

Hardware ABS fog modes apply the absolute value to the computed factor in the inspected reference. They should not be confused with an absolute-planar fog-coordinate source.

The reference's exceptional-value behavior is mode-dependent:

| Fog mode | Infinite input distance | NaN computed factor |
|---|---:|---:|
| Linear / linear ABS | 1 | 1 |
| Exponential | 1 | 1 |
| Exponential ABS | 0 | 0 |
| Squared exponential / squared exponential ABS | 0 | 0 |

Other infinite calculated factors are restricted to finite float magnitude before interpolation. Hardware-oriented exceptional-fog tests exist, but no hardware outputs were captured during this audit.

`end == start` cannot be specified safely by applying ordinary host division and clamp. The API-to-register parameter calculation and exceptional-value behavior must be treated together.

Tests should cover oFog write masks, disabled fog, nonlinear interpolation, negative distances, ABS modes, equal start/end, infinities, and NaNs.

## Point size and texture-coordinate outputs

### Point size

**Status: partly.**

Relevant controls:

- oPts
- `NV097_SET_POINT_SIZE`
- `NV_PGRAPH_POINTSIZE`
- `NV_PGRAPH_CSV0_D.POINTPARAMSENABLE`

Hardware-oriented tests explicitly distinguish two cases:

- Point parameters disabled: point size comes from the point-size register; shader oPts does not control rasterized size.
- Point parameters enabled: shader oPts controls programmable point size.

The point-size register uses eighth-pixel units. Tests also document that register values above `0x1FF` are ignored rather than simply masked.

xbcompat emits oPts as a host point-size output and separately configures legacy point-size state. The inspected code did not establish the complete enable/selection path required to reproduce these two cases. Audit state wiring before changing shader math.

Test the same oPts values with point parameters both enabled and disabled. Include non-X write masks and register limits.

### Texture coordinates

**Status: matches for ordinary programmable pass-through.**

The four outputs oT0–oT3 reach the corresponding fragment texture-coordinate inputs as four-component values. Projection and dependent-read interpretation belong to the texture stage, not an unconditional vertex-side divide.

Point-sprite replacement is a separate raster/texture-stage behavior and must be coordinated with the pixel specification.

## Front/back colors and flat shading

**Status: partly.**

Relevant outputs are oD0/oD1 and oB0/oB1. Relevant controls include:

- `NV_PGRAPH_CONTROL_3.SHADEMODE`
- `NV_PGRAPH_CSV0_C.SPECULAR_ENABLE`
- `NV_PGRAPH_CSV0_C.ALPHA_FROM_MATERIAL_SPECULAR`
- `NV097_SET_LIGHT_CONTROL`

Ordinary color components are restricted to [0,1] before interpolation. The inspected reference converts NaN color components to 1 before this clamp; xbcompat uses host clamp without explicit NaN conversion.

The reference separately controls whether secondary/specular colors participate and whether their alpha is supplied or forced to 1. This should be coordinated with final-combiner specular behavior rather than fixed solely in the vertex translator.

Flat shading applies to color interpolation. Fog and texture coordinates retain their ordinary interpolation behavior. Provoking-vertex conventions and primitive conversion require rendered tests.

xbcompat falls back from unwritten back-color outputs to corresponding front-color outputs. The examined reference initializes back outputs independently. This difference needs hardware evidence before removing the fallback.

Suggested tests:

- Opposite-facing triangles with distinct front/back primary and secondary colors.
- Unwritten back outputs.
- Flat-shaded triangles, strips, and fans.
- Texture-coordinate gradients across flat-shaded triangles.
- NaN and out-of-range colors.
- Specular enable and specular-alpha control independently varied.

## Implementation order and unresolved work

1. Fix paired ARL address publication and add independent behavioral tests.
2. Preserve W-buffer behavior while adding a regression that captures the current successful Splinter Cell result.
3. Resolve arithmetic exceptional-value conflicts using hardware evidence before changing LOG, LIT, signed-zero comparisons, overflow, or R1 suppression.
4. Add full point-size state selection and investigate fog-table placement.
5. Implement general W/Z raster depth, clip-range behavior, quantization, and floating formats as coordinated changes.
6. Audit constant-mode transitions, two-sided defaults, and provoking vertices.
7. Leave fixed-function vertex-pipeline replacement to its separate specification and implementation pass.

## References

- [NVIDIA NV_vertex_program1_1 specification — DPH and RCC](https://registry.khronos.org/OpenGL/extensions/NV/NV_vertex_program1_1.txt)
- [Public-domain NV2A vertex CPU reference, pinned revision](https://github.com/xemu-project/nv2a_vsh_cpu/tree/1115255708c10c4841b65dcd2223262e7a316598)
- [NV2A hardware-oriented rendering tests, pinned revision](https://github.com/abaire/nxdk_pgraph_tests/tree/706a41e61188d3b08e5b1bb2a32b74ad062a883d)
- [XDK hardware/API definitions in the owner repository](https://github.com/sokkycakes/xbox_leak_may_2020/blob/claude/project-thread-zvdmzt/xbox_leak_may_2020/xbox%20trunk/xbox/public/xdk/inc/d3d8types.h)
- [xemu behavioral reference revision](https://github.com/xemu-project/xemu/tree/478b4f49/hw/xbox/nv2a)

Implementers should work from this behavioral document, the permitted XDK definitions, and xbcompat's own source. The final reference is provenance for the specification writer, not an implementation input.
