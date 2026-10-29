package main

// Play as you download for Xbox disc images: -stream URL fills a local
// image file from an HTTP server that takes Range requests (the Archive's
// datanodes do) while xbcompat plays from it. The image is the size of the
// whole disc from the start; IMAGE.map holds a byte per chunk, set once that
// chunk is written; IMAGE.stream tells xbcompat the chunk size and the
// socket. A read in xbcompat that needs a missing chunk sends its number down
// the socket and gets it back once the chunk is in (or "!number" when it
// can't be had). Demand reads come first; the rest of the disc is filled
// front to back behind them. With -xbe, the disc's directory and its
// default.xbe are fetched first and the XBE is written out, so the title can
// start (the launch gate). Once every chunk is in, the image is checked
// against -md5 / -sha1.

import (
	"bufio"
	"context"
	"crypto/md5"
	"crypto/sha1"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"flag"
	"fmt"
	"hash"
	"io"
	"log"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"
)

var (
	streamURL   = flag.String("stream", "", "stream this disc image (an HTTP URL) into -image instead of running the store")
	streamImage = flag.String("image", "", "-stream: the local image file")
	streamSock  = flag.String("socket", "/tmp/frolick-stream.sock", "-stream: the socket xbcompat asks for chunks on")
	streamChunk = flag.Int64("chunk", 2<<20, "-stream: bytes per chunk")
	streamConns = flag.Int("conns", 4, "-stream: parallel range requests (one of them only ever takes demand reads)")
	streamMD5   = flag.String("md5", "", "-stream: the image's md5, checked once it is all in")
	streamSHA1  = flag.String("sha1", "", "-stream: the image's sha1, checked once it is all in")
	streamXBE   = flag.String("xbe", "", "-stream: write the disc's default.xbe here once its directory and the XBE are in")
	streamStats = flag.String("stats", "", "-stream: a status file, rewritten four times a second")
	streamFill  = flag.String("fill", "", "-stream: fill the image front to back only while this file is there (demand reads are always served)")
)

const xgd1Base = 0x18300000 // a pressed disc's game partition

type streamer struct {
	src    string // the URL as given, resolved again when a datanode fails
	url    string // where range requests go (the redirect's target)
	size   int64
	chunk  int64
	n      int
	img    *os.File
	mapf   *os.File
	start  time.Time
	mu     sync.Mutex
	cond   *sync.Cond
	have   []bool
	busy   []bool
	wait   map[int][]chan bool
	demand []int
	next   int // where the front-to-back fill goes on
	nhave  int

	fetched, misses int64
	readyAt         time.Duration
	verified        string
	filling         bool // -fill's file is there (or no -fill)
}

func runStream(ctx context.Context) {
	if *streamImage == "" {
		log.Fatal("-stream needs -image")
	}
	s := &streamer{src: *streamURL, chunk: *streamChunk, wait: map[int][]chan bool{}, start: time.Now()}
	s.cond = sync.NewCond(&s.mu)
	for {
		err := s.resolve(ctx)
		if err == nil {
			break
		}
		log.Printf("stream: %v; trying again", err)
		time.Sleep(3 * time.Second)
	}
	if err := s.open(); err != nil {
		log.Fatal(err)
	}
	log.Printf("stream: %s, %d bytes, %d of %d chunks here", s.url, s.size, s.nhave, s.n)
	if err := s.serve(); err != nil {
		log.Fatal(err)
	}
	s.filling = *streamFill == ""
	if *streamFill != "" {
		go func() {
			for {
				_, err := os.Stat(*streamFill)
				s.mu.Lock()
				if f := err == nil; f != s.filling {
					s.filling = f
					s.cond.Broadcast()
				}
				s.mu.Unlock()
				time.Sleep(time.Second)
			}
		}()
	}
	if *streamStats != "" {
		go func() {
			for {
				s.writeStats()
				time.Sleep(250 * time.Millisecond)
			}
		}()
	}
	for w := 0; w < *streamConns; w++ {
		go s.worker(ctx, w == 0)
	}
	if *streamXBE != "" {
		go func() {
			if err := s.gate(); err != nil {
				log.Printf("stream: the launch gate: %v", err)
				return
			}
			s.mu.Lock()
			s.readyAt = time.Since(s.start)
			f := s.fetched
			s.mu.Unlock()
			log.Printf("stream: %s is ready after %v, %d bytes fetched", *streamXBE, s.readyAt.Round(time.Millisecond), f)
		}()
	}
	s.mu.Lock()
	for s.nhave < s.n {
		s.cond.Wait()
	}
	s.mu.Unlock()
	log.Printf("stream: all %d chunks in after %v", s.n, time.Since(s.start).Round(time.Second))
	s.verify()
	s.writeStats()
	select {} // xbcompat keeps reading from the image; stopped from outside
}

// resolve follows the URL's redirect once and reads the size off a 1-byte range.
func (s *streamer) resolve(ctx context.Context) error {
	ctx, cancel := context.WithTimeout(ctx, 60*time.Second)
	defer cancel()
	resp, err := get(ctx, s.src, map[string]string{"Range": "bytes=0-0"})
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusPartialContent {
		return fmt.Errorf("%s: HTTP %d to a range request", s.src, resp.StatusCode)
	}
	cr := resp.Header.Get("Content-Range") // bytes 0-0/SIZE
	i := strings.LastIndexByte(cr, '/')
	size, err := strconv.ParseInt(cr[i+1:], 10, 64)
	if i < 0 || err != nil || size <= 0 {
		return fmt.Errorf("%s: no size in %q", s.src, cr)
	}
	if s.size != 0 && size != s.size {
		return fmt.Errorf("%s changed size (%d, was %d)", s.src, size, s.size)
	}
	s.size = size
	s.url = resp.Request.URL.String()
	return nil
}

func (s *streamer) open() error {
	var err error
	path := *streamImage
	os.MkdirAll(filepath.Dir(path), 0755)
	s.n = int((s.size + s.chunk - 1) / s.chunk)
	if s.img, err = os.OpenFile(path, os.O_RDWR|os.O_CREATE, 0644); err != nil {
		return err
	}
	if s.mapf, err = os.OpenFile(path+".map", os.O_RDWR|os.O_CREATE, 0644); err != nil {
		return err
	}
	st, _ := s.img.Stat()
	m := make([]byte, s.n)
	if st.Size() == s.size {
		io.ReadFull(io.NewSectionReader(s.mapf, 0, int64(s.n)), m)
	} else {
		// new, or another disc's: start over
		if err := s.img.Truncate(0); err != nil {
			return err
		}
		if err := s.img.Truncate(s.size); err != nil {
			return err
		}
	}
	if err := s.mapf.Truncate(int64(s.n)); err != nil {
		return err
	}
	if _, err := s.mapf.WriteAt(m, 0); err != nil {
		return err
	}
	s.have = make([]bool, s.n)
	s.busy = make([]bool, s.n)
	for i, b := range m {
		if b != 0 {
			s.have[i] = true
			s.nhave++
		}
	}
	return writeAtomic(path+".stream", []byte(fmt.Sprintf("chunk %d\nsocket %s\n", s.chunk, *streamSock)))
}

func (s *streamer) serve() error {
	os.Remove(*streamSock)
	l, err := net.Listen("unix", *streamSock)
	if err != nil {
		return err
	}
	go func() {
		for {
			c, err := l.Accept()
			if err != nil {
				log.Printf("stream: %v", err)
				return
			}
			go s.client(c)
		}
	}()
	return nil
}

// One xbcompat connection: a chunk number per line, answered when it's in.
func (s *streamer) client(c net.Conn) {
	defer c.Close()
	sc := bufio.NewScanner(c)
	for sc.Scan() {
		n, err := strconv.Atoi(strings.TrimSpace(sc.Text()))
		ok := err == nil && n >= 0 && n < s.n && s.need(n)
		reply := strconv.Itoa(n) + "\n"
		if !ok {
			reply = "!" + reply
		}
		if _, err := io.WriteString(c, reply); err != nil {
			return
		}
	}
}

// need blocks until chunk n is in, asking for it ahead of everything else.
func (s *streamer) need(n int) bool {
	s.mu.Lock()
	if s.have[n] {
		s.mu.Unlock()
		return true
	}
	ch := make(chan bool, 1)
	s.wait[n] = append(s.wait[n], ch)
	s.demand = append([]int{n}, s.demand...)
	s.misses++
	s.cond.Broadcast()
	s.mu.Unlock()
	return <-ch
}

// pick the next chunk to fetch: a demand first, else the next missing one
// front to back (not for the demand-only connection). -1: nothing to do.
func (s *streamer) pick(demandOnly bool) int {
	for len(s.demand) > 0 {
		n := s.demand[0]
		s.demand = s.demand[1:]
		if !s.have[n] && !s.busy[n] {
			s.busy[n] = true
			return n
		}
	}
	if demandOnly || !s.filling {
		return -1
	}
	for ; s.next < s.n; s.next++ {
		if !s.have[s.next] && !s.busy[s.next] {
			s.busy[s.next] = true
			return s.next
		}
	}
	return -1
}

func (s *streamer) worker(ctx context.Context, demandOnly bool) {
	buf := make([]byte, s.chunk)
	for {
		s.mu.Lock()
		n := s.pick(demandOnly)
		for n < 0 {
			if s.nhave == s.n {
				s.mu.Unlock()
				return
			}
			s.cond.Wait()
			n = s.pick(demandOnly)
		}
		s.mu.Unlock()
		for try := 0; ; try++ {
			err := s.fetch(ctx, n, buf)
			if err == nil {
				break
			}
			log.Printf("stream: chunk %d: %v", n, err)
			wait := time.Duration(1<<min(try, 5)) * time.Second
			var ra retryAfter
			if errors.As(err, &ra) && ra.d > 0 {
				wait = ra.d
			}
			time.Sleep(wait)
			if try%3 == 2 {
				// datanodes move: ask the Archive where the file is now
				if err := s.resolve(ctx); err != nil {
					log.Printf("stream: %v", err)
				}
			}
		}
	}
}

type retryAfter struct {
	code int
	d    time.Duration
}

func (r retryAfter) Error() string { return fmt.Sprintf("HTTP %d", r.code) }

func (s *streamer) fetch(ctx context.Context, n int, buf []byte) error {
	off := int64(n) * s.chunk
	end := min(off+s.chunk, s.size)
	ctx, cancel := context.WithTimeout(ctx, 2*time.Minute)
	defer cancel()
	s.mu.Lock()
	u := s.url
	s.mu.Unlock()
	resp, err := get(ctx, u, map[string]string{"Range": fmt.Sprintf("bytes=%d-%d", off, end-1)})
	if err != nil {
		return err
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusPartialContent {
		r := retryAfter{code: resp.StatusCode}
		if sec, err := strconv.Atoi(resp.Header.Get("Retry-After")); err == nil {
			r.d = time.Duration(sec) * time.Second
		}
		return r
	}
	b := buf[:end-off]
	if _, err := io.ReadFull(resp.Body, b); err != nil {
		return err
	}
	if _, err := s.img.WriteAt(b, off); err != nil {
		return err
	}
	// the data before its map byte: xbcompat reads the map, then the image
	if _, err := s.mapf.WriteAt([]byte{1}, int64(n)); err != nil {
		return err
	}
	s.mu.Lock()
	s.have[n] = true
	s.busy[n] = false
	s.nhave++
	s.fetched += int64(len(b))
	for _, ch := range s.wait[n] {
		ch <- true
	}
	delete(s.wait, n)
	s.cond.Broadcast()
	s.mu.Unlock()
	return nil
}

// readAt reads the image through the chunks, fetching what isn't in.
func (s *streamer) readAt(b []byte, off int64) error {
	if off < 0 || off+int64(len(b)) > s.size {
		return fmt.Errorf("read past the end of the disc (%d+%d)", off, len(b))
	}
	for c := int(off / s.chunk); len(b) > 0 && int64(c)*s.chunk < off+int64(len(b)); c++ {
		if !s.need(c) {
			return fmt.Errorf("chunk %d didn't arrive", c)
		}
	}
	_, err := s.img.ReadAt(b, off)
	return err
}

// gate fetches the disc's volume descriptor, root directory and default.xbe,
// and writes the XBE out for the title to start from.
func (s *streamer) gate() error {
	vd := make([]byte, 2048)
	var base int64 = -1
	for _, b := range []int64{0, xgd1Base} {
		if b+34*2048 > s.size {
			continue
		}
		if err := s.readAt(vd, b+32*2048); err != nil {
			return err
		}
		if string(vd[:20]) == "MICROSOFT*XBOX*MEDIA" {
			base = b
			break
		}
	}
	if base < 0 {
		return errors.New("no Xbox file system on the disc")
	}
	rootSec := binary.LittleEndian.Uint32(vd[20:])
	rootSize := binary.LittleEndian.Uint32(vd[24:])
	dir := make([]byte, rootSize)
	if err := s.readAt(dir, base+int64(rootSec)*2048); err != nil {
		return err
	}
	sec, size, ok := xdvdfsFind(dir, "default.xbe")
	if !ok {
		return errors.New("no default.xbe in the disc's root")
	}
	xbe := make([]byte, size)
	if err := s.readAt(xbe, base+int64(sec)*2048); err != nil {
		return err
	}
	return writeAtomic(*streamXBE, xbe)
}

// xdvdfsFind walks a directory table's tree (entries: left and right
// offsets in dwords, sector, size, attributes, name length, name).
func xdvdfsFind(tab []byte, name string) (uint32, uint32, bool) {
	seen := map[int]bool{}
	var walk func(off int) (uint32, uint32, bool)
	walk = func(off int) (uint32, uint32, bool) {
		if off < 0 || off+14 > len(tab) || seen[off] {
			return 0, 0, false
		}
		seen[off] = true
		e := tab[off:]
		l, r := int(binary.LittleEndian.Uint16(e)), int(binary.LittleEndian.Uint16(e[2:]))
		nl := int(e[13])
		if l == 0xFFFF || off+14+nl > len(tab) {
			return 0, 0, false
		}
		if strings.EqualFold(string(e[14:14+nl]), name) {
			return binary.LittleEndian.Uint32(e[4:]), binary.LittleEndian.Uint32(e[8:]), true
		}
		if l != 0 {
			if a, b, ok := walk(l * 4); ok {
				return a, b, true
			}
		}
		if r != 0 {
			return walk(r * 4)
		}
		return 0, 0, false
	}
	return walk(0)
}

func (s *streamer) verify() {
	var hs []hash.Hash
	var want []string
	if *streamMD5 != "" {
		hs, want = append(hs, md5.New()), append(want, strings.ToLower(*streamMD5))
	}
	if *streamSHA1 != "" {
		hs, want = append(hs, sha1.New()), append(want, strings.ToLower(*streamSHA1))
	}
	if len(hs) == 0 {
		s.verified = "unchecked"
		return
	}
	w := make([]io.Writer, len(hs))
	for i, h := range hs {
		w[i] = h
	}
	if _, err := io.Copy(io.MultiWriter(w...), io.NewSectionReader(s.img, 0, s.size)); err != nil {
		s.verified = "unreadable: " + err.Error()
		return
	}
	s.verified = "ok"
	for i, h := range hs {
		if got := hex.EncodeToString(h.Sum(nil)); got != want[i] {
			s.verified = "MISMATCH " + got
		}
	}
	log.Printf("stream: the image checks %s", s.verified)
}

func (s *streamer) writeStats() {
	if *streamStats == "" {
		return
	}
	s.mu.Lock()
	t := fmt.Sprintf("elapsed_ms %d\nsize %d\nchunks %d\nhave %d\nfetched %d\nmisses %d\nready_ms %d\nverified %s\n",
		time.Since(s.start).Milliseconds(), s.size, s.n, s.nhave, s.fetched, s.misses,
		s.readyAt.Milliseconds(), s.verified)
	s.mu.Unlock()
	writeAtomic(*streamStats, []byte(t))
}
