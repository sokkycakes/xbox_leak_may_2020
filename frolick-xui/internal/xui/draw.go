package xui

import (
	"image"
	"image/color"
	"math"
	"strings"
)

// Vertex is a 2D vertex in scene pixels with texture coords and a
// non-premultiplied RGBA tint. Textures (image.RGBA) are premultiplied, so a
// backend outputs texel * (R*A, G*A, B*A, A) and blends with
// (ONE, ONE_MINUS_SRC_ALPHA).
type Vertex struct {
	X, Y, U, V float32
	R, G, B, A float32
}

// Texture is a backend texture handle. 0 means "no texture" (solid white).
type Texture uint32

// Blend selects how a batch is composited.
type Blend int

const (
	BlendNormal Blend = iota
	BlendAdd
	BlendMultiply
)

// Backend receives draw calls. Implementations: the GLES2 one in the app,
// and Soft (software) for tests and offline renders.
type Backend interface {
	// Upload creates a texture from an image.
	Upload(img *image.RGBA) Texture
	// Triangles draws indexed triangles.
	Triangles(tex Texture, blend Blend, v []Vertex, idx []uint16)
	// Clip limits drawing to an axis-aligned rectangle in scene pixels
	// (x0, y0, x1, y1), or removes the limit when r is nil.
	Clip(r *[4]float32)
}

// Renderer draws scenes through a Backend, caching textures.
type Renderer struct {
	B     Backend
	Res   *Resources
	texes map[string]Texture
	sizes map[Texture][2]int
	fonts *fontCache
	clip  *[4]float32
}

// NewRenderer makes a renderer for a backend and resource resolver.
func NewRenderer(b Backend, res *Resources) *Renderer {
	return &Renderer{B: b, Res: res, texes: map[string]Texture{}, sizes: map[Texture][2]int{}, fonts: newFontCache(res)}
}

type affine struct{ a, b, c, d, tx, ty float32 } // x' = a x + c y + tx; y' = b x + d y + ty

func (m affine) mul(n affine) affine { // m * n (apply n first)
	return affine{
		a: m.a*n.a + m.c*n.b, b: m.b*n.a + m.d*n.b,
		c: m.a*n.c + m.c*n.d, d: m.b*n.c + m.d*n.d,
		tx: m.a*n.tx + m.c*n.ty + m.tx, ty: m.b*n.tx + m.d*n.ty + m.ty,
	}
}
func (m affine) apply(x, y float32) (float32, float32) {
	return m.a*x + m.c*y + m.tx, m.b*x + m.d*y + m.ty
}

var identity = affine{a: 1, d: 1}

// Draw renders the scene with its origin at (x, y), scaled by s.
func (r *Renderer) Draw(s *Scene, x, y, scale float32) {
	r.node(s.Root, affine{a: scale, d: scale, tx: x, ty: y}, 1, BlendNormal)
	if r.clip != nil {
		r.clip = nil
		r.B.Clip(nil)
	}
}

func (r *Renderer) node(n *Node, parent affine, alpha float32, blend Blend) {
	if !propB(n.Props, "Show", true) {
		return
	}
	alpha *= propF(n.Props, "Opacity", 1)
	if alpha <= 0.001 {
		return
	}
	switch propU(n.Props, "BlendMode", 0) {
	case 3:
		blend = BlendAdd
	case 2:
		blend = BlendMultiply
	}
	pos := propV(n.Props, "Position", Vec3{})
	sc := propV(n.Props, "Scale", Vec3{1, 1, 1})
	pv := propV(n.Props, "Pivot", Vec3{})
	m := parent.mul(affine{a: 1, d: 1, tx: pos.X + pv.X, ty: pos.Y + pv.Y})
	if q, ok := n.Props["Rotation"].(Quat); ok && (q.Z != 0) {
		ang := 2 * math.Atan2(float64(q.Z), float64(q.W))
		cs, sn := float32(math.Cos(ang)), float32(math.Sin(ang))
		m = m.mul(affine{a: cs, b: sn, c: -sn, d: cs})
	}
	m = m.mul(affine{a: sc.X, d: sc.Y}).mul(affine{a: 1, d: 1, tx: -pv.X, ty: -pv.Y})

	switch {
	case n.Class == "XuiFigure":
		r.figure(n, m, alpha, blend)
	case n.Class == "XuiImage" || n.Class == "XuiImagePresenter":
		r.image(n, m, alpha, blend)
	case n.Class == "XuiText" || n.Class == "XuiTextPresenter":
		r.text(n, m, alpha, blend)
	}
	saved := r.clip
	if propB(n.Props, "ClipChildren", false) {
		x0, y0 := m.apply(0, 0)
		x1, y1 := m.apply(n.Width(), n.Height())
		c := [4]float32{min(x0, x1), min(y0, y1), max(x0, x1), max(y0, y1)}
		if saved != nil {
			c = [4]float32{max(c[0], saved[0]), max(c[1], saved[1]), min(c[2], saved[2]), min(c[3], saved[3])}
		}
		r.clip = &c
		r.B.Clip(r.clip)
	}
	if n.visual != nil {
		r.node(n.visual, m, alpha, blend)
	}
	for _, c := range n.Children {
		r.node(c, m, alpha, blend)
	}
	if r.clip != saved {
		r.clip = saved
		r.B.Clip(saved)
	}
}

// ---- figures -------------------------------------------------------------

func (r *Renderer) figure(n *Node, m affine, alpha float32, blend Blend) {
	pts, _ := n.Props["Points"].(*Points)
	w, h := n.Width(), n.Height()
	var poly [][2]float32
	pw, ph := w, h
	if pts != nil && len(pts.Pts) > 0 {
		pw, ph = pts.W, pts.H
		poly = flatten(pts.Pts, propB(n.Props, "Closed", true))
	} else {
		poly = [][2]float32{{0, 0}, {w, 0}, {w, h}, {0, h}}
	}
	if pw <= 0 {
		pw = 1
	}
	if ph <= 0 {
		ph = 1
	}
	sx, sy := w/pw, h/ph
	if pts == nil {
		sx, sy, pw, ph = 1, 1, w, h
	}
	fm := m.mul(affine{a: sx, d: sy})

	fill, _ := n.Props["Fill"].(Object)
	ftype := uint32(1)
	if fill == nil {
		ftype = 0
	} else if v, ok := fill["FillType"].(uint32); ok {
		ftype = v
	}
	if ftype != 0 && len(poly) >= 3 {
		idx := triangulate(poly)
		var tex Texture
		col := Color(0xffffffff)
		uvOf := func(x, y float32) (float32, float32) { return x / pw, y / ph }
		switch ftype {
		case 1:
			col = Color(0xff000000)
			if c, ok := fill["FillColor"].(Color); ok {
				col = c
			}
		case 2, 3:
			g, _ := fill["Gradient"].(Object)
			radial := ftype == 3 || (g != nil && propB(g, "Radial", false))
			tex = r.gradient(g, radial)
			uvOf = brushUV(fill, pw, ph, radial)
		case 4:
			tex = r.imageTex(propS(fill, "TextureFileName"))
			uvOf = brushUV(fill, pw, ph, false)
		}
		cr, cg, cb, ca := col.RGBA()
		verts := make([]Vertex, len(poly))
		for i, p := range poly {
			x, y := fm.apply(p[0], p[1])
			u, v := uvOf(p[0], p[1])
			verts[i] = Vertex{x, y, u, v, cr, cg, cb, ca * alpha}
		}
		r.B.Triangles(tex, blend, verts, idx)
	}
	if st, ok := n.Props["Stroke"].(Object); ok {
		sw := propF(st, "StrokeWidth", 0)
		sc := propC(st, "StrokeColor", 0)
		if sw > 0 && sc>>24 != 0 && len(poly) >= 2 {
			r.stroke(poly, propB(n.Props, "Closed", true), fm, sw, sc, alpha, blend)
		}
	}
}

// brushUV maps figure-space points into texture space using the fill's
// brush transform (Translation, Scale, Rotation about the centre).
func brushUV(fill Object, pw, ph float32, radial bool) func(x, y float32) (float32, float32) {
	t := propV(fill, "Translation", Vec3{})
	s := propV(fill, "Scale", Vec3{1, 1, 1})
	rot := float64(propF(fill, "Rotation", 0)) * math.Pi / 180
	if s.X == 0 {
		s.X = 1
	}
	if s.Y == 0 {
		s.Y = 1
	}
	cs, sn := float32(math.Cos(-rot)), float32(math.Sin(-rot))
	return func(x, y float32) (float32, float32) {
		u, v := (x-t.X)/pw-0.5, (y-t.Y)/ph-0.5
		u, v = u*cs-v*sn, u*sn+v*cs
		return u/s.X + 0.5, v/s.Y + 0.5
	}
}

// flatten turns XUI Bezier points into a polyline.
func flatten(pts []Point, closed bool) [][2]float32 {
	var out [][2]float32
	n := len(pts)
	segs := n - 1
	if closed {
		segs = n
	}
	out = append(out, pts[0].P)
	for i := 0; i < segs; i++ {
		a, b := pts[i], pts[(i+1)%n]
		p0, c1, c2, p1 := a.P, a.C1, a.C2, b.P
		straight := c1 == p0 && (c2 == p1 || c2 == p0)
		if straight || (c1 == p0 && c2 == p1) {
			out = append(out, p1)
			continue
		}
		steps := 10
		for k := 1; k <= steps; k++ {
			t := float32(k) / float32(steps)
			u := 1 - t
			x := u*u*u*p0[0] + 3*u*u*t*c1[0] + 3*u*t*t*c2[0] + t*t*t*p1[0]
			y := u*u*u*p0[1] + 3*u*u*t*c1[1] + 3*u*t*t*c2[1] + t*t*t*p1[1]
			out = append(out, [2]float32{x, y})
		}
	}
	if closed && len(out) > 1 && out[0] == out[len(out)-1] {
		out = out[:len(out)-1]
	}
	// drop consecutive duplicates
	dd := out[:1]
	for _, p := range out[1:] {
		if p != dd[len(dd)-1] {
			dd = append(dd, p)
		}
	}
	return dd
}

// triangulate ear-clips a simple polygon.
func triangulate(poly [][2]float32) []uint16 {
	n := len(poly)
	idx := make([]int, n)
	area := float32(0)
	for i := range poly {
		j := (i + 1) % n
		area += poly[i][0]*poly[j][1] - poly[j][0]*poly[i][1]
		idx[i] = i
	}
	if area < 0 { // make counter-clockwise in y-down space consistent
		for i, j := 0, n-1; i < j; i, j = i+1, j-1 {
			idx[i], idx[j] = idx[j], idx[i]
		}
	}
	var out []uint16
	cross := func(a, b, c [2]float32) float32 {
		return (b[0]-a[0])*(c[1]-a[1]) - (b[1]-a[1])*(c[0]-a[0])
	}
	inTri := func(p, a, b, c [2]float32) bool {
		return cross(a, b, p) >= 0 && cross(b, c, p) >= 0 && cross(c, a, p) >= 0
	}
	for guard := 0; len(idx) > 3 && guard < n*n; guard++ {
		found := false
		for i := range idx {
			ia, ib, ic := idx[(i+len(idx)-1)%len(idx)], idx[i], idx[(i+1)%len(idx)]
			a, b, c := poly[ia], poly[ib], poly[ic]
			if cross(a, b, c) <= 0 {
				continue
			}
			ear := true
			for _, j := range idx {
				if j != ia && j != ib && j != ic && inTri(poly[j], a, b, c) {
					ear = false
					break
				}
			}
			if !ear {
				continue
			}
			out = append(out, uint16(ia), uint16(ib), uint16(ic))
			idx = append(idx[:i], idx[i+1:]...)
			found = true
			break
		}
		if !found {
			break
		}
	}
	if len(idx) >= 3 {
		// Fan the rest (degenerate or self-touching input).
		for i := 1; i+1 < len(idx); i++ {
			out = append(out, uint16(idx[0]), uint16(idx[i]), uint16(idx[i+1]))
		}
	}
	return out
}

func (r *Renderer) stroke(poly [][2]float32, closed bool, m affine, w float32, c Color, alpha float32, blend Blend) {
	cr, cg, cb, ca := c.RGBA()
	var verts []Vertex
	var idx []uint16
	n := len(poly)
	segs := n - 1
	if closed {
		segs = n
	}
	for i := 0; i < segs; i++ {
		ax, ay := m.apply(poly[i][0], poly[i][1])
		bx, by := m.apply(poly[(i+1)%n][0], poly[(i+1)%n][1])
		dx, dy := bx-ax, by-ay
		l := float32(math.Hypot(float64(dx), float64(dy)))
		if l == 0 {
			continue
		}
		nx, ny := -dy/l*w/2, dx/l*w/2
		// extend along the segment to cover joins
		ex, ey := dx/l*w/2, dy/l*w/2
		base := uint16(len(verts))
		for _, p := range [][2]float32{{ax - ex + nx, ay - ey + ny}, {bx + ex + nx, by + ey + ny}, {bx + ex - nx, by + ey - ny}, {ax - ex - nx, ay - ey - ny}} {
			verts = append(verts, Vertex{p[0], p[1], 0, 0, cr, cg, cb, ca * alpha})
		}
		idx = append(idx, base, base+1, base+2, base, base+2, base+3)
	}
	if len(verts) > 0 {
		r.B.Triangles(0, blend, verts, idx)
	}
}

// gradient builds (and caches) a gradient ramp texture.
func (r *Renderer) gradient(g Object, radial bool) Texture {
	type stop struct {
		pos float32
		c   Color
	}
	var stops []stop
	cols, _ := g["StopColor"].([]any)
	poss, _ := g["StopPos"].([]any)
	for i := range cols {
		c, _ := cols[i].(Color)
		p := float32(i) / float32(max(1, len(cols)-1))
		if i < len(poss) {
			if f, ok := poss[i].(float32); ok {
				p = f
			}
		}
		stops = append(stops, stop{p, c})
	}
	if len(stops) == 0 {
		stops = []stop{{0, 0xffffffff}, {1, 0xff000000}}
	}
	var key strings.Builder
	if radial {
		key.WriteString("rad:")
	} else {
		key.WriteString("lin:")
	}
	for _, s := range stops {
		key.WriteString(ftoa(s.pos))
		key.WriteByte('/')
		key.WriteString(ctoa(s.c))
		key.WriteByte(';')
	}
	if t, ok := r.texes[key.String()]; ok {
		return t
	}
	at := func(t float32) color.NRGBA {
		if t <= stops[0].pos {
			return nrgba(stops[0].c)
		}
		for i := 1; i < len(stops); i++ {
			if t <= stops[i].pos {
				a, b := stops[i-1], stops[i]
				f := float32(0)
				if b.pos > a.pos {
					f = (t - a.pos) / (b.pos - a.pos)
				}
				ca, cb := nrgba(a.c), nrgba(b.c)
				l := func(x, y uint8) uint8 { return uint8(float32(x) + (float32(y)-float32(x))*f + 0.5) }
				return color.NRGBA{l(ca.R, cb.R), l(ca.G, cb.G), l(ca.B, cb.B), l(ca.A, cb.A)}
			}
		}
		return nrgba(stops[len(stops)-1].c)
	}
	var img *image.RGBA
	if radial {
		const sz = 128
		img = image.NewRGBA(image.Rect(0, 0, sz, sz))
		for y := 0; y < sz; y++ {
			for x := 0; x < sz; x++ {
				dx, dy := (float32(x)+0.5)/sz*2-1, (float32(y)+0.5)/sz*2-1
				d := float32(math.Sqrt(float64(dx*dx + dy*dy)))
				img.Set(x, y, at(d))
			}
		}
	} else {
		const sz = 256
		img = image.NewRGBA(image.Rect(0, 0, sz, 1))
		for x := 0; x < sz; x++ {
			img.Set(x, 0, at((float32(x)+0.5)/sz))
		}
	}
	t := r.B.Upload(img)
	r.texes[key.String()] = t
	return t
}

func nrgba(c Color) color.NRGBA {
	return color.NRGBA{uint8(c >> 16), uint8(c >> 8), uint8(c), uint8(c >> 24)}
}

// ---- images ----------------------------------------------------------------

func (r *Renderer) imageTex(path string) Texture {
	if path == "" {
		return 0
	}
	if t, ok := r.texes["img:"+path]; ok {
		return t
	}
	var t Texture
	if img := r.Res.Image(path); img != nil {
		t = r.B.Upload(img)
		r.sizes[t] = [2]int{img.Bounds().Dx(), img.Bounds().Dy()}
	}
	r.texes["img:"+path] = t
	return t
}

func (r *Renderer) image(n *Node, m affine, alpha float32, blend Blend) {
	path := propS(n.Props, "ImagePath")
	if n.owner != nil && n.Class == "XuiImagePresenter" {
		if p := propS(n.owner.Props, "ImagePath"); p != "" {
			path = p
		}
	}
	tex := r.imageTex(path)
	if tex == 0 {
		return
	}
	w, h := n.Width(), n.Height()
	x0, y0, x1, y1 := float32(0), float32(0), w, h
	sz := r.sizes[tex]
	iw, ih := float32(sz[0]), float32(sz[1])
	switch mode := propU(n.Props, "SizeMode", 0); {
	case mode&4 != 0 && iw > 0 && ih > 0: // keep aspect, fit, centred
		s := min(w/iw, h/ih)
		x0, y0 = (w-iw*s)/2, (h-ih*s)/2
		x1, y1 = x0+iw*s, y0+ih*s
	case mode&16 != 0 && iw > 0: // centre at native size
		x0, y0 = (w-iw)/2, (h-ih)/2
		x1, y1 = x0+iw, y0+ih
	}
	r.quad(tex, blend, m, x0, y0, x1, y1, 0, 0, 1, 1, 0xffffffff, alpha)
}

func (r *Renderer) quad(tex Texture, blend Blend, m affine, x0, y0, x1, y1, u0, v0, u1, v1 float32, c Color, alpha float32) {
	cr, cg, cb, ca := c.RGBA()
	ca *= alpha
	ax, ay := m.apply(x0, y0)
	bx, by := m.apply(x1, y0)
	cx, cy := m.apply(x1, y1)
	dx, dy := m.apply(x0, y1)
	r.B.Triangles(tex, blend, []Vertex{
		{ax, ay, u0, v0, cr, cg, cb, ca}, {bx, by, u1, v0, cr, cg, cb, ca},
		{cx, cy, u1, v1, cr, cg, cb, ca}, {dx, dy, u0, v1, cr, cg, cb, ca},
	}, []uint16{0, 1, 2, 0, 2, 3})
}
