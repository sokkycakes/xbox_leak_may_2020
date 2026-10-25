package main

// Console Settings: what Frolick's Settings pages change on the Sion
// itself, and what they show about it. Frolick (nodes/SysSettings.cpp)
// writes E:\FROLICK\SETTINGS\settings.txt:
//
//	frolick-settings 1
//	display <mode>     480i, 480p, 720p or 1080p: the TV's mode
//	keep <0|1>         0 while the mode is on trial (the Keep prompt)
//	gpu <0|1>          games run on xbcompat-gpu (Developer Settings)
//	sync <n>           a new n asks for the clock from the Internet
//	screensaver <0|1>  (Frolick's own)
//	end
//
// and this helper writes E:\FROLICK\STORE\system.txt every few seconds:
//
//	frolick-system 1
//	gpu <0|1> <1 xbcompat-gpu is installed|0>
//	display <mode> <the modes the TV has, as 480i,480p,...>
//	sync <n> <ok|failed|running> <detail>
//	info <label> <value>      (System Info, in order)
//	end
//
// The mode changes between titles: xbox-av, which holds the TV while no
// title runs, takes the mode in /run/xbox-av-mode when a title leaves
// (Frolick relaunches itself to switch), and the next title keeps it. A
// kept mode goes in /etc/default/kms (KMS_MODE, persisted) for the next
// boot.

import (
	"bufio"
	"crypto/md5"
	"fmt"
	"io"
	"log"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
)

var (
	buildRev  = "dev" // -ldflags -X: the commit and day this helper was built
	buildDate = ""
)

var (
	settingsFile = "/data/xbox/hdd/partition1/FROLICK/SETTINGS/settings.txt"
	useGPUFile   = "/data/xbox/use-gpu"
	gpuBin       = "/opt/xbcompat/bin/xbcompat-gpu"
	xbcBin       = "/opt/xbcompat/bin/xbcompat"
	avModeFile   = "/run/xbox-av-mode"
	kmsFile      = "/etc/default/kms"
)

// The Xbox's video modes as KMS modes (xbox-av, KMS_MODE), the first one
// the TV lists: HDMI sends 480i pixel-doubled (1440x480i) on most TVs.
var displayModes = []struct {
	name string
	kms  []string
}{
	{"480i", []string{"720x480i", "1440x480i"}}, {"480p", []string{"720x480"}},
	{"720p", []string{"1280x720"}}, {"1080p", []string{"1920x1080"}},
}

// connectorModes: the modes the connected displays list (WxH, WxHi).
func connectorModes() map[string]bool {
	have := map[string]bool{}
	files, _ := filepath.Glob("/sys/class/drm/card*-*/modes")
	for _, f := range files {
		st, _ := os.ReadFile(filepath.Join(filepath.Dir(f), "status"))
		if strings.TrimSpace(string(st)) != "connected" {
			continue
		}
		b, _ := os.ReadFile(f)
		for _, l := range strings.Fields(string(b)) {
			have[l] = true
		}
	}
	return have
}

type sysState struct {
	mu        sync.Mutex
	lastSync  string // the sync counter last acted on
	syncState string
	syncText  string
	display   string // the mode last handed to xbox-av
	kept      string // the mode last written to KMS_MODE
	gpu       string
	info      [][2]string
	infoAt    time.Time
	written   string
}

var sys = sysState{syncState: "none"}

func readSettings() map[string]string {
	m := map[string]string{}
	b, err := os.ReadFile(settingsFile)
	if err != nil {
		return nil
	}
	whole := false
	for _, line := range strings.Split(strings.ReplaceAll(string(b), "\r", ""), "\n") {
		if line == "end" {
			whole = true
			break
		}
		k, v, _ := strings.Cut(line, "\t")
		m[k] = v
	}
	if !whole {
		return nil
	}
	return m
}

// watchSettings is called every second by the main loop.
func watchSettings() {
	s := &sys
	s.mu.Lock()
	defer s.mu.Unlock()
	if set := readSettings(); set != nil {
		s.applyGPU(set["gpu"])
		s.applyDisplay(set["display"], set["keep"] == "1")
		if n := set["sync"]; n != "" && n != s.lastSync {
			first := s.lastSync == ""
			s.lastSync = n
			if !first || n != "0" {
				s.syncState, s.syncText = "running", "Setting the clock..."
				go syncClock()
			}
		}
	}
	if time.Since(s.infoAt) > 30*time.Second {
		s.infoAt = time.Now()
		s.info = systemInfo()
	}
	s.write()
}

func (s *sysState) applyGPU(v string) {
	if v != "0" && v != "1" {
		return
	}
	on := fileExists(useGPUFile)
	if (v == "1") == on {
		return
	}
	if v == "1" {
		if !fileExists(gpuBin) {
			return
		}
		os.WriteFile(useGPUFile, nil, 0644)
		log.Printf("settings: games run on xbcompat-gpu")
	} else {
		os.Remove(useGPUFile)
		log.Printf("settings: games run on xbcompat")
	}
}

func kmsMode(name string) string {
	have := connectorModes()
	for _, m := range displayModes {
		if m.name != name {
			continue
		}
		for _, k := range m.kms {
			if have[k] {
				return k
			}
		}
		return m.kms[0]
	}
	return ""
}

func (s *sysState) applyDisplay(name string, keep bool) {
	kms := kmsMode(name)
	if kms == "" {
		return
	}
	if name != s.display {
		if err := os.WriteFile(avModeFile, []byte(kms+"\n"), 0644); err != nil {
			log.Printf("settings: %v", err)
			return
		}
		s.display = name
		log.Printf("settings: display %s (%s) from the next title", name, kms)
	}
	if keep && s.kept != name {
		cur := ""
		if b, err := os.ReadFile(kmsFile); err == nil {
			cur = string(b)
		}
		var out []string
		for _, l := range strings.Split(strings.TrimRight(cur, "\n"), "\n") {
			if l != "" && !strings.HasPrefix(l, "KMS_MODE=") {
				out = append(out, l)
			}
		}
		out = append(out, fmt.Sprintf("KMS_MODE=%q", kms))
		if err := os.WriteFile(kmsFile, []byte(strings.Join(out, "\n")+"\n"), 0644); err != nil {
			log.Printf("settings: %v", err)
			return
		}
		persist(kmsFile)
		s.kept = name
		log.Printf("settings: display %s kept (KMS_MODE %s)", name, kms)
	}
}

// persist keeps a file across reboots (or its removal), as installers do.
func persist(path string) {
	if _, err := exec.LookPath("sion"); err != nil {
		return
	}
	if out, err := exec.Command("sion", "persist", path).CombinedOutput(); err != nil {
		log.Printf("settings: sion persist %s: %v %s", path, err, strings.TrimSpace(string(out)))
	}
}

// syncClock sets the clock from the Internet (busybox ntpd), then the
// hardware clock.
func syncClock() {
	state, text := "ok", ""
	out, err := exec.Command("ntpd", "-n", "-q", "-p", "pool.ntp.org").CombinedOutput()
	if err != nil {
		state, text = "failed", "Couldn't reach a time server."
		log.Printf("settings: ntpd: %v %s", err, strings.TrimSpace(string(out)))
	} else {
		exec.Command("hwclock", "-w", "-u").Run()
		text = "Set from the Internet " + time.Now().UTC().Format("15:04 UTC")
		log.Printf("settings: clock set from the Internet")
	}
	sys.mu.Lock()
	sys.syncState, sys.syncText = state, text
	sys.mu.Unlock()
}

// tvModes: the Xbox modes the TV offers (the HDMI connector's mode list).
func tvModes() []string {
	have := connectorModes()
	var out []string
	for _, m := range displayModes {
		for _, k := range m.kms {
			if have[k] {
				out = append(out, m.name)
				break
			}
		}
	}
	return out
}

func (s *sysState) write() {
	var b strings.Builder
	b.WriteString("frolick-system 1\n")
	gpu, avail := "0", "0"
	if fileExists(useGPUFile) {
		gpu = "1"
	}
	if fileExists(gpuBin) {
		avail = "1"
	}
	fmt.Fprintf(&b, "gpu\t%s\t%s\n", gpu, avail)
	cur := s.display
	if cur == "" {
		cur = currentDisplay()
	}
	fmt.Fprintf(&b, "display\t%s\t%s\n", cur, strings.Join(tvModes(), ","))
	fmt.Fprintf(&b, "sync\t%s\t%s\t%s\n", s.lastSync, s.syncState, s.syncText)
	for _, kv := range s.info {
		fmt.Fprintf(&b, "info\t%s\t%s\n", kv[0], clean(kv[1]))
	}
	b.WriteString("end\n")
	if b.String() == s.written {
		return
	}
	if err := writeFileAtomic(filepath.Join(*storeDir, "system.txt"), []byte(b.String())); err == nil {
		s.written = b.String()
	}
}

func writeFileAtomic(path string, b []byte) error {
	tmp := path + ".new"
	if err := os.WriteFile(tmp, b, 0644); err != nil {
		return err
	}
	return os.Rename(tmp, path)
}

// currentDisplay: the boot's mode (KMS_MODE) as an Xbox mode name.
func currentDisplay() string {
	b, _ := os.ReadFile(kmsFile)
	for _, l := range strings.Split(string(b), "\n") {
		if v, ok := strings.CutPrefix(l, "KMS_MODE="); ok {
			v = strings.Trim(v, `"' `)
			for _, m := range displayModes {
				for _, k := range m.kms {
					if k == v {
						return m.name
					}
				}
			}
		}
	}
	return "480p"
}

// ---- System Info ---------------------------------------------------------

func firstLine(path string) string {
	b, err := os.ReadFile(path)
	if err != nil {
		return ""
	}
	l, _, _ := strings.Cut(string(b), "\n")
	return strings.TrimSpace(l)
}

func fileField(path, key string) string {
	f, err := os.Open(path)
	if err != nil {
		return ""
	}
	defer f.Close()
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		k, v, ok := strings.Cut(sc.Text(), ":")
		if ok && strings.TrimSpace(k) == key {
			return strings.TrimSpace(v)
		}
	}
	return ""
}

func sizeText(bytes float64) string {
	switch {
	case bytes >= 1e12:
		return fmt.Sprintf("%.1f TB", bytes/1e12)
	case bytes >= 1e9:
		return fmt.Sprintf("%.0f GB", bytes/1e9)
	default:
		return fmt.Sprintf("%.0f MB", bytes/1e6)
	}
}

// binInfo: a program's short md5 and the day it was built (its file's).
func binInfo(path string) string {
	f, err := os.Open(path)
	if err != nil {
		return "not installed"
	}
	defer f.Close()
	h := md5.New()
	io.Copy(h, f)
	st, _ := f.Stat()
	return fmt.Sprintf("%x, %s", h.Sum(nil)[:4], st.ModTime().UTC().Format("2006-01-02"))
}

// Intel GPUs by PCI device id (the Sion's and its kin).
var gpuNames = map[string]string{
	"0x5906": "Intel HD Graphics 610", "0x5902": "Intel HD Graphics 610", "0x5916": "Intel HD Graphics 620",
	"0x591e": "Intel HD Graphics 615", "0x3185": "Intel UHD Graphics 600", "0x3184": "Intel UHD Graphics 605",
}

func systemInfo() [][2]string {
	var info [][2]string
	add := func(k, v string) {
		if v != "" {
			info = append(info, [2]string{k, v})
		}
	}
	add("Console", "Sion")
	vendor, product := firstLine("/sys/class/dmi/id/sys_vendor"), firstLine("/sys/class/dmi/id/product_name")
	board := firstLine("/sys/class/dmi/id/board_name")
	if product != "" {
		add("Hardware", strings.TrimSpace(vendor+" "+product+" ("+board+")"))
	}
	cpu := fileField("/proc/cpuinfo", "model name")
	cpu = strings.NewReplacer("(R)", "", "(TM)", "", " CPU", "").Replace(cpu)
	add("Processor", fmt.Sprintf("%s, %d cores", strings.Join(strings.Fields(cpu), " "), runtime.NumCPU()))
	if kb, err := strconv.ParseFloat(strings.Fields(fileField("/proc/meminfo", "MemTotal") + " 0")[0], 64); err == nil && kb > 0 {
		add("Memory", fmt.Sprintf("%.0f GB", kb/1024/1024))
	}
	for _, card := range []string{"/sys/class/drm/card0", "/sys/class/drm/card1"} {
		id := firstLine(card + "/device/device")
		if id == "" {
			continue
		}
		name := gpuNames[id]
		if name == "" {
			name = firstLine(card+"/device/vendor") + ":" + id
		}
		add("Graphics", name)
		break
	}
	disks, _ := filepath.Glob("/sys/block/*/device/model")
	sort.Strings(disks)
	for _, m := range disks {
		dir := filepath.Dir(filepath.Dir(m))
		if firstLine(dir+"/removable") == "1" {
			continue
		}
		sectors, _ := strconv.ParseFloat(firstLine(dir+"/size"), 64)
		add("Storage", fmt.Sprintf("%s, %s", firstLine(m), sizeText(sectors*512)))
		break
	}
	if cards := firstLine("/proc/asound/cards"); cards != "" {
		if _, name, ok := strings.Cut(cards, "]: "); ok {
			add("Audio", strings.TrimSpace(name))
		}
	}
	modes := tvModes()
	if len(modes) > 0 {
		add("TV modes", strings.Join(modes, ", "))
	}
	if b, err := os.ReadFile("/etc/os-release"); err == nil {
		for _, l := range strings.Split(string(b), "\n") {
			if v, ok := strings.CutPrefix(l, "PRETTY_NAME="); ok {
				add("System", strings.Trim(v, `"`))
			}
		}
	}
	add("Kernel", firstLine("/proc/sys/kernel/osrelease"))
	add("xbcompat", binInfo(xbcBin))
	add("xbcompat-gpu", binInfo(gpuBin))
	rev := buildRev
	if buildDate != "" {
		rev += ", " + buildDate
	}
	add("Store helper", rev)
	if up := firstLine("/proc/uptime"); up != "" {
		if sec, err := strconv.ParseFloat(strings.Fields(up)[0], 64); err == nil {
			d := time.Duration(sec) * time.Second
			add("Uptime", fmt.Sprintf("%dh %02dm", int(d.Hours()), int(d.Minutes())%60))
		}
	}
	return info
}
