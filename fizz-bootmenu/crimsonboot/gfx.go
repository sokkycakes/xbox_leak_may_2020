// Copyright 2026 the crimson project authors. SPDX-License-Identifier: BSD-3-Clause

package main

// The graphical menu: drawn on a 320x240 canvas (square pixels on a 4:3 TV)
// and shown full screen through DRM, by default at 2880x240p. That is the
// 2880x480i super resolution made progressive: the same 54 MHz pixel clock
// and 15.734 kHz line, 262 lines a frame, so each canvas pixel is 9 screen
// pixels wide and one line tall, like a console's 240p. Before an entry runs
// the menu hands the screen back to the console (which goes back to 480i)
// with its last picture copied into the console framebuffer, so there's no
// flash of text.
//
// Everything can be themed from SIONBOOT's sion/menu/ (all optional):
//
//	font.ttf, font-bold.ttf   TrueType/OpenType fonts (default: Go fonts)
//	background.png            drawn under everything, scaled to 320x240
//	logo.png                  replaces the CRIMSON title (fits in 288x40)
//	icons/<name>.png          16x16 next to an entry: crimson, uefi, shell,
//	                          reboot, poweroff
//	theme.conf                name=#rrggbb lines: accent, text, dim, status,
//	                          top, bottom (the background gradient), and
//	                          size=N (entry text size, default 13)

import (
	"bufio"
	"fmt"
	"image"
	"image/color"
	"image/png"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"unsafe"

	xdraw "golang.org/x/image/draw"
	"golang.org/x/image/font"
	"golang.org/x/image/font/gofont/gobold"
	"golang.org/x/image/font/gofont/goregular"
	"golang.org/x/image/font/opentype"
	"golang.org/x/image/math/fixed"
	"golang.org/x/sys/unix"
)

const (
	canvasW = 320
	canvasH = 240
)

var (
	gfxMenu = true // crimsonboot.menu=text turns it off
	menu240 = true // crimsonboot.menures=480 keeps the console's mode
)

// ---------------------------------------------------------------------------
// DRM mode setting through the raw ioctls (amd64 layouts).

type drmModeInfo struct {
	clock                                         uint32
	hdisplay, hsyncStart, hsyncEnd, htotal, hskew uint16
	vdisplay, vsyncStart, vsyncEnd, vtotal, vscan uint16
	vrefresh, flags, typ                          uint32
	name                                          [32]byte
}

type drmCardRes struct {
	fbs, crtcs, connectors, encoders                    uint64
	countFbs, countCrtcs, countConnectors, countEncoders uint32
	minW, maxW, minH, maxH                              uint32
}

type drmGetConnector struct {
	encoders, modes, props, propValues           uint64
	countModes, countProps, countEncoders        uint32
	encoderID, connectorID, typ, typID           uint32
	connection, mmW, mmH, subpixel, _            uint32
}

type drmGetEncoder struct {
	encoderID, typ, crtcID, possibleCrtcs, possibleClones uint32
}

type drmCrtc struct {
	setConnectors                   uint64
	countConnectors, crtcID, fbID   uint32
	x, y, gammaSize, modeValid      uint32
	mode                            drmModeInfo
}

type drmCreateDumb struct {
	height, width, bpp, flags, handle, pitch uint32
	size                                     uint64
}

type drmMapDumb struct {
	handle, _ uint32
	offset    uint64
}

type drmFbCmd struct {
	fbID, width, height, pitch, bpp, depth, handle uint32
}

const (
	drmModeFlagInterlace = 1 << 4
	drmModeFlagDblscan   = 1 << 5
	drmModeTypeUserdef   = 1 << 5
)

func drmIOWR(nr, size uintptr) uintptr { return 3<<30 | size<<16 | 'd'<<8 | nr }

func drmIoctl(fd int, nr uintptr, arg unsafe.Pointer, size uintptr) error {
	req := drmIOWR(nr, size)
	if size == 0 {
		req = 'd'<<8 | nr
	}
	for {
		_, _, e := unix.Syscall(unix.SYS_IOCTL, uintptr(fd), req, uintptr(arg))
		if e == unix.EINTR || e == unix.EAGAIN {
			continue
		}
		if e != 0 {
			return e
		}
		return nil
	}
}

// mode240p turns an interlaced mode into the progressive one with the same
// lines: half the lines a frame (DRM gives interlaced timings per frame).
func mode240p(m drmModeInfo) drmModeInfo {
	p := m
	p.vdisplay = m.vdisplay / 2
	p.vsyncStart = m.vsyncStart / 2
	p.vsyncEnd = m.vsyncEnd / 2
	p.vtotal = m.vtotal / 2
	p.flags &^= drmModeFlagInterlace | drmModeFlagDblscan
	p.typ = drmModeTypeUserdef
	if p.htotal > 0 && p.vtotal > 0 {
		p.vrefresh = uint32((uint64(p.clock)*1000 + uint64(p.htotal)*uint64(p.vtotal)/2) / (uint64(p.htotal) * uint64(p.vtotal)))
	}
	p.name = [32]byte{}
	copy(p.name[:], fmt.Sprintf("%dx%d", p.hdisplay, p.vdisplay))
	return p
}

// ---------------------------------------------------------------------------

type theme struct {
	regular, bold, small, title font.Face
	background, logo           image.Image
	icons                      map[string]image.Image
	accent, text, dim, status  color.RGBA
	top, bottom                color.RGBA
	size                       float64
}

type gfxScreen struct {
	tty      *os.File
	themeDir func() string // SIONBOOT's sion/menu, or ""
	fallback *screen       // the text menu, when DRM can't be used
	failed   bool

	fd           int
	crtc, conn   uint32
	saved        drmCrtc
	mode         drmModeInfo
	fb, handle   uint32
	pitch        int
	mem          []byte
	w, h         int
	canvas       *image.RGBA
	th           *theme
	loadedFrom   string
}

func newScreen(tty *os.File, themeDir func() string) menuScreen {
	text := &screen{tty: tty}
	if !gfxMenu {
		return text
	}
	return &gfxScreen{tty: tty, themeDir: themeDir, fallback: text, fd: -1,
		canvas: image.NewRGBA(image.Rect(0, 0, canvasW, canvasH))}
}

func (g *gfxScreen) open() error {
	var fd int
	var err error
	for _, p := range []string{"/dev/dri/card0", "/dev/dri/card1"} {
		if fd, err = unix.Open(p, unix.O_RDWR|unix.O_CLOEXEC, 0); err == nil {
			break
		}
	}
	if err != nil {
		return err
	}
	g.fd = fd
	if err := g.setup(); err != nil {
		g.close()
		return err
	}
	return nil
}

func (g *gfxScreen) setup() error {
	drmIoctl(g.fd, 0x1e, nil, 0) // SET_MASTER; the first opener already is
	var res drmCardRes
	if err := drmIoctl(g.fd, 0xa0, unsafe.Pointer(&res), unsafe.Sizeof(res)); err != nil {
		return fmt.Errorf("GETRESOURCES: %v", err)
	}
	if res.countConnectors == 0 || res.countCrtcs == 0 {
		return fmt.Errorf("no connectors")
	}
	conns := make([]uint32, res.countConnectors)
	crtcs := make([]uint32, res.countCrtcs)
	res2 := drmCardRes{connectors: uint64(uintptr(unsafe.Pointer(&conns[0]))), countConnectors: res.countConnectors,
		crtcs: uint64(uintptr(unsafe.Pointer(&crtcs[0]))), countCrtcs: res.countCrtcs}
	if err := drmIoctl(g.fd, 0xa0, unsafe.Pointer(&res2), unsafe.Sizeof(res2)); err != nil {
		return fmt.Errorf("GETRESOURCES: %v", err)
	}
	for _, id := range conns[:res2.countConnectors] {
		var c drmGetConnector
		c.connectorID = id
		if drmIoctl(g.fd, 0xa7, unsafe.Pointer(&c), unsafe.Sizeof(c)) != nil || c.connection != 1 || c.countModes == 0 {
			continue
		}
		modes := make([]drmModeInfo, c.countModes)
		encs := make([]uint32, c.countEncoders+1)
		c2 := drmGetConnector{connectorID: id, countModes: c.countModes, modes: uint64(uintptr(unsafe.Pointer(&modes[0]))),
			countEncoders: c.countEncoders, encoders: uint64(uintptr(unsafe.Pointer(&encs[0])))}
		if drmIoctl(g.fd, 0xa7, unsafe.Pointer(&c2), unsafe.Sizeof(c2)) != nil || c2.countModes == 0 {
			continue
		}
		crtc := uint32(0)
		if c2.encoderID != 0 {
			e := drmGetEncoder{encoderID: c2.encoderID}
			if drmIoctl(g.fd, 0xa6, unsafe.Pointer(&e), unsafe.Sizeof(e)) == nil {
				crtc = e.crtcID
			}
		}
		if crtc == 0 {
			for _, eid := range encs[:c2.countEncoders] {
				e := drmGetEncoder{encoderID: eid}
				if drmIoctl(g.fd, 0xa6, unsafe.Pointer(&e), unsafe.Sizeof(e)) != nil {
					continue
				}
				for i, cid := range crtcs[:res2.countCrtcs] {
					if e.possibleCrtcs&(1<<i) != 0 {
						crtc = cid
						break
					}
				}
				if crtc != 0 {
					break
				}
			}
		}
		if crtc == 0 {
			continue
		}
		g.conn, g.crtc = id, crtc
		g.saved = drmCrtc{crtcID: crtc}
		drmIoctl(g.fd, 0xa1, unsafe.Pointer(&g.saved), unsafe.Sizeof(g.saved))
		base := modes[0] // the preferred mode
		if g.saved.modeValid != 0 {
			base = g.saved.mode
		}
		var tries []drmModeInfo
		if menu240 && base.flags&drmModeFlagInterlace != 0 {
			tries = append(tries, mode240p(base))
		}
		tries = append(tries, base)
		var last error
		for _, m := range tries {
			if last = g.modeset(m); last == nil {
				logf("graphical menu: %s (%d Hz) on connector %d", strings.TrimRight(string(m.name[:]), "\x00"), m.vrefresh, id)
				return nil
			}
			logf("graphical menu: %dx%d%s refused: %v", m.hdisplay, m.vdisplay,
				map[bool]string{true: "i", false: "p"}[m.flags&drmModeFlagInterlace != 0], last)
		}
		return last
	}
	return fmt.Errorf("no connected display")
}

// modeset makes a framebuffer for m and shows it.
func (g *gfxScreen) modeset(m drmModeInfo) error {
	g.freeFb()
	cd := drmCreateDumb{width: uint32(m.hdisplay), height: uint32(m.vdisplay), bpp: 32}
	if err := drmIoctl(g.fd, 0xb2, unsafe.Pointer(&cd), unsafe.Sizeof(cd)); err != nil {
		return fmt.Errorf("CREATE_DUMB: %v", err)
	}
	g.handle, g.pitch = cd.handle, int(cd.pitch)
	fc := drmFbCmd{width: cd.width, height: cd.height, pitch: cd.pitch, bpp: 32, depth: 24, handle: cd.handle}
	if err := drmIoctl(g.fd, 0xae, unsafe.Pointer(&fc), unsafe.Sizeof(fc)); err != nil {
		g.freeFb()
		return fmt.Errorf("ADDFB: %v", err)
	}
	g.fb = fc.fbID
	md := drmMapDumb{handle: cd.handle}
	if err := drmIoctl(g.fd, 0xb3, unsafe.Pointer(&md), unsafe.Sizeof(md)); err != nil {
		g.freeFb()
		return fmt.Errorf("MAP_DUMB: %v", err)
	}
	mem, err := unix.Mmap(g.fd, int64(md.offset), int(cd.size), unix.PROT_READ|unix.PROT_WRITE, unix.MAP_SHARED)
	if err != nil {
		g.freeFb()
		return fmt.Errorf("mmap: %v", err)
	}
	g.mem, g.w, g.h = mem, int(m.hdisplay), int(m.vdisplay)
	g.paint() // whatever is on the canvas, so the switch shows no garbage
	conn := g.conn
	sc := drmCrtc{setConnectors: uint64(uintptr(unsafe.Pointer(&conn))), countConnectors: 1,
		crtcID: g.crtc, fbID: g.fb, modeValid: 1, mode: m}
	if err := drmIoctl(g.fd, 0xa2, unsafe.Pointer(&sc), unsafe.Sizeof(sc)); err != nil {
		g.freeFb()
		return err
	}
	g.mode = m
	return nil
}

func (g *gfxScreen) freeFb() {
	if g.mem != nil {
		unix.Munmap(g.mem)
		g.mem = nil
	}
	if g.fb != 0 {
		fb := g.fb
		drmIoctl(g.fd, 0xaf, unsafe.Pointer(&fb), 4)
		g.fb = 0
	}
	if g.handle != 0 {
		h := g.handle
		drmIoctl(g.fd, 0xb4, unsafe.Pointer(&h), 4)
		g.handle = 0
	}
}

func (g *gfxScreen) close() {
	if g.fd < 0 {
		return
	}
	g.freeFb()
	drmIoctl(g.fd, 0x1f, nil, 0) // DROP_MASTER
	// Closing the last DRM file brings the console's own mode and picture
	// back (the fbdev emulation restores itself on last close).
	unix.Close(g.fd)
	g.fd = -1
}

// paint scales the canvas into the scanout buffer (nearest pixel: 9x1 at
// 2880x240, 9x2 at 2880x480).
func (g *gfxScreen) paint() {
	scaleTo(g.canvas, g.mem, g.w, g.h, g.pitch)
}

func scaleTo(src *image.RGBA, dst []byte, w, h, pitch int) {
	if dst == nil || w <= 0 || h <= 0 {
		return
	}
	xmap := make([]int, w)
	for x := range xmap {
		xmap[x] = (x * canvasW / w) * 4
	}
	row := make([]byte, w*4)
	prev := -1
	for y := 0; y < h; y++ {
		sy := y * canvasH / h
		if sy != prev {
			s := src.Pix[sy*src.Stride:]
			for x, o := range xmap {
				row[x*4+0] = s[o+2]
				row[x*4+1] = s[o+1]
				row[x*4+2] = s[o+0]
				row[x*4+3] = 0xff
			}
			prev = sy
		}
		if off := y * pitch; off+len(row) <= len(dst) {
			copy(dst[off:], row)
		}
	}
}

// toFbdev copies the canvas into the console framebuffer, so the console
// shows the last menu picture when the screen goes back to it.
func (g *gfxScreen) toFbdev() {
	w, h, ok := fbSize()
	if !ok {
		return
	}
	stride := w * 4
	if b, err := os.ReadFile("/sys/class/graphics/fb0/stride"); err == nil {
		if n, err := strconv.Atoi(strings.TrimSpace(string(b))); err == nil && n > 0 {
			stride = n
		}
	}
	if b, err := os.ReadFile("/sys/class/graphics/fb0/bits_per_pixel"); err == nil && strings.TrimSpace(string(b)) != "32" {
		return
	}
	// Only the visible part: fb0's mode, not its virtual size.
	if b, err := os.ReadFile("/sys/class/graphics/fb0/modes"); err == nil {
		var mw, mh int
		var c rune
		if n, _ := fmt.Sscanf(strings.TrimSpace(string(b)), "U:%dx%d%c", &mw, &mh, &c); n >= 2 && mw <= w && mh <= h {
			w, h = mw, mh
		}
	}
	f, err := os.OpenFile("/dev/fb0", os.O_RDWR, 0)
	if err != nil {
		return
	}
	defer f.Close()
	mem, err := unix.Mmap(int(f.Fd()), 0, stride*h, unix.PROT_READ|unix.PROT_WRITE, unix.MAP_SHARED)
	if err != nil {
		return
	}
	scaleTo(g.canvas, mem, w, h, stride)
	unix.Munmap(mem)
}

func (g *gfxScreen) release() {
	if g.fd < 0 {
		return
	}
	// The console keeps its text underneath; clear it so only the copied
	// picture shows.
	fmt.Fprint(g.tty, "\x1b[0m\x1b[2J\x1b[H\x1b[?25l")
	g.toFbdev()
	g.close()
}

func (g *gfxScreen) draw(entries []entry, sel int, footer, status string) {
	if g.failed {
		g.fallback.draw(entries, sel, footer, status)
		return
	}
	dir := g.themeDir()
	if g.th == nil || dir != g.loadedFrom {
		g.th = loadTheme(dir)
		g.loadedFrom = dir
	}
	g.render(entries, sel, footer, status)
	if g.fd < 0 {
		if err := g.open(); err != nil {
			logf("graphical menu unavailable, using the text one: %v", err)
			g.failed = true
			g.fallback.draw(entries, sel, footer, status)
			return
		}
	}
	g.paint()
}

// ---------------------------------------------------------------------------
// Drawing on the 320x240 canvas.

func (g *gfxScreen) render(entries []entry, sel int, footer, status string) {
	th, c := g.th, g.canvas
	// Background: picture or gradient.
	if th.background != nil {
		xdraw.ApproxBiLinear.Scale(c, c.Bounds(), th.background, th.background.Bounds(), xdraw.Src, nil)
	} else {
		for y := 0; y < canvasH; y++ {
			col := lerp(th.top, th.bottom, float64(y)/float64(canvasH-1))
			for x := 0; x < canvasW; x++ {
				c.SetRGBA(x, y, col)
			}
		}
	}
	// Title (TVs crop about 5% each side: keep 16 px clear).
	y := 14
	if th.logo != nil {
		b := th.logo.Bounds()
		w, h := b.Dx(), b.Dy()
		if w > 288 {
			h, w = h*288/w, 288
		}
		if h > 40 {
			w, h = w*40/h, 40
		}
		r := image.Rect((canvasW-w)/2, y, (canvasW-w)/2+w, y+h)
		xdraw.ApproxBiLinear.Scale(c, r, th.logo, b, xdraw.Over, nil)
		y += 46
	} else {
		drawSpaced(c, th.title, "CRIMSON", y+26, th.accent, 4)
		fill(c, image.Rect(96, y+34, 224, y+35), th.accent)
		y += 46
	}

	// Entries.
	rowH := int(th.size) + 5
	n := len(entries)
	maxRows := (198 - y) / rowH
	first := 0
	if n > maxRows && maxRows > 0 {
		first = sel - maxRows/2
		if first < 0 {
			first = 0
		}
		if first > n-maxRows {
			first = n - maxRows
		}
	} else {
		maxRows = n
	}
	left, right := 44, canvasW-44
	for i := first; i < first+maxRows && i < n; i++ {
		top := y + (i-first)*rowH
		col := th.dim
		face := th.regular
		if i == sel {
			fill(c, image.Rect(left, top, right, top+rowH-2), th.accent)
			fill(c, image.Rect(left, top+rowH-2, right, top+rowH-1), darker(th.accent))
			col = th.text
			face = th.bold
		}
		tx := left + 8
		if ic := th.icons[iconFor(entries[i].label)]; ic != nil {
			r := image.Rect(tx, top+(rowH-2-16)/2, tx+16, top+(rowH-2-16)/2+16)
			xdraw.ApproxBiLinear.Scale(c, r, ic, ic.Bounds(), xdraw.Over, nil)
			tx += 22
		}
		asc := face.Metrics().Ascent.Ceil()
		drawText(c, face, entries[i].label, tx, top+(rowH-2+asc)/2-1, col)
	}

	if status != "" {
		drawCentered(c, th.small, status, 206, th.status)
	}
	drawCentered(c, th.small, footer, 222, th.dim)
}

func iconFor(label string) string {
	switch {
	case strings.HasPrefix(label, "Crimson"):
		return "crimson"
	case strings.HasPrefix(label, "UEFI"):
		return "uefi"
	case strings.Contains(label, "shell"):
		return "shell"
	case label == "Reboot":
		return "reboot"
	case label == "Power off":
		return "poweroff"
	}
	return ""
}

func fill(c *image.RGBA, r image.Rectangle, col color.RGBA) {
	xdraw.Draw(c, r, &image.Uniform{col}, image.Point{}, xdraw.Src)
}

func lerp(a, b color.RGBA, t float64) color.RGBA {
	m := func(x, y uint8) uint8 { return uint8(float64(x) + (float64(y)-float64(x))*t + 0.5) }
	return color.RGBA{m(a.R, b.R), m(a.G, b.G), m(a.B, b.B), 0xff}
}

func darker(c color.RGBA) color.RGBA {
	return color.RGBA{c.R / 2, c.G / 2, c.B / 2, 0xff}
}

func drawText(c *image.RGBA, face font.Face, s string, x, y int, col color.RGBA) {
	d := font.Drawer{Dst: c, Src: &image.Uniform{col}, Face: face, Dot: fixed.P(x, y)}
	d.DrawString(s)
}

func drawCentered(c *image.RGBA, face font.Face, s string, y int, col color.RGBA) {
	w := font.MeasureString(face, s).Ceil()
	drawText(c, face, s, (canvasW-w)/2, y, col)
}

// drawSpaced draws s centred with extra space between the letters.
func drawSpaced(c *image.RGBA, face font.Face, s string, y int, col color.RGBA, gap int) {
	w := 0
	for _, r := range s {
		w += font.MeasureString(face, string(r)).Ceil() + gap
	}
	x := (canvasW - w + gap) / 2
	for _, r := range s {
		drawText(c, face, string(r), x, y, col)
		x += font.MeasureString(face, string(r)).Ceil() + gap
	}
}

// ---------------------------------------------------------------------------
// The theme.

func loadTheme(dir string) *theme {
	th := &theme{
		accent: color.RGBA{0xc8, 0x10, 0x2e, 0xff},
		text:   color.RGBA{0xff, 0xff, 0xff, 0xff},
		dim:    color.RGBA{0xb4, 0xb4, 0xbc, 0xff},
		status: color.RGBA{0xff, 0xcc, 0x33, 0xff},
		top:    color.RGBA{0x2a, 0x04, 0x0c, 0xff},
		bottom: color.RGBA{0x04, 0x00, 0x02, 0xff},
		size:   13,
		icons:  map[string]image.Image{},
	}
	regular, bold := goregular.TTF, gobold.TTF
	if dir != "" {
		if f, err := os.Open(filepath.Join(dir, "theme.conf")); err == nil {
			sc := bufio.NewScanner(f)
			for sc.Scan() {
				k, v, ok := strings.Cut(strings.TrimSpace(sc.Text()), "=")
				if !ok || strings.HasPrefix(k, "#") {
					continue
				}
				k, v = strings.TrimSpace(k), strings.TrimSpace(v)
				if k == "size" {
					if n, err := strconv.ParseFloat(v, 64); err == nil && n >= 8 && n <= 24 {
						th.size = n
					}
					continue
				}
				col, ok := parseColor(v)
				if !ok {
					continue
				}
				switch k {
				case "accent":
					th.accent = col
				case "text":
					th.text = col
				case "dim":
					th.dim = col
				case "status":
					th.status = col
				case "top":
					th.top = col
				case "bottom":
					th.bottom = col
				}
			}
			f.Close()
		}
		if b, err := os.ReadFile(filepath.Join(dir, "font.ttf")); err == nil {
			regular, bold = b, b
		}
		if b, err := os.ReadFile(filepath.Join(dir, "font-bold.ttf")); err == nil {
			bold = b
		}
		th.background = loadPNG(filepath.Join(dir, "background.png"))
		th.logo = loadPNG(filepath.Join(dir, "logo.png"))
		for _, n := range []string{"crimson", "uefi", "shell", "reboot", "poweroff"} {
			if im := loadPNG(filepath.Join(dir, "icons", n+".png")); im != nil {
				th.icons[n] = im
			}
		}
	}
	th.regular = face(regular, goregular.TTF, th.size)
	th.bold = face(bold, gobold.TTF, th.size)
	th.small = face(regular, goregular.TTF, 10)
	th.title = face(bold, gobold.TTF, 26)
	return th
}

func face(data, fallback []byte, size float64) font.Face {
	for _, d := range [][]byte{data, fallback} {
		f, err := opentype.Parse(d)
		if err != nil {
			continue
		}
		fc, err := opentype.NewFace(f, &opentype.FaceOptions{Size: size, DPI: 72, Hinting: font.HintingFull})
		if err == nil {
			return fc
		}
	}
	return nil
}

func loadPNG(p string) image.Image {
	f, err := os.Open(p)
	if err != nil {
		return nil
	}
	defer f.Close()
	im, err := png.Decode(f)
	if err != nil {
		logf("%s: %v", p, err)
		return nil
	}
	return im
}

func parseColor(s string) (color.RGBA, bool) {
	s = strings.TrimPrefix(s, "#")
	if len(s) != 6 {
		return color.RGBA{}, false
	}
	v, err := strconv.ParseUint(s, 16, 32)
	if err != nil {
		return color.RGBA{}, false
	}
	return color.RGBA{uint8(v >> 16), uint8(v >> 8), uint8(v), 0xff}, true
}
