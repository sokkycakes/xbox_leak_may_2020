package xui

import (
	"encoding/binary"
	"errors"
	"os"
	"strings"
)

// Package is an XUIZ (.xzp) archive: scenes, images and sounds by path.
type Package struct {
	Files map[string][]byte // keys use '/' separators, original case
}

// OpenXZP reads a .xzp file from disk.
func OpenXZP(path string) (*Package, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	return ParseXZP(b)
}

// ParseXZP decodes an XUIZ archive held in memory.
func ParseXZP(d []byte) (*Package, error) {
	if len(d) < 22 || string(d[:4]) != "XUIZ" {
		return nil, errors.New("xui: not an XUIZ package")
	}
	be := binary.BigEndian
	hsize := int(be.Uint32(d[16:]))
	count := int(be.Uint16(d[20:]))
	base := 22 + hsize
	pk := &Package{Files: map[string][]byte{}}
	p := 22
	for i := 0; i < count; i++ {
		if p+9 > len(d) {
			return nil, errors.New("xui: truncated XUIZ directory")
		}
		size, off, n := int(be.Uint32(d[p:])), int(be.Uint32(d[p+4:])), int(d[p+8])
		p += 9
		if p+n*2 > len(d) || base+off+size > len(d) {
			return nil, errors.New("xui: bad XUIZ entry")
		}
		name := strings.ReplaceAll(utf16be(d[p:p+n*2]), "\\", "/")
		p += n * 2
		pk.Files[name] = d[base+off : base+off+size]
	}
	return pk, nil
}

// Get finds a file by path, ignoring case.
func (pk *Package) Get(name string) ([]byte, bool) {
	name = strings.ReplaceAll(name, "\\", "/")
	if b, ok := pk.Files[name]; ok {
		return b, true
	}
	for k, b := range pk.Files {
		if strings.EqualFold(k, name) {
			return b, true
		}
	}
	return nil, false
}
