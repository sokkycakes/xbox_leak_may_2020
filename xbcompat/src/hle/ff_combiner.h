/*
 * Fixed-function texture stages as NV2A register-combiner state.
 *
 * Without a pixel shader the Xbox D3D library itself turns the texture
 * stage states into a combiner program (LazySetCombiners in the leak's
 * d3d8/se/combiner.cpp) and picks each texture unit's shader mode
 * (LazySetShaderStageProgram in lazy.cpp).  ff_combiner_state follows
 * those rules and writes the same words into the D3DRS_PS* slots of a
 * render-state array, in the PS_* layout psh_translate reads, so a stage
 * setup GL's texture environment cannot express (bump env mapping: a
 * dependent read of the next stage's texture) runs as a fragment program.
 *
 *   - D3D stage s is combiner stage s - first, where first is 3 with point
 *     sprites (they always use stage 3) and 0 otherwise.  The chain ends at
 *     the first COLOROP DISABLE; a first stage that is disabled outputs the
 *     diffuse color and alpha.
 *   - D3DTA_DIFFUSE, CURRENT (diffuse in the first stage), TEXTURE,
 *     TFACTOR, SPECULAR and TEMP read V0, R0, Ts, C1, V1 and R1.  TFACTOR
 *     is combiner factor 1, the same in all stages: the caller loads C1.
 *   - An op naming the texture of a stage that has none selects CURRENT.
 *   - The result goes to R0, or to R1 with D3DTSS_RESULTARG = D3DTA_TEMP.
 *   - BUMPENVMAP(LUMINANCE) passes CURRENT on, and makes the next unit's
 *     texture mode BUMPENVMAP(_LUM) reading this stage's texture.  The
 *     hardware takes that lookup's matrix from the next unit, which the
 *     library loads from this D3D stage's BUMPENV states.
 *   - Both final combiner words stay zero: the library's
 *     LazySetSpecFogCombiner setup, which psh_translate synthesizes from
 *     D3DRS_SPECULARENABLE and the fog states.
 *
 * Only behavior is taken from the library source; this is xbcompat code.
 */
#ifndef XBCOMPAT_FF_COMBINER_H
#define XBCOMPAT_FF_COMBINER_H
#include <stdint.h>
#include <string.h>

/* Texture kinds of the stages, for the shader stage program. */
enum { FFC_TEX_NONE = 0, FFC_TEX_2D, FFC_TEX_3D, FFC_TEX_CUBE };

/* Combiner inputs: the byte of the input word each one occupies. */
enum { FFC_A = 3, FFC_B = 2, FFC_C = 1, FFC_D = 0 };
/* Input modifiers (as D3DTA_COMPLEMENT / D3DTA_ALPHAREPLICATE) and the
   expand-normal map the dot product uses. */
#define FFC_COMPLEMENT  0x10u
#define FFC_ALPHAREP    0x20u
#define FFC_EXPAND      0x40u
#define FFC_MISSING     0xFFFFFFFFu   /* a texture argument of a stage without one */

/* The constant 1 (or -1) as input `input`: zero mapped through
   UNSIGNED_INVERT (or EXPAND_NORMAL). */
static uint32_t ffc_one(int input, int negative)
{
    return (negative ? 0x40u : 0x20u) << (8 * input);
}

/* Input `input` reading the D3DTA_* argument `arg` in D3D stage `stage`.
   `alpha` is set for the alpha half, which always reads alpha. */
static uint32_t ffc_input(int input, uint32_t flags, uint32_t arg, int stage, int first, int alpha,
                          unsigned textures)
{
    uint32_t icw;
    switch (arg & 0xF) {
    case 2: /* TEXTURE */
        if (!((textures >> stage) & 1)) return FFC_MISSING << (8 * input);
        icw = 8 + stage;
        break;
    case 3: icw = 0x1; break;   /* TFACTOR: C1 */
    case 4: icw = 0x5; break;   /* SPECULAR: V1 */
    case 5: icw = 0xD; break;   /* TEMP: R1 */
    case 1: icw = first ? 0x4 : 0xC; break;   /* CURRENT: R0, the diffuse in the first stage */
    default: icw = 0x4; break;  /* DIFFUSE: V0 */
    }
    if ((flags | arg | (alpha ? FFC_ALPHAREP : 0)) & FFC_ALPHAREP) icw |= 0x10;
    if ((flags ^ arg) & FFC_COMPLEMENT) icw |= 0x20;   /* UNSIGNED_INVERT */
    icw |= flags & FFC_EXPAND;                         /* EXPAND_NORMAL */
    return icw << (8 * input);
}

/* One half (RGB or alpha) of combiner stage for D3D stage `stage`.
   Returns false when the op is the DOTPRODUCT3, which takes the whole
   stage (its alpha half is then left off and the RGB result's blue goes
   to alpha). */
static int ffc_half(const uint32_t *tss, int stage, int first, int alpha, unsigned textures,
                    uint32_t *icw_out, uint32_t *ocw_out)
{
    const uint32_t *st = tss + stage * 32;
    uint32_t op = st[alpha ? 16 : 12], arg0 = st[alpha ? 17 : 13], arg1 = st[alpha ? 18 : 14],
             arg2 = st[alpha ? 19 : 15];
    uint32_t ocw = st[20] == 5 /* D3DTA_TEMP */ ? 0xD00u : 0xC00u, icw;
#define IN(input, flags, arg) ffc_input(input, flags, arg, stage, first, alpha, textures)
    int whole = 0;
    switch (op) {
    case 1: /* DISABLE */
        if (first) icw = (0x4u << 24) | ffc_one(FFC_B, 0) | (alpha ? 0x10000000u : 0);
        else icw = ocw = 0;   /* the colour op runs, its alpha half does not */
        break;
    case 2: icw = IN(FFC_A, 0, arg1) | ffc_one(FFC_B, 0); break;              /* SELECTARG1 */
    case 3: icw = ffc_one(FFC_C, 0) | IN(FFC_D, 0, arg2); break;              /* SELECTARG2 */
    case 6: ocw |= 4u << 15; /* fall through */                              /* MODULATE4X */
    case 4: case 5:                                                          /* MODULATE(2X) */
        if (op == 5) ocw |= 2u << 15;
        icw = IN(FFC_A, 0, arg1) | IN(FFC_B, 0, arg2);
        break;
    case 9: ocw |= 3u << 15; /* fall through */                              /* ADDSIGNED2X */
    case 8: ocw |= 1u << 15; /* fall through */                              /* ADDSIGNED */
    case 7:                                                                  /* ADD */
        icw = IN(FFC_A, 0, arg1) | ffc_one(FFC_B, 0) | ffc_one(FFC_C, 0) | IN(FFC_D, 0, arg2);
        break;
    case 10:                                                                 /* SUBTRACT */
        icw = IN(FFC_A, 0, arg1) | ffc_one(FFC_B, 0) | ffc_one(FFC_C, 1) | IN(FFC_D, 0, arg2);
        break;
    case 11:                                                                 /* ADDSMOOTH */
        icw = IN(FFC_A, 0, arg1) | ffc_one(FFC_B, 0) | IN(FFC_C, FFC_COMPLEMENT, arg1) | IN(FFC_D, 0, arg2);
        break;
    case 12: case 13: case 14: case 15: {                                    /* BLEND*ALPHA */
        uint32_t factor = op - 12;   /* DIFFUSE, CURRENT, TEXTURE, TFACTOR */
        icw = IN(FFC_A, 0, arg1) | IN(FFC_B, FFC_ALPHAREP, factor)
            | IN(FFC_C, FFC_COMPLEMENT | FFC_ALPHAREP, factor) | IN(FFC_D, 0, arg2);
        break;
    }
    case 16:                                                                 /* BLENDTEXTUREALPHAPM */
        icw = IN(FFC_A, 0, arg1) | ffc_one(FFC_B, 0) | IN(FFC_C, FFC_COMPLEMENT | FFC_ALPHAREP, 2)
            | IN(FFC_D, 0, arg2);
        break;
    case 17:                                                                 /* PREMODULATE */
        icw = IN(FFC_A, 0, arg1) | (first ? IN(FFC_B, 0, 2) : ffc_one(FFC_B, 0));
        break;
    case 18: case 20:                                                        /* MODULATE(INV)ALPHA_ADDCOLOR */
        icw = IN(FFC_A, 0, arg1) | ffc_one(FFC_B, 0)
            | IN(FFC_C, FFC_ALPHAREP | (op == 20 ? FFC_COMPLEMENT : 0), arg1) | IN(FFC_D, 0, arg2);
        break;
    case 19: case 21:                                                        /* MODULATE(INV)COLOR_ADDALPHA */
        icw = IN(FFC_A, op == 21 ? FFC_COMPLEMENT : 0, arg1) | IN(FFC_B, 0, arg2)
            | IN(FFC_C, FFC_ALPHAREP, arg1) | ffc_one(FFC_D, 0);
        break;
    case 22:                                                                 /* DOTPRODUCT3 */
        icw = IN(FFC_A, FFC_EXPAND, arg1) | IN(FFC_B, FFC_EXPAND, arg2);
        if (!alpha) {
            /* The product goes to the AB destination with dot and
               blue-to-alpha on; the alpha half stays off. */
            ocw = (ocw >> 4) | (1u << 13) | (1u << 19);
            whole = 1;
        }
        break;
    case 23:                                                                 /* MULTIPLYADD */
        icw = IN(FFC_A, 0, arg0) | ffc_one(FFC_B, 0) | IN(FFC_C, 0, arg1) | IN(FFC_D, 0, arg2);
        break;
    case 24:                                                                 /* LERP */
        icw = IN(FFC_A, 0, arg0) | IN(FFC_B, 0, arg1) | IN(FFC_C, FFC_COMPLEMENT, arg0) | IN(FFC_D, 0, arg2);
        break;
    case 25: case 26:                                                        /* BUMPENVMAP(LUMINANCE) */
        icw = IN(FFC_A, 0, 1 /* CURRENT */) | ffc_one(FFC_B, 0);
        break;
    default:   /* not a valid op: select the current color */
        icw = FFC_MISSING;
        break;
    }
#undef IN
    if ((icw & 0xFF000000u) == 0xFF000000u)
        /* An argument names a missing texture: select current. */
        icw = ((first ? 0x4u : 0xCu) << 24) | ffc_one(FFC_B, 0) | (alpha ? 0x10000000u : 0);
    *icw_out = icw;
    *ocw_out = ocw;
    return !whole;
}

/* Fill rs[0..56] (the D3DRS_PS* slots of the 4400 numbering) and rs[117]
   (D3DRS_PSTEXTUREMODES) for the fixed-function stages in `tss`
   (D3D__TextureState[4][32]).  `kind[s]` is FFC_TEX_* for stage s's
   texture; `point_sprites` is D3DRS_POINTSPRITEENABLE. */
static void ff_combiner_state(uint32_t *rs, const uint32_t *tss, const int kind[4], int point_sprites)
{
    unsigned textures = 0;
    for (int s = 0; s < 4; s++) if (kind[s] != FFC_TEX_NONE) textures |= 1u << s;
    memset(rs, 0, 57 * sizeof(*rs));
    int first = point_sprites ? 3 : 0, stage = first, n = 0;
    do {
        uint32_t icw, ocw;
        if (tss[stage * 32 + 12] == 1 /* DISABLE */) {
            /* Only reached for the first stage: diffuse colour and alpha. */
            ffc_half(tss, stage, 1, 0, textures, &icw, &ocw);
            rs[34 + n] = icw;
            rs[45 + n] = ocw;
            rs[0 + n] = icw | 0x10000000u;
            rs[26 + n] = ocw;
            n++;
            break;
        }
        int alpha_too = ffc_half(tss, stage, stage == first, 0, textures, &icw, &ocw);
        rs[34 + n] = icw;   /* PSRGBInputs */
        rs[45 + n] = ocw;   /* PSRGBOutputs */
        if (alpha_too) {
            ffc_half(tss, stage, stage == first, 1, textures, &icw, &ocw);
            rs[0 + n] = icw;    /* PSAlphaInputs */
            rs[26 + n] = ocw;   /* PSAlphaOutputs */
        }
        n++;
        stage++;
    } while (stage < 4 && tss[stage * 32 + 12] != 1);
    rs[53] = n;   /* PSCombinerCount: mux on the LSB, one factor pair for all stages */

    /* Each unit's shader mode; a bump op also selects the stage's texture
       as the next unit's input (stage s reads s - 1). */
    uint32_t modes = 0;
    for (int s = 3; s >= 0; s--) {
        uint32_t mode;
        uint32_t prev = s ? tss[(s - 1) * 32 + 12] : 0;
        if (kind[s] == FFC_TEX_NONE) mode = 0x00;                   /* NONE */
        else if (prev == 25) mode = 0x06;                           /* BUMPENVMAP */
        else if (prev == 26) mode = 0x07;                           /* BUMPENVMAP_LUM */
        else if (kind[s] == FFC_TEX_CUBE) mode = 0x03;              /* CUBEMAP */
        else if (kind[s] == FFC_TEX_3D) mode = 0x02;                /* PROJECT3D */
        else mode = 0x01;                                           /* PROJECT2D */
        modes = (modes << 5) | mode;
    }
    rs[117] = modes;
    rs[56] = (1u << 16) | (2u << 20);   /* PSInputTexture: stage 2 reads 1, stage 3 reads 2 */
}

/* Whether the fixed-function stages in `tss` use bump env mapping, which
   GL's texture environment cannot express. */
static int ff_combiner_needs_program(const uint32_t *tss, int point_sprites)
{
    for (int s = point_sprites ? 3 : 0; s < 3; s++) {
        uint32_t op = tss[s * 32 + 12];
        if (op == 1) break;
        if (op == 25 || op == 26) return 1;
    }
    return 0;
}
#endif
