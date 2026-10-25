package xui

import (
	"bytes"
	"encoding/xml"
	"fmt"
	"strconv"
	"strings"
)

// ParseXUI loads an XML scene in the layout XUI Tool saves (.xui):
//
//	<XuiCanvas version="000c">
//	  <Properties><Width>1280</Width>...</Properties>
//	  <XuiScene>
//	    <Properties><Id>Main</Id><Position>10,20,0</Position>...</Properties>
//	    <XuiText>...</XuiText>
//	    <Timelines>
//	      <NamedFrames><NamedFrame><Name>Normal</Name><Time>0</Time><Command>play</Command></NamedFrame></NamedFrames>
//	      <Timeline><Id>child</Id><TimelineProp>Opacity</TimelineProp>
//	        <KeyFrame><Time>0</Time><Interpolation>0</Interpolation><Prop>1</Prop></KeyFrame></Timeline>
//	    </Timelines>
//	  </XuiScene>
//	</XuiCanvas>
//
// Values: vectors "x,y,z", quaternions "x,y,z,w", colours "0xAARRGGBB",
// booleans "true"/"false". Indexed properties repeat their tag. Nested
// properties (Fill, Stroke, Gradient) nest their tags. Points is
// "W,H,closed;x,y,c1x,c1y,c2x,c2y;..." (controls may be omitted for straight
// edges: "x,y").
func ParseXUI(b []byte) (*Element, error) {
	var root xnode
	if err := xml.NewDecoder(bytes.NewReader(b)).Decode(&root); err != nil {
		return nil, fmt.Errorf("xui: %w", err)
	}
	return xuiElement(&root)
}

type xnode struct {
	XMLName xml.Name
	Attrs   []xml.Attr `xml:",any,attr"`
	Text    string     `xml:",chardata"`
	Kids    []xnode    `xml:",any"`
}

func (x *xnode) kid(name string) *xnode {
	for i := range x.Kids {
		if x.Kids[i].XMLName.Local == name {
			return &x.Kids[i]
		}
	}
	return nil
}

func xuiElement(x *xnode) (*Element, error) {
	cls := x.XMLName.Local
	chain := ClassChain(cls)
	if chain == nil {
		return nil, fmt.Errorf("xui: unknown class %s", cls)
	}
	e := &Element{Class: cls, Props: map[string]any{}}
	for i := range x.Kids {
		k := &x.Kids[i]
		switch k.XMLName.Local {
		case "Properties":
			defs := map[string]*PropDef{}
			for _, lvl := range chain {
				for _, d := range lvl {
					defs[d.Name] = d
				}
			}
			if err := xuiProps(k, defs, e.Props); err != nil {
				return nil, fmt.Errorf("%s: %w", cls, err)
			}
		case "Timelines":
		default:
			c, err := xuiElement(k)
			if err != nil {
				return nil, err
			}
			e.Children = append(e.Children, c)
		}
	}
	if t := x.kid("Timelines"); t != nil {
		if err := xuiTimelines(t, e); err != nil {
			return nil, fmt.Errorf("%s %s timelines: %w", cls, e.ID(), err)
		}
	}
	return e, nil
}

func xuiProps(x *xnode, defs map[string]*PropDef, out map[string]any) error {
	for i := range x.Kids {
		k := &x.Kids[i]
		d := defs[k.XMLName.Local]
		if d == nil {
			return fmt.Errorf("unknown property %s", k.XMLName.Local)
		}
		var v any
		if d.Type == TObject {
			sub := map[string]*PropDef{}
			for _, s := range d.Sub {
				sub[s.Name] = s
			}
			obj := Object{}
			if err := xuiProps(k, sub, obj); err != nil {
				return fmt.Errorf("%s: %w", d.Name, err)
			}
			v = obj
		} else {
			var err error
			if v, err = parseValue(d, strings.TrimSpace(k.Text)); err != nil {
				return fmt.Errorf("%s: %w", d.Name, err)
			}
		}
		if d.Indexed() {
			l, _ := out[d.Name].([]any)
			out[d.Name] = append(l, v)
		} else {
			out[d.Name] = v
		}
	}
	return nil
}

func parseValue(d *PropDef, s string) (any, error) {
	switch d.Type {
	case TBool:
		return s == "true" || s == "1", nil
	case TInt:
		v, err := strconv.ParseInt(s, 0, 32)
		return int32(v), err
	case TUint:
		v, err := strconv.ParseUint(s, 0, 32)
		return uint32(v), err
	case TFloat:
		v, err := strconv.ParseFloat(s, 32)
		return float32(v), err
	case TString:
		return s, nil
	case TColor:
		v, err := strconv.ParseUint(s, 0, 32)
		return Color(v), err
	case TVector:
		f, err := floats(s, 3)
		return Vec3{f[0], f[1], f[2]}, err
	case TQuat:
		f, err := floats(s, 4)
		return Quat{f[0], f[1], f[2], f[3]}, err
	case TCustom:
		if d.Name == "Points" {
			return parsePoints(s)
		}
		return []byte(s), nil
	}
	return nil, fmt.Errorf("type %d", d.Type)
}

func floats(s string, n int) ([]float32, error) {
	parts := strings.Split(s, ",")
	out := make([]float32, n)
	for i := 0; i < n && i < len(parts); i++ {
		v, err := strconv.ParseFloat(strings.TrimSpace(parts[i]), 32)
		if err != nil {
			return out, err
		}
		out[i] = float32(v)
	}
	if len(parts) < n && n != 3 {
		return out, fmt.Errorf("want %d numbers in %q", n, s)
	}
	return out, nil
}

func parsePoints(s string) (*Points, error) {
	groups := strings.Split(s, ";")
	head, err := floats(groups[0], 2)
	if err != nil {
		return nil, err
	}
	p := &Points{W: head[0], H: head[1]}
	for _, g := range groups[1:] {
		g = strings.TrimSpace(g)
		if g == "" {
			continue
		}
		n := len(strings.Split(g, ","))
		f, err := floats(g, n)
		if err != nil {
			return nil, err
		}
		pt := Point{P: [2]float32{f[0], f[1]}}
		pt.C1, pt.C2 = pt.P, [2]float32{}
		if n >= 6 {
			pt.C1, pt.C2 = [2]float32{f[2], f[3]}, [2]float32{f[4], f[5]}
		}
		p.Pts = append(p.Pts, pt)
	}
	// Straight edges: a control left unset points at the next vertex.
	for i := range p.Pts {
		if p.Pts[i].C2 == ([2]float32{}) {
			p.Pts[i].C2 = p.Pts[(i+1)%len(p.Pts)].P
		}
	}
	return p, nil
}

var commands = map[string]int{"play": 0, "stop": 1, "goto": 2, "gotoandplay": 3, "gotoandstop": 4}

func xuiTimelines(t *xnode, e *Element) error {
	if nfs := t.kid("NamedFrames"); nfs != nil {
		for i := range nfs.Kids {
			k := &nfs.Kids[i]
			nf := NamedFrame{}
			if n := k.kid("Name"); n != nil {
				nf.Name = strings.TrimSpace(n.Text)
			}
			if n := k.kid("Time"); n != nil {
				nf.Time, _ = strconv.Atoi(strings.TrimSpace(n.Text))
			}
			if n := k.kid("Command"); n != nil {
				nf.Command = commands[strings.ToLower(strings.TrimSpace(n.Text))]
			}
			if n := k.kid("CommandParams"); n != nil {
				nf.Target = strings.TrimSpace(n.Text)
			}
			e.NamedFrames = append(e.NamedFrames, nf)
		}
	}
	for i := range t.Kids {
		k := &t.Kids[i]
		if k.XMLName.Local != "Timeline" {
			continue
		}
		tl := Timeline{}
		if n := k.kid("Id"); n != nil {
			tl.Target = strings.TrimSpace(n.Text)
		}
		var target *Element
		for _, c := range e.Children {
			if c.ID() == tl.Target {
				target = c
			}
		}
		if target == nil {
			return fmt.Errorf("no child %q", tl.Target)
		}
		var defs []*PropDef
		for j := range k.Kids {
			kk := &k.Kids[j]
			switch kk.XMLName.Local {
			case "TimelineProp":
				pp, d, err := resolvePath(target.Class, strings.TrimSpace(kk.Text))
				if err != nil {
					return err
				}
				tl.Props = append(tl.Props, pp)
				defs = append(defs, d)
			case "KeyFrame":
				kf := Keyframe{}
				vi := 0
				for m := range kk.Kids {
					f := &kk.Kids[m]
					txt := strings.TrimSpace(f.Text)
					switch f.XMLName.Local {
					case "Time":
						kf.Time, _ = strconv.Atoi(txt)
					case "Interpolation":
						kf.Interp, _ = strconv.Atoi(txt)
					case "EaseIn":
						v, _ := strconv.Atoi(txt)
						kf.Ease[0] = int8(v)
					case "Prop":
						if vi >= len(defs) {
							return fmt.Errorf("too many Prop values at time %d", kf.Time)
						}
						v, err := parseValue(defs[vi], txt)
						if err != nil {
							return err
						}
						kf.Values = append(kf.Values, v)
						vi++
					}
				}
				tl.Keys = append(tl.Keys, kf)
			}
		}
		e.Timelines = append(e.Timelines, tl)
	}
	return nil
}

// resolvePath turns "Fill.Gradient.StopColor[1]" into a PropPath.
func resolvePath(class, path string) (PropPath, *PropDef, error) {
	pp := PropPath{Index: -1}
	if i := strings.Index(path, "["); i >= 0 && strings.HasSuffix(path, "]") {
		pp.Index, _ = strconv.Atoi(path[i+1 : len(path)-1])
		path = path[:i]
	}
	var props []*PropDef
	for _, lvl := range ClassChain(class) {
		props = append(props, lvl...)
	}
	var d *PropDef
	for _, name := range strings.Split(path, ".") {
		d = nil
		for _, p := range props {
			if p.Name == name {
				d = p
			}
		}
		if d == nil {
			return pp, nil, fmt.Errorf("%s has no property %s", class, path)
		}
		pp.Names = append(pp.Names, name)
		props = d.Sub
	}
	pp.Type = d.Type
	return pp, d, nil
}
