package main

// Streaming Download: an Xbox disc image from the Archive installed as a
// game on the hard disk, playable while it downloads. Pressing Play on it
// in Spotlight writes play.txt in the store folder:
//
//	frolick-play 1
//	p <id>          play this one
//	f <id>          fill this one before the others (Downloads, Start Now)
//	x <id>          remove this one and its game (Downloads, Remove)
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
// is filled again in the background.
//
// Every unfinished game has its own streamer and socket
// (/tmp/frolick-stream-<hash>.sock, named in its default.iso.stream), so a
// game always reads from its own; one of them fills at a time (the game
// asked to play, else the one Start Now picked, else the one filling,
// else the oldest), the others serve their game's reads only. Status
// lines in status.txt:
//
//	stream <id> <starting|ready|error> <bytes in> <bytes> <detail>
//	        (ready: the detail is the game's folder name in E:\Games)
//	streaming <id> <bytes in> <bytes> <1 filling|0> <title>
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
	"sort"
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

const streamFile = "frolick-stream.txt"

// streamerBin runs the streamers instead of this program (tests).
var streamerBin string

// readPlay is what play.txt asks for, and whether the file was whole.
func readPlay() (playReq, bool) {
	var req playReq
	b, err := os.ReadFile(filepath.Join(*storeDir, "play.txt"))
	if err != nil {
		return req, os.IsNotExist(err)
	}
	for _, line := range strings.Split(strings.ReplaceAll(string(b), "\r", ""), "\n") {
		f := strings.Split(line, "\t")
		if f[0] == "end" {
			return req, true
		}
		if len(f) < 2 {
			continue
		}
		switch f[0] {
		case "p":
			req.play = f[1]
		case "f":
			req.first = f[1]
		case "x":
			req.remove = append(req.remove, f[1])
		}
	}
	return playReq{}, false
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

// A streamer for each streamed game not yet all in, each on its own
// socket, so every game can be played (its reads served) at any time. Only
// one fills its disc front to back at a time (its -fill file is there):
// the game last asked to play, else the one the Downloads page put first,
// else the oldest; the others only fetch what their game reads.
type streamProc struct {
	id, dir string
	cmd     *exec.Cmd
	exited  chan struct{}
	retry   time.Time
}

type players struct {
	mu     sync.Mutex
	procs  map[string]*streamProc
	want   string // what play.txt asks to play
	first  string // what the Downloads page asks to fill first
	state  string // the game asked to play: starting, ready or error
	detail string
	filler string // the one filling now
}

var play = players{procs: map[string]*streamProc{}}

// playReq is play.txt: p the game to play, f the one to fill first, x
// the ones to remove (their games are deleted).
type playReq struct {
	play, first string
	remove      []string
}

// watchPlay is called every second by the main loop.
func watchPlay(ctx context.Context) {
	req, ok := readPlay()
	p := &play
	p.mu.Lock()
	defer p.mu.Unlock()
	if ok {
		if req.play != p.want {
			p.state, p.detail = "starting", ""
		}
		p.want, p.first = req.play, req.first
		for _, id := range req.remove {
			p.remove(id)
		}
	}
	// the streamed games not yet all in, and the one asked for
	todo := map[string]string{}
	for folder, r := range streamedGames() {
		if r.State != "complete" {
			todo[r.ID] = filepath.Join(*gamesDir, folder)
		}
	}
	if p.want != "" {
		dir := filepath.Join(*gamesDir, gameFolder(p.want))
		if r, have := readRec(dir); !have || r.State != "complete" {
			todo[p.want] = dir
		}
	}
	// deleted (a Memory screen) or done: stop
	for id, sp := range p.procs {
		if _, ok := todo[id]; !ok {
			if _, err := os.Stat(filepath.Join(sp.dir, streamFile)); err != nil {
				log.Printf("play %s: deleted; stopping", id)
				os.RemoveAll(stateDir(id))
			}
			sp.stop()
			delete(p.procs, id)
		}
	}
	for id, dir := range todo {
		sp := p.procs[id]
		if sp == nil {
			sp = &streamProc{id: id, dir: dir}
			p.procs[id] = sp
		}
		r, have := readRec(sp.dir)
		if sp.cmd == nil && time.Now().After(sp.retry) {
			if err := sp.start(ctx, r, have); err != nil {
				log.Printf("play %s: %v", id, err)
				if p.want == id {
					p.state, p.detail = "error", "Can't reach the Internet Archive"
				}
				sp.retry = time.Now().Add(10 * time.Second)
			}
		}
		if sp.cmd != nil && streamVerified(id) != "" {
			sp.finish()
		}
	}
	p.pickFiller()
	if want := p.procs[p.want]; want != nil && p.state == "starting" {
		if st, err := os.Stat(filepath.Join(want.dir, "default.xbe")); err == nil && st.Size() > 0 {
			if _, have := readRec(want.dir); have {
				p.state, p.detail = "ready", gameFolder(p.want)
				log.Printf("play %s: ready in %s", p.want, want.dir)
			}
		}
	} else if p.want != "" && p.state == "starting" && p.procs[p.want] == nil {
		// asked to play a game that is all in already
		p.state, p.detail = "ready", gameFolder(p.want)
	}
}

// pickFiller gives the -fill file to one streamer.
func (p *players) pickFiller() {
	f := ""
	for _, id := range []string{p.want, p.first, p.filler} {
		if id != "" && p.procs[id] != nil {
			f = id
			break
		}
	}
	if f == "" {
		var oldest time.Time
		for id, sp := range p.procs {
			st, err := os.Stat(filepath.Join(sp.dir, streamFile))
			if err == nil && (f == "" || st.ModTime().Before(oldest)) {
				f, oldest = id, st.ModTime()
			}
		}
	}
	if f != p.filler && f != "" {
		log.Printf("play %s: filling it first", f)
	}
	p.filler = f
	for id := range p.procs {
		fill := filepath.Join(stateDir(id), "fill")
		if id == f {
			if !fileExists(fill) {
				os.WriteFile(fill, nil, 0644)
			}
		} else {
			os.Remove(fill)
		}
	}
}

// remove: the Downloads page removed a streamed game: it is deleted.
func (p *players) remove(id string) {
	dir := filepath.Join(*gamesDir, gameFolder(id))
	r, have := readRec(dir)
	if !have || r.ID != id || r.State == "complete" {
		return
	}
	if sp := p.procs[id]; sp != nil {
		sp.stop()
		delete(p.procs, id)
	}
	os.RemoveAll(dir)
	os.RemoveAll(stateDir(id))
	if p.want == id {
		p.want, p.state, p.detail = "", "", ""
	}
	log.Printf("play %s: removed from the Downloads page; deleted %s", id, dir)
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
func (sp *streamProc) finish() {
	v := streamVerified(sp.id)
	sp.stop()
	r, _ := readRec(sp.dir)
	img := filepath.Join(sp.dir, "default.iso")
	if v == "ok" || v == "unchecked" {
		for _, ext := range []string{".stream", ".map", ".trace"} {
			os.Remove(img + ext)
		}
		r.State = "complete"
		writeRec(sp.dir, r)
		log.Printf("play %s: the whole disc is in (%s)", sp.id, v)
	} else {
		log.Printf("play %s: the disc didn't check out (%s); fetching it again", sp.id, v)
		os.Remove(img + ".map")
		os.Truncate(img, 0)
	}
	os.Remove(filepath.Join(stateDir(sp.id), "stats"))
}

func (sp *streamProc) stop() {
	if sp.cmd != nil {
		sp.cmd.Process.Kill()
		<-sp.exited
		sp.cmd = nil
	}
	os.Remove(sockPath(sp.id)) // nothing answers there now
}

// sockPath is a game's streamer socket (named in its default.iso.stream).
func sockPath(id string) string {
	return "/tmp/frolick-stream-" + strings.TrimSuffix(tileName(id), ".png") + ".sock"
}

// start runs the streamer for sp.id into its folder, making the folder and
// its record first (hashes from the Archive).
func (sp *streamProc) start(ctx context.Context, r streamRec, have bool) error {
	if !have {
		ident, file, ok := strings.Cut(sp.id, "/")
		if !ok {
			return fmt.Errorf("not a disc image")
		}
		c, cancel := context.WithTimeout(ctx, 90*time.Second)
		md5, sha1, err := fileHashes(c, ident, file)
		cancel()
		if err != nil {
			return err
		}
		if err := os.MkdirAll(sp.dir, 0755); err != nil {
			return err
		}
		r = streamRec{ID: sp.id, URL: fileURL(ident, file), MD5: md5, SHA1: sha1, State: "streaming"}
		if err := writeRec(sp.dir, r); err != nil {
			return err
		}
		log.Printf("play %s: installing in %s", sp.id, sp.dir)
	}
	sd := stateDir(sp.id)
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
	self := streamerBin
	if self == "" {
		self, _ = os.Executable()
	}
	cmd := exec.Command(self, "-stream", r.URL, "-image", filepath.Join(sp.dir, "default.iso"),
		"-socket", sockPath(sp.id), "-xbe", filepath.Join(sp.dir, "default.xbe"), "-stats", filepath.Join(sd, "stats"),
		"-fill", filepath.Join(sd, "fill"), "-md5", r.MD5, "-sha1", r.SHA1)
	cmd.Stdout, cmd.Stderr = logf, logf
	cmd.SysProcAttr = &syscall.SysProcAttr{Pdeathsig: syscall.SIGKILL} // never outlives the helper
	if err := cmd.Start(); err != nil {
		logf.Close()
		return err
	}
	log.Printf("play %s: streaming into %s", sp.id, sp.dir)
	sp.cmd = cmd
	exited := make(chan struct{})
	sp.exited = exited
	go func() {
		cmd.Wait()
		logf.Close()
		close(exited)
		play.mu.Lock()
		if sp.cmd == cmd {
			log.Printf("play %s: the streamer stopped; again shortly", sp.id)
			sp.cmd = nil
			sp.retry = time.Now().Add(5 * time.Second)
		}
		play.mu.Unlock()
	}()
	return nil
}

// streamIn is how much of a streamed game's disc is in, and its size.
func streamIn(sp *streamProc) (int64, int64) {
	st := readStats(sp.id)
	size, _ := strconv.ParseInt(st["size"], 10, 64)
	chunks, _ := strconv.ParseInt(st["chunks"], 10, 64)
	have, _ := strconv.ParseInt(st["have"], 10, 64)
	var in int64
	if chunks > 0 {
		in = min(size, have*((size+chunks-1)/chunks))
	}
	return in, size
}

// playStatus is the status.txt lines for streamed games:
//
//	installed <id> <folder>
//	streaming <id> <bytes in> <bytes> <1 filling | 0 only what it reads> <title>
//	stream <id> <state> <bytes in> <bytes> <detail>   (the game asked to play)
func playStatus() string {
	var out strings.Builder
	for folder, r := range streamedGames() {
		fmt.Fprintf(&out, "installed\t%s\t%s\n", r.ID, clean(folder))
	}
	p := &play
	p.mu.Lock()
	defer p.mu.Unlock()
	ids := make([]string, 0, len(p.procs))
	for id := range p.procs {
		ids = append(ids, id)
	}
	sort.Strings(ids)
	for _, id := range ids {
		sp := p.procs[id]
		in, size := streamIn(sp)
		if size == 0 {
			continue // its streamer hasn't said yet
		}
		fill := 0
		if id == p.filler {
			fill = 1
		}
		fmt.Fprintf(&out, "streaming\t%s\t%d\t%d\t%d\t%s\n", id, in, size, fill, clean(gameFolder(id)))
	}
	if p.want == "" {
		return out.String()
	}
	var in, size int64
	if sp := p.procs[p.want]; sp != nil {
		in, size = streamIn(sp)
	} else if fi, err := os.Stat(filepath.Join(*gamesDir, gameFolder(p.want), "default.iso")); err == nil {
		in, size = fi.Size(), fi.Size()
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
