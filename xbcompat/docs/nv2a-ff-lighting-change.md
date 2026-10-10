# Fixed-function lighting GLSL migration (verified subset)

This change introduces xbcompat-owned GLSL 1.20 vertex lighting for ordinary transformed geometry using point or directional lights. It enforces the point-light range, evaluates constant/linear/quadratic attenuation, selects camera-relative versus infinite-viewer specular, selects each material term independently from material or primary vertex color, and preserves material powers above the legacy OpenGL shininess limit of 128. Specular remains a separate output for addition after texture stages.

The shader uses xbcompat's existing model-view, projection and texture matrices and preserves the pixel-shader interface. Absent requested vertex color and COLORVERTEX disabled both select material values. State values are uploaded per draw; one shader object is reused.

Tests in `tests/ff_lighting_test.c` render 15 fixtures with hand-calculated expected colors. They cover inside/outside range (not equality), linear and quadratic attenuation, optional normal normalization, ambient/diffuse/emissive/specular primary-color sources, diffuse alpha, power 256, local/infinite viewer and suppression of specular behind the surface. The test does not use a reference shader generator.

The fixed viewer direction retains +Z. Microsoft's Direct3D specular-lighting documentation explicitly defines the infinite-viewer halfway vector by combining +Z with the direction toward the light; the local-viewer case points from the vertex toward the camera. This matches the pre-existing OpenGL infinite-viewer convention. Source: https://learn.microsoft.com/en-us/windows/win32/direct3d9/specular-lighting

Integration deliberately retains the prior path for skinning, spotlights, two-sided lighting, texture generation, fog, W-buffering, point attenuation/sprites and material sources that require a supplied COLOR2 input. It also retains legacy behavior for unlit and pretransformed draws. Secondary-color alpha, exact spotlight hardware behavior, range equality and degenerate inputs remain unverified. This is the first tested migration step, not a claim that the entire NV2A fixed-function pipeline is complete.

Provenance: implementer read xbcompat's existing source, the sanitized texture/FF specification and API documentation; the implementer never opened xemu or Cxbx source.

Integration regression: `renderer_smoke` checks nine HLE draws covering material, primary and absent-secondary selection, point range, constant attenuation, and local-viewer uniform changes without a pixel shader.
