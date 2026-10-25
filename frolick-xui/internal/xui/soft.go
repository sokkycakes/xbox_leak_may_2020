package xui

import (
	"image"
	"math"
)

// Soft is a software Backend that rasterises into an RGBA image. It is used
// for tests and for rendering scenes to PNG without a GPU.
type Soft struct {
	Dst  *image.RGBA
	texs []*image.RGBA
	clip *[4]float32
}

func (s *Soft) Clip(r *[4]float32) { s.clip = r }

func (s *Soft) Upload(img *image.RGBA) Texture {
	s.texs = append(s.texs, img)
	return Texture(len(s.texs))
}

func (s *Soft) sample(t Texture, u, v float32) (r, g, b, a float32) {
	if t == 0 || int(t) > len(s.texs) {
		return 1, 1, 1, 1
	}
	img := s.texs[t-1]
	w, h := img.Bounds().Dx(), img.Bounds().Dy()
	x := float64(u)*float64(w) - 0.5
	y := float64(v)*float64(h) - 0.5
	x0, y0 := int(math.Floor(x)), int(math.Floor(y))
	fx, fy := float32(x-float64(x0)), float32(y-float64(y0))
	px := func(xx, yy int) [4]float32 {
		xx = clampi(xx, 0, w-1)
		yy = clampi(yy, 0, h-1)
		o := yy*img.Stride + xx*4
		p := img.Pix[o : o+4]
		return [4]float32{float32(p[0]) / 255, float32(p[1]) / 255, float32(p[2]) / 255, float32(p[3]) / 255}
	}
	a00, a10, a01, a11 := px(x0, y0), px(x0+1, y0), px(x0, y0+1), px(x0+1, y0+1)
	var o [4]float32
	for i := range o {
		top := a00[i] + (a10[i]-a00[i])*fx
		bot := a01[i] + (a11[i]-a01[i])*fx
		o[i] = top + (bot-top)*fy
	}
	return o[0], o[1], o[2], o[3]
}

func clampi(v, lo, hi int) int {
	if v < lo {
		return lo
	}
	if v > hi {
		return hi
	}
	return v
}

func (s *Soft) Triangles(tex Texture, blend Blend, v []Vertex, idx []uint16) {
	for i := 0; i+2 < len(idx); i += 3 {
		s.tri(tex, blend, v[idx[i]], v[idx[i+1]], v[idx[i+2]])
	}
}

func (s *Soft) tri(tex Texture, blend Blend, a, b, c Vertex) {
	area := (b.X-a.X)*(c.Y-a.Y) - (b.Y-a.Y)*(c.X-a.X)
	if area == 0 {
		return
	}
	bd := s.Dst.Bounds()
	minx := clampi(int(math.Floor(float64(min(a.X, b.X, c.X)))), bd.Min.X, bd.Max.X)
	maxx := clampi(int(math.Ceil(float64(max(a.X, b.X, c.X)))), bd.Min.X, bd.Max.X)
	miny := clampi(int(math.Floor(float64(min(a.Y, b.Y, c.Y)))), bd.Min.Y, bd.Max.Y)
	maxy := clampi(int(math.Ceil(float64(max(a.Y, b.Y, c.Y)))), bd.Min.Y, bd.Max.Y)
	if s.clip != nil {
		minx = max(minx, int(math.Floor(float64(s.clip[0]))))
		miny = max(miny, int(math.Floor(float64(s.clip[1]))))
		maxx = min(maxx, int(math.Ceil(float64(s.clip[2]))))
		maxy = min(maxy, int(math.Ceil(float64(s.clip[3]))))
	}
	for y := miny; y < maxy; y++ {
		for x := minx; x < maxx; x++ {
			px, py := float32(x)+0.5, float32(y)+0.5
			w0 := ((b.X-px)*(c.Y-py) - (b.Y-py)*(c.X-px)) / area
			w1 := ((c.X-px)*(a.Y-py) - (c.Y-py)*(a.X-px)) / area
			w2 := 1 - w0 - w1
			if w0 < -1e-5 || w1 < -1e-5 || w2 < -1e-5 {
				continue
			}
			u := a.U*w0 + b.U*w1 + c.U*w2
			vv := a.V*w0 + b.V*w1 + c.V*w2
			tr, tg, tb, ta := s.sample(tex, u, vv)
			va := a.A*w0 + b.A*w1 + c.A*w2
			sr := tr * (a.R*w0 + b.R*w1 + c.R*w2) * va
			sg := tg * (a.G*w0 + b.G*w1 + c.G*w2) * va
			sb := tb * (a.B*w0 + b.B*w1 + c.B*w2) * va
			sa := ta * va
			o := s.Dst.PixOffset(x, y)
			d := s.Dst.Pix[o : o+4]
			dr, dg, db, da := float32(d[0])/255, float32(d[1])/255, float32(d[2])/255, float32(d[3])/255
			switch blend {
			case BlendAdd:
				dr, dg, db = dr+sr, dg+sg, db+sb
			case BlendMultiply:
				dr, dg, db = sr*dr+dr*(1-sa), sg*dg+dg*(1-sa), sb*db+db*(1-sa)
			default:
				dr, dg, db = sr+dr*(1-sa), sg+dg*(1-sa), sb+db*(1-sa)
				da = sa + da*(1-sa)
			}
			d[0], d[1], d[2], d[3] = u8(dr), u8(dg), u8(db), u8(da)
		}
	}
}

func u8(f float32) uint8 {
	if f <= 0 {
		return 0
	}
	if f >= 1 {
		return 255
	}
	return uint8(f*255 + 0.5)
}
