---
artifact_contract: "ce-handoff/v1"
created_at: "2026-10-19T20:15:11Z"
title: "GameTap game belt render pseudocode and scene data"
summary: "Reconstructed GameTap game-belt scroll, draw pass, and the Beta2 shape and animation data another agent can port."
keywords: ["gametap", "game-belt", "platter", "granny", "beta2scene", "rendering"]
cwd: "/home/sokky/Projects/emulauncher"
resume_focus: "Port the GameTap game belt behavior using this pseudocode and the extracted Beta2 scene data. The destination project was not named."
repository: "emulauncher"
repo_root_sha: "7a897f29849ef2578adfdf82c7b2154383a2c1a1"
branch: "main"
head: "7a897f29849ef2578adfdf82c7b2154383a2c1a1"
worktree_path: "/home/sokky/Projects/emulauncher"
---

# GameTap game belt

The user asked for pseudocode and the 3D data for the GameTap game belt so a different agent can change another project. They did not name that project. This file is the spec. It is reconstructed from `gametap.dll` inside the local installer extract, plus the Beta2 XML and Granny scene pulled out of that installer. It is not a change to the emulauncher tree.

Machine-local sources, all under `/home/sokky/Downloads/gametap`:

- `bin/Release/gametap.dll` — client that plays the clips and draws the shapes. Image base `0x60500000`.
- `local/assets/data/core/Beta2Shapes.xml` — draw order, picking, two-sided, visibility. This is the shape table to follow.
- `local/assets/data/core/Beta2Animations.xml` — clip names, frame ranges, which scene nodes move.
- `local/assets/data/core/vaultConfig.xml` — the five vault rings (categories), bottom to top. Not the scrolling belt.
- `local/assets/vault/scene/beta2scene.gr2` — Granny 2 scene, 1,437,114 bytes. Decoded belt poses, curves, and cover meshes are in `beta2scene-belt.json` next to this file.

The pseudocode below is the decompilation result. This document does not depend on any `/tmp` scratch files.

## What the belt is

`GameBelt` is a widget name. `TGN::VaultForm` at `0x606383b0` looks it up and `dynamic_cast`s it to `TGN::GameSelectorWidget`, stored at `vault+0x124`.

The scrolling covers are 13 scene nodes, `gsb_0shape` through `gsb_12shape`. Clip `beltControl` in `Beta2Animations.xml` drives all 13. `Belt_Left` / `Belt_Right` are frame ranges on that control, not procedural vertex code.

The three platters (`platterLeft*`, `platterCenter*`, `platterRight*`) are the landing discs for My GameTap, the vault, and Mediaplex. `TGN::PlatterWidget` tints those icons. They are not the scrolling belt. An earlier reading treated the platter widget as the belt; the XML separates them.

## Animation data

From `Beta2Animations.xml`. `speed` is the authored clip speed. Frame numbers are Granny timeline frames.

Belt, 13 nodes `gsb_0shape` .. `gsb_12shape`:

| Clip | Frames | Speed |
|---|---|---|
| `Belt_Right` | 1000 → 1020 | 2.5 |
| `Belt_Right_Final` | 1000 → 1020 | 2.5 |
| `Belt_Left` | 1020 → 1000 | 2.5 |
| `Belt_Left_Final` | 1020 → 1000 | 2.5 |

`Belt_*` and `Belt_*_Final` share frame ranges. The client still plays the `Final` name on the last step of a multi-step scroll.

Related, not the belt:

- Subcat scroll: `subcat00` .. `subcat08`, frames 1050–1100, speed 7.
- Platter / icon camera moves: `vaultCameraMoves` (platters, `MGT_*`, `MP_*`, `GV_*`, camera `TGNcam`). Examples: `MyGameTap_Enter` frames 610–620 speed 0.5; `raiseGVIcon` 730–740 speed 0.5; `lowerGVIcon` 740–731 speed 0.5.

## Shape draw attributes

From `Beta2Shapes.xml`. Names are prefix matches. Higher `drawOrder` draws later.

| Match | Pick | Mouseover | Draw order | Extra |
|---|---|---|---|---|
| `gsb*` | yes | no | 280 | the belt covers |
| `subcat*` | yes | no | 295 | |
| `platterCenterSkirtShape` | yes | no | 203 | |
| `platterLeftRingShape`, `platterCenterRingShape`, `platterRightRingShape` | yes | yes | 203 | |
| `platterLeftSurfaceShape`, `platterCenterSurfaceShape`, `platterRightSurfaceShape` | yes | no | 204 | |
| `platterLeftHighRingShape`, `platterCenterHighRingShape`, `platterRightHighRingShape` | yes | no | 205 | `twosided=true` |
| `MGT_shape*`, `MP_shape*`, `GV_shape*` | no | no | 208 | |
| `MGT_text*`, `MP_text*` | no | no | 209 | `noz=true` |
| `GV_text*` | no | no | 209 | `noz=false` |
| `MGT_proxy*`, `GV_proxy*`, `MP_proxy*` | yes | | | `proxy=true`, `visible=false` |

`platterRightHighRingShape` is named inside the mesh drawer at `0x60702a40`, and the compare result is discarded. Visibility comes from shape flags, not from that string. Do not special-case the ring on the name.

## Platter icon tint

`PlatterWidget` init `0x6058a440`. Prefix string at `this+0x7bc`:

- `MGT` → mode 0
- `GV` → mode 1
- `MP` → mode 2

Materials loaded by name, then two colors written at material `+0x28` and `+0x38` as RGBA floats:

| Material | Field | RGBA |
|---|---|---|
| `platterWidget` | `+0x804` | 0.7, 0.7, 0.7, 1 and 0.7, 0.7, 0.7, 1 |
| `platterWidgetFocus` | `+0x7fc` | 1, 1, 1, 1 and 1, 1, 1, 1 |
| `platterWidgetTextFocus` | `+0x814` | 1, 1, 1, 1 and 1, 1, 1, 1 |
| `platterWidgetTransparent` | `+0x80c` | 1, 1, 1, 0.4 and 1, 1, 1, 0.4 |

Unfocused icons are 70% gray. Focus is white. The transparent variant is white at 40% alpha. Game-vault mode also references clips `raiseGVIcon` and `lowerGVIcon`.

## Pseudocode

Addresses are `gametap.dll` VA. `this+0x864` on `GameSelectorWidget` is the in-progress lock.

```
scroll_belt_right(widget, step_count, duration):   # 0x6052b910
    if widget.animating: return
    select_animation_channel(2, 1)                 # 0x60588470
    if duration == 1.0:
        play_sound("gameBeltRightEvent_" + decimal(step_count))
    widget.animating = true
    scaled = step_count * duration
    for i in 0 .. step_count-1:
        clip = "Belt_Right_Final" if i == step_count-1 else "Belt_Right"
        play_scene_clip(clip, rate=1, time=scaled, scene_or_null, 0)
        # scene pointer is vault_scene+0x5c when the global scene pointer is set

scroll_belt_left(widget, step_count, duration):    # 0x6052baf0
    # same structure
    # channel is select_animation_channel(4, 1)
    # sound is "gameBeltLeftEvent_" + decimal(step_count), only when duration == 1.0
    # clips are Belt_Left, and Belt_Left_Final on the last step

# Authored motion those clips start (Beta2Animations.xml):
#   Belt_Right / Belt_Right_Final: frames 1000..1020, speed 2.5, nodes gsb_0shape..gsb_12shape
#   Belt_Left  / Belt_Left_Final:  frames 1020..1000, speed 2.5, same nodes
```

```
draw_scene(scene):                                  # 0x607042c0
    # shape list is a vector of 8-byte entries at scene+0x2c
    for shape in scene.shapes:
        draw_shape(scene, shape, pass_flags=0x0A)   # bits: 0x2 color override, 0x8 extra visibility
    if scene.flags_at_0xc0 & 0x400:
        for shape in scene.shapes:
            draw_shape(scene, shape, pass_flags=0x04)  # line overlay
    # then screen-space quads via 0x606f7b70, split by a per-widget flag at +0x91

draw_shape(scene, shape, pass_flags):               # 0x60702a40
    name = shape.name                                # compare with "platterRightHighRingShape" is dead
    visible = shape.flag_2c or (shape.flag_2f and (pass_flags & 0x8))
    if not visible: return
    if shape.flag_32: glDisable(<cap not recovered>)
    bind_material(shape)
    glLightModeli(GL_LIGHT_MODEL_TWO_SIDE, <value not recovered>)
    glMatrixMode(GL_MODELVIEW)
    glPushMatrix()
    local_xform = shape.matrix_at_0xa8 if use_alternate_matrix else shape.matrix_at_0x68
    glMultMatrixf(local_xform)
    glMultMatrixf(shape.matrix_at_0x28)
    if pass_flags & 0x4:
        # debug lines, one segment per entry, endpoints offset by component * 5
        for segment in shape.line_segments:
            glBegin(GL_LINES)
            glVertex3fv(segment.a)
            glVertex3fv(segment.b)
            glEnd()
    else:
        if pass_flags & 0x2:
            if shape.flag_2f: glColor4f(0, 1, 0, 1)
            else:             glColor4f(1, 0, 0, 1)
        glDrawElements(GL_TRIANGLES, index_count, GL_UNSIGNED_SHORT, indices)
    glPopMatrix()
    glEnable(GL_DEPTH_TEST)
```

`GL_TRIANGLES` is 4, `GL_LINES` is 1, `GL_MODELVIEW` is `0x1700`, `GL_DEPTH_TEST` is `0x0B71`, `GL_LIGHT_MODEL_TWO_SIDE` is `0x0B52`, `GL_UNSIGNED_SHORT` is `0x1403`. The index-type helper at `0x6070ab60` returns the constant 2, and the draw call uses unsigned short in that case.

Sort belt and platter meshes by the `drawOrder` column before drawing if the port does not already have the client's shape list order. The client stores that order on the shape when `Beta2Shapes.xml` is applied; the vector walk above assumes the list is already in that order.

## Decoded belt poses

`/home/sokky/Downloads/gametap/beta2scene-belt.json` is the belt taken out of `beta2scene.gr2`.

Granny curve time is seconds. An XML frame is that time divided by 30, so frames 1000–1020 are times 33.333–34.0. `Belt_Right` walks those frames forward and `Belt_Left` walks them backward, both at speed 2.5. Matrices are column-major, the order `glMultMatrixf` consumes. Quaternions are xyzw.

Each cover is its own model. `worldMatrix` is the model placement times the animated bone local. Use that matrix to draw the cover. There are 21 samples, one per frame from 1000 through 1020, plus the raw degree-2 B-spline knots and controls.

Over that step, `gsb_1shape` through `gsb_10shape` each slide onto the frame-1000 pose of the next cover. `gsb_6shape` starts on the near slot, about `(-776, 121, -3)`, and ends on `gsb_7shape`'s start pose. `gsb_0shape`, `gsb_11shape`, and `gsb_12shape` have no position or orientation keys anywhere in the 50-second scene animation, so they stay on their placement for the whole timeline.

Cover meshes are named `gsb_NshapeShape`, bound to bone `gsb_Nshape`. Each has 42 vertices, positions, normals, UVs, and 180 triangle indices in local space. Multiply those positions by that cover's `worldMatrix`.

## Verification

Cabinet files were inflated and checked against the InstallShield MD5 before write. `Beta2Shapes.xml`, `Beta2Animations.xml`, and `vaultConfig.xml` were read in full. The scroll and draw pseudocode matches the Ghidra decompilation of `0x6052b910`, `0x6052baf0`, `0x607042c0`, and `0x60702a40`. The platter tint constants match `0x6058a440`. The `platterRightHighRingShape` compare is present and unused in that decompilation. `beta2scene-belt.json` parses as JSON: 13 nodes, 21 samples each, 13 meshes of 42 vertices. From frame 1000 to 1020, covers 1 through 10 land on the next cover's start pose.
