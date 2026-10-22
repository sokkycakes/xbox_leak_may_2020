package main

// The store's cards as the dashboards' Memory screens see them.
//
// A card lives in three places: its stub in E:\Games\<cid> (all that the
// Memory screens list), its files in /data/frolick/carts and its Wine
// prefix in /data/wincarts/<cid>.  So that the Memory screens show what the
// game really takes, the stub folder holds frolick-size.dat, a sparse file
// as long as the other two folders (it takes no space itself).  And when
// the stub is deleted there (or from Frolick's Game Storage), the rest of
// the card goes too, with its download record, so the store offers it
// again.  Only the store's own cards: a .kzi in -carts that this helper
// wrote, whose stub it saw (.frolick-listed in the cart's folder).

import (
	"bufio"
	"log"
	"os"
	"path/filepath"
	"strings"
	"syscall"
	"time"
)

const sizeFile = "frolick-size.dat"

var lastSized time.Time

func syncCards() {
	if st, err := os.Stat(*gamesDir); err != nil || !st.IsDir() {
		return // E:\Games isn't there: never take that for a deletion
	}
	resize := time.Since(lastSized) > time.Minute
	if resize {
		lastSized = time.Now()
	}
	kzis, _ := filepath.Glob(filepath.Join(*cartDir, "*.kzi"))
	running, _ := os.ReadFile(*runningCar)
	for _, k := range kzis {
		c, ok := readKzi(k)
		if !ok || !storeKzi(k) || strings.TrimSpace(string(running)) == k {
			continue
		}
		folder := filepath.Join(*cartDir, c.CID)
		listed := filepath.Join(folder, ".frolick-listed")
		stub := stubFolder(c.CID)
		if fileExists(filepath.Join(stub, ".wincart")) {
			if !fileExists(listed) {
				os.WriteFile(listed, nil, 0644)
			}
			if resize {
				sizeStub(c.CID)
			}
		} else if fileExists(listed) && !fileExists(stub) {
			uninstall(c)
		}
	}
}

// A .kzi this helper wrote.
func storeKzi(path string) bool {
	f, err := os.Open(path)
	if err != nil {
		return false
	}
	defer f.Close()
	line, _ := bufio.NewReader(f).ReadString('\n')
	return strings.HasPrefix(line, "# A Frolick store download (frolick-store)")
}

// frolick-size.dat as long as the card's files and prefix.
func sizeStub(cid string) {
	total := spaceUsed(filepath.Join(*cartDir, cid)) + spaceUsed(filepath.Join(*wincartDir, cid))
	p := filepath.Join(stubFolder(cid), sizeFile)
	if st, err := os.Stat(p); err == nil && abs64(st.Size()-total) < 1<<20 {
		return
	}
	f, err := os.OpenFile(p, os.O_CREATE|os.O_WRONLY, 0644)
	if err != nil {
		return
	}
	f.Truncate(0)
	err = f.Truncate(total)
	f.Close()
	// only while it stays sparse: it must never take the space it reports
	var st syscall.Stat_t
	if err != nil || syscall.Stat(p, &st) != nil || st.Blocks*512 > 1<<20 {
		os.Remove(p)
	}
}

// What a folder takes on disk, not following links (the prefix links the
// game's files on the card).
func spaceUsed(dir string) int64 {
	var n int64
	filepath.WalkDir(dir, func(p string, d os.DirEntry, err error) error {
		if err != nil {
			return nil
		}
		var st syscall.Stat_t
		if syscall.Lstat(p, &st) == nil {
			n += st.Blocks * 512
		}
		return nil
	})
	return n
}

func abs64(n int64) int64 {
	if n < 0 {
		return -n
	}
	return n
}

// The card's stub was deleted: its files, prefix and download record go.
func uninstall(c cart) {
	prefix := filepath.Join(*wincartDir, c.CID)
	folder := filepath.Join(*cartDir, c.CID)
	for _, d := range []string{prefix, folder} {
		if m := mountUnder(d); m != "" {
			log.Printf("%s: deleted from the dashboard, but %s is mounted: left as it is", c.CID, m)
			return
		}
	}
	os.RemoveAll(prefix)
	os.RemoveAll(folder)
	os.Remove(kziPath(c.CID))
	ents, _ := os.ReadDir(*dlDir)
	for _, e := range ents {
		b, err := os.ReadFile(filepath.Join(*dlDir, e.Name(), "frolick-item.txt"))
		if err == nil && strings.Contains(string(b), "\ncard\t"+c.CID+"\t") {
			os.RemoveAll(filepath.Join(*dlDir, e.Name()))
		}
	}
	log.Printf("%s: deleted from the dashboard: removed its files, Wine prefix and download", c.CID)
}

// A mount point at or under dir, or "".
func mountUnder(dir string) string {
	b, _ := os.ReadFile("/proc/self/mounts")
	for _, line := range strings.Split(string(b), "\n") {
		f := strings.Fields(line)
		if len(f) < 2 {
			continue
		}
		m := strings.ReplaceAll(f[1], `\040`, " ")
		if m == dir || strings.HasPrefix(m, dir+"/") {
			return m
		}
	}
	return ""
}
