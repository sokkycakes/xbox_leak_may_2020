package main

// A small ISO 9660 reader for the discs the Archive hands out: plain .iso
// (2048-byte sectors) and raw .bin / .img / .mdf (2352-byte sectors, mode 1
// or mode 2 form 1), with Joliet's long names when the disc has them.

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"unicode/utf16"
)

type isoImage struct {
	f      *os.File
	sector int64 // bytes per sector in the file
	offset int64 // where the 2048 data bytes start in each
	joliet bool
	root   [2]uint32 // root directory: extent, size
}

func openISO(path string) (*isoImage, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	for _, layout := range [][2]int64{{2048, 0}, {2352, 16}, {2352, 24}} {
		img := &isoImage{f: f, sector: layout[0], offset: layout[1]}
		if img.descriptors() == nil {
			return img, nil
		}
	}
	f.Close()
	return nil, fmt.Errorf("%s: not an ISO 9660 disc image", filepath.Base(path))
}

func (img *isoImage) Close() { img.f.Close() }

func (img *isoImage) readSector(lba uint32, b []byte) error {
	_, err := img.f.ReadAt(b[:2048], int64(lba)*img.sector+img.offset)
	return err
}

// The primary volume descriptor, and the Joliet one that replaces its root
// when there is one.
func (img *isoImage) descriptors() error {
	b := make([]byte, 2048)
	found := false
	for lba := uint32(16); lba < 64; lba++ {
		if err := img.readSector(lba, b); err != nil {
			return err
		}
		if string(b[1:6]) != "CD001" {
			return errors.New("no CD001")
		}
		switch b[0] {
		case 1:
			if !found {
				img.root = [2]uint32{binary.LittleEndian.Uint32(b[156+2:]), binary.LittleEndian.Uint32(b[156+10:])}
				found = true
			}
		case 2:
			esc := string(b[88:91])
			if esc == "%/@" || esc == "%/C" || esc == "%/E" {
				img.root = [2]uint32{binary.LittleEndian.Uint32(b[156+2:]), binary.LittleEndian.Uint32(b[156+10:])}
				img.joliet = true
				found = true
			}
		case 255:
			if found {
				return nil
			}
			return errors.New("no primary descriptor")
		}
	}
	if found {
		return nil
	}
	return errors.New("no descriptor")
}

type isoEntry struct {
	name   string
	extent uint32
	size   uint32
	dir    bool
}

func (img *isoImage) readDir(extent, size uint32) ([]isoEntry, error) {
	var out []isoEntry
	b := make([]byte, 2048)
	for done := uint32(0); done < size; done += 2048 {
		if err := img.readSector(extent+done/2048, b); err != nil {
			return nil, err
		}
		for p := 0; p < 2048; {
			n := int(b[p])
			if n == 0 || p+n > 2048 || n < 34 {
				break
			}
			r := b[p : p+n]
			p += n
			nl := int(r[32])
			if 33+nl > len(r) {
				continue
			}
			raw := r[33 : 33+nl]
			if nl == 1 && (raw[0] == 0 || raw[0] == 1) {
				continue // . and ..
			}
			var name string
			if img.joliet {
				u := make([]uint16, nl/2)
				for i := range u {
					u[i] = binary.BigEndian.Uint16(raw[2*i:])
				}
				name = string(utf16.Decode(u))
			} else {
				name = string(raw)
			}
			if i := strings.LastIndexByte(name, ';'); i > 0 {
				name = name[:i]
			}
			name = strings.TrimSuffix(name, ".")
			if name == "" || strings.ContainsAny(name, "/\\") || name == ".." {
				continue
			}
			out = append(out, isoEntry{name: name, extent: binary.LittleEndian.Uint32(r[2:]), size: binary.LittleEndian.Uint32(r[10:]), dir: r[25]&2 != 0})
		}
	}
	return out, nil
}

// Extracts the whole disc into dir.
func (img *isoImage) extract(dir string) error {
	return img.extractDir(img.root[0], img.root[1], dir, 0)
}

func (img *isoImage) extractDir(extent, size uint32, dir string, depth int) error {
	if depth > 16 {
		return nil
	}
	ents, err := img.readDir(extent, size)
	if err != nil {
		return err
	}
	if err := os.MkdirAll(dir, 0755); err != nil {
		return err
	}
	for _, e := range ents {
		p := filepath.Join(dir, e.name)
		if e.dir {
			if err := img.extractDir(e.extent, e.size, p, depth+1); err != nil {
				return err
			}
			continue
		}
		if err := img.extractFile(e, p); err != nil {
			return err
		}
	}
	return nil
}

func (img *isoImage) extractFile(e isoEntry, path string) error {
	out, err := os.Create(path)
	if err != nil {
		return err
	}
	if img.sector == 2048 {
		_, err = io.Copy(out, io.NewSectionReader(img.f, int64(e.extent)*2048, int64(e.size)))
	} else {
		b := make([]byte, 2048)
		left := int64(e.size)
		for lba := e.extent; left > 0 && err == nil; lba++ {
			if err = img.readSector(lba, b); err != nil {
				break
			}
			n := int64(2048)
			if left < n {
				n = left
			}
			_, err = out.Write(b[:n])
			left -= n
		}
	}
	if cerr := out.Close(); err == nil {
		err = cerr
	}
	return err
}
