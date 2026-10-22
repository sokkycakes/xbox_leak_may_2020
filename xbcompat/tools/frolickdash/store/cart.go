package main

// A finished download becomes a Windows game card the Sion can play:
//
//   /data/frolick/carts/<cid>.kzi       a Kazeta cart (Runtime=windows),
//   /data/frolick/carts/<cid>/          its files: zips unpacked, disc
//                                       images extracted, installers as is
//   E:\Games\<cid>\default.xbe          a stub title for the dashboards
//   E:\Games\<cid>\.wincart             naming the .kzi: xbcompat runs
//                                       wincart-run on it instead of the stub
//   E:\Games\<cid>\icon.png             the Archive's tile, for the library
//
// So the game is listed with the console's installed games, and launching
// it from Frolick or the Xbox dashboard runs it under Wine, with the Guide,
// as the game card's Windows carts run (bootani/sion/xbox/push-wincart).
//
// Exec is the game's own program when the files have one that runs as it
// is.  When they only have an installer, Exec is frolick-install.bat, which
// runs it (silently when it is an Inno Setup, NSIS or MSI installer, into
// C:\Games\<cid>).  After that first
// run, watchInstalls finds the installed program in the cart's Wine prefix
// (/data/wincarts/<cid>/pfx) and points Exec at it.

import (
	"archive/zip"
	"bytes"
	"encoding/binary"
	"flag"
	"fmt"
	"io"
	"log"
	"math"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"time"
	"unicode/utf16"
)

var (
	cartDir    = flag.String("carts", "/data/frolick/carts", "where the store's game cards go")
	gamesDir   = flag.String("games", "/data/xbox/hdd/partition1/Games", "E:\\Games, where the dashboards list installed games")
	wincartDir = flag.String("wincarts", "/data/wincarts", "wincart-run's per-cart state (the Wine prefixes)")
	runningCar = flag.String("running", "/var/run/wincart/cart", "the .kzi wincart-run is running now")
)

var reCID = regexp.MustCompile(`[^a-z0-9._-]+`)

// The cart's Id and folder name: the identifier, made safe for a FATX
// folder name (42 characters) and for wincart-run's paths.
func cartID(id string) string {
	c := strings.Trim(reCID.ReplaceAllString(strings.ToLower(id), "-"), "-.")
	if len(c) > 40 {
		c = c[:40]
	}
	if c == "" {
		c = "game"
	}
	return c
}

// ---- unpacking -----------------------------------------------------------

var (
	reZip   = regexp.MustCompile(`(?i)\.zip$`)
	reImage = regexp.MustCompile(`(?i)\.(iso|bin|img|mdf)$`)
)

func unzip(src, dst string) error {
	z, err := zip.OpenReader(src)
	if err != nil {
		return err
	}
	defer z.Close()
	for _, f := range z.File {
		name := strings.ReplaceAll(f.Name, "\\", "/")
		p := filepath.Join(dst, filepath.FromSlash(name))
		if !strings.HasPrefix(p, filepath.Clean(dst)+string(os.PathSeparator)) {
			continue // ../ in the archive
		}
		if f.FileInfo().IsDir() || strings.HasSuffix(name, "/") {
			os.MkdirAll(p, 0755)
			continue
		}
		os.MkdirAll(filepath.Dir(p), 0755)
		r, err := f.Open()
		if err != nil {
			return err
		}
		w, err := os.Create(p)
		if err != nil {
			r.Close()
			return err
		}
		_, err = io.Copy(w, r)
		r.Close()
		if cerr := w.Close(); err == nil {
			err = cerr
		}
		if err != nil {
			return err
		}
	}
	return nil
}

func extractImage(src, dst string) error {
	img, err := openISO(src)
	if err != nil {
		return err
	}
	defer img.Close()
	return img.extract(dst)
}

// Puts the downloaded files in the cart folder: archives unpacked, disc
// images extracted (the first disc at the top, the others in Disc 2, ...),
// anything else moved in as it is.  An archive that held a disc image and
// no program has that image extracted too.
func unpack(files []string, dst string) error {
	discs := 0
	for _, src := range files {
		base := filepath.Base(src)
		switch {
		case reZip.MatchString(base):
			if err := unzip(src, dst); err != nil {
				return fmt.Errorf("%s: %v", base, err)
			}
		case reImage.MatchString(base) && !strings.HasPrefix(strings.ToLower(base), "setup"):
			to := dst
			if discs > 0 {
				to = filepath.Join(dst, fmt.Sprintf("Disc %d", discs+1))
			}
			if err := extractImage(src, to); err != nil {
				return err
			}
			discs++
		default:
			if err := os.Rename(src, filepath.Join(dst, base)); err != nil {
				return err
			}
		}
	}
	if len(exes(dst)) == 0 {
		var inner []string
		filepath.WalkDir(dst, func(p string, d os.DirEntry, err error) error {
			if err == nil && !d.IsDir() && reImage.MatchString(p) {
				inner = append(inner, p)
			}
			return nil
		})
		for _, p := range inner {
			if extractImage(p, filepath.Dir(p)) == nil {
				os.Remove(p)
			}
		}
	}
	return nil
}

// ---- which program ---------------------------------------------------------

type exeFile struct {
	rel   string // relative to the folder, with /
	depth int
	size  int64
}

func exes(dir string) []exeFile {
	var out []exeFile
	filepath.WalkDir(dir, func(p string, d os.DirEntry, err error) error {
		if err != nil {
			return nil
		}
		rel, _ := filepath.Rel(dir, p)
		depth := strings.Count(filepath.ToSlash(rel), "/")
		if d.IsDir() {
			if depth > 4 {
				return filepath.SkipDir
			}
			return nil
		}
		low := strings.ToLower(d.Name())
		if strings.HasSuffix(low, ".exe") || strings.HasSuffix(low, ".msi") {
			if st, err := d.Info(); err == nil {
				out = append(out, exeFile{rel: filepath.ToSlash(rel), depth: depth, size: st.Size()})
			}
		}
		return nil
	})
	return out
}

var (
	reInstaller = regexp.MustCompile(`(?i)^(setup|install|instal|autorun|setup_.*|.*[-_ ]setup|.*installer.*)\.(exe|msi)$|\.msi$`)
	reNotGame   = regexp.MustCompile(`(?i)^(unins|uninst|uninstall|dxsetup|dxwebsetup|directx|dx9|dx8|vcredist|vc_redist|dotnet|netfx|readme|register|regist|crashrep|bugreport|quicktime|qtinstall|acrord|acroread|ie\d|msie|wmp|indeo|ddhelp|dplay|update|patch|support|cleanup|eula|manual|help|config|setupreg|launcher_setup)`)
	reNotDir    = regexp.MustCompile(`(?i)(^|/)(redist|redistributable|directx|drivers?|extras?|bonus|demos?|tools|support|_commonredist|commonredist|install|setup|system|windows|__macosx|mac)(/|$)`)
	reWords     = regexp.MustCompile(`[a-z0-9]+`)
	reOtherLang = regexp.MustCompile(`japanese|german|deutsch|french|francais|spanish|espanol|italian|russian|polish|portug|chinese|korean|_jp\.|_de\.|_fr\.`)
)

func isInstaller(rel string) bool { return reInstaller.MatchString(filepath.Base(rel)) }

// The program that most looks like the game: near the top, big, named like
// the title, not an uninstaller, a runtime installer or a tool.
func bestGame(list []exeFile, title string) (exeFile, bool) {
	words := map[string]bool{}
	for _, w := range reWords.FindAllString(strings.ToLower(title), -1) {
		if len(w) > 2 {
			words[w] = true
		}
	}
	best, bestScore, ok := exeFile{}, math.Inf(-1), false
	for _, e := range list {
		base := filepath.Base(e.rel)
		if isInstaller(e.rel) || reNotGame.MatchString(base) || reNotDir.MatchString(filepath.Dir(e.rel)) {
			continue
		}
		score := -15*float64(e.depth) + math.Log2(float64(e.size+1))
		for _, w := range reWords.FindAllString(strings.ToLower(e.rel), -1) {
			if words[w] {
				score += 12
			}
		}
		if strings.Contains(strings.ToLower(base), "launch") {
			score -= 3
		}
		// one build per language: English first
		switch low := strings.ToLower(e.rel); {
		case strings.Contains(low, "english") || strings.Contains(low, "_en."):
			score += 6
		case reOtherLang.MatchString(low):
			score -= 6
		}
		if score > bestScore {
			best, bestScore, ok = e, score, true
		}
	}
	return best, ok
}

// The installer at the top of the files (the shallowest, then the biggest).
// A disc's SETUP.EXE can be a little stub that only starts its AUTORUN.INF
// program (Diablo's starts Blizzard's installer menu and quits), so the
// program AUTORUN.INF names is taken instead when it's the bigger one.
func bestInstaller(list []exeFile, folder string) (exeFile, bool) {
	inst, ok := topInstaller(list)
	if auto, aok := autorunExe(list, folder); aok && ok && inst.size < 64<<10 && auto.size > inst.size {
		return auto, true
	}
	return inst, ok
}

// The program AUTORUN.INF at the top of the files opens, if it's there.
func autorunExe(list []exeFile, folder string) (exeFile, bool) {
	ents, _ := os.ReadDir(folder)
	for _, e := range ents {
		if !strings.EqualFold(e.Name(), "autorun.inf") {
			continue
		}
		b, err := os.ReadFile(filepath.Join(folder, e.Name()))
		if err != nil {
			break
		}
		for _, line := range strings.Split(string(b), "\n") {
			k, v, ok := strings.Cut(strings.TrimSpace(line), "=")
			if !ok || !strings.EqualFold(strings.TrimSpace(k), "open") {
				continue
			}
			v = strings.TrimSpace(v)
			if strings.HasPrefix(v, `"`) {
				v = strings.SplitN(v[1:], `"`, 2)[0]
			} else if f := strings.Fields(v); len(f) > 0 {
				v = f[0]
			}
			v = strings.TrimPrefix(strings.ReplaceAll(v, `\`, "/"), "./")
			for _, x := range list {
				if strings.EqualFold(x.rel, v) {
					return x, true
				}
			}
		}
		break
	}
	return exeFile{}, false
}

// The installers by name, shallowest then biggest first.
func topInstaller(list []exeFile) (exeFile, bool) {
	var ins []exeFile
	for _, e := range list {
		if isInstaller(e.rel) && !strings.HasPrefix(strings.ToLower(filepath.Base(e.rel)), "autorun") && !reNotGame.MatchString(filepath.Base(e.rel)) {
			ins = append(ins, e)
		}
	}
	if len(ins) == 0 {
		return exeFile{}, false
	}
	sort.Slice(ins, func(i, j int) bool {
		if ins[i].depth != ins[j].depth {
			return ins[i].depth < ins[j].depth
		}
		return ins[i].size > ins[j].size
	})
	return ins[0], true
}

// What kind of installer: its silent switches into dir (Windows path).
func silentArgs(path, dir string) string {
	if strings.HasSuffix(strings.ToLower(path), ".msi") {
		return fmt.Sprintf(`/qb TARGETDIR="%s" INSTALLDIR="%s"`, dir, dir)
	}
	f, err := os.Open(path)
	if err != nil {
		return ""
	}
	defer f.Close()
	b := make([]byte, 8<<20)
	n, _ := io.ReadFull(f, b)
	b = b[:n]
	switch {
	case bytes.Contains(b, []byte("Inno Setup")):
		return fmt.Sprintf(`/SILENT /SUPPRESSMSGBOXES /SP- /NORESTART /DIR="%s"`, dir)
	case bytes.Contains(b, []byte("Nullsoft")):
		return fmt.Sprintf(`/S /D=%s`, dir)
	case bytes.Contains(b, []byte("Wise Installation")):
		return `/s` // into the folder its script names
	}
	return "" // InstallShield and the rest: run as they are
}

func winPath(rel string) string { return strings.ReplaceAll(rel, "/", `\`) }

// ---- the card ---------------------------------------------------------------

type cart struct {
	CID, Name, Exec, Mode, Publisher string
	Input                            string // "mouse": its installer needs a mouse or keyboard
	Installed                        int64  // when the installer was set up (unix)
}

func kziPath(cid string) string { return filepath.Join(*cartDir, cid+".kzi") }

func writeKzi(c cart) error {
	var b strings.Builder
	fmt.Fprintf(&b, "# A Frolick store download (frolick-store); the files are in %s/\n", c.CID)
	fmt.Fprintf(&b, "Name=%s\nId=%s\nExec=%s\nRuntime=windows\nIcon=%s/icon.png\n", c.Name, c.CID, c.Exec, c.CID)
	if c.Publisher != "" {
		fmt.Fprintf(&b, "Publisher=%s\n", c.Publisher)
	}
	fmt.Fprintf(&b, "FrolickMode=%s\nFrolickInstalled=%d\n", c.Mode, c.Installed)
	if c.Input != "" && c.Mode == "install" {
		fmt.Fprintf(&b, "FrolickInput=%s\n", c.Input)
	}
	return writeAtomic(kziPath(c.CID), []byte(b.String()))
}

func readKzi(path string) (cart, bool) {
	b, err := os.ReadFile(path)
	if err != nil {
		return cart{}, false
	}
	var c cart
	for _, line := range strings.Split(string(b), "\n") {
		k, v, ok := strings.Cut(strings.TrimRight(line, "\r"), "=")
		if !ok {
			continue
		}
		switch k {
		case "Name":
			c.Name = v
		case "Id":
			c.CID = v
		case "Exec":
			c.Exec = v
		case "Publisher":
			c.Publisher = v
		case "FrolickMode":
			c.Mode = v
		case "FrolickInstalled":
			fmt.Sscan(v, &c.Installed)
		case "FrolickInput":
			c.Input = v
		}
	}
	return c, c.CID != ""
}

// Makes item q's downloaded files (in dir) a game card.  Returns the
// card's mode: "game" (plays as it is), "install" (its installer runs
// first), or an error.
func makeCart(q queued, title, publisher, dir string, files []iaFile) (string, error) {
	cid := cartID(q.ID)
	folder := filepath.Join(*cartDir, cid)
	os.RemoveAll(folder)
	if err := os.MkdirAll(folder, 0755); err != nil {
		return "", err
	}
	var paths []string
	for _, f := range files {
		paths = append(paths, localName(dir, f))
	}
	if err := unpack(paths, folder); err != nil {
		os.RemoveAll(folder)
		return "", err
	}
	// a single folder at the top (most zips): the cart is that folder
	if ents, _ := os.ReadDir(folder); len(ents) == 1 && ents[0].IsDir() {
		inner := filepath.Join(folder, ents[0].Name())
		tmp := folder + ".tmp"
		if os.Rename(inner, tmp) == nil {
			os.Remove(folder)
			os.Rename(tmp, folder)
		}
	}

	list := exes(folder)
	c := cart{CID: cid, Name: shortName(title), Publisher: publisher}
	game, hasGame := bestGame(list, title)
	inst, hasInst := bestInstaller(list, folder)
	switch {
	case hasInst && (!hasGame || inst.depth <= game.depth):
		// a disc with an installer: the game has to be installed first
		quiet, err := installBat(folder, cid, inst)
		if err != nil {
			return "", err
		}
		c.Exec, c.Mode, c.Installed = "frolick-install.bat", "install", time.Now().Unix()
		if !quiet {
			c.Input = "mouse"
		}
	case hasGame:
		c.Exec, c.Mode = runBat(folder, game.rel), "game"
	default:
		os.RemoveAll(folder)
		return "", fmt.Errorf("no Windows program in %s", q.ID)
	}
	if tile := filepath.Join(*storeDir, "img", tileName(q.ID)); fileExists(tile) {
		copyFile(tile, filepath.Join(folder, "icon.png"))
	}
	if err := writeKzi(c); err != nil {
		return "", err
	}
	if err := writeStub(c); err != nil {
		return "", err
	}
	// the downloaded originals are in the card now
	for _, p := range paths {
		os.Remove(p)
	}
	log.Printf("%s: game card %s, %s, Exec=%s", q.ID, cid, c.Mode, c.Exec)
	return c.Mode, nil
}

// frolick-install.bat, which runs installer inst once.  Says whether it
// runs by itself (silently); the rest need a mouse or keyboard.
func installBat(folder, cid string, inst exeFile) (bool, error) {
	args := silentArgs(filepath.Join(folder, filepath.FromSlash(inst.rel)), `C:\Games\`+cid)
	bat := "@echo off\r\n" +
		"rem frolick-store: installs the game once, into its own Wine prefix\r\n" +
		fmt.Sprintf("start /wait \"\" \"%s\" %s\r\n", winPath(inst.rel), args)
	return args != "", os.WriteFile(filepath.Join(folder, "frolick-install.bat"), []byte(bat), 0644)
}

// Cards made before bestInstaller knew about AUTORUN.INF stubs: their
// installer chosen again (while not running and not yet installed).
func recheckInstallers() {
	kzis, _ := filepath.Glob(filepath.Join(*cartDir, "*.kzi"))
	running, _ := os.ReadFile(*runningCar)
	for _, k := range kzis {
		c, ok := readKzi(k)
		if !ok || c.Mode != "install" || strings.TrimSpace(string(running)) == k {
			continue
		}
		folder := filepath.Join(*cartDir, c.CID)
		inst, ok := bestInstaller(exes(folder), folder)
		if !ok {
			continue
		}
		old, _ := os.ReadFile(filepath.Join(folder, "frolick-install.bat"))
		if bytes.Contains(old, []byte(`"`+winPath(inst.rel)+`"`)) {
			continue
		}
		quiet, err := installBat(folder, c.CID, inst)
		if err != nil {
			continue
		}
		c.Input = ""
		if !quiet {
			c.Input = "mouse"
		}
		if writeKzi(c) == nil {
			log.Printf("%s: installer is now %s", c.CID, inst.rel)
		}
	}
}

// Exec for a program: wincart-run starts it from the cart's folder, so a
// program anywhere else gets frolick-run.bat, which starts it from its
// own folder (where games look for their files).
func runBat(folder, rel string) string {
	dir, exe := filepath.Split(rel)
	if dir == "" {
		return rel
	}
	bat := "@echo off\r\n" +
		"rem frolick-store: starts the game from its own folder\r\n" +
		fmt.Sprintf("cd /d \"%s\"\r\n", winPath(strings.TrimSuffix(dir, "/"))) +
		fmt.Sprintf("start /wait \"\" \"%s\"\r\n", exe)
	os.WriteFile(filepath.Join(folder, "frolick-run.bat"), []byte(bat), 0644)
	return "frolick-run.bat"
}

// The certificate holds 40 characters: the title without what follows a
// bracket or parenthesis when it is longer than that.
func shortName(title string) string {
	t := title
	if len([]rune(t)) > 40 {
		if i := strings.IndexAny(t, "([{"); i > 3 {
			t = strings.TrimSpace(t[:i])
		}
	}
	if r := []rune(t); len(r) > 40 {
		t = string(r[:40])
	}
	return t
}

func fileExists(p string) bool { _, err := os.Stat(p); return err == nil }

func copyFile(src, dst string) error {
	b, err := os.ReadFile(src)
	if err != nil {
		return err
	}
	return os.WriteFile(dst, b, 0644)
}

// ---- the stub title -----------------------------------------------------------

// 'SN' + CRC-16/CCITT of the Id, case-insensitive: the title ID xbcompat
// gives a Windows cart (src/kernel/wincart.c), where its saves go in UDATA.
func titleID(id string) uint32 {
	crc := uint32(0xFFFF)
	for i := 0; i < len(id); i++ {
		crc ^= uint32(id[i]|0x20) << 8
		for k := 0; k < 8; k++ {
			if crc&0x8000 != 0 {
				crc = crc<<1 ^ 0x1021
			} else {
				crc <<= 1
			}
		}
		crc &= 0xFFFF
	}
	return 0x534E0000 | crc
}

// The stub XBE xbcompat writes for a cart on the game card (wincart.c
// write_stub_xbe): headers, a certificate with the cart's name and title
// ID, one empty section.  Nothing in it runs: xbcompat sees the .wincart
// beside it and runs wincart-run instead.
func stubXBE(name string, tid, stamp uint32) []byte {
	const base, hdr = 0x10000, 0x1000
	const textVA = base + 0x1000
	img := make([]byte, hdr+0x1000)
	le := binary.LittleEndian
	put := func(off int, v uint32) { le.PutUint32(img[off:], v) }
	const hdrSize, certSize, secSize = 0x178, 0xB0, 0x38
	cert := uint32(base + hdrSize)
	secs := cert + certSize
	names := secs + secSize
	put(0x000, 0x48454258) // XBEH
	put(0x104, base)
	put(0x108, hdr)
	put(0x10C, uint32(len(img)))
	put(0x110, hdrSize)
	put(0x114, stamp)
	put(0x118, cert)
	put(0x11C, 1)
	put(0x120, secs)
	put(0x128, (textVA+0x10)^0xA8FC57AB) // entry, retail key
	put(0x130, 0x10000)                  // stack commit
	put(0x134, 0x100000)                 // heap reserve
	put(0x138, 0x1000)                   // heap commit
	put(0x13C, 0x10000)
	put(0x140, uint32(len(img)))
	put(0x14C, names+8)
	put(0x150, names+8)
	put(0x154, names+8)
	put(0x158, textVA^0x5B6D40B6) // kernel thunk, retail key
	put(0x164, names+8)
	c := int(cert - base)
	put(c+0x00, certSize)
	put(c+0x04, stamp)
	put(c+0x08, tid)
	u := utf16.Encode([]rune(name))
	for i := 0; i < len(u) && i < 39; i++ {
		le.PutUint16(img[c+0x0C+2*i:], u[i])
	}
	put(c+0x9C, 0xFF)       // media types
	put(c+0xA0, 0xFFFFFFFF) // region
	put(c+0xA4, 0xFFFFFFFF) // ratings
	put(c+0xAC, 1)          // version
	s := int(secs - base)
	put(s+0x00, 0x2|0x1|0x4) // preload, writable, executable
	put(s+0x04, textVA)
	put(s+0x08, 0x1000)
	put(s+0x0C, hdr)
	put(s+0x10, 0x1000)
	put(s+0x14, names)
	copy(img[names-base:], ".text")
	img[hdr+0x10] = 0xC3
	return img
}

func stubFolder(cid string) string { return filepath.Join(*gamesDir, cid) }

func writeStub(c cart) error {
	dir := stubFolder(c.CID)
	if err := os.MkdirAll(dir, 0755); err != nil {
		return err
	}
	tid := titleID(c.CID)
	if err := writeAtomic(filepath.Join(dir, "default.xbe"), stubXBE(c.Name, tid, uint32(time.Now().Unix()))); err != nil {
		return err
	}
	text := fmt.Sprintf("kzi=%s\ntitle=%08x\nname=%s\nid=%s\nruntime=windows\nexec=%s\npublisher=%s\nsource=frolick-store\n",
		kziPath(c.CID), tid, c.Name, c.CID, c.Exec, c.Publisher)
	if err := writeAtomic(filepath.Join(dir, ".wincart"), []byte(text)); err != nil {
		return err
	}
	if icon := filepath.Join(*cartDir, c.CID, "icon.png"); fileExists(icon) {
		copyFile(icon, filepath.Join(dir, "icon.png"))
	}
	return nil
}

// ---- after an installer ran -------------------------------------------------

// Every so often: carts whose installer has run get Exec pointed at the
// installed game (C:\Games\<cid>\..., else a folder the installer made in
// Program Files or at the top of C:).  Never while that cart is running.
func watchInstalls() {
	kzis, _ := filepath.Glob(filepath.Join(*cartDir, "*.kzi"))
	running, _ := os.ReadFile(*runningCar)
	for _, k := range kzis {
		c, ok := readKzi(k)
		if !ok || c.Mode != "install" || strings.TrimSpace(string(running)) == k {
			continue
		}
		driveC := filepath.Join(*wincartDir, c.CID, "pfx", "drive_c")
		roots := []string{filepath.Join(driveC, "Games", c.CID)}
		for _, top := range []string{filepath.Join(driveC, "Program Files"), filepath.Join(driveC, "Program Files (x86)"), driveC} {
			ents, _ := os.ReadDir(top)
			for _, e := range ents {
				if !e.IsDir() {
					continue
				}
				switch strings.ToLower(e.Name()) {
				case "windows", "users", "programdata", "program files", "program files (x86)", "games", "common files", "internet explorer", "windows media player", "windows nt", "windowspowershell", "microsoft.net":
					continue
				}
				if st, err := e.Info(); err == nil && st.ModTime().Unix() >= c.Installed {
					roots = append(roots, filepath.Join(top, e.Name()))
				}
			}
		}
		for _, root := range roots {
			game, ok := bestGame(exes(root), c.Name)
			if !ok {
				continue
			}
			rel, _ := filepath.Rel(driveC, filepath.Join(root, filepath.FromSlash(game.rel)))
			c.Exec, c.Mode = runBat(filepath.Join(*cartDir, c.CID), `C:/`+filepath.ToSlash(rel)), "installed"
			if writeKzi(c) == nil {
				writeStub(c)
				log.Printf("%s: installed, Exec=%s", c.CID, c.Exec)
			}
			break
		}
	}
}
