package xui

import (
	_ "embed"
	"encoding/json"
)

// Property types as stored in XUIB.
const (
	TBool   = 1
	TInt    = 2
	TUint   = 3
	TFloat  = 4
	TString = 5
	TColor  = 6
	TVector = 7
	TQuat   = 8
	TObject = 9
	TCustom = 10
)

// PropDef describes one class property (from the GFWL XUI class dump).
type PropDef struct {
	Name  string     `json:"n"`
	Type  int        `json:"t"`
	Flags int        `json:"f"`
	Sub   []*PropDef `json:"s,omitempty"`
}

// Indexed reports whether the value is a counted array.
func (p *PropDef) Indexed() bool { return p.Flags&1 != 0 }

//go:embed classes.json
var classesJSON []byte

// classChains maps a class name to its property levels, base class first.
var classChains map[string][][]*PropDef

func init() {
	if err := json.Unmarshal(classesJSON, &classChains); err != nil {
		panic("xui: bad classes.json: " + err.Error())
	}
	patch360()
}

// patch360 adjusts the GFWL class dump to the Xbox 360 dashboard's layout.
// The 360 XuiControl keeps Text in slot 8 and ImagePath in slot 10, where
// GFWL has NavTabForward and Text (verified on Blades scenes: buttons carry
// their label in slot 8 and icon file names in slot 10).
func patch360() {
	rename := map[int]string{8: "Text", 9: "_Slot9", 10: "ImagePath", 12: "_Slot12"}
	seen := map[*PropDef]bool{}
	for _, chain := range classChains {
		for _, level := range chain {
			if len(level) != 13 || level[0].Name != "ClassOverride" || level[7].Name != "NavDown" {
				continue
			}
			for i, n := range rename {
				if !seen[level[i]] {
					seen[level[i]] = true
					level[i].Name = n
					level[i].Type = TString
				}
			}
		}
	}
}

// ClassChain returns the property levels of a class, base first, or nil.
func ClassChain(class string) [][]*PropDef { return classChains[class] }

// IsA reports whether class derives from (or is) base, judged by its chain.
func IsA(class, base string) bool {
	if class == base {
		return true
	}
	c, b := classChains[class], classChains[base]
	if c == nil || b == nil || len(b) > len(c) {
		return false
	}
	// Chains share their base levels; compare the level that base itself adds.
	return levelEqual(c[len(b)-1], b[len(b)-1])
}

func levelEqual(a, b []*PropDef) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i].Name != b[i].Name || a[i].Type != b[i].Type {
			return false
		}
	}
	return true
}

// BaseClass returns the class that name derives from directly, or "".
func BaseClass(name string) string {
	if b, ok := knownBase[name]; ok {
		return b
	}
	c := classChains[name]
	if len(c) < 2 {
		return ""
	}
	for k, b := range classChains {
		if len(b) == len(c)-1 && k != name && levelEqual(b[len(b)-1], c[len(b)-1]) {
			ok := true
			for i := range b {
				if !levelEqual(b[i], c[i]) {
					ok = false
					break
				}
			}
			if ok {
				return k
			}
		}
	}
	return ""
}

// knownBase pins parents the chain comparison cannot tell apart (classes
// that add no properties of their own).
var knownBase = map[string]string{
	"XuiBackButton":  "XuiButton",
	"XuiNavButton":   "XuiButton",
	"XuiCheckbox":    "XuiButton",
	"XuiRadioButton": "XuiCheckbox",
	"XuiListItem":    "XuiButton",
	"XuiButton":      "XuiControl",
	"XuiLabel":       "XuiControl",
}
