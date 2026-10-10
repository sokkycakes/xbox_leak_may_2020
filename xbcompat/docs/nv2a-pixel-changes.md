# NV2A pixel changes

Implementation was written from the sanitized behavior specification, permitted hardware/XDK descriptions, and xbcompat's existing translator. The implementer did not read xemu or Cxbx source. No reference-emulator source is included.

| Change | Behavior | Regression |
| --- | --- | --- |
| Implicit final combiner | `D3DRS_SPECULARENABLE` adds the secondary RGB color to R0, saturates the sum, then applies fog. Alpha remains R0 alpha. Explicit final combiners retain their specified inputs. | `psh_render`: specular off/on, pre-fog saturation, alpha preservation, explicit-final exemption. |
| Constant-eye reflection | `PS_TEXTUREMODES_DOT_RFLCT_SPEC_CONST` uses the XYZ eye-vector state and the same non-unit-normal reflection equation as ordinary reflection. The host supplies the eye vector as a uniform. | `psh_render`: two constant eye vectors select different cube faces; an asymmetric eye and non-unit normal distinguish normalization errors. `psh_test`: this mode must translate and compile. |
| Alpha kill | Enabled sampling stages discard exactly when their completed stage alpha is zero. It runs after bump luminance, before color key; it does not run on PASSTHRU or other non-sampling stages. | `psh_render`: zero versus byte value 1, key-generated zero alpha, PASSTHRU exemption, zero bump luminance. |
| Texture color key | Compare the completed filtered stage result in rounded eight-bit component space; the three operations set alpha to zero, set RGBA to zero, or discard. X1/X8 RGB source formats omit alpha from matching. | `psh_render`: all operations, alpha matching/ignoring, filtering before comparison, ordering with alpha kill. `psh_test`: mixed per-stage operation compilation. |
| X8L8V8U8 bump source interpretation | When a BGRA-uploaded X8L8V8U8 image is a bump source, uploaded B supplies U and uploaded R supplies luminance; G supplies V. Ordinary X8R8G8B8 sampling retains its color interpretation. | `psh_render`: asymmetric L=64/U=127 chooses a displacement-dependent texture color and scales it by L. |
| Projected shadow modes | PROJECT2D uses zero comparison reference; PROJECT3D on a 2D depth texture uses R/Q converted from native depth units, clamped to the normalized range. Ordinary PROJECT3D volume sampling remains separate. | `psh_render`: high and low projected references in D16 and D24, with Q=2. `psh_test`: both shadow variants compile. |

## Integration

Shader variants include texture-control options and SPECULARENABLE in the host cache. The legacy `psh_translate(rs)` entry point remains a wrapper with zero options. The eye vector is sourced from the hardware eye-vector methods; no unverified alias to pixel constant zero is assumed. Detailed host-state changes are documented separately.

## Confidence and limits

The shadow PROJECT2D/PROJECT3D distinction, alpha-kill ordering, and color-key quantization are the pinned reference implementation's modeled behavior, not a new claim of independent hardware measurement. The regression tests check xbcompat's implementation of that specification. They do not establish behavior on physical NV2A hardware.

The existing signed dot-map formulas were retained: the unusual D3D endpoint at -128/127 is documented. No change was made to dependent AR/GB coordinate scaling because the available evidence did not establish a correction for linear destinations. Existing bump-luminance saturation was retained pending stronger evidence.

These isolated tests do not establish that Splinter Cell night or thermal vision is fixed. Game captures and the ATG suite remain necessary integration regressions. Execution results belong in the task's validation record.

The HLE integration test in `renderer_smoke --render` additionally reuses one shader and texture while toggling alpha kill and color-key operations off/on/off, then changes only the key-color uniform. This covers shader-cache identity and per-draw state uploads beyond the isolated translator tests.

## Continuation from 16927d2

Paired programmable vertex/pixel stages now evaluate ordinary finite EXP, EXP2 and LINEAR fog at vertices. A shared per-draw uniform distinguishes interpolated factors from legacy fog distances; the fragment shader clamps the interpolated factor. The renderer regression distinguishes nonlinear vertex-factor interpolation from fragment evaluation and checks masked scalar writes, unclamped linear factors and fog disable. Equal endpoints, exceptional values, ABS modes and mixed/FF stages are not newly specified.

Fixed-function texture transforms no longer normalize linear texture coordinates when a programmable pixel stage already applies tex_scale. Literal packed-color copy tests exposed and now guard the duplicate normalization. This does not change the unresolved rule for dependent reads into linear destinations.
