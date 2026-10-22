// frolick-store: Frolick's storefront on the Sion, from the Internet
// Archive (archive.org).
//
// Frolick runs as an Xbox title under xbcompat and has no TLS, so this
// helper does the archive.org work on the Linux side and talks to the
// dashboard through files on its hard disk (E:\FROLICK\STORE, which is
// /data/xbox/hdd/partition1/FROLICK/STORE on the Sion):
//
//	catalog.txt  written here: Spotlight's tabs and their items (search +
//	             /metadata/{id}), refreshed at start and every few hours
//	img/<n>.png  written here: each item's tile (/services/img/{id}), 96x72
//	queue.txt    written by the dashboard (nodes/Downloads.cpp): the
//	             download queue, first line the active download
//	status.txt   written here: whether the Archive answers, what the active
//	             download is doing, what the others already have on disk,
//	             and which queued items have finished
//
// Downloads go to /data/frolick/downloads/<identifier>/: the item's game
// files (pickFiles), fetched from /download/{id}/{file} with redirects
// followed and resumed with Range from <file>.part.  Then it becomes a
// Windows game card (cart.go) and the item keeps only frolick-item.txt.
//
// Text files are UTF-8, tab-separated, and end with a line "end" so that a
// reader never acts on a half-written file.  Not part of Microsoft's
// dashboard; added for Frolick (tools/frolickdash).
package main

import (
	"bufio"
	"bytes"
	"context"
	"crypto/tls"
	"crypto/x509"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"html"
	"image"
	"image/draw"
	_ "image/gif"
	_ "image/jpeg"
	"image/png"
	"io"
	"log"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"

	"golang.org/x/crypto/x509roots/fallback/bundle"
)

const userAgent = "frolick-store/1 (Frolick dashboard on the Sion)"

var (
	storeDir = flag.String("store", "/data/xbox/hdd/partition1/FROLICK/STORE", "the dashboard's store folder (E:\\FROLICK\\STORE)")
	dlDir    = flag.String("downloads", "/data/frolick/downloads", "where downloaded items go")
	confPath = flag.String("conf", "/data/frolick/store.conf", "tabs and their searches (built-in defaults when missing)")
	refresh  = flag.Duration("refresh", 6*time.Hour, "how often the catalog is rebuilt")
	once     = flag.Bool("once", false, "build the catalog once and exit (no downloads)")
	base     = flag.String("base", "https://archive.org", "the Internet Archive")
)

// ---- tabs ----------------------------------------------------------------

// A Spotlight tab: a hand-picked list of identifiers, or an advanced
// search, newest-popular first.  Kind is the second line of its rows.
type tab struct {
	Name  string
	Kind  string
	Query string
	IDs   []string
	Rows  int
}

// The built-in tabs (store.conf replaces them).  Windows games only for
// now (sokky, 2026-10-22); Featured is a hand-picked list of free,
// shareware and demo releases.
var defaultTabs = []tab{
	{Name: "Featured", Kind: "Windows game", IDs: []string{
		"oni-ae",
		"DiabloSharewareWindowsBlizzardEntertainment1997",
		"SURVEY_PROGRAM",
		"CometBusters14Image",
		"bejeweled_blitz_demo",
		"UltimateRideKelloggs",
		"tennis-critters-full-english-version",
		"quibbleracev1.5",
	}},
	{Name: "Demos", Kind: "Demo", Rows: 24,
		Query: "mediatype:software AND subject:(windows) AND subject:(game) AND subject:(demo)"},
	{Name: "Shareware", Kind: "Shareware", Rows: 24,
		Query: "mediatype:software AND subject:(windows) AND subject:(game) AND subject:(shareware)"},
	{Name: "Freeware", Kind: "Freeware", Rows: 24,
		Query: "mediatype:software AND subject:(windows) AND subject:(game) AND subject:(freeware)"},
}

// store.conf:
//
//	tab <name> | <kind>
//	query <lucene query>       (the tab's search) or
//	id <identifier>            (one per line: a hand-picked list)
//	rows <n>
func loadTabs(path string) []tab {
	f, err := os.Open(path)
	if err != nil {
		return defaultTabs
	}
	defer f.Close()
	var tabs []tab
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		line := strings.TrimSpace(sc.Text())
		if line == "" || line[0] == '#' {
			continue
		}
		key, val, _ := strings.Cut(line, " ")
		val = strings.TrimSpace(val)
		if key == "tab" {
			name, kind, _ := strings.Cut(val, "|")
			tabs = append(tabs, tab{Name: strings.TrimSpace(name), Kind: strings.TrimSpace(kind)})
			continue
		}
		if len(tabs) == 0 {
			continue
		}
		t := &tabs[len(tabs)-1]
		switch key {
		case "query":
			t.Query = val
		case "id":
			t.IDs = append(t.IDs, val)
		case "rows":
			t.Rows, _ = strconv.Atoi(val)
		}
	}
	if len(tabs) == 0 {
		return defaultTabs
	}
	return tabs
}

// ---- HTTP ----------------------------------------------------------------

// The Sion has no CA store of its own: the Mozilla roots are built in.
func newClient() *http.Client {
	pool := x509.NewCertPool()
	for c := range bundle.Roots() {
		cert, err := x509.ParseCertificate(c.Certificate)
		if err == nil {
			pool.AddCert(cert)
		}
	}
	if sys, err := x509.SystemCertPool(); err == nil && len(sys.Subjects()) > 0 { //nolint:staticcheck
		pool = sys
		for c := range bundle.Roots() {
			if cert, err := x509.ParseCertificate(c.Certificate); err == nil {
				pool.AddCert(cert)
			}
		}
	}
	tr := http.DefaultTransport.(*http.Transport).Clone()
	tr.TLSClientConfig = &tls.Config{RootCAs: pool}
	tr.ResponseHeaderTimeout = 60 * time.Second
	return &http.Client{Transport: tr}
}

var client *http.Client

func get(ctx context.Context, u string, hdr map[string]string) (*http.Response, error) {
	req, err := http.NewRequestWithContext(ctx, "GET", u, nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("User-Agent", userAgent)
	for k, v := range hdr {
		req.Header.Set(k, v)
	}
	return client.Do(req)
}

func getJSON(ctx context.Context, u string, v any) error {
	ctx, cancel := context.WithTimeout(ctx, 90*time.Second)
	defer cancel()
	resp, err := get(ctx, u, nil)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		return fmt.Errorf("%s: HTTP %d", u, resp.StatusCode)
	}
	return json.NewDecoder(resp.Body).Decode(v)
}

// ---- catalog -------------------------------------------------------------

type iaFile struct {
	Name   string `json:"name"`
	Format string `json:"format"`
	Source string `json:"source"`
	Size   string `json:"size"`
}

type iaMeta struct {
	Metadata map[string]any `json:"metadata"`
	Files    []iaFile       `json:"files"`
}

func metaString(m map[string]any, k string) string {
	switch v := m[k].(type) {
	case string:
		return v
	case []any:
		var s []string
		for _, x := range v {
			if t, ok := x.(string); ok {
				s = append(s, t)
			}
		}
		return strings.Join(s, " ")
	}
	return ""
}

// The game's files out of an item's file list: its disc images when it
// has any (every disc; a .cue keeps its .bin), else its installers (a GOG
// setup .exe and its .bin parts), else its largest archive.  Metadata,
// torrents, thumbnails, scans, covers and manuals are left out.
var (
	reDisc    = regexp.MustCompile(`(?i)\.(iso|cue|bin|img|mdf|mds|ccd|sub|nrg|chd)$`)
	reSetup   = regexp.MustCompile(`(?i)\.(exe|msi)$`)
	reArchive = regexp.MustCompile(`(?i)\.(zip|7z|rar)$`)
	reExtras  = regexp.MustCompile(`(?i)(scan|cover|manual|booklet|artwork|box|insert|_meta|_files|_reviews|__ia_thumb|mdf\+mds)`)
)

func pickFiles(files []iaFile) []iaFile {
	var disc, setup, arch []iaFile
	for _, f := range files {
		if f.Source != "original" || f.Format == "Metadata" || strings.Contains(f.Format, "Torrent") || reExtras.MatchString(f.Name) {
			continue
		}
		switch {
		case reDisc.MatchString(f.Name):
			disc = append(disc, f)
		case reSetup.MatchString(f.Name):
			setup = append(setup, f)
		case reArchive.MatchString(f.Name):
			arch = append(arch, f)
		}
	}
	if len(setup) > 0 && len(disc) > 0 {
		// GOG-style: setup_x.exe with setup_x-1.bin parts
		for _, d := range disc {
			if strings.HasSuffix(strings.ToLower(d.Name), ".bin") && strings.HasPrefix(strings.ToLower(d.Name), "setup") {
				return append(setup, disc...)
			}
		}
	}
	if len(disc) > 0 {
		// an .mds/.ccd beside an .iso is the same disc twice: keep the .iso set
		var iso []iaFile
		for _, d := range disc {
			if strings.HasSuffix(strings.ToLower(d.Name), ".iso") {
				iso = append(iso, d)
			}
		}
		if len(iso) > 0 {
			return iso
		}
		return disc
	}
	if len(setup) > 0 {
		return setup
	}
	if len(arch) > 0 {
		sort.Slice(arch, func(i, j int) bool { return size(arch[i]) > size(arch[j]) })
		return arch[:1]
	}
	return nil
}

func size(f iaFile) int64 {
	n, _ := strconv.ParseInt(f.Size, 10, 64)
	return n
}

type item struct {
	ID    string
	Title string
	Kind  string
	Desc  string
	Size  int64
	Files []iaFile
}

var reTags = regexp.MustCompile(`(?s)<[^>]*>`)
var reSpace = regexp.MustCompile(`\s+`)

// One line of plain text out of an item's description (HTML on archive.org).
func plain(s string, max int) string {
	s = strings.NewReplacer("<br>", " ", "<br/>", " ", "<br />", " ", "</p>", " ", "</div>", " ").Replace(s)
	s = html.UnescapeString(reTags.ReplaceAllString(s, ""))
	s = clean(s)
	if r := []rune(s); len(r) > max {
		s = strings.TrimSpace(string(r[:max-1])) + "…"
	}
	return s
}

// Text for the dashboard: one line, and only what its font (DejaVu Sans
// in the XIP) is sure to have: Latin letters, digits, punctuation and
// common symbols.  Box drawing, block art, CJK and emoji are dropped.
func clean(s string) string {
	var b strings.Builder
	for _, r := range s {
		switch {
		case r == '\t' || r == '\r' || r == '\n' || r == 0xA0 || r >= 0x2000 && r <= 0x200B || r == 0x202F:
			// (the dashboard wraps text at plain spaces only)
			b.WriteRune(' ')
		case r < 0x20:
		case r < 0x250, r >= 0x2000 && r < 0x2070, r == 0x20AC, r == 0x2122:
			b.WriteRune(r)
		default:
			b.WriteRune(' ')
		}
	}
	return strings.TrimSpace(reSpace.ReplaceAllString(b.String(), " "))
}

// Mostly in a script the dashboard can't show (a Japanese title, say).
func unreadable(s string) bool {
	n, bad := 0, 0
	for _, r := range s {
		if r == ' ' {
			continue
		}
		n++
		if !(r < 0x250 || r >= 0x2000 && r < 0x2070) {
			bad++
		}
	}
	return n == 0 || bad*3 > n
}

var (
	metaMu    sync.Mutex
	metaCache = map[string]*iaMeta{}
)

func metadata(ctx context.Context, id string) (*iaMeta, error) {
	metaMu.Lock()
	m0, ok := metaCache[id]
	metaMu.Unlock()
	if ok {
		return m0, nil
	}
	var m iaMeta
	if err := getJSON(ctx, *base+"/metadata/"+url.PathEscape(id), &m); err != nil {
		return nil, err
	}
	if m.Metadata == nil {
		return nil, fmt.Errorf("%s: no such item", id)
	}
	metaMu.Lock()
	metaCache[id] = &m
	metaMu.Unlock()
	return &m, nil
}

func search(ctx context.Context, q string, rows int) ([]string, error) {
	v := url.Values{}
	v.Set("q", q)
	v.Add("fl[]", "identifier")
	v.Add("sort[]", "downloads desc")
	v.Set("rows", strconv.Itoa(rows))
	v.Set("page", "1")
	v.Set("output", "json")
	var r struct {
		Response struct {
			Docs []struct {
				Identifier string `json:"identifier"`
			} `json:"docs"`
		} `json:"response"`
	}
	if err := getJSON(ctx, *base+"/advancedsearch.php?"+v.Encode(), &r); err != nil {
		return nil, err
	}
	var ids []string
	for _, d := range r.Response.Docs {
		ids = append(ids, d.Identifier)
	}
	return ids, nil
}

func makeItem(ctx context.Context, id, kind string) (*item, error) {
	m, err := metadata(ctx, id)
	if err != nil {
		return nil, err
	}
	files := pickFiles(m.Files)
	if len(files) == 0 {
		return nil, fmt.Errorf("%s: no game files", id)
	}
	it := &item{ID: id, Kind: kind, Files: files}
	title := metaString(m.Metadata, "title")
	if unreadable(title) {
		return nil, fmt.Errorf("%s: title not in Latin script", id)
	}
	it.Title = clean(title)
	it.Desc = plain(metaString(m.Metadata, "description"), 400)
	for _, f := range files {
		it.Size += size(f)
	}
	return it, nil
}

// A small PNG tile for an item (the dashboard decodes PNG, not JPEG).
func fetchTile(ctx context.Context, id, path string) {
	if st, err := os.Stat(path); err == nil && time.Since(st.ModTime()) < 30*24*time.Hour {
		return
	}
	ctx, cancel := context.WithTimeout(ctx, 60*time.Second)
	defer cancel()
	resp, err := get(ctx, *base+"/services/img/"+url.PathEscape(id), nil)
	if err != nil {
		return
	}
	defer resp.Body.Close()
	if resp.StatusCode != 200 {
		return
	}
	src, _, err := image.Decode(io.LimitReader(resp.Body, 8<<20))
	if err != nil {
		return
	}
	dst := fit(src, 96, 72)
	var buf bytes.Buffer
	if png.Encode(&buf, dst) == nil {
		writeAtomic(path, buf.Bytes())
	}
}

// Scales src (box filter) to fit w x h, centred on transparent.
func fit(src image.Image, w, h int) *image.NRGBA {
	b := src.Bounds()
	sw, sh := b.Dx(), b.Dy()
	dst := image.NewNRGBA(image.Rect(0, 0, w, h))
	if sw == 0 || sh == 0 {
		return dst
	}
	scale := float64(w) / float64(sw)
	if s := float64(h) / float64(sh); s < scale {
		scale = s
	}
	tw, th := int(float64(sw)*scale+0.5), int(float64(sh)*scale+0.5)
	ox, oy := (w-tw)/2, (h-th)/2
	rgba := image.NewNRGBA(b)
	draw.Draw(rgba, b, src, b.Min, draw.Src)
	for y := 0; y < th; y++ {
		y0, y1 := b.Min.Y+y*sh/th, b.Min.Y+(y+1)*sh/th
		if y1 <= y0 {
			y1 = y0 + 1
		}
		for x := 0; x < tw; x++ {
			x0, x1 := b.Min.X+x*sw/tw, b.Min.X+(x+1)*sw/tw
			if x1 <= x0 {
				x1 = x0 + 1
			}
			var r, g, bl, a, n uint32
			for yy := y0; yy < y1; yy++ {
				for xx := x0; xx < x1; xx++ {
					o := rgba.PixOffset(xx, yy)
					p := rgba.Pix[o : o+4]
					r += uint32(p[0])
					g += uint32(p[1])
					bl += uint32(p[2])
					a += uint32(p[3])
					n++
				}
			}
			o := dst.PixOffset(ox+x, oy+y)
			dst.Pix[o], dst.Pix[o+1], dst.Pix[o+2], dst.Pix[o+3] = uint8(r/n), uint8(g/n), uint8(bl/n), uint8(a/n)
		}
	}
	return dst
}

// Tile file names: the dashboard's drive is FATX-like, so a short name.
func tileName(id string) string {
	h := uint32(2166136261)
	for i := 0; i < len(id); i++ {
		h = (h ^ uint32(id[i])) * 16777619
	}
	return fmt.Sprintf("%08x.png", h)
}

func buildCatalog(ctx context.Context, tabs []tab) error {
	var out strings.Builder
	out.WriteString("frolick-catalog 1\n")
	fmt.Fprintf(&out, "updated\t%d\n", time.Now().Unix())
	okTabs := 0
	os.MkdirAll(filepath.Join(*storeDir, "img"), 0755)
	for ti, t := range tabs {
		fmt.Fprintf(&out, "tab\t%d\t%s\n", ti, clean(t.Name))
		ids := t.IDs
		if t.Query != "" {
			rows := t.Rows
			if rows <= 0 {
				rows = 24
			}
			found, err := search(ctx, t.Query, rows)
			if err != nil {
				log.Printf("tab %s: %v", t.Name, err)
				continue
			}
			ids = append(append([]string{}, ids...), found...)
		}
		okTabs++
		n := 0
		for _, id := range ids {
			it, err := makeItem(ctx, id, t.Kind)
			if err != nil {
				log.Printf("tab %s: %v", t.Name, err)
				continue
			}
			tile := tileName(id)
			fetchTile(ctx, id, filepath.Join(*storeDir, "img", tile))
			if _, err := os.Stat(filepath.Join(*storeDir, "img", tile)); err != nil {
				tile = ""
			}
			fmt.Fprintf(&out, "item\t%d\t%s\t%d\t%s\t%s\t%s\t%s\n", ti, it.ID, it.Size, it.Kind, tile, it.Title, it.Desc)
			n++
		}
		log.Printf("tab %s: %d items", t.Name, n)
	}
	if okTabs == 0 {
		return errors.New("the Internet Archive could not be reached")
	}
	out.WriteString("end\n")
	return writeAtomic(filepath.Join(*storeDir, "catalog.txt"), []byte(out.String()))
}

func writeAtomic(path string, b []byte) error {
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, b, 0644); err != nil {
		return err
	}
	return os.Rename(tmp, path)
}

// ---- downloads -----------------------------------------------------------

type queued struct {
	ID    string
	Go    bool
	Title string
	Kind  string
}

// queue.txt, written by the dashboard:
//
//	frolick-queue 1
//	q <id> <go|pause> <bytes> <kind> <title>
//	end
func readQueue() ([]queued, bool) {
	b, err := os.ReadFile(filepath.Join(*storeDir, "queue.txt"))
	if err != nil {
		return nil, os.IsNotExist(err)
	}
	var q []queued
	complete := false
	for _, line := range strings.Split(strings.ReplaceAll(string(b), "\r", ""), "\n") {
		f := strings.Split(line, "\t")
		if f[0] == "end" {
			complete = true
			break
		}
		if f[0] == "q" && len(f) >= 6 {
			q = append(q, queued{ID: f[1], Go: f[2] == "go", Kind: f[4], Title: f[5]})
		}
	}
	return q, complete
}

type status struct {
	ID     string
	State  string // downloading, waiting, error, nospace
	Done   int64
	Total  int64
	Detail string
}

// cur is shared by the main loop and the download goroutine: curMu.
var (
	curMu    sync.Mutex
	cur      status
	catState = "Connecting to the Internet Archive"
	online   = 0
)

func writeStatus(queue []queued) {
	var out strings.Builder
	out.WriteString("frolick-status 1\n")
	fmt.Fprintf(&out, "online\t%d\t%s\n", online, catState)
	curMu.Lock()
	c := cur
	curMu.Unlock()
	if c.ID != "" {
		fmt.Fprintf(&out, "active\t%s\t%s\t%d\t%d\t%s\n", c.ID, c.State, c.Done, c.Total, clean(c.Detail))
	}
	for _, q := range queue {
		if finished(q.ID) {
			fmt.Fprintf(&out, "done\t%s\n", q.ID)
		} else if q.ID != c.ID {
			if n := bytesIn(itemDir(q.ID)); n > 0 {
				fmt.Fprintf(&out, "have\t%s\t%d\n", q.ID, n)
			}
		}
	}
	out.WriteString("end\n")
	writeAtomic(filepath.Join(*storeDir, "status.txt"), []byte(out.String()))
}

// What a paused or waiting item already has on disk.
func bytesIn(dir string) int64 {
	var n int64
	filepath.WalkDir(dir, func(_ string, d os.DirEntry, err error) error {
		if err == nil && !d.IsDir() {
			if st, err := d.Info(); err == nil {
				n += st.Size()
			}
		}
		return nil
	})
	return n
}

func itemDir(id string) string { return filepath.Join(*dlDir, filepath.Base(id)) }

func finished(id string) bool {
	_, err := os.Stat(filepath.Join(itemDir(id), "frolick-item.txt"))
	return err == nil
}

func freeBytes(path string) int64 {
	var st syscall.Statfs_t
	if syscall.Statfs(path, &st) != nil {
		return -1
	}
	return int64(st.Bavail) * int64(st.Bsize)
}

// Downloads item id's game files, resuming each from its .part.  Stops
// (keeping what it has) when ctx is cancelled.
func download(ctx context.Context, q queued, report func()) error {
	m, err := metadata(ctx, q.ID)
	if err != nil {
		return err
	}
	files := pickFiles(m.Files)
	if len(files) == 0 {
		return fmt.Errorf("no game files in %s", q.ID)
	}
	dir := itemDir(q.ID)
	if err := os.MkdirAll(dir, 0755); err != nil {
		return err
	}
	var total, have int64
	for _, f := range files {
		total += size(f)
		have += sizeOnDisk(dir, f)
	}
	setCur(func(c *status) { c.Total, c.Done = total, have })
	if free := freeBytes(dir); free >= 0 && free < total-have+256<<20 {
		setCur(func(c *status) {
			c.State = "nospace"
			c.Detail = fmt.Sprintf("Needs %d MB free", (total-have)>>20)
		})
		report()
		return errNoSpace
	}
	for _, f := range files {
		if err := fetchFile(ctx, q.ID, dir, f, report); err != nil {
			return err
		}
	}
	// the game card
	setCur(func(c *status) { c.State, c.Detail = "installing", "" })
	report()
	mode, err := makeCart(q, clean(metaString(m.Metadata, "title")), clean(metaString(m.Metadata, "creator")), dir, files)
	if err != nil {
		log.Printf("%s: no game card: %v", q.ID, err)
		mode = "none"
	}
	var rec strings.Builder
	fmt.Fprintf(&rec, "identifier\t%s\ntitle\t%s\nkind\t%s\nsource\t%s/details/%s\ncard\t%s\t%s\n", q.ID, clean(metaString(m.Metadata, "title")), q.Kind, *base, q.ID, cartID(q.ID), mode)
	for _, f := range files {
		fmt.Fprintf(&rec, "file\t%s\t%s\n", f.Name, f.Size)
	}
	rec.WriteString("credit\tFrom the Internet Archive (archive.org)\nend\n")
	return writeAtomic(filepath.Join(dir, "frolick-item.txt"), []byte(rec.String()))
}

func setCur(f func(*status)) {
	curMu.Lock()
	f(&cur)
	curMu.Unlock()
}

var errNoSpace = errors.New("not enough free space")

func localName(dir string, f iaFile) string {
	// item files can sit in subfolders on archive.org
	return filepath.Join(dir, filepath.FromSlash(strings.TrimLeft(filepath.Clean("/"+f.Name), "/")))
}

func sizeOnDisk(dir string, f iaFile) int64 {
	p := localName(dir, f)
	if st, err := os.Stat(p); err == nil {
		return st.Size()
	}
	if st, err := os.Stat(p + ".part"); err == nil {
		return st.Size()
	}
	return 0
}

func fetchFile(ctx context.Context, id, dir string, f iaFile, report func()) error {
	dst := localName(dir, f)
	want := size(f)
	if st, err := os.Stat(dst); err == nil && (want == 0 || st.Size() == want) {
		return nil
	}
	os.MkdirAll(filepath.Dir(dst), 0755)
	part := dst + ".part"
	var have int64
	if st, err := os.Stat(part); err == nil {
		have = st.Size()
	}
	if want > 0 && have > want {
		os.Remove(part)
		d := have
		setCur(func(c *status) { c.Done -= d })
		have = 0
	}
	var esc []string
	for _, s := range strings.Split(f.Name, "/") {
		esc = append(esc, url.PathEscape(s))
	}
	u := *base + "/download/" + url.PathEscape(id) + "/" + strings.Join(esc, "/")
	hdr := map[string]string{}
	if have > 0 {
		hdr["Range"] = fmt.Sprintf("bytes=%d-", have)
	}
	resp, err := get(ctx, u, hdr)
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	flags := os.O_CREATE | os.O_WRONLY
	switch {
	case resp.StatusCode == 206:
		flags |= os.O_APPEND
	case resp.StatusCode == 200:
		flags |= os.O_TRUNC
		d := have
		setCur(func(c *status) { c.Done -= d })
		have = 0
	case resp.StatusCode == 416 && want > 0 && have == want:
		return os.Rename(part, dst)
	default:
		return fmt.Errorf("%s: HTTP %d", f.Name, resp.StatusCode)
	}
	out, err := os.OpenFile(part, flags, 0644)
	if err != nil {
		return err
	}
	buf := make([]byte, 256<<10)
	last := time.Now()
	for {
		n, rerr := resp.Body.Read(buf)
		if n > 0 {
			if _, err := out.Write(buf[:n]); err != nil {
				out.Close()
				return err
			}
			have += int64(n)
			setCur(func(c *status) { c.Done += int64(n) })
			if time.Since(last) > 500*time.Millisecond {
				last = time.Now()
				report()
			}
		}
		if rerr == io.EOF {
			break
		}
		if rerr != nil {
			out.Close()
			return rerr
		}
	}
	if err := out.Close(); err != nil {
		return err
	}
	if want > 0 && have != want {
		return fmt.Errorf("%s: got %d of %d bytes", f.Name, have, want)
	}
	return os.Rename(part, dst)
}

// Partial downloads of items no longer queued are deleted (the dashboard
// removed them); finished items are kept.
func prune(queue []queued) {
	keep := map[string]bool{}
	for _, q := range queue {
		keep[filepath.Base(q.ID)] = true
	}
	ents, _ := os.ReadDir(*dlDir)
	for _, e := range ents {
		if !e.IsDir() || keep[e.Name()] || finished(e.Name()) {
			continue
		}
		os.RemoveAll(filepath.Join(*dlDir, e.Name()))
	}
}

// ---- main ----------------------------------------------------------------

func main() {
	flag.Parse()
	log.SetFlags(log.LstdFlags)
	client = newClient()
	os.MkdirAll(*storeDir, 0755)
	ctx := context.Background()
	tabs := loadTabs(*confPath)

	if *once {
		if err := buildCatalog(ctx, tabs); err != nil {
			log.Fatal(err)
		}
		return
	}
	os.MkdirAll(*dlDir, 0755)

	// The catalog in the background: at start (retried every minute until
	// it works), then every -refresh.
	catDone := make(chan error, 1)
	go func() {
		for {
			err := buildCatalog(ctx, tabs)
			catDone <- err
			if err != nil {
				time.Sleep(time.Minute)
				continue
			}
			time.Sleep(*refresh)
		}
	}()

	var (
		active    string
		cancelDL  context.CancelFunc
		dlResult  = make(chan error, 1)
		backoff   time.Time
		lastQueue []queued
	)
	report := make(chan struct{}, 1)
	poke := func() {
		select {
		case report <- struct{}{}:
		default:
		}
	}
	tick := time.NewTicker(time.Second)
	lastWatch := time.Time{}
	for {
		if time.Since(lastWatch) > 15*time.Second {
			lastWatch = time.Now()
			recheckInstallers()
			watchInstalls()
		}
		watchSession()
		select {
		case err := <-catDone:
			if err != nil {
				online, catState = 0, "Can't reach the Internet Archive"
				log.Printf("catalog: %v", err)
			} else {
				online, catState = 1, "From the Internet Archive"
			}
		case err := <-dlResult:
			cancelDL = nil
			switch {
			case err == nil:
				log.Printf("%s: finished", active)
				setCur(func(c *status) { *c = status{} })
			case errors.Is(err, context.Canceled):
			case errors.Is(err, errNoSpace):
				backoff = time.Now().Add(30 * time.Second)
			default:
				log.Printf("%s: %v", active, err)
				setCur(func(c *status) { c.State, c.Detail = "error", "Download failed, retrying" })
				backoff = time.Now().Add(20 * time.Second)
			}
			active = ""
		case <-report:
		case <-tick.C:
		}

		queue, ok := readQueue()
		if !ok {
			queue = lastQueue // half-written: the last good one
		}
		lastQueue = queue
		var want *queued
		for i := range queue {
			if !finished(queue[i].ID) {
				if queue[i].Go {
					want = &queue[i]
				}
				break
			}
		}
		if active != "" && (want == nil || want.ID != active) {
			if cancelDL != nil {
				cancelDL()
				<-dlResult
				cancelDL = nil
			}
			active = ""
			setCur(func(c *status) { *c = status{} })
		}
		if want != nil && active == "" && time.Now().After(backoff) {
			active = want.ID
			id := want.ID
			setCur(func(c *status) { *c = status{ID: id, State: "downloading"} })
			c, cancel := context.WithCancel(ctx)
			cancelDL = cancel
			q := *want
			go func() { dlResult <- download(c, q, poke) }()
		}
		if ok {
			prune(queue)
		}
		writeStatus(queue)
	}
}
