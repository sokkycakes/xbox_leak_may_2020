package main

// A store card's Wine session that has nothing left to show.
//
// wincart-run ends a run when the game's own processes are gone, but only
// once it has seen them: a program that quits within a moment of starting
// (Diablo's SETUP.EXE, a stub that starts another program and exits) is
// never seen, and Wine's services (winedevice.exe and the rest) keep
// gamescope up on a blank screen.  So, for the store's own cards only:
// while gamescope runs and no Windows program but Wine's services has been
// running for idleLimit, gamescope is stopped, and wincart-run takes the
// rest down and goes back to the dashboard as when a game quits.

import (
	"flag"
	"log"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"time"
)

var (
	contyRoot  = flag.String("conty", "/mnt/conty", "the root wincart-run's Wine runs in")
	gamescopeP = flag.String("gamescope-pid", "/var/run/wincart/gamescope.pid", "wincart-run's gamescope")
)

const idleLimit = 15 * time.Second

// Wine's own programs that mean something is still happening: the batch
// file that starts the game or its installer, prefix setup, MSI installs.
var busyWine = map[string]bool{
	"cmd.exe": true, "start.exe": true, "wineboot.exe": true, "msiexec.exe": true,
	"rundll32.exe": true, "regsvr32.exe": true, "reg.exe": true, "regedit.exe": true,
	"wscript.exe": true, "cscript.exe": true, "control.exe": true, "notepad.exe": true,
	"winhlp32.exe": true, "hh.exe": true, "iexplore.exe": true, "winefile.exe": true,
}

var idleSince time.Time

func watchSession() {
	running, _ := os.ReadFile(*runningCar)
	kzi := strings.TrimSpace(string(running))
	pid := readPid(*gamescopeP)
	if kzi == "" || filepath.Dir(kzi) != filepath.Clean(*cartDir) || pid <= 0 || syscall.Kill(pid, 0) != nil {
		idleSince = time.Time{}
		return
	}
	if sessionBusy() {
		idleSince = time.Time{}
		return
	}
	if idleSince.IsZero() {
		idleSince = time.Now()
		return
	}
	if time.Since(idleSince) < idleLimit {
		return
	}
	log.Printf("%s: no Windows program left running for %s: closing the session", filepath.Base(kzi), idleLimit)
	syscall.Kill(pid, syscall.SIGTERM)
	idleSince = time.Time{}
}

// Whether any process in Wine's root is a Windows program other than Wine's
// background services.
func sessionBusy() bool {
	procs, _ := filepath.Glob("/proc/[0-9]*")
	for _, p := range procs {
		if root, err := os.Readlink(p + "/root"); err != nil || root != *contyRoot {
			continue
		}
		b, err := os.ReadFile(p + "/cmdline")
		if err != nil || len(b) == 0 {
			continue
		}
		arg0 := string(b)
		if i := strings.IndexByte(arg0, 0); i >= 0 {
			arg0 = arg0[:i]
		}
		if busyProgram(arg0) {
			return true
		}
	}
	return false
}

// arg0 of a Wine process is its Windows path ("C:\windows\system32\cmd.exe",
// "Z:\srv\game\SETUP.EXE"); everything else in the root (gamescope,
// Xwayland, the Guide, wineserver) has a Unix one.
func busyProgram(arg0 string) bool {
	if len(arg0) < 3 || arg0[1] != ':' || arg0[2] != '\\' {
		return false
	}
	low := strings.ToLower(arg0)
	if !strings.HasPrefix(low[2:], `\windows\`) {
		return true
	}
	return busyWine[low[strings.LastIndexByte(low, '\\')+1:]]
}

func readPid(path string) int {
	b, _ := os.ReadFile(path)
	n, _ := strconv.Atoi(strings.TrimSpace(string(b)))
	return n
}
