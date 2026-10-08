# frolick-xui

An XUI (Xbox 360 UI) scene runtime for Frolick's `rmlui-go-starter`, plus a
360-style library panel authored in XUI. Files are laid out relative to
`rmlui-go-starter/`; copy them over the project root.

## What's here

| Path | What it is |
| --- | --- |
| `internal/xui` | Pure-Go XUI runtime: `.xzp` packages, binary `.xur` (XUIB v5) and XML `.xui` scenes, skin visuals, timelines/named frames, focus navigation, hit testing, and a renderer that emits triangles to a `Backend`. Includes `Soft`, a software backend for tests. |
| `internal/xui/xuigl` | GLES2 backend + `Panel`, which draws a scene into the bound framebuffer (the panel FBO). |
| `internal/gles2/gles2_xui.go` | GL calls the backend needs (`Scissor`, `BlendFuncSeparate`, `ONE`, `SCISSOR_TEST`). `gles2.Ptr` also needs a `[]uint16` case (see below). |
| `assets/xui/library.xui` | The library panel: 586×402 card at 219,55 in the 1024×512 panel FBO, three focusable tiles (`tileSpotlight`, `tileExplore`, `tileDownloads`), `Intro` timeline. |
| `assets/xui/frolick_skin.xui` | Skin with the `FrolickTile` visual: Normal/Focus/Press states, green focus glow, Blades-style shine sweep. |
| `tools/gen_library.py` | Regenerates both `.xui` files (keeps the rounded Bezier shapes exact). Hand edits to the XML work too. |
| `cmd/xuidump` | Prints a scene tree (`.xur`, `.xui`, or every scene in a `.xzp`). |
| `cmd/xuirender` | Renders a scene to PNG with the software backend. |
| `cmd/xuiglshot` | Same through GLES2 on headless EGL (`-tags xuiglshot`, needs EGL headers). |

## Wiring into the app

```go
// after the GL context exists (main thread, runtime.LockOSThread)
lib, err := xuigl.NewPanel(xuigl.PanelConfig{
    Scene: "assets/xui/library.xui",
    Skin:  "assets/xui/frolick_skin.xui",
    Font:  "assets/ui/fonts/DejaVuSans.ttf", // used for every XUI font name
    W: uiFbo.W, H: uiFbo.H,                  // 1024×512
})

// each frame, for the library pad
uiFbo.Bind()
gles2.ClearColor(0, 0, 0, 0); gles2.Clear(gles2.COLOR_BUFFER_BIT)
lib.Update(dt)
lib.Render()
uiFbo.Unbind()

// input on the library pad
lib.Scene.Navigate("up" | "down" | "left" | "right") // false at an edge
lib.Scene.Press()                                     // OnPress fires
lib.Scene.SetFocus(lib.Scene.ControlAt(x, y))         // mouse, FBO pixels
lib.Scene.Root.Play("Intro")                          // when the pad opens
lib.Scene.Find("dlPct").SetText("70%")                // live data
```

`gles2.Ptr` must handle index buffers:

```go
case []uint16:
    if len(v) == 0 { return nil }
    return unsafe.Pointer(&v[0])
```

Images referenced by the scene (`glyph_a.png`, `glyph_x.png`, `glyph_y.png`,
`cover.png`) are looked up next to the scene file. Missing images just don't
draw.

## Using 360 dashboard files

`xui.Resources` can also load the extracted Blades packages, e.g. a skin of
`dash_dashuisk.xzp#skin.xur` with the `.xzp` files listed in
`Resources.Packages`. Those files are Microsoft's: keep them out of the repo
and point at them on disk.

## Notes

- Class property tables come from the GFWL XUI class dump, patched for the
  360's `XuiControl` (Text is slot 8, ImagePath slot 10).
- Textures are premultiplied alpha; the panel FBO ends up premultiplied too.
- Timelines run at 30 frames per second.
- BlendMode 2/3 map to multiply/add; other modes draw as normal. Anchors,
  ClipChildren, figures (solid/linear/radial/texture fills, strokes), images
  (SizeMode stretch/fit/centre) and text (wrap, align, drop shadow) are
  supported. Lists, edit boxes, sounds and video are not yet.
