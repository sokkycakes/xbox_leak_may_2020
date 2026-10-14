# Fixed-function lighting GLSL migration (verified subset)

This change introduces xbcompat-owned GLSL 1.20 vertex lighting for ordinary transformed geometry using point or directional lights. It enforces the point-light range, evaluates constant/linear/quadratic attenuation, selects camera-relative versus infinite-viewer specular, selects each material term independently from material, primary or secondary vertex color, and preserves material powers above the legacy OpenGL shininess limit of 128. Specular remains a separate output for addition after texture stages.

The shader uses xbcompat's existing model-view, projection and texture matrices and preserves the pixel-shader interface. Absent requested vertex color and COLORVERTEX disabled both select material values. State values are uploaded per draw; one shader object is reused.

Tests in `tests/ff_lighting_test.c` render 22 fixtures with hand-calculated expected colors. They cover inside/outside range (not equality), linear and quadratic attenuation, optional normal normalization, ambient/diffuse/emissive/specular primary-color sources, diffuse alpha, power 256, local/infinite viewer and suppression of specular behind the surface. The test does not use a reference shader generator.

The fixed viewer direction retains +Z. Microsoft's Direct3D specular-lighting documentation explicitly defines the infinite-viewer halfway vector by combining +Z with the direction toward the light; the local-viewer case points from the vertex toward the camera. This matches the pre-existing OpenGL infinite-viewer convention. Source: https://learn.microsoft.com/en-us/windows/win32/direct3d9/specular-lighting

The continuation from 16927d2 adds all six XDK vertex-blend modes (two/three/four matrices, generated/explicit final weight). Positions and inverse-transpose normals are blended before optional normal normalization; explicit weights are not renormalized. Five additional direct fixtures cover nonsumming weights, a negative generated weight, three/four transforms and normalization after blending. HLE fixtures exercise every mode through both FVF and vertex declarations, including distinct translations and nonuniform normal transforms.

A dedicated four-component secondary-color attribute preserves COLOR2 alpha. Two direct fixtures cover secondary diffuse alpha and secondary emissive RGB. HLE fixtures verify RGBA through both FVF and declaration inputs and toggle COLORVERTEX off/on without replacing the cached program.

Normal, camera-space position and object-position API texgen modes now run in the owned shader, before texture matrices, preserving input W. HLE fixtures cover these modes, matrix ordering and normalization on an unlit texgen draw. Unlit skinning and these texgen modes also select the owned path.

Integration retains the prior path for spotlights, fixed-function two-sided lighting, reflection/sphere texgen, fog, W-buffering and point attenuation/sprites. Ordinary unlit draws without blending/texgen/active texture transforms retain their existing path. Active texture transforms also select the owned shader for pretransformed draws, treating their colors as unlit. Explicit COUNT1-4 output selection preserves the requested projected divisor and forces nonprojected output W to one; it avoids an unwanted division by a four-component input's W. The renderer fixtures cover both fragment paths and varying projection divisors. Independent per-component raw texgen planes and viewer modes are not added. Exact spotlight behavior, range equality, singular transforms and degenerate inputs remain unverified. This is a tested subset, not a complete NV2A fixed-function pipeline.

Provenance: implementer read xbcompat's existing source, the sanitized texture/FF specification and API documentation; the implementer never opened xemu or Cxbx source.

Integration regression: `renderer_smoke` checks nine HLE draws covering material, primary and absent-secondary selection, point range, constant attenuation, and local-viewer uniform changes without a pixel shader.
