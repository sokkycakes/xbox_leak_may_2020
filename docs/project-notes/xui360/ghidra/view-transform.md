# Blades dash view transform (dash.xex 2.0.1888)

Ghidra 11.4.2, headless. dash.img (the decompressed XEX image) loaded as raw binary at 0x92000000,
language PowerPC:BE:64:A2ALT-32addr. Decompiled with scripts/Decomp.java.

FUN_920a41e0(ctx, dc), where ctx+0x48 is the back buffer width and ctx+0x4c is its height:

    if (width == 960)    // 4:3
        M = Scaling(960/1120, 720/770, 1)
    else
        M = Scaling(W/1280 * 1280/1120, (H/720) * 12/11, 1); M.ty = (H/720) * -64
    SetViewTransform(dc, M)          // 0x920fd8c0 = matrix scaling, 0x920fbaa0 = set transform

Constants are at 0x92000c58..0x92000c74: 0.857143, 0.935065, -64, 1.090909, 1.142857, 1/1280, 1/720, 1.
At 1280x720 this works out to x scale 1.142857, y scale 1.090909, no x shift, and a 64 px shift up.
