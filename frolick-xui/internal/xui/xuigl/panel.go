package xuigl

import (
	"path/filepath"

	"emulauncher-rmlui-starter/internal/xui"
)

// Panel is an XUI scene rendered into a panel framebuffer.
type Panel struct {
	Scene *xui.Scene
	Res   *xui.Resources
	be    *Backend
	r     *xui.Renderer
	W, H  int
}

// PanelConfig says where a panel's files live.
type PanelConfig struct {
	Scene string   // .xui or .xur path
	Skin  string   // optional skin scene (.xui/.xur, or pkg.xzp#skin.xur)
	Dirs  []string // extra directories for images (scene dir is added)
	Font  string   // fallback TTF/OTF used for every XUI font name
	W, H  int      // framebuffer size (the panel FBO)
}

// NewPanel loads a scene. Needs a current GL context.
func NewPanel(c PanelConfig) (*Panel, error) {
	res := &xui.Resources{
		Dirs:  append([]string{filepath.Dir(c.Scene)}, c.Dirs...),
		Fonts: map[string]string{"": c.Font},
	}
	root, err := res.Scene(c.Scene)
	if err != nil {
		return nil, err
	}
	var skin *xui.Element
	if c.Skin != "" {
		if skin, err = res.Scene(c.Skin); err != nil {
			return nil, err
		}
	}
	be, err := New()
	if err != nil {
		return nil, err
	}
	p := &Panel{Scene: xui.NewScene(root, skin), Res: res, be: be, W: c.W, H: c.H}
	p.r = xui.NewRenderer(be, res)
	return p, nil
}

// Update advances timelines by dt seconds.
func (p *Panel) Update(dt float32) { p.Scene.Update(float64(dt)) }

// Render draws the scene into the currently bound framebuffer. The caller
// clears it first (Frolick clears the panel FBO to transparent).
func (p *Panel) Render() {
	p.be.Begin(p.W, p.H)
	p.r.Draw(p.Scene, 0, 0, 1)
	p.be.End()
}

// Destroy frees GL resources.
func (p *Panel) Destroy() { p.be.Destroy() }
