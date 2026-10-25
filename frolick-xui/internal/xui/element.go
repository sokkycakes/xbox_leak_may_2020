package xui

// Vec3 is an XUI vector property (Position, Scale, Pivot...).
type Vec3 struct{ X, Y, Z float32 }

// Quat is an XUI rotation.
type Quat struct{ X, Y, Z, W float32 }

// Color is 0xAARRGGBB.
type Color uint32

func (c Color) RGBA() (r, g, b, a float32) {
	return float32(c>>16&0xff) / 255, float32(c>>8&0xff) / 255, float32(c&0xff) / 255, float32(c>>24) / 255
}

// Object is a nested property block (Fill, Stroke, Gradient...).
type Object map[string]any

// Point is one figure point: the vertex and the two Bezier controls of the
// segment that leaves it.
type Point struct{ P, C1, C2 [2]float32 }

// Points is the decoded Points custom property of an XuiFigure.
type Points struct {
	W, H float32
	Pts  []Point
}

// NamedFrame marks a time on an element's timeline (Normal, Focus, ...).
type NamedFrame struct {
	Name    string
	Time    int
	Command int // 0 play, 1 stop, 2 goto, 3 gotoAndPlay, 4 gotoAndStop
	Target  string
}

// PropPath addresses a (possibly nested) property on a timeline target.
type PropPath struct {
	Names []string // e.g. ["Fill", "FillColor"]
	Type  int
	Index int // for indexed props; -1 otherwise
}

// Keyframe holds one value per animated property.
type Keyframe struct {
	Time   int
	Interp int // 0 linear, 1 none (step), 2 ease
	Ease   [3]int8
	Values []any
}

// Timeline animates properties of one child element.
type Timeline struct {
	Target string
	Props  []PropPath
	Keys   []Keyframe
}

// Element is a node of a loaded scene description (immutable template).
type Element struct {
	Class       string
	Props       map[string]any // property name -> value (indexed props: []any)
	Children    []*Element
	NamedFrames []NamedFrame
	Timelines   []Timeline
}

// ID returns the element's Id property.
func (e *Element) ID() string { s, _ := e.Props["Id"].(string); return s }

// Find returns the first descendant (or e) with the given Id.
func (e *Element) Find(id string) *Element {
	if e.ID() == id {
		return e
	}
	for _, c := range e.Children {
		if f := c.Find(id); f != nil {
			return f
		}
	}
	return nil
}
