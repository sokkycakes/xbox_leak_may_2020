package xui

import (
	"strings"
)

// FPS is the XUI timeline rate (frames per second).
const FPS = 30

// Node is a live element: a copy of its template's properties that timelines
// and code can change.
type Node struct {
	Tmpl     *Element
	Class    string
	Props    map[string]any
	Children []*Node
	Parent   *Node

	// visual is the skin template instance drawn for a control.
	visual *Node
	// owner is the control a visual subtree belongs to (for presenters).
	owner *Node

	// timeline state (for nodes with named frames / timelines)
	time     float64
	playing  bool
	timeline bool

	focused bool
}

// Scene is an instantiated XUI scene with focus and timeline state.
type Scene struct {
	Root    *Node
	Skin    *Element // root of the skin scene (children are XuiVisuals), may be nil
	Focus   *Node
	OnPress func(n *Node)
	OnFocus func(n *Node)
}

// NewScene instantiates a scene. skin may be nil, in which case controls draw
// only their own children.
func NewScene(root, skin *Element) *Scene {
	s := &Scene{Skin: skin}
	s.Root = s.instantiate(root, nil)
	s.Root.walk(func(n *Node) bool {
		if n.timeline && !n.Play("Normal") {
			// Timelines without a Normal frame run from the start.
			n.time, n.playing = 0, true
			n.apply()
		}
		return true
	})
	// Initial focus: the first focusable control in tree order.
	s.Root.walk(func(n *Node) bool {
		if s.Focus == nil && n.Focusable() {
			s.SetFocus(n)
		}
		return s.Focus == nil
	})
	return s
}

func (s *Scene) instantiate(e *Element, parent *Node) *Node {
	n := &Node{Tmpl: e, Class: e.Class, Props: copyProps(e.Props), Parent: parent}
	n.timeline = len(e.NamedFrames) > 0 || len(e.Timelines) > 0
	for _, c := range e.Children {
		n.Children = append(n.Children, s.instantiate(c, n))
	}
	if IsA(e.Class, "XuiControl") && e.Class != "XuiScene" && !IsA(e.Class, "XuiTabScene") {
		s.attachVisual(n)
	}
	return n
}

// attachVisual instantiates the skin visual named by the control's Visual
// property (or its class name), resized to the control.
func (s *Scene) attachVisual(n *Node) {
	if s.Skin == nil {
		return
	}
	name, _ := n.Props["Visual"].(string)
	if name == "" {
		name = n.Class
	}
	v := s.skinVisual(name)
	if v == nil && propS(n.Props, "Visual") == "" {
		// Fall back along the class chain (XuiNavButton -> XuiButton).
		for c := BaseClass(n.Class); c != "" && v == nil; c = BaseClass(c) {
			v = s.skinVisual(c)
		}
	}
	if v == nil {
		return
	}
	vis := s.instantiate(v, n)
	vis.Props["Position"] = Vec3{}
	vis.markOwner(n)
	w0, h0 := propF(v.Props, "Width", 60), propF(v.Props, "Height", 30)
	w, h := n.Width(), n.Height()
	vis.Props["Width"], vis.Props["Height"] = w, h
	for _, c := range vis.Children {
		c.anchor(w-w0, h-h0)
	}
	n.visual = vis
}

func (s *Scene) skinVisual(name string) *Element {
	var find func(e *Element) *Element
	find = func(e *Element) *Element {
		for _, c := range e.Children {
			if c.Class == "XuiVisual" && c.ID() == name {
				return c
			}
		}
		for _, c := range e.Children {
			if c.Class != "XuiVisual" {
				if f := find(c); f != nil {
					return f
				}
			}
		}
		return nil
	}
	return find(s.Skin)
}

func (n *Node) markOwner(owner *Node) {
	n.owner = owner
	for _, c := range n.Children {
		if c.visual == nil {
			c.markOwner(owner)
		}
	}
}

// anchor moves or stretches n after its parent grew by dw, dh.
func (n *Node) anchor(dw, dh float32) {
	a := propU(n.Props, "Anchor", 0)
	pos := propV(n.Props, "Position", Vec3{})
	switch {
	case a&1 != 0 && a&4 != 0:
		n.Props["Width"] = n.Width() + dw
	case a&4 != 0:
		pos.X += dw
	}
	switch {
	case a&2 != 0 && a&8 != 0:
		n.Props["Height"] = n.Height() + dh
	case a&8 != 0:
		pos.Y += dh
	}
	n.Props["Position"] = pos
}

func copyProps(p map[string]any) map[string]any {
	m := make(map[string]any, len(p))
	for k, v := range p {
		if o, ok := v.(Object); ok {
			v = copyObject(o)
		}
		m[k] = v
	}
	return m
}

func copyObject(o Object) Object {
	m := Object{}
	for k, v := range o {
		if c, ok := v.(Object); ok {
			v = copyObject(c)
		}
		if l, ok := v.([]any); ok {
			v = append([]any(nil), l...)
		}
		m[k] = v
	}
	return m
}

func (n *Node) walk(f func(*Node) bool) bool {
	if !f(n) {
		return false
	}
	if n.visual != nil && !n.visual.walk(f) {
		return false
	}
	for _, c := range n.Children {
		if !c.walk(f) {
			return false
		}
	}
	return true
}

// ID returns the node's Id.
func (n *Node) ID() string { s, _ := n.Props["Id"].(string); return s }

func (n *Node) Width() float32  { return propF(n.Props, "Width", 60) }
func (n *Node) Height() float32 { return propF(n.Props, "Height", 30) }

// Find looks up a descendant by Id (not entering skin visuals).
func (n *Node) Find(id string) *Node {
	if n.ID() == id {
		return n
	}
	for _, c := range n.Children {
		if f := c.Find(id); f != nil {
			return f
		}
	}
	return nil
}

// Find looks up an element of the scene by Id.
func (s *Scene) Find(id string) *Node { return s.Root.Find(id) }

// SetText sets the Text of a text element or control.
func (n *Node) SetText(t string) { n.Props["Text"] = t }

// SetImage sets the ImagePath of an image element or control.
func (n *Node) SetImage(path string) { n.Props["ImagePath"] = path }

// Focusable reports whether the node can take focus.
func (n *Node) Focusable() bool {
	if !IsA(n.Class, "XuiControl") || n.Class == "XuiScene" || IsA(n.Class, "XuiTabScene") || n.owner != nil {
		return false
	}
	if n.Class == "XuiLabel" || n.Class == "XuiText" || n.Class == "XuiImagePresenter" || n.Class == "XuiTextPresenter" {
		return false
	}
	if en, ok := n.Props["Enabled"].(bool); ok && !en {
		return false
	}
	if sh, ok := n.Props["Show"].(bool); ok && !sh {
		return false
	}
	return true
}

// SetFocus moves focus to n, playing KillFocus/Focus on the visuals.
func (s *Scene) SetFocus(n *Node) {
	if n == s.Focus {
		return
	}
	first := s.Focus == nil
	if s.Focus != nil {
		s.Focus.focused = false
		s.Focus.playState("KillFocus", "Normal")
	}
	s.Focus = n
	if n != nil {
		n.focused = true
		if first {
			n.playState("InitFocus", "Focus")
		} else {
			n.playState("Focus")
		}
		if s.OnFocus != nil {
			s.OnFocus(n)
		}
	}
}

// Navigate moves focus along "left", "right", "up" or "down" using the
// control's Nav* properties.
func (s *Scene) Navigate(dir string) bool {
	if s.Focus == nil {
		return false
	}
	key := "Nav" + strings.ToUpper(dir[:1]) + dir[1:]
	id, _ := s.Focus.Props[key].(string)
	if id == "" {
		return false
	}
	t := s.Root.Find(id)
	if t == nil || !t.Focusable() {
		return false
	}
	s.SetFocus(t)
	return true
}

// Press activates the focused control.
func (s *Scene) Press() {
	if s.Focus == nil {
		return
	}
	s.Focus.playState("Press")
	if s.OnPress != nil {
		s.OnPress(s.Focus)
	}
}

// playState plays the first named frame that exists on the control's visual
// (or on the control itself).
func (n *Node) playState(names ...string) {
	t := n.visual
	if t == nil {
		t = n
	}
	for _, name := range names {
		if t.Play(name) {
			return
		}
	}
}

// Play jumps to a named frame and starts playing. It reports whether the
// frame exists.
func (n *Node) Play(name string) bool {
	for _, nf := range n.Tmpl.NamedFrames {
		if nf.Name == name {
			n.time = float64(nf.Time)
			n.playing = true
			n.apply()
			return true
		}
	}
	return false
}

// Update advances all timelines by dt seconds.
func (s *Scene) Update(dt float64) {
	s.Root.walk(func(n *Node) bool {
		if n.timeline && n.playing {
			n.advance(dt * FPS)
		}
		return true
	})
}

func (n *Node) advance(frames float64) {
	from := n.time
	to := from + frames
	// Fire named-frame commands crossed in (from, to].
	for guard := 0; guard < 8; guard++ {
		var hit *NamedFrame
		for i := range n.Tmpl.NamedFrames {
			nf := &n.Tmpl.NamedFrames[i]
			t := float64(nf.Time)
			if t > from && t <= to && nf.Command != 0 && (hit == nil || t < float64(hit.Time)) {
				hit = nf
			}
		}
		if hit == nil {
			break
		}
		switch hit.Command {
		case 1, 4: // stop, gotoAndStop
			n.time = float64(hit.Time)
			if hit.Command == 4 {
				n.time = n.frameTime(hit.Target, n.time)
			}
			n.playing = false
			n.apply()
			return
		case 2, 3: // goto(AndPlay)
			rest := to - float64(hit.Time)
			t := n.frameTime(hit.Target, float64(hit.Time))
			if t >= float64(hit.Time) { // avoid a forward goto loop
				rest = 0
			}
			from, to = t, t+rest
			if hit.Command == 2 {
				n.time = t
				n.playing = false
				n.apply()
				return
			}
			continue
		}
		break
	}
	n.time = to
	if last := n.lastKey(); n.time >= last && !n.hasCommandsAfter(from) {
		n.time = last
		n.playing = false
	}
	n.apply()
}

func (n *Node) frameTime(name string, def float64) float64 {
	for _, nf := range n.Tmpl.NamedFrames {
		if nf.Name == name {
			return float64(nf.Time)
		}
	}
	return def
}

func (n *Node) lastKey() float64 {
	last := 0
	for _, tl := range n.Tmpl.Timelines {
		if k := len(tl.Keys); k > 0 && tl.Keys[k-1].Time > last {
			last = tl.Keys[k-1].Time
		}
	}
	for _, nf := range n.Tmpl.NamedFrames {
		if nf.Time > last {
			last = nf.Time
		}
	}
	return float64(last)
}

func (n *Node) hasCommandsAfter(t float64) bool {
	for _, nf := range n.Tmpl.NamedFrames {
		if float64(nf.Time) > t && nf.Command != 0 {
			return true
		}
	}
	return false
}

// apply writes the timeline values at n.time into the target children.
func (n *Node) apply() {
	for _, tl := range n.Tmpl.Timelines {
		var target *Node
		for _, c := range n.Children {
			if c.ID() == tl.Target {
				target = c
				break
			}
		}
		if target == nil || len(tl.Keys) == 0 {
			continue
		}
		k0, k1 := 0, 0
		for i, k := range tl.Keys {
			if float64(k.Time) <= n.time {
				k0, k1 = i, i
			} else {
				k1 = i
				break
			}
		}
		if float64(tl.Keys[0].Time) > n.time {
			k0, k1 = 0, 0
		}
		a, b := tl.Keys[k0], tl.Keys[k1]
		f := 0.0
		if k1 != k0 && b.Time > a.Time {
			f = (n.time - float64(a.Time)) / float64(b.Time-a.Time)
			switch a.Interp {
			case 1:
				f = 0
			case 2:
				f = ease(f, a.Ease)
			}
		}
		for pi, pp := range tl.Props {
			if pi >= len(a.Values) || pi >= len(b.Values) {
				continue
			}
			target.setPath(pp, lerpValue(a.Values[pi], b.Values[pi], f))
		}
	}
}

func ease(f float64, e [3]int8) float64 {
	// XUI eases with in/out strengths in -100..100; approximate with a
	// smoothstep blended toward linear.
	s := f * f * (3 - 2*f)
	w := float64(e[0]) / 100
	if w < 0 {
		w = -w
	}
	if w == 0 {
		w = 1
	}
	return f + (s-f)*w
}

func (n *Node) setPath(pp PropPath, v any) {
	if len(pp.Names) == 1 {
		if pp.Index >= 0 {
			setIndexed(n.Props, pp.Names[0], pp.Index, v)
			return
		}
		n.Props[pp.Names[0]] = v
		return
	}
	var m map[string]any = n.Props
	for _, name := range pp.Names[:len(pp.Names)-1] {
		o, ok := m[name].(Object)
		if !ok {
			o = Object{}
			m[name] = o
		}
		m = o
	}
	last := pp.Names[len(pp.Names)-1]
	if pp.Index >= 0 {
		setIndexed(m, last, pp.Index, v)
		return
	}
	m[last] = v
}

func setIndexed(m map[string]any, name string, i int, v any) {
	l, _ := m[name].([]any)
	for len(l) <= i {
		l = append(l, v)
	}
	l[i] = v
	m[name] = l
}

func lerpValue(a, b any, f float64) any {
	if f == 0 {
		return a
	}
	ff := float32(f)
	switch x := a.(type) {
	case float32:
		if y, ok := b.(float32); ok {
			return x + (y-x)*ff
		}
	case int32:
		if y, ok := b.(int32); ok {
			return x + int32(float64(y-x)*f)
		}
	case Vec3:
		if y, ok := b.(Vec3); ok {
			return Vec3{x.X + (y.X-x.X)*ff, x.Y + (y.Y-x.Y)*ff, x.Z + (y.Z-x.Z)*ff}
		}
	case Quat:
		if y, ok := b.(Quat); ok {
			return Quat{x.X + (y.X-x.X)*ff, x.Y + (y.Y-x.Y)*ff, x.Z + (y.Z-x.Z)*ff, x.W + (y.W-x.W)*ff}
		}
	case Color:
		if y, ok := b.(Color); ok {
			var out Color
			for sh := 0; sh < 32; sh += 8 {
				ca, cb := float64(x>>sh&0xff), float64(y>>sh&0xff)
				out |= Color(uint32(ca+(cb-ca)*f+0.5)&0xff) << sh
			}
			return out
		}
	}
	if f >= 1 {
		return b
	}
	return a
}

func propF(p map[string]any, k string, def float32) float32 {
	if v, ok := p[k].(float32); ok {
		return v
	}
	return def
}

func propU(p map[string]any, k string, def uint32) uint32 {
	if v, ok := p[k].(uint32); ok {
		return v
	}
	return def
}

func propV(p map[string]any, k string, def Vec3) Vec3 {
	if v, ok := p[k].(Vec3); ok {
		return v
	}
	return def
}

func propB(p map[string]any, k string, def bool) bool {
	if v, ok := p[k].(bool); ok {
		return v
	}
	return def
}

func propC(p map[string]any, k string, def Color) Color {
	if v, ok := p[k].(Color); ok {
		return v
	}
	return def
}

func propS(p map[string]any, k string) string { s, _ := p[k].(string); return s }

// ControlAt returns the focusable control under scene point (x, y), using
// each element's position and scale (rotation is ignored).
func (s *Scene) ControlAt(x, y float32) *Node {
	var hit *Node
	var visit func(n *Node, ox, oy, sx, sy float32)
	visit = func(n *Node, ox, oy, sx, sy float32) {
		if !propB(n.Props, "Show", true) {
			return
		}
		pos := propV(n.Props, "Position", Vec3{})
		sc := propV(n.Props, "Scale", Vec3{1, 1, 1})
		pv := propV(n.Props, "Pivot", Vec3{})
		// x' = pos + pv + (x - pv) * scale, in parent space
		nx := ox + sx*(pos.X+pv.X-pv.X*sc.X)
		ny := oy + sy*(pos.Y+pv.Y-pv.Y*sc.Y)
		nsx, nsy := sx*sc.X, sy*sc.Y
		if n.Focusable() && x >= nx && y >= ny && x < nx+n.Width()*nsx && y < ny+n.Height()*nsy {
			hit = n
		}
		for _, c := range n.Children {
			visit(c, nx, ny, nsx, nsy)
		}
	}
	visit(s.Root, 0, 0, 1, 1)
	return hit
}
