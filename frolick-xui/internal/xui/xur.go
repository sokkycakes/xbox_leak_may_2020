package xui

import (
	"encoding/binary"
	"errors"
	"fmt"
	"math"
)

// ParseXUR loads a binary XUIB v5 scene (big-endian, as built for Xbox 360).
func ParseXUR(data []byte) (*Element, error) {
	if len(data) < 0x14 || string(data[:4]) != "XUIB" {
		return nil, errors.New("xui: not an XUIB file")
	}
	be := binary.BigEndian
	if v := be.Uint32(data[4:]); v != 5 {
		return nil, fmt.Errorf("xui: XUIB version %d unsupported", v)
	}
	flags := be.Uint32(data[8:])
	nsec := int(be.Uint16(data[0x12:]))
	table := 0x14
	if flags&1 != 0 {
		table += 0x28
	}
	p := &xurParser{d: data}
	for i := 0; i < nsec; i++ {
		o := table + i*12
		if o+12 > len(data) {
			return nil, errors.New("xui: truncated section table")
		}
		tag := string(data[o : o+4])
		off, size := int(be.Uint32(data[o+4:])), int(be.Uint32(data[o+8:]))
		if off+size > len(data) {
			return nil, fmt.Errorf("xui: section %s out of range", tag)
		}
		sec := data[off : off+size]
		switch tag {
		case "STRN":
			for q := 0; q+2 <= len(sec); {
				n := int(be.Uint16(sec[q:]))
				q += 2
				if q+n*2 > len(sec) {
					return nil, errors.New("xui: bad STRN")
				}
				p.strs = append(p.strs, utf16be(sec[q:q+n*2]))
				q += n * 2
			}
		case "VECT":
			for q := 0; q+12 <= len(sec); q += 12 {
				p.vects = append(p.vects, Vec3{f32(sec[q:]), f32(sec[q+4:]), f32(sec[q+8:])})
			}
		case "QUAT":
			for q := 0; q+16 <= len(sec); q += 16 {
				p.quats = append(p.quats, Quat{f32(sec[q:]), f32(sec[q+4:]), f32(sec[q+8:]), f32(sec[q+12:])})
			}
		case "CUST":
			p.cust = sec
		case "DATA":
			p.data = sec
		}
	}
	if p.data == nil {
		return nil, errors.New("xui: no DATA section")
	}
	var root *Element
	err := func() (err error) {
		defer func() {
			if r := recover(); r != nil {
				err = fmt.Errorf("xui: corrupt scene at DATA+0x%x: %v", p.pos, r)
			}
		}()
		root = p.element()
		return nil
	}()
	if err != nil {
		return nil, err
	}
	if p.pos != len(p.data) {
		return nil, fmt.Errorf("xui: %d trailing bytes in DATA", len(p.data)-p.pos)
	}
	return root, nil
}

type xurParser struct {
	d, data, cust []byte
	pos           int
	strs          []string
	vects         []Vec3
	quats         []Quat
}

func (p *xurParser) u8() int { v := p.data[p.pos]; p.pos++; return int(v) }
func (p *xurParser) u16() int {
	v := binary.BigEndian.Uint16(p.data[p.pos:])
	p.pos += 2
	return int(v)
}
func (p *xurParser) u32() uint32 {
	v := binary.BigEndian.Uint32(p.data[p.pos:])
	p.pos += 4
	return v
}
func (p *xurParser) str(i int) string {
	if i <= 0 || i > len(p.strs) {
		return ""
	}
	return p.strs[i-1]
}
func (p *xurParser) vec(i uint32) Vec3 {
	if int(i) < len(p.vects) {
		return p.vects[i]
	}
	return Vec3{}
}
func (p *xurParser) quat(i uint32) Quat {
	if int(i) < len(p.quats) {
		return p.quats[i]
	}
	return Quat{W: 1}
}

func (p *xurParser) element() *Element {
	cls := p.str(p.u16())
	flags := p.u8()
	if cls == "" || flags&^7 != 0 {
		panic("bad element header")
	}
	e := &Element{Class: cls, Props: map[string]any{}}
	chain := ClassChain(cls)
	if flags&1 != 0 {
		if chain == nil {
			panic(fmt.Sprintf("unknown class %s with properties", cls))
		}
		p.u16() // total value count
		for _, level := range chain {
			p.level(level, e.Props)
		}
	}
	if flags&2 != 0 {
		n := int(p.u32())
		if n > 1<<14 {
			panic("child count")
		}
		for i := 0; i < n; i++ {
			e.Children = append(e.Children, p.element())
		}
	}
	if flags&4 != 0 {
		p.timelines(e)
	}
	return e
}

// level reads one class level's mask and values into out.
func (p *xurParser) level(props []*PropDef, out map[string]any) {
	hdr := p.u8()
	n := hdr & 7
	var mask uint32
	for i := 0; i < n; i++ {
		mask = mask<<8 | uint32(p.u8())
	}
	for i, pd := range props {
		if mask>>uint(i)&1 == 0 {
			continue
		}
		if pd.Indexed() {
			cnt := p.count()
			vals := make([]any, cnt)
			for k := range vals {
				vals[k] = p.value(pd)
			}
			out[pd.Name] = vals
		} else {
			out[pd.Name] = p.value(pd)
		}
	}
}

func (p *xurParser) count() int {
	b := p.u8()
	if b < 0x80 {
		return b
	}
	v := 0
	for i := 0; i < b&0x7f; i++ {
		v = v<<8 | p.u8()
	}
	return v
}

func (p *xurParser) value(pd *PropDef) any {
	switch pd.Type {
	case TBool:
		return p.u8() != 0
	case TInt:
		return int32(p.u32())
	case TUint:
		return p.u32()
	case TFloat:
		return math.Float32frombits(p.u32())
	case TString:
		return p.str(p.u16())
	case TColor:
		return Color(p.u32())
	case TVector:
		return p.vec(p.u32())
	case TQuat:
		return p.quat(p.u32())
	case TObject:
		p.u16()
		obj := Object{}
		p.level(pd.Sub, obj)
		return obj
	case TCustom:
		return p.custom(int(p.u32()), pd.Name)
	}
	panic(fmt.Sprintf("property %s has unknown type %d", pd.Name, pd.Type))
}

func (p *xurParser) custom(off int, name string) any {
	c := p.cust
	if off+4 > len(c) {
		return nil
	}
	size := int(binary.BigEndian.Uint32(c[off:]))
	if off+4+size > len(c) {
		return nil
	}
	b := c[off+4 : off+4+size]
	if name != "Points" || len(b) < 12 {
		return append([]byte(nil), b...)
	}
	pts := &Points{W: f32(b), H: f32(b[4:])}
	n := int(binary.BigEndian.Uint32(b[8:]))
	for i := 0; i < n && 12+i*24+24 <= len(b); i++ {
		q := b[12+i*24:]
		pts.Pts = append(pts.Pts, Point{
			P:  [2]float32{f32(q), f32(q[4:])},
			C1: [2]float32{f32(q[8:]), f32(q[12:])},
			C2: [2]float32{f32(q[16:]), f32(q[20:])},
		})
	}
	return pts
}

func (p *xurParser) timelines(e *Element) {
	n := int(p.u32())
	for i := 0; i < n; i++ {
		nf := NamedFrame{Name: p.str(p.u16()), Time: int(p.u32()), Command: p.u8()}
		nf.Target = p.str(p.u16())
		e.NamedFrames = append(e.NamedFrames, nf)
	}
	if len(e.Children) == 0 {
		return
	}
	tn := int(p.u32())
	for i := 0; i < tn; i++ {
		tl := Timeline{Target: p.str(p.u16())}
		var target *Element
		for _, c := range e.Children {
			if c.ID() == tl.Target {
				target = c
				break
			}
		}
		np := int(p.u32())
		defs := make([]*PropDef, np)
		for k := 0; k < np; k++ {
			pp, pd := p.propPath(target)
			tl.Props = append(tl.Props, pp)
			defs[k] = pd
		}
		nk := int(p.u32())
		for k := 0; k < nk; k++ {
			kf := Keyframe{Time: int(p.u32()), Interp: p.u8()}
			kf.Ease = [3]int8{int8(p.u8()), int8(p.u8()), int8(p.u8())}
			for _, pd := range defs {
				kf.Values = append(kf.Values, p.keyValue(pd))
			}
			tl.Keys = append(tl.Keys, kf)
		}
		e.Timelines = append(e.Timelines, tl)
	}
}

func (p *xurParser) keyValue(pd *PropDef) any {
	if pd == nil {
		return p.u32()
	}
	if pd.Type == TObject || pd.Type == TCustom {
		return p.u32()
	}
	return p.value(pd)
}

func (p *xurParser) propPath(target *Element) (PropPath, *PropDef) {
	b := p.u8()
	depth, indexed := b&0x7f, b&0x80 != 0
	pp := PropPath{Index: -1}
	if depth == 0 {
		panic("empty property path")
	}
	up, idx := p.u8(), p.u8()
	steps := []int{idx}
	for i := 1; i < depth; i++ {
		steps = append(steps, p.u8())
	}
	if indexed {
		pp.Index = int(p.u32())
	}
	if target == nil {
		panic("timeline target not found")
	}
	chain := ClassChain(target.Class)
	if up >= len(chain) {
		panic("property path level")
	}
	props := chain[len(chain)-1-up]
	var pd *PropDef
	for i, s := range steps {
		if s >= len(props) {
			panic("property path index")
		}
		pd = props[s]
		pp.Names = append(pp.Names, pd.Name)
		if i < len(steps)-1 {
			props = pd.Sub
		}
	}
	pp.Type = pd.Type
	return pp, pd
}

func f32(b []byte) float32 { return math.Float32frombits(binary.BigEndian.Uint32(b)) }

func utf16be(b []byte) string {
	r := make([]rune, 0, len(b)/2)
	for i := 0; i+1 < len(b); i += 2 {
		c := rune(b[i])<<8 | rune(b[i+1])
		if c >= 0xd800 && c < 0xdc00 && i+3 < len(b) {
			c2 := rune(b[i+2])<<8 | rune(b[i+3])
			c = 0x10000 + (c-0xd800)<<10 + (c2 - 0xdc00)
			i += 2
		}
		r = append(r, c)
	}
	return string(r)
}
