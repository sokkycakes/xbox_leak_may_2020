package main

// Streaming Download: an Xbox disc image from the Archive installed as a
// game on the hard disk, playable while it downloads. Pressing Play on it
// in Spotlight writes play.txt in the store folder:
//
//	frolick-play 1
//	p <id>
//	end
//
// (an empty one when nothing is to be played; the dashboard writes that
// when it starts). The game goes in E:\Games\<title> like any installed
// game: default.iso (the disc image, the size of the whole disc from the
// start and filled in as it comes), its default.xbe, and
// frolick-stream.txt (where it comes from). xbcompat runs a game whose
// folder holds a default.iso with that image as its DVD drive (misc.c,
// relaunch), and streams it while default.iso.stream and .map are beside it
// (dvd.c). This helper runs itself with -stream to fill the image, demand
// reads first; once the disc's directory and default.xbe are in (the launch
// gate) the game is ready and the dashboard starts it from E:\Games. The
// rest fills in behind the game's reads, and after the game, until the
// whole disc is in and checked (md5, sha1); then the stream files go and it
// is an ordinary installed game. Deleting it on a Memory screen removes it
// all (the helper stops filling it). An image left unfinished (a reboot)
// is filled again in the background. Status lines in status.txt:
//
//	stream <id> <starting|ready|error> <bytes in> <bytes> <detail>
//	        (ready: the detail is the game's folder name in E:\Games)
//	installed <id> <folder>

import (
	"bufio"
	"context"
	"flag"
	"fmt"
	"log"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"
)

var (
	hddDir     = flag.String("hdd", "/data/xbox/hdd", "the Xbox hard disk")
	streamsDir = flag.String("streams", "/data/frolick/stream", "the streamer's state and logs for each streamed game")
)

const (
	playSock   = "/tmp/frolick-stream.sock"
	streamFile = "frolick-stream.txt"
)

// readPlay is what play.txt asks for: an id, "" for nothing, and whether
// the file was whole.
func readPlay() (string, bool) {
	b, err := os.ReadFile(filepath.Join(*storeDir, "play.txt"))
	if err != nil {
		return "", os.IsNotExist(err)
	}
	id := ""
	for _, line := range strings.Split(strings.ReplaceAll(string(b), "\r", ""), "\n") {
		f := strings.Split(line, "\t")
		if f[0] == "end" {
			return id, true
		}
		if f[0] == "p" && len(f) >= 2 {
			id = f[1]
		}
	}
	return "", false
}

// gameFolder is a streamed game's folder name in E:\Games: its title, kept
// to what FATX allows (42 characters, no \/:*?"<>|).
func gameFolder(id string) string {
	_, file, _ := strings.Cut(id, "/")
	name := clean(strings.TrimSuffix(file, filepath.Ext(file)))
	name = strings.Map(func(r rune) rune {
		if r < 32 || r > 126 || strings.ContainsRune(`\/:*?"<>|`, r) {
			return -1
		}
		return r
	}, name)
	name = strings.TrimRight(strings.TrimSpace(name), ".")
	if len(name) > 42 {
		name = strings.TrimSpace(name[:42])
	}
	if name == "" {
		name = strings.TrimSuffix(tileName(id), ".png")
	}
	return name
}

// stateDir holds the streamer's log and stats for a game (not on E:).
func stateDir(id string) string {
	return filepath.Join(*streamsDir, strings.TrimSuffix(tileName(id), ".png"))
}

// A streamed game's frolick-stream.txt: id, url, md5, sha1, state
// (streaming or complete).
type streamRec struct {
	ID, URL, MD5, SHA1, State string
}

func readRec(dir string) (streamRec, bool) {
	var r streamRec
	b, err := os.ReadFile(filepath.Join(dir, streamFile))
	if err != nil {
		return r, false
	}
	for _, line := range strings.Split(string(b), "\n") {
		k, v, _ := strings.Cut(strings.TrimRight(line, "\r"), "\t")
		switch k {
		case "id":
			r.ID = v
		case "url":
			r.URL = v
		case "md5":
			r.MD5 = v
		case "sha1":
			r.SHA1 = v
		case "state":
			r.State = v
		}
	}
	return r, r.ID != ""
}

func writeRec(dir string, r streamRec) error {
	return writeAtomic(filepath.Join(dir, streamFile), []byte(fmt.Sprintf(
		"id\t%s\nurl\t%s\nmd5\t%s\nsha1\t%s\nstate\t%s\n", r.ID, r.URL, r.MD5, r.SHA1, r.State)))
}

// streamedGames are the games in E:\Games that came from here, by folder.
func streamedGames() map[string]streamRec {
	out := map[string]streamRec{}
	ents, _ := os.ReadDir(*gamesDir)
	for _, e := range ents {
		if e.IsDir() {
			if r, ok := readRec(filepath.Join(*gamesDir, e.Name())); ok {
				out[e.Name()] = r
			}
		}
	}
	return out
}

type player struct {
	mu     sync.Mutex
	id     string // the game being filled ("" none)
	dir    string // its folder in E:\Games
	cmd    *exec.Cmd
	exited chan struct{}
	want   string // what play.txt asks for
	state  string
	detail string
	retry  time.Time
}

var play player

// watchPlay is called every second by the main loop.
func watchPlay(ctx context.Context) {
	want, ok := readPlay()
	p := &play
	p.mu.Lock()
	defer p.mu.Unlock()
	if ok {
		if want != p.want {
			p.state, p.detail, p.retry = "starting", "", time.Time{}
		}
		p.want = want
	}
	// the game being filled was deleted (a Memory screen): stop
	if p.id != "" && p.cmd != nil {
		if _, err := os.Stat(filepath.Join(p.dir, streamFile)); err != nil {
			log.Printf("play %s: deleted; stopping", p.id)
			p.stop()
			os.RemoveAll(stateDir(p.id))
			p.id = ""
		}
	}
	// what to fill: the game asked for, else one left unfinished
	next := p.want
	if next == "" {
		next = p.id
	}
	if next == "" {
		for _, r := range streamedGames() {
			if r.State != "complete" {
				next = r.ID
				break
			}
		}
	}
	if next != p.id {
		p.stop()
		p.id, p.dir = next, filepath.Join(*gamesDir, gameFolder(next))
	}
	if p.id == "" {
		return
	}
	r, have := readRec(p.dir)
	if p.cmd == nil && (!have || r.State != "complete") && time.Now().After(p.retry) {
		if err := p.start(ctx, r, have); err != nil {
			log.Printf("play %s: %v", p.id, err)
			if p.want == p.id {
				p.state, p.detail = "error", "Can't reach the Internet Archive"
			}
			p.retry = time.Now().Add(10 * time.Second)
		}
	}
	if p.cmd != nil && streamVerified(p.id) != "" {
		p.finish()
		r, have = readRec(p.dir)
	}
	if p.want == p.id && p.state == "starting" {
		if st, err := os.Stat(filepath.Join(p.dir, "default.xbe")); err == nil && st.Size() > 0 && have {
			p.state, p.detail = "ready", gameFolder(p.id)
			log.Printf("play %s: ready in %s", p.id, p.dir)
		}
	}
}

// streamVerified is the stats file's "verified" line, "" until the whole
// disc is in and checked.
func streamVerified(id string) string {
	v, _ := readStats(id)["verified"]
	if v == "" || v == "0" {
		return ""
	}
	return v
}

func readStats(id string) map[string]string {
	m := map[string]string{}
	f, err := os.Open(filepath.Join(stateDir(id), "stats"))
	if err != nil {
		return m
	}
	defer f.Close()
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		k, v, _ := strings.Cut(sc.Text(), " ")
		m[k] = v
	}
	return m
}

// finish: the whole disc is in. Checked ok, it becomes a plain installed
// game (no stream files); otherwise the image starts over.
func (p *player) finish() {
	v := streamVerified(p.id)
	p.stop()
	r, _ := readRec(p.dir)
	img := filepath.Join(p.dir, "default.iso")
	if v == "ok" || v == "unchecked" {
		for _, ext := range []string{".stream", ".map", ".trace"} {
			os.Remove(img + ext)
		}
		r.State = "complete"
		writeRec(p.dir, r)
		log.Printf("play %s: the whole disc is in (%s)", p.id, v)
	} else {
		log.Printf("play %s: the disc didn't check out (%s); fetching it again", p.id, v)
		os.Remove(img + ".map")
		os.Truncate(img, 0)
	}
	os.Remove(filepath.Join(stateDir(p.id), "stats"))
}

func (p *player) stop() {
	if p.cmd != nil {
		p.cmd.Process.Kill()
		<-p.exited
		p.cmd = nil
	}
}

// start runs the streamer for p.id into its folder, making the folder and
// its record first (hashes from the Archive).
func (p *player) start(ctx context.Context, r streamRec, have bool) error {
	if !have {
		ident, file, ok := strings.Cut(p.id, "/")
		if !ok {
			return fmt.Errorf("not a disc image")
		}
		c, cancel := context.WithTimeout(ctx, 90*time.Second)
		md5, sha1, err := fileHashes(c, ident, file)
		cancel()
		if err != nil {
			return err
		}
		if err := os.MkdirAll(p.dir, 0755); err != nil {
			return err
		}
		r = streamRec{ID: p.id, URL: fileURL(ident, file), MD5: md5, SHA1: sha1, State: "streaming"}
		if err := writeRec(p.dir, r); err != nil {
			return err
		}
		log.Printf("play %s: installing in %s", p.id, p.dir)
	}
	sd := stateDir(p.id)
	if err := os.MkdirAll(sd, 0755); err != nil {
		return err
	}
	os.Remove(filepath.Join(sd, "stats"))
	logf, err := os.OpenFile(filepath.Join(sd, "fetch.log"), os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0644)
	if err != nil {
		return err
	}
	// the gate writes default.xbe beside the image once it's in (again
	// each time: the same bytes)
	self, _ := os.Executable()
	cmd := exec.Command(self, "-stream", r.URL, "-image", filepath.Join(p.dir, "default.iso"),
		"-socket", playSock, "-xbe", filepath.Join(p.dir, "default.xbe"), "-stats", filepath.Join(sd, "stats"),
		"-md5", r.MD5, "-sha1", r.SHA1)
	cmd.Stdout, cmd.Stderr = logf, logf
	cmd.SysProcAttr = &syscall.SysProcAttr{Pdeathsig: syscall.SIGKILL} // never outlives the helper
	if err := cmd.Start(); err != nil {
		logf.Close()
		return err
	}
	log.Printf("play %s: streaming into %s", p.id, p.dir)
	p.cmd = cmd
	exited := make(chan struct{})
	p.exited = exited
	go func() {
		cmd.Wait()
		logf.Close()
		close(exited)
		p.mu.Lock()
		if p.cmd == cmd {
			log.Printf("play %s: the streamer stopped; again shortly", p.id)
			p.cmd = nil
			p.retry = time.Now().Add(5 * time.Second)
		}
		p.mu.Unlock()
	}()
	return nil
}

// playStatus is the status.txt lines for streamed games.
func playStatus() string {
	var out strings.Builder
	for folder, r := range streamedGames() {
		fmt.Fprintf(&out, "installed\t%s\t%s\n", r.ID, clean(folder))
	}
	p := &play
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.want == "" {
		return out.String()
	}
	var in, size int64
	if p.want == p.id {
		st := readStats(p.id)
		size, _ = strconv.ParseInt(st["size"], 10, 64)
		chunks, _ := strconv.ParseInt(st["chunks"], 10, 64)
		have, _ := strconv.ParseInt(st["have"], 10, 64)
		if chunks > 0 {
			in = min(size, have*((size+chunks-1)/chunks))
		}
		if r, ok := readRec(p.dir); ok && r.State == "complete" {
			if fi, err := os.Stat(filepath.Join(p.dir, "default.iso")); err == nil {
				in, size = fi.Size(), fi.Size()
			}
		}
	}
	fmt.Fprintf(&out, "stream\t%s\t%s\t%d\t%d\t%s\n", p.want, p.state, in, size, clean(p.detail))
	return out.String()
}

// migrateStreams moves discs streamed before streamed games were installed
// games (bfce8545: /data/frolick/stream/<name>/disc.iso, put in the tray
// through <hdd>/.dvdimage) into E:\Games, keeping what was fetched.
func migrateStreams(tabs []tab) {
	if b, err := os.ReadFile(filepath.Join(*hddDir, ".dvdimage")); err == nil &&
		strings.HasPrefix(strings.TrimSpace(string(b)), *streamsDir+"/") {
		os.Remove(filepath.Join(*hddDir, ".dvdimage"))
	}
	for _, t := range tabs {
		for _, entry := range t.IDs {
			id := strings.TrimSpace(strings.Split(entry, "|")[0])
			ident, file, ok := strings.Cut(id, "/")
			if !ok {
				continue
			}
			old := stateDir(id)
			img := filepath.Join(old, "disc.iso")
			if _, err := os.Stat(img); err != nil {
				continue
			}
			dir := filepath.Join(*gamesDir, gameFolder(id))
			if _, err := os.Stat(filepath.Join(dir, "default.iso")); err == nil {
				continue
			}
			if err := migrateOne(id, ident, file, old, dir); err != nil {
				log.Printf("migrate %s: %v", id, err)
				continue
			}
			log.Printf("migrate %s: now %s", id, dir)
		}
	}
}

func migrateOne(id, ident, file, old, dir string) error {
	stats := readStats(id)
	r := streamRec{ID: id, URL: fileURL(ident, file), State: "streaming"}
	if md5, sha1, err := fileHashes(context.Background(), ident, file); err == nil {
		r.MD5, r.SHA1 = md5, sha1
	} else if stats["verified"] != "ok" {
		return err // try again once the Archive answers
	}
	if stats["verified"] == "ok" {
		r.State = "complete"
	}
	if err := os.MkdirAll(dir, 0755); err != nil {
		return err
	}
	if err := copyFile(filepath.Join(old, "default.xbe"), filepath.Join(dir, "default.xbe")); err != nil {
		return err
	}
	img := filepath.Join(dir, "default.iso")
	if err := os.Rename(filepath.Join(old, "disc.iso"), img); err != nil {
		return err
	}
	if r.State == "complete" {
		os.Remove(filepath.Join(old, "disc.iso.map"))
	} else {
		os.Rename(filepath.Join(old, "disc.iso.map"), img+".map")
	}
	for _, f := range []string{"disc.iso.stream", "disc.iso.trace", "default.xbe", "stats", "result.txt", "xbcompat.log"} {
		os.Remove(filepath.Join(old, f))
	}
	return writeRec(dir, r)
}
