package xui

import (
	"bytes"
	"image"
	"image/draw"
	_ "image/jpeg"
	_ "image/png"
	"os"
	"path/filepath"
	"strings"
)

// Resources resolves image, scene and font paths used by scenes. Paths may
// carry XUI schemes (sharedres://, common://, file://); the scheme is
// dropped and the name is looked up in the packages, then the directories.
type Resources struct {
	Packages []*Package
	Dirs     []string
	// Fonts maps an XUI font name (lower case) to a TTF/OTF path. The ""
	// entry is the fallback for every name not listed.
	Fonts map[string]string
}

// Read returns a resource's bytes.
func (r *Resources) Read(path string) ([]byte, bool) {
	if !strings.Contains(path, "://") && !strings.Contains(path, "#") {
		if b, err := os.ReadFile(path); err == nil {
			return b, true
		}
	}
	name := path
	if i := strings.Index(name, "://"); i >= 0 {
		name = name[i+3:]
	}
	name = strings.TrimLeft(strings.ReplaceAll(name, "\\", "/"), "/")
	if i := strings.Index(name, "#"); i >= 0 { // pkg.xzp#file
		pk, err := OpenXZP(r.find(name[:i]))
		if err == nil {
			return pk.Get(name[i+1:])
		}
		name = name[i+1:]
	}
	for _, pk := range r.Packages {
		if b, ok := pk.Get(name); ok {
			return b, true
		}
	}
	base := filepath.Base(name)
	for _, pk := range r.Packages {
		for k, b := range pk.Files {
			if strings.EqualFold(filepath.Base(k), base) {
				return b, true
			}
		}
	}
	if p := r.find(name); p != "" {
		if b, err := os.ReadFile(p); err == nil {
			return b, true
		}
	}
	return nil, false
}

func (r *Resources) find(name string) string {
	if filepath.IsAbs(name) {
		return name
	}
	for _, d := range r.Dirs {
		p := filepath.Join(d, name)
		if _, err := os.Stat(p); err == nil {
			return p
		}
	}
	return ""
}

// Image decodes an image resource to RGBA, or returns nil.
func (r *Resources) Image(path string) *image.RGBA {
	b, ok := r.Read(path)
	if !ok {
		return nil
	}
	img, _, err := image.Decode(bytes.NewReader(b))
	if err != nil {
		return nil
	}
	out := image.NewRGBA(img.Bounds().Sub(img.Bounds().Min))
	draw.Draw(out, out.Bounds(), img, img.Bounds().Min, draw.Src)
	return out
}

// Scene loads a scene (.xur or .xui) by path.
func (r *Resources) Scene(path string) (*Element, error) {
	b, ok := r.Read(path)
	if !ok {
		return nil, &os.PathError{Op: "open", Path: path, Err: os.ErrNotExist}
	}
	return ParseAny(b)
}

// ParseAny parses binary XUIB or XML .xui data.
func ParseAny(b []byte) (*Element, error) {
	if bytes.HasPrefix(b, []byte("XUIB")) {
		return ParseXUR(b)
	}
	return ParseXUI(b)
}
