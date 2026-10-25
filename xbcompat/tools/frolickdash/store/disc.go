package main

// The tile of an Xbox disc image on the Archive: the title image inside
// the disc's default.xbe (its $$XTIMAGE section, an XPR texture), read
// with a few range requests (the volume descriptor, the root directory,
// the XBE's headers and that section), so no more than a few hundred KB
// of the disc are fetched.

import (
	"bytes"
	"context"
	"encoding/binary"
	"errors"
	"fmt"
	"image"
	"image/color"
	"image/png"
	"io"
	"net/http"
	"net/url"
	"os"
	"strings"
	"time"
)

// fileURL is where an item's file is downloaded from.
func fileURL(ident, file string) string {
	return *base + "/download/" + url.PathEscape(ident) + "/" + url.PathEscape(file)
}

// rangeAt reads len(b) bytes at off of the file at u, trying again when
// the Archive answers with a passing server error (it does, now and then).
func rangeAt(ctx context.Context, u string, b []byte, off int64) error {
	var err error
	for try := 0; try < 4; try++ {
		if try > 0 {
			select {
			case <-ctx.Done():
				return ctx.Err()
			case <-time.After(time.Duration(try) * time.Second):
			}
		}
		if err = rangeOnce(ctx, u, b, off); err == nil {
			return nil
		}
	}
	return err
}

func rangeOnce(ctx context.Context, u string, b []byte, off int64) error {
	resp, err := get(ctx, u, map[string]string{"Range": fmt.Sprintf("bytes=%d-%d", off, off+int64(len(b))-1)})
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusPartialContent {
		return fmt.Errorf("HTTP %d to a range request", resp.StatusCode)
	}
	_, err = io.ReadFull(resp.Body, b)
	return err
}

// discTile writes the title image of the disc image at u as a tile.
func discTile(ctx context.Context, u, path string) error {
	if st, err := os.Stat(path); err == nil && time.Since(st.ModTime()) < 30*24*time.Hour {
		return nil
	}
	ctx, cancel := context.WithTimeout(ctx, 2*time.Minute)
	defer cancel()
	img, err := discTitleImage(ctx, u)
	if err != nil {
		return err
	}
	var buf bytes.Buffer
	if err := png.Encode(&buf, fit(img, 96, 72)); err != nil {
		return err
	}
	return writeAtomic(path, buf.Bytes())
}

func discTitleImage(ctx context.Context, u string) (image.Image, error) {
	// the redirect to a datanode once, then range requests there
	var resp *http.Response
	var err error
	for try := 0; ; try++ {
		resp, err = get(ctx, u, map[string]string{"Range": "bytes=0-0"})
		if err != nil {
			return nil, err
		}
		resp.Body.Close()
		if resp.StatusCode == http.StatusPartialContent {
			break
		}
		if try == 3 || resp.StatusCode < 500 {
			return nil, fmt.Errorf("HTTP %d to a range request", resp.StatusCode)
		}
		time.Sleep(time.Duration(try+1) * time.Second)
	}
	u = resp.Request.URL.String()
	vd := make([]byte, 2048)
	base := int64(-1)
	for _, b := range []int64{0, xgd1Base} {
		if rangeAt(ctx, u, vd, b+32*2048) == nil && string(vd[:20]) == "MICROSOFT*XBOX*MEDIA" {
			base = b
			break
		}
	}
	if base < 0 {
		return nil, errors.New("no Xbox file system on the disc")
	}
	dir := make([]byte, binary.LittleEndian.Uint32(vd[24:]))
	if err := rangeAt(ctx, u, dir, base+int64(binary.LittleEndian.Uint32(vd[20:]))*2048); err != nil {
		return nil, err
	}
	sec, size, ok := xdvdfsFind(dir, "default.xbe")
	if !ok {
		return nil, errors.New("no default.xbe in the disc's root")
	}
	xbe := base + int64(sec)*2048
	hdr := make([]byte, 4096)
	if err := rangeAt(ctx, u, hdr, xbe); err != nil {
		return nil, err
	}
	if string(hdr[:4]) != "XBEH" {
		return nil, errors.New("default.xbe isn't an XBE")
	}
	le := binary.LittleEndian
	va, hsize := le.Uint32(hdr[0x104:]), le.Uint32(hdr[0x108:])
	if hsize > 1<<20 || hsize > size {
		return nil, errors.New("default.xbe's headers are too big")
	}
	if int(hsize) > len(hdr) {
		hdr = make([]byte, hsize)
		if err := rangeAt(ctx, u, hdr, xbe); err != nil {
			return nil, err
		}
	}
	at := func(addr uint32) int { return int(addr - va) }
	nsec, sh := int(le.Uint32(hdr[0x11C:])), at(le.Uint32(hdr[0x120:]))
	for i := 0; i < nsec; i++ {
		s := sh + i*56
		if s < 0 || s+56 > len(hdr) {
			break
		}
		n := at(le.Uint32(hdr[s+20:]))
		if n < 0 || n >= len(hdr) {
			continue
		}
		name := hdr[n:]
		if j := bytes.IndexByte(name, 0); j >= 0 {
			name = name[:j]
		}
		if string(name) != "$$XTIMAGE" {
			continue
		}
		raw, rsize := le.Uint32(hdr[s+12:]), le.Uint32(hdr[s+16:])
		if rsize == 0 || rsize > 4<<20 || raw+rsize > size {
			return nil, errors.New("the title image section is the wrong size")
		}
		data := make([]byte, rsize)
		if err := rangeAt(ctx, u, data, xbe+int64(raw)); err != nil {
			return nil, err
		}
		return decodeXPR(data)
	}
	return nil, errors.New("default.xbe has no title image")
}

// decodeXPR decodes an XPR0 texture (or a DDS, which some titles use).
func decodeXPR(b []byte) (image.Image, error) {
	le := binary.LittleEndian
	if len(b) >= 128 && string(b[:4]) == "DDS " {
		h, w := int(le.Uint32(b[12:])), int(le.Uint32(b[16:]))
		switch string(b[84:88]) {
		case "DXT1":
			return dxt(b[128:], w, h, 1)
		case "DXT2", "DXT3":
			return dxt(b[128:], w, h, 3)
		case "DXT4", "DXT5":
			return dxt(b[128:], w, h, 5)
		}
		return nil, errors.New("a DDS title image in a format not read here")
	}
	if len(b) < 32 || string(b[:4]) != "XPR0" {
		return nil, errors.New("the title image isn't an XPR texture")
	}
	hsize := int(le.Uint32(b[8:]))
	format := le.Uint32(b[12+12:])
	w, h := 1<<((format>>20)&15), 1<<((format>>24)&15)
	if hsize > len(b) || w > 1024 || h > 1024 {
		return nil, errors.New("a bad XPR header")
	}
	pix := b[hsize:]
	switch (format >> 8) & 0xFF {
	case 0x0C:
		return dxt(pix, w, h, 1)
	case 0x0E:
		return dxt(pix, w, h, 3)
	case 0x0F:
		return dxt(pix, w, h, 5)
	case 0x06, 0x07:
		return argb(pix, w, h, true, (format>>8)&0xFF == 0x07)
	case 0x12, 0x1E:
		return argb(pix, w, h, false, (format>>8)&0xFF == 0x1E)
	case 0x05:
		return rgb565(pix, w, h)
	}
	return nil, fmt.Errorf("an XPR title image in format %#x, not read here", (format>>8)&0xFF)
}

// swizzled is pixel (x, y)'s index in an Xbox swizzled texture: the bits of
// x and y interleaved, x first, while both have bits left.
func swizzled(x, y, w, h int) int {
	i, bit := 0, 0
	for mw, mh := w>>1, h>>1; mw > 0 || mh > 0; mw, mh = mw>>1, mh>>1 {
		if mw > 0 {
			i |= (x & 1) << bit
			x >>= 1
			bit++
		}
		if mh > 0 {
			i |= (y & 1) << bit
			y >>= 1
			bit++
		}
	}
	return i
}

func argb(pix []byte, w, h int, swz, opaque bool) (image.Image, error) {
	if len(pix) < w*h*4 {
		return nil, errors.New("the title image is cut short")
	}
	img := image.NewNRGBA(image.Rect(0, 0, w, h))
	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			i := y*w + x
			if swz {
				i = swizzled(x, y, w, h)
			}
			p := pix[i*4:]
			a := p[3]
			if opaque {
				a = 255
			}
			img.SetNRGBA(x, y, color.NRGBA{p[2], p[1], p[0], a})
		}
	}
	return img, nil
}

func rgb565(pix []byte, w, h int) (image.Image, error) {
	if len(pix) < w*h*2 {
		return nil, errors.New("the title image is cut short")
	}
	img := image.NewNRGBA(image.Rect(0, 0, w, h))
	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			v := binary.LittleEndian.Uint16(pix[swizzled(x, y, w, h)*2:])
			img.SetNRGBA(x, y, c565(v))
		}
	}
	return img, nil
}

func c565(v uint16) color.NRGBA {
	r, g, b := uint8(v>>11&31), uint8(v>>5&63), uint8(v&31)
	return color.NRGBA{r<<3 | r>>2, g<<2 | g>>4, b<<3 | b>>2, 255}
}

// dxt decodes DXT1 (kind 1), DXT3 (3) or DXT5 (5) blocks, which Xbox
// textures keep in linear block order.
func dxt(pix []byte, w, h, kind int) (image.Image, error) {
	bs := 16
	if kind == 1 {
		bs = 8
	}
	bw, bh := (w+3)/4, (h+3)/4
	if len(pix) < bw*bh*bs {
		return nil, errors.New("the title image is cut short")
	}
	le := binary.LittleEndian
	img := image.NewNRGBA(image.Rect(0, 0, w, h))
	for by := 0; by < bh; by++ {
		for bx := 0; bx < bw; bx++ {
			blk := pix[(by*bw+bx)*bs:]
			var alpha [16]uint8
			for i := range alpha {
				alpha[i] = 255
			}
			switch kind {
			case 3:
				a := le.Uint64(blk)
				for i := 0; i < 16; i++ {
					v := uint8(a >> (4 * i) & 15)
					alpha[i] = v<<4 | v
				}
				blk = blk[8:]
			case 5:
				a0, a1 := blk[0], blk[1]
				var pal [8]uint8
				pal[0], pal[1] = a0, a1
				for i := 2; i < 8; i++ {
					if a0 > a1 {
						pal[i] = uint8((int(a0)*(8-i) + int(a1)*(i-1)) / 7)
					} else if i < 6 {
						pal[i] = uint8((int(a0)*(6-i) + int(a1)*(i-1)) / 5)
					} else if i == 6 {
						pal[i] = 0
					} else {
						pal[i] = 255
					}
				}
				bits := uint64(blk[2]) | uint64(blk[3])<<8 | uint64(blk[4])<<16 | uint64(blk[5])<<24 | uint64(blk[6])<<32 | uint64(blk[7])<<40
				for i := 0; i < 16; i++ {
					alpha[i] = pal[bits>>(3*i)&7]
				}
				blk = blk[8:]
			}
			v0, v1 := le.Uint16(blk), le.Uint16(blk[2:])
			c0, c1 := c565(v0), c565(v1)
			var pal [4]color.NRGBA
			pal[0], pal[1] = c0, c1
			mix := func(a, b color.NRGBA, wa, wb, d int) color.NRGBA {
				return color.NRGBA{uint8((int(a.R)*wa + int(b.R)*wb) / d), uint8((int(a.G)*wa + int(b.G)*wb) / d), uint8((int(a.B)*wa + int(b.B)*wb) / d), 255}
			}
			if v0 > v1 || kind != 1 {
				pal[2], pal[3] = mix(c0, c1, 2, 1, 3), mix(c0, c1, 1, 2, 3)
			} else {
				pal[2], pal[3] = mix(c0, c1, 1, 1, 2), color.NRGBA{}
			}
			idx := le.Uint32(blk[4:])
			for i := 0; i < 16; i++ {
				x, y := bx*4+i%4, by*4+i/4
				if x >= w || y >= h {
					continue
				}
				c := pal[idx>>(2*i)&3]
				if c.A != 0 {
					c.A = alpha[i]
				}
				img.SetNRGBA(x, y, c)
			}
		}
	}
	return img, nil
}

// fileHashes are a file's md5 and sha1 from the item's metadata.
func fileHashes(ctx context.Context, ident, file string) (string, string, error) {
	m, err := metadata(ctx, ident)
	if err != nil {
		return "", "", err
	}
	for _, f := range m.Files {
		if strings.EqualFold(f.Name, file) {
			return f.MD5, f.SHA1, nil
		}
	}
	return "", "", fmt.Errorf("%s: no file %q", ident, file)
}
