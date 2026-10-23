package main

// Play as you download from the dashboard. Pressing Play on a Streaming
// Download item writes play.txt in the store folder:
//
//	frolick-play 1
//	p <id>
//	end
//
// (an empty one when nothing is to be played; the dashboard writes that
// when it starts, so a title that has ended takes its disc out). This
// helper then runs itself with -stream for that disc image into
// -streams/<name>/disc.iso, and once the launch gate is open (the disc's
// directory and default.xbe are in) names the image in <hdd>/.dvdimage:
// xbcompat puts it in the DVD tray, and the dashboard starts it from
// there as it would a disc. The streamer goes on filling the rest of the
// disc behind the game's reads, and after the game too, until it is all
// in; a later Play starts at once. Status lines in status.txt:
//
//	stream <id> <starting|ready|error> <bytes in> <bytes> <detail>

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
	"time"
)

var (
	hddDir     = flag.String("hdd", "/data/xbox/hdd", "the Xbox hard disk (the tray's .dvdimage goes here)")
	streamsDir = flag.String("streams", "/data/frolick/stream", "where streamed disc images go")
)

const playSock = "/tmp/frolick-stream.sock"

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

type player struct {
	mu     sync.Mutex
	id     string // the disc being streamed ("" none)
	dir    string
	cmd    *exec.Cmd
	exited chan struct{}
	state  string
	detail string
	retry  time.Time
}

var play player

func streamDir(id string) string {
	return filepath.Join(*streamsDir, strings.TrimSuffix(tileName(id), ".png"))
}

func dvdImageFile() string { return filepath.Join(*hddDir, ".dvdimage") }

// takeOut empties the tray, if it holds one of ours.
func takeOut() {
	b, err := os.ReadFile(dvdImageFile())
	if err == nil && strings.HasPrefix(strings.TrimSpace(string(b)), *streamsDir+"/") {
		os.Remove(dvdImageFile())
		log.Printf("play: the streamed disc is out of the tray")
	}
}

// watchPlay is called every second by the main loop.
func watchPlay(ctx context.Context) {
	want, ok := readPlay()
	if !ok {
		return
	}
	p := &play
	p.mu.Lock()
	defer p.mu.Unlock()
	if want == "" {
		takeOut()
		// the streamer keeps filling the disc it has
		if p.state == "ready" {
			p.state = "idle"
		}
		return
	}
	if p.state == "idle" {
		p.state = "starting" // in the tray again below
	}
	if want != p.id {
		takeOut()
		p.stop()
		p.id, p.dir = want, streamDir(want)
		p.state, p.detail, p.retry = "starting", "", time.Time{}
	}
	if p.cmd == nil && time.Now().After(p.retry) {
		if err := p.start(ctx); err != nil {
			log.Printf("play %s: %v", p.id, err)
			p.state, p.detail = "error", "Can't reach the Internet Archive"
			p.retry = time.Now().Add(10 * time.Second)
		}
	}
	if p.state == "starting" {
		if st, err := os.Stat(filepath.Join(p.dir, "default.xbe")); err == nil && st.Size() > 0 {
			img := filepath.Join(p.dir, "disc.iso")
			if err := writeAtomic(dvdImageFile(), []byte(img+"\n")); err != nil {
				p.state, p.detail = "error", "Can't put the disc in the tray"
				return
			}
			p.state = "ready"
			log.Printf("play %s: ready, %s is in the tray", p.id, img)
		}
	}
}

func (p *player) stop() {
	if p.cmd != nil {
		p.cmd.Process.Kill()
		<-p.exited
		p.cmd = nil
	}
}

// start runs the streamer for p.id (its hashes and size from the Archive).
func (p *player) start(ctx context.Context) error {
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
	// a fresh gate: default.xbe is written again once it's in
	os.Remove(filepath.Join(p.dir, "default.xbe"))
	if p.state != "starting" {
		p.state, p.detail = "starting", ""
	}
	logf, err := os.OpenFile(filepath.Join(p.dir, "fetch.log"), os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0644)
	if err != nil {
		return err
	}
	self, _ := os.Executable()
	cmd := exec.Command(self, "-stream", fileURL(ident, file), "-image", filepath.Join(p.dir, "disc.iso"),
		"-socket", playSock, "-xbe", filepath.Join(p.dir, "default.xbe"), "-stats", filepath.Join(p.dir, "stats"),
		"-md5", md5, "-sha1", sha1)
	cmd.Stdout, cmd.Stderr = logf, logf
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

// playStatus is the status.txt line for the disc being streamed.
func playStatus() string {
	p := &play
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.id == "" {
		return ""
	}
	var have, size, chunks int64
	if f, err := os.Open(filepath.Join(p.dir, "stats")); err == nil {
		sc := bufio.NewScanner(f)
		for sc.Scan() {
			k, v, _ := strings.Cut(sc.Text(), " ")
			n, _ := strconv.ParseInt(v, 10, 64)
			switch k {
			case "size":
				size = n
			case "chunks":
				chunks = n
			case "have":
				have = n
			}
		}
		f.Close()
	}
	in := int64(0)
	if chunks > 0 {
		in = min(size, have*((size+chunks-1)/chunks))
	}
	return fmt.Sprintf("stream\t%s\t%s\t%d\t%d\t%s\n", p.id, p.state, in, size, clean(p.detail))
}
