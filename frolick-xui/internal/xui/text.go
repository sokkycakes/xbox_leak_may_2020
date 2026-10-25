package xui

import (
	"image"
	"image/color"
	"math"
	"os"
	"strconv"
	"strings"

	"golang.org/x/image/font"
	"golang.org/x/image/font/opentype"
	"golang.org/x/image/math/fixed"
)

// Text style bits (XuiText TextStyle).
const (
	styleOneLine = 0x10
	styleRight   = 0x200
	styleCenterH = 0x400
	styleCenterV = 0x1000
	styleShadow  = 0x4000
)

type fontCache struct {
	res   *Resources
	fonts map[string]*opentype.Font
	faces map[string]font.Face
}

func newFontCache(res *Resources) *fontCache {
	return &fontCache{res: res, fonts: map[string]*opentype.Font{}, faces: map[string]font.Face{}}
}

func (fc *fontCache) face(name string, size float32) font.Face {
	key := strings.ToLower(name) + "@" + ftoa(size)
	if f, ok := fc.faces[key]; ok {
		return f
	}
	path := fc.res.Fonts[strings.ToLower(name)]
	if path == "" {
		path = fc.res.Fonts[""]
	}
	f, ok := fc.fonts[path]
	if !ok {
		if b, err := os.ReadFile(path); err == nil {
			f, _ = opentype.Parse(b)
		}
		fc.fonts[path] = f
	}
	var face font.Face
	if f != nil {
		face, _ = opentype.NewFace(f, &opentype.FaceOptions{Size: float64(size), DPI: 72, Hinting: font.HintingFull})
	}
	fc.faces[key] = face
	return face
}

func (r *Renderer) text(n *Node, m affine, alpha float32, blend Blend) {
	txt := propS(n.Props, "Text")
	if n.owner != nil && n.Class == "XuiTextPresenter" {
		txt = propS(n.owner.Props, "Text")
	}
	txt = strings.ReplaceAll(txt, "\r", "")
	if txt == "" {
		return
	}
	size := propF(n.Props, "PointSize", 14)
	style := propU(n.Props, "TextStyle", 0)
	// Draw glyphs at the on-screen scale so text stays sharp.
	scale := float32(math.Sqrt(float64(abs32(m.a*m.d - m.b*m.c))))
	if scale <= 0 {
		return
	}
	face := r.fonts.face(propS(n.Props, "Font"), size*scale)
	if face == nil {
		return
	}
	w, h := n.Width(), n.Height()
	lines := wrapText(face, txt, w*scale, style&styleOneLine == 0)
	met := face.Metrics()
	lh := float32(met.Height.Ceil()) / scale
	asc := float32(met.Ascent.Ceil()) / scale
	total := lh * float32(len(lines))
	y := float32(0)
	if style&styleCenterV != 0 {
		y = (h - total) / 2
	}
	col := propC(n.Props, "TextColor", 0xffffffff)
	shadow := propC(n.Props, "DropShadowColor", 0)
	inv := affine{a: 1 / scale, d: 1 / scale}
	for _, line := range lines {
		img, adv := r.textImage(face, line)
		lw := float32(adv) / scale
		x := float32(0)
		if style&styleRight != 0 {
			x = w - lw
		} else if style&styleCenterH != 0 {
			x = (w - lw) / 2
		}
		key := "txt:" + ftoa(size*scale) + ":" + propS(n.Props, "Font") + ":" + line
		tex, ok := r.texes[key]
		if !ok {
			tex = r.B.Upload(img)
			r.texes[key] = tex
		}
		iw, ih := float32(img.Bounds().Dx()), float32(img.Bounds().Dy())
		top := y + asc - float32(met.Ascent.Ceil())/scale
		lm := m.mul(affine{a: 1, d: 1, tx: x, ty: top}).mul(inv)
		if style&styleShadow != 0 && shadow>>24 != 0 {
			sm := m.mul(affine{a: 1, d: 1, tx: x + 1.5, ty: top + 1.5}).mul(inv)
			r.quad(tex, blend, sm, 0, 0, iw, ih, 0, 0, 1, 1, shadow, alpha)
		}
		r.quad(tex, blend, lm, 0, 0, iw, ih, 0, 0, 1, 1, col, alpha)
		y += lh
	}
}

// textImage rasterises one line as premultiplied white.
func (r *Renderer) textImage(face font.Face, s string) (*image.RGBA, int) {
	met := face.Metrics()
	adv := font.MeasureString(face, s).Ceil()
	w, h := adv+2, met.Height.Ceil()+met.Descent.Ceil()
	if w < 1 {
		w = 1
	}
	img := image.NewRGBA(image.Rect(0, 0, w, h))
	d := &font.Drawer{Dst: img, Src: image.NewUniform(color.White), Face: face,
		Dot: fixed.Point26_6{X: fixed.I(1), Y: fixed.I(met.Ascent.Ceil())}}
	d.DrawString(s)
	return img, adv
}

func wrapText(face font.Face, s string, width float32, wrap bool) []string {
	var out []string
	for _, para := range strings.Split(s, "\n") {
		if !wrap || width <= 0 {
			out = append(out, para)
			continue
		}
		line := ""
		for _, word := range strings.Fields(para) {
			try := word
			if line != "" {
				try = line + " " + word
			}
			if line != "" && float32(font.MeasureString(face, try).Ceil()) > width {
				out = append(out, line)
				line = word
			} else {
				line = try
			}
		}
		out = append(out, line)
	}
	return out
}

func abs32(f float32) float32 {
	if f < 0 {
		return -f
	}
	return f
}

func ftoa(f float32) string { return strconv.FormatFloat(float64(f), 'g', 6, 32) }
func ctoa(c Color) string   { return strconv.FormatUint(uint64(c), 16) }
