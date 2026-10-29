package main

// Windows games' Wine setups and the runtimes they run in, on the
// dashboards' Memory screens.
//
// A Windows game's Wine prefixes and what it writes (wincart-run's
// /data/wincarts/<Id>: pfx*, game/, logs/) and the runtimes (Conty,
// /data/conty_lite.sh; Proton builds in /data/proton) live outside the Xbox
// hard disk, so the Memory screens can't see them.  They are shown as
// downloads (TDATA\<title>\$C\<item>, which the Memory screens list under
// the title with its saves): a folder holding ContentMeta.xbx (its name)
// and frolick-size.dat, a sparse file as long as what it stands for.  The
// prefixes themselves are never put on E: (a prefix's dosdevices\z: links
// to /, which the dashboard would walk and delete through).
//
//   - each card game's "Windows setup" under its own title (store games
//     already count theirs in their E:\Games stub, uninstall.go);
//     deleting it removes its prefixes, game/ and logs/ (the game makes a
//     new prefix on its next launch); overrides and save-dirs stay, and its
//     saves are in UDATA, apart;
//   - "Windows Runtimes" (title 57494E52, "WINR"): Conty, which every
//     Windows game runs in, comes back if it is deleted; a Proton build is
//     only for tests (WINCART_PROTON) and goes.
//
// An item the helper made has a marker (.frolick-setup in the cart's
// folder, runtimes/<item>.listed in the store's folder): a marked item
// whose folder is gone was deleted from a Memory screen.

import (
	"encoding/binary"
	"log"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"unicode/utf16"
)

const (
	runtimesTitle = 0x57494E52
	setupItem     = "Windows Setup"
	setupMarker   = ".frolick-setup"
)

var (
	contyFile  = "/data/conty_lite.sh"
	protonDir  = "/data/proton"
	wincartRun = "/var/run/wincart"
)

// E:\TDATA, in whatever case it was made.
func tdataDir() string {
	p1 := filepath.Join(*hddDir, "partition1")
	ents, _ := os.ReadDir(p1)
	for _, e := range ents {
		if strings.EqualFold(e.Name(), "TDATA") {
			return filepath.Join(p1, e.Name())
		}
	}
	return filepath.Join(p1, "TDATA")
}

func contentDir(tid uint32, item string) string {
	return filepath.Join(tdataDir(), strings.ToLower(strconv.FormatUint(uint64(tid), 16)), "$C", item)
}

func syncWinSetups() {
	if st, err := os.Stat(tdataDir()); err != nil || !st.IsDir() {
		return // no TDATA: never take that for a deletion
	}
	running := ""
	if b, err := os.ReadFile(filepath.Join(wincartRun, "gamescope.pid")); err == nil {
		if pid, err := strconv.Atoi(strings.TrimSpace(string(b))); err == nil && syscall.Kill(pid, 0) == nil {
			running = "?" // a Windows game is running: leave every prefix alone
			if c, err := os.ReadFile(filepath.Join(wincartRun, "cart")); err == nil {
				running = strings.TrimSpace(string(c))
			}
		}
	}
	ents, _ := os.ReadDir(*wincartDir)
	for _, e := range ents {
		id := e.Name()
		if !e.IsDir() || strings.HasPrefix(id, ".") || fileExists(kziPath(id)) {
			continue
		}
		state := filepath.Join(*wincartDir, id)
		syncSetup(id, state, running != "")
	}
	syncRuntimes(running != "")
}

// What deleting a game's setup removes.
func setupParts(state string) []string {
	parts, _ := filepath.Glob(filepath.Join(state, "pfx*"))
	return append(parts, filepath.Join(state, "game"), filepath.Join(state, "logs"))
}

func syncSetup(id, state string, busy bool) {
	dir := contentDir(titleID(id), setupItem)
	marker := filepath.Join(state, setupMarker)
	var used int64
	for _, p := range setupParts(state) {
		used += spaceUsed(p)
	}
	switch {
	case fileExists(dir):
		if !fileExists(marker) {
			os.WriteFile(marker, nil, 0644)
		}
		sizeItem(dir, used)
	case fileExists(marker):
		if busy {
			return
		}
		for _, p := range setupParts(state) {
			if m := mountUnder(p); m != "" {
				log.Printf("%s: Windows setup deleted from the dashboard, but %s is mounted: left as it is", id, m)
				return
			}
		}
		for _, p := range setupParts(state) {
			os.RemoveAll(p) // links are removed, never followed
		}
		os.Remove(marker)
		log.Printf("%s: Windows setup deleted from the dashboard: removed its Wine prefixes, game folder and logs", id)
	case used > 0:
		if makeMemItem(dir, "Windows setup", used) {
			os.WriteFile(marker, nil, 0644)
		}
	}
}

type runtimeItem struct {
	item, name, path string
	keep             bool // comes back when deleted
}

func runtimeItems() []runtimeItem {
	var rs []runtimeItem
	if fileExists(contyFile) {
		rs = append(rs, runtimeItem{"Conty", "Conty (Wine for Windows games)", contyFile, true})
	}
	ents, _ := os.ReadDir(protonDir)
	for _, e := range ents {
		if e.IsDir() && !strings.HasPrefix(e.Name(), ".") {
			name := e.Name()
			if !strings.Contains(strings.ToLower(name), "proton") {
				name = "Proton " + name
			}
			rs = append(rs, runtimeItem{fatxName(name), name, filepath.Join(protonDir, e.Name()), false})
		}
	}
	return rs
}

func syncRuntimes(busy bool) {
	marks := filepath.Join(*storeDir, "runtimes")
	os.MkdirAll(marks, 0755)
	title := filepath.Dir(filepath.Dir(contentDir(runtimesTitle, "x")))
	rs := runtimeItems()
	have := map[string]bool{}
	for _, r := range rs {
		have[r.item] = true
		dir := contentDir(runtimesTitle, r.item)
		marker := filepath.Join(marks, r.item+".listed")
		switch {
		case fileExists(dir):
			if !fileExists(marker) {
				os.WriteFile(marker, nil, 0644)
			}
			sizeItem(dir, spaceUsed(r.path))
		case fileExists(marker) && !r.keep:
			if busy {
				continue
			}
			os.RemoveAll(r.path)
			os.Remove(marker)
			log.Printf("%s deleted from the dashboard: removed %s", r.name, r.path)
		default:
			// new, or Conty deleted (Windows games need it): (re)listed
			os.MkdirAll(title, 0755)
			writeXbx(filepath.Join(title, "TitleMeta.xbx"), "TitleName=Windows Runtimes")
			if makeMemItem(dir, r.name, spaceUsed(r.path)) {
				os.WriteFile(marker, nil, 0644)
			}
		}
	}
	// runtimes no longer there
	ms, _ := filepath.Glob(filepath.Join(marks, "*.listed"))
	for _, m := range ms {
		item := strings.TrimSuffix(filepath.Base(m), ".listed")
		if !have[item] {
			os.RemoveAll(contentDir(runtimesTitle, item))
			os.Remove(m)
		}
	}
	if len(rs) == 0 {
		os.RemoveAll(title)
	}
}

// A Memory screen item: ContentMeta.xbx and frolick-size.dat.
func makeMemItem(dir, name string, size int64) bool {
	if err := os.MkdirAll(dir, 0755); err != nil {
		return false
	}
	if err := os.WriteFile(filepath.Join(dir, "ContentMeta.xbx"), contentMeta(name), 0644); err != nil {
		return false
	}
	sizeItem(dir, size)
	return true
}

// frolick-size.dat as long as size, kept sparse (as uninstall.go's sizeStub).
func sizeItem(dir string, size int64) {
	p := filepath.Join(dir, sizeFile)
	if st, err := os.Stat(p); err == nil && abs64(st.Size()-size) < 1<<20 {
		return
	}
	f, err := os.OpenFile(p, os.O_CREATE|os.O_WRONLY, 0644)
	if err != nil {
		return
	}
	f.Truncate(0)
	err = f.Truncate(size)
	f.Close()
	var st syscall.Stat_t
	if err != nil || syscall.Stat(p, &st) != nil || st.Blocks*512 > 1<<20 {
		os.Remove(p)
	}
}

// A ContentMeta.xbx the dashboard reads the name of: an "XCSF" header whose
// title metadata (offset and size at 0x34) is TitleMeta-style UTF-16 text.
func contentMeta(name string) []byte {
	text := utf16.Encode([]rune("\uFEFF[Default]\r\nName=\"" + name + "\"\r\n"))
	b := make([]byte, 0x3C+2*len(text))
	copy(b[0x14:], "XCSF")
	le := binary.LittleEndian
	le.PutUint32(b[0x18:], 0x3C)
	le.PutUint32(b[0x34:], 0x3C)
	le.PutUint32(b[0x38:], uint32(2*len(text)))
	for i, u := range text {
		le.PutUint16(b[0x3C+2*i:], u)
	}
	return b
}

// An .xbx (UTF-16LE with a BOM, CRLF), if it isn't there.
func writeXbx(path, text string) {
	if fileExists(path) {
		return
	}
	u := utf16.Encode([]rune("\uFEFF" + text + "\r\n"))
	b := make([]byte, 2*len(u))
	for i, c := range u {
		binary.LittleEndian.PutUint16(b[2*i:], c)
	}
	os.WriteFile(path, b, 0644)
}

// A FATX-safe folder name: at most 42 characters, none of the reserved ones.
func fatxName(s string) string {
	s = strings.Map(func(r rune) rune {
		if r < 0x20 || r > 0x7E || strings.ContainsRune(`<>:"/\|*?`, r) {
			return '_'
		}
		return r
	}, s)
	if len(s) > 42 {
		s = s[:42]
	}
	return strings.TrimRight(s, ". ")
}
