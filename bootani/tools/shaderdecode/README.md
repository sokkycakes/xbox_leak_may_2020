# shaderdecode: Xbox NV2A shader decoder for `ani2/shaders.h`

`xshaderdecode.py` (Python 3, no dependencies) reads the `const BYTE g_*[]` arrays in
`xbox trunk/xbox/private/ntos/ani2/shaders.h`. It disassembles them into xvs/xps-style asm
and can also translate them to GLSL 330 core.

```
H="…/xbox trunk/xbox/private/ntos/ani2/shaders.h"
python3 xshaderdecode.py "$H"                          # list arrays and sizes
python3 xshaderdecode.py "$H" slash_interior           # disassemble g_slash_interior_xvu + _xpu
python3 xshaderdecode.py "$H" g_vblob_xvu --glsl       # disassembly followed by generated GLSL
python3 xshaderdecode.py "$H" --all                    # everything
```

Names can be given in full (`g_vblob_xpu`), without the prefix and suffix (`vblob`), or as any substring.

## Files

| file | content |
|---|---|
| `xshaderdecode.py` | the decoder, disassembler and GLSL generator |
| `slash_interior.vsh.txt` | disassembly of `g_slash_interior_xvu`, plus a de-optimised reconstruction |
| `slash_interior.psh.txt` | disassembly of `g_slash_interior_xpu` |
| `slash_interior.glsl.txt` | hand-cleaned GLSL 330 vertex and fragment shaders, split at the `=====` markers |

## Formats

### `*_xvu`: Xbox vertex shader microcode
The format is documented in `xgraphics/shadeasm/api.cpp` (`XGSUCode_*`, `ConvertMicrocodeToVsInstructions`).

```
WORD  magic   0x2078 ordinary | 0x7778 read/write | 0x7378 state shader
WORD  count   number of instructions
count × { DWORD 0 (pad), DWORD Y, DWORD Z, DWORD W }        little endian
```
Y, Z and W are the NV2A ("kelvin") program words. The bit fields come from `PGM_UWORD{Y,Z,W}`
in `xgraphics/shadeasm/microcodeformat.h`:

* **Y**: a swizzle (`aws/azs/ays/axs` in bits 0–7), `ane` (bit 8), `va` input reg (9–12), `ca` constant (13–20, biased by 96, so c0 = 96), `mac` op (21–24), `ilu` op (25–27)
* **Z**: `crr` bits 2–3 (0–1), c swizzle and negate (2–10), `bmx/brr` (11–16), b swizzle and negate (17–25), `amx` (26–27), `arr` (28–31)
* **W**: `eos` (0), `cin` a0-relative constant (1), `om` output mux MAC/ILU (2), `oc` output address (3–11; bit 8 set = output register, otherwise constant write), `owm` (12–15), `swm` ILU temp mask (16–19), `rw` (20–23), `rwm` (24–27), `cmx` (28–29), `crr` bits 0–1 (30–31)

Mux codes: 1 = temp r, 2 = input v, 3 = constant c. Write masks use x=8, y=4, z=2, w=1.

The MAC unit handles nop, mov, mul, add, mad, dp3, dph, dp4, dst, min, max, slt, sge and arl. It reads operands a, b and c, and `add` uses a and c. The ILU unit handles mov, rcp, rcc, rsq, expp, logp and lit. It reads only operand c, and scalar ops use c's x swizzle. When one instruction issues both a MAC and an ILU op, the ILU temp write always goes to r1. The disassembly rules follow `InstructionDisassembler` in `api.cpp`. Output registers are numbered oPos=0, oD0=3, oD1=4, oFog=5, oPts=6, oB0=7, oB1=8, oT0..3=9..12. A read of r12 returns oPos.

xsasm appends a screen-space postfix to the compiled shader (`kStandardPostfix` in api.cpp):
`mul oPos.xyz, r12, c-38 + rcc r1.x, r12.w` / `mad oPos.xyz, r12, r1.x, c-37`. Here c-38 and c-37
are the viewport scale and offset. The optimiser can move the first instruction earlier in the
program. The disassembly marks these instructions `[vp]` and the GLSL generator drops them, so
`gl_Position` is the clip-space value.

### `*_xpu`: `D3DPIXELSHADERDEF_FILE`
This is `DWORD FileID = 0x30425350 ("PSB0")` followed by `D3DPIXELSHADERDEF` (60 DWORDs, see
`public/xdk/inc/d3d8types.h`):
PSAlphaInputs[8], PSFinalCombinerInputsABCD, PSFinalCombinerInputsEFG, PSConstant0[8],
PSConstant1[8], PSAlphaOutputs[8], PSRGBInputs[8], PSCompareMode, PSFinalCombinerConstant0/1,
PSRGBOutputs[8], PSCombinerCount, PSTextureModes, PSDotMapping, PSInputTexture, PSC0Mapping,
PSC1Mapping, PSFinalCombinerConstants.

* Each input byte holds a register (bits 0–3: zero, c0, c1, fog, v0, v1, t0–t3, r0, r1, v1r0sum, EFprod), a channel bit (bit 4: rgb/blue or alpha) and a mapping (bits 5–7: unsigned, 1-x, bx2, -bx2, bias, -bias, signed, -signed). The disassembler prints these as xps source modifiers (`_sat`, `1-`, `_bx2`, `_bias`, `-`).
* Each output word holds the CD, AB and SUM destination registers, the dot flags for CD and AB, a mux flag, the output mapping (bias, x2, bx2, x4, d2) and blue-to-alpha flags for AB and CD.
* The disassembler reconstructs xps-like instructions: mov, mul, add, mad, lrp, dp3, `xmma`/`xmmc`, `xfc`. It merges the RGB and alpha halves of a stage when they match, and otherwise prints the alpha half as a `+` co-issued instruction.
* Constants: stage-local C0 and C1 map to D3D constant numbers through the nibbles of PSC0Mapping and PSC1Mapping (0xF = unused). The value embedded in the def is replaced whenever the app calls `SetPixelShaderConstant`. In the generated GLSL a mapped constant becomes `pc[n]`, indexed by the D3D constant number, and an unmapped one becomes a literal taken from the def.
* When ABCD = EFG = 0, `D3DDevice_SetPixelShader` does not load the final combiner, so the fixed-function one stays active (`d3d8/se/lazy.cpp`). With fog and specular off, that is `D = r0.rgb, G = r0.a`.

## GLSL generator semantics
* **Vertex shader**: the MAC and ILU ops of one instruction read their operands before either writes, and writes follow the masks. rcc, expp, logp and lit have helper functions. Unwritten outputs default to `T = (0,0,0,1)` and `D = 0`.
* **Fragment shader**:
  * Each combiner stage reads the register values from before the stage.
  * Inputs go through the input mappings above.
  * AB and CD are products or 3-component dots. SUM is AB+CD, or a mux on r0.a (MSB).
  * The output mapping is applied, then values are clamped to [-1,1].
  * Blue-to-alpha is supported. r0.a starts as t0.a.
  * The final combiner computes `A*B + (1-A)*C + D` and G, with inputs clamped to [0,1]. It supports EF_PROD, V1R0_SUM, and the CLAMP_SUM and COMPLEMENT flags.
  * Texture modes PROJECT2D/3D and CUBEMAP become a 2D projective `texture()` call. PASSTHRU is supported. Dependent and dot-product texture modes are not emulated and are flagged with a comment.

Every array in `shaders.h` produces GLSL that passes `glslangValidator`.

## Calibration against the known sources (`ani2/shaders/*.vsh|*.psh`)
* **greenfog**: the VS matches instruction for instruction, with the source's r0/r1/r2 renamed to r2/r3/r4. The PS matches all six combiner ops exactly.
* **scene_zr**: the VS matches after register renaming and pairing, for example `mov r3, r2.z + mov oPos, r2` and `sge r6.w ... + rcp r1, r4.w`. The optimiser dropped redundant components. The PS matches exactly: `texcoord t0; dp3 r1, t0_bx2, t0_bx2; mul r1, 1-r1, 1-r1; mul r0, v0, r1; xfc zero,zero,zero,r0,zero,zero,1-zero` (the decoder prints G as `1`).
* **vblob**: the VS matches with reordering, renaming and MAC/ILU pairing, for example `mov r10.w, c8.y + rsq r1.x, r2.x`. The PS matches exactly, including `mov r1, r0_sat`, `mul r0.rgb, 1-r0, c0` and `mov r0.a, c0` in its own stage, with C0 mapped to D3D c0/c1 per stage.
