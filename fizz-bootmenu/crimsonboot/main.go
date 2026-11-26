// Copyright 2026 the crimson project authors. SPDX-License-Identifier: BSD-3-Clause

// crimsonboot is the LinuxBoot boot menu for the Sion (Acer Chromebox CXI3,
// Google fizz). coreboot starts it in place of edk2. It shows a menu on the
// TV at the 15 kHz 2880x480i super resolution, with:
//
//	Crimson                  kexec EFI/BOOT/BOOTX64.EFI on the SIONBOOT partition
//	Crimson, previous image  kexec EFI/sion/previous.efi (if there is one)
//	UEFI (edk2)              reboot into the ROM's edk2 payload, once
//	LinuxBoot shell          gosh, for debugging; exit returns to the menu
//	Reboot / Power off
//
// The first Crimson entry boots by itself after a short countdown. A keyboard
// or a gamepad (d-pad and A/Start) works the menu.
//
// The payload choice lives in one CMOS byte that coreboot reads (see the
// payload_select driver in coreboot-patches/): 0xC5 asks coreboot for edk2 on
// the next boot only, and 0xB7 is coreboot's "LinuxBoot is starting" mark,
// which this program clears once the menu is on screen. A mark still set at
// the next power-on means LinuxBoot never got this far, so coreboot boots
// edk2 that once instead.
//
// Kernel command line options:
//
//	crimsonboot.timeout=N  seconds before the default entry boots (default 3;
//	                       0 boots it at once, -1 waits forever)
//	crimsonboot.cmos=0xNN  CMOS offset of the payload byte (default 0x6c,
//	                       must match CONFIG_PAYLOAD_CMOS_SELECT_OFFSET)
//	crimsonboot.font=0     keep the kernel's font (no widening for 480i)
//	crimsonboot.font=tall  also double the font's height (less interlace
//	                       flicker, but tall letters and 15 rows)
//	crimsonboot.show=hold  hide the menu: boot Crimson straight away unless a
//	                       button is held while LinuxBoot starts (the power
//	                       button, any key, or any gamepad button); the menu
//	                       then waits for a choice. Default: always show it.
//	crimsonboot.bootani=0  with show=hold, don't play the boot animation
//	                       (from SIONBOOT's sion/bootani/, when installed) while
//	                       Crimson loads
//	crimsonboot.bootaniargs=A,B  extra bootani options, comma-separated
//	                       (e.g. --short or --msaa,1)
//	crimsonboot.holdwait=MS how long to look for a held button, counted from
//	                       kernel start (default 2500), so a USB gamepad has
//	                       time to show up
package main

import (
	"encoding/binary"
	"fmt"
	"io"
	"log"
	"os"
	"os/exec"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"time"
	"unsafe"

	"github.com/u-root/u-root/pkg/libinit"
	"golang.org/x/sys/unix"
)

const (
	cmosNormalOnce = 0xc5 // edk2 once (coreboot clears it)
	cmosAltPending = 0xb7 // set by coreboot before starting LinuxBoot

	mountRoot = "/run/crimsonboot"
	ttyPath   = "/dev/tty0"
)

var (
	timeout    = 3
	cmosOffset = int64(0x6c)
	widenFont  = true
	tallFont   = false
	hideMenu   = false
	playAni    = true
	aniArgs    []string
	holdWait   = 2500 * time.Millisecond
)

// ---------------------------------------------------------------------------
// Logging goes to the kernel log, so it never lands on the menu screen.

var kmsg *os.File

func logf(format string, a ...any) {
	if kmsg == nil {
		kmsg, _ = os.OpenFile("/dev/kmsg", os.O_WRONLY, 0)
	}
	if kmsg != nil {
		fmt.Fprintf(kmsg, "<6>crimsonboot: "+format+"\n", a...)
	}
}

func parseCmdline() {
	b, err := os.ReadFile("/proc/cmdline")
	if err != nil {
		return
	}
	for _, f := range strings.Fields(string(b)) {
		k, v, ok := strings.Cut(f, "=")
		if !ok {
			continue
		}
		switch k {
		case "crimsonboot.timeout":
			if n, err := strconv.Atoi(v); err == nil {
				timeout = n
			}
		case "crimsonboot.cmos":
			if n, err := strconv.ParseInt(v, 0, 64); err == nil && n >= 0x0e && n <= 0x7f {
				cmosOffset = n
			}
		case "crimsonboot.bootani":
			playAni = v != "0"
		case "crimsonboot.bootaniargs":
			aniArgs = strings.Split(v, ",")
		case "crimsonboot.show":
			hideMenu = v == "hold"
		case "crimsonboot.holdwait":
			if n, err := strconv.Atoi(v); err == nil && n >= 0 {
				holdWait = time.Duration(n) * time.Millisecond
			}
		case "crimsonboot.font":
			widenFont = v != "0"
			tallFont = v == "tall"
		}
	}
}

// ---------------------------------------------------------------------------
// CMOS through /dev/port (index 0x70, data 0x71).

func cmosAccess(write bool, v byte) (byte, error) {
	f, err := os.OpenFile("/dev/port", os.O_RDWR, 0)
	if err != nil {
		return 0, err
	}
	defer f.Close()
	if _, err := f.WriteAt([]byte{byte(cmosOffset)}, 0x70); err != nil {
		return 0, err
	}
	b := []byte{v}
	if write {
		_, err = f.WriteAt(b, 0x71)
	} else {
		_, err = f.ReadAt(b, 0x71)
	}
	return b[0], err
}

func cmosRead() (byte, error) { return cmosAccess(false, 0) }
func cmosWrite(v byte) error  { _, err := cmosAccess(true, v); return err }

// ---------------------------------------------------------------------------
// "Show the menu" triggers for crimsonboot.show=hold.

// ecPowerButtonHeld reads the Chrome EC's switch byte from its LPC memory map
// (0x900 + EC_MEMMAP_SWITCHES). The EC sees the button itself, so a press
// that powered the machine on still reads as held here. The map is only
// trusted when it carries the EC's "EC" signature (QEMU reads 0xff).
func ecPowerButtonHeld() bool {
	f, err := os.Open("/dev/port")
	if err != nil {
		return false
	}
	defer f.Close()
	b := make([]byte, 2)
	if _, err := f.ReadAt(b, 0x900+0x20); err != nil || b[0] != 'E' || b[1] != 'C' {
		return false
	}
	if _, err := f.ReadAt(b[:1], 0x900+0x30); err != nil {
		return false
	}
	return b[0]&0x02 != 0 // EC_SWITCH_POWER_BUTTON_PRESSED
}

// heldInput names the first input device that has a key or button down
// right now (EVIOCGKEY), so a button held since before the device appeared
// counts as well as one pressed later.
func heldInput() string {
	paths, _ := filepath.Glob("/dev/input/event*")
	for _, p := range paths {
		f, err := os.Open(p)
		if err != nil {
			continue
		}
		var bits [96]byte // KEY_MAX 0x2ff
		req := uintptr(2<<30 | len(bits)<<16 | 'E'<<8 | 0x18)
		_, _, e := unix.Syscall(unix.SYS_IOCTL, f.Fd(), req, uintptr(unsafe.Pointer(&bits[0])))
		f.Close()
		if e != 0 {
			continue
		}
		for _, c := range bits {
			if c != 0 {
				name, _ := os.ReadFile("/sys/class/input/" + filepath.Base(p) + "/device/name")
				return strings.TrimSpace(string(name))
			}
		}
	}
	return ""
}

// buttonHeld reports a held power button, key or gamepad button.
func buttonHeld() bool {
	if ecPowerButtonHeld() {
		logf("power button held")
		return true
	}
	if n := heldInput(); n != "" {
		logf("button held on %q", n)
		return true
	}
	return false
}

// menuWanted looks for a held button until holdWait after kernel start.
func menuWanted() bool {
	for {
		if buttonHeld() {
			return true
		}
		var ts unix.Timespec
		unix.ClockGettime(unix.CLOCK_BOOTTIME, &ts)
		if time.Duration(ts.Nano()) >= holdWait {
			return false
		}
		time.Sleep(50 * time.Millisecond)
	}
}

// ---------------------------------------------------------------------------
// Console: a font widened for the 2880x480i super resolution, ANSI drawing.

type consoleFontOp struct {
	op, flags, width, height, charcount uint32
	_                                   uint32 // padding before the pointer on 64-bit
	data                                *byte
}

const (
	kdFontOpSet = 0
	kdFontOpGet = 1
	kdFontOp    = 0x4b72
)

func fbSize() (int, int, bool) {
	b, err := os.ReadFile("/sys/class/graphics/fb0/virtual_size")
	if err != nil {
		return 0, 0, false
	}
	x, y, ok := strings.Cut(strings.TrimSpace(string(b)), ",")
	if !ok {
		return 0, 0, false
	}
	w, _ := strconv.Atoi(x)
	h, _ := strconv.Atoi(y)
	return w, h, w > 0 && h > 0
}

// fitFont widens the console font so text has normal proportions on a 4:3
// TV. 2880x480: each pixel is a quarter of a normal 720-wide one, so the
// 8x16 font becomes 32x16 (90x30 characters, like 8x16 text on a 720x480
// screen). crimsonboot.font=tall doubles the height too (32x32, 90x15).
func fitFont(tty *os.File) {
	w, h, ok := fbSize()
	if !ok || !widenFont {
		return
	}
	sx := w * 3 / (h * 4) // horizontal stretch to get back to 4:3 pixels
	if sx < 1 {
		sx = 1
	}
	if sx > 4 {
		sx = 4
	}
	sy := 1
	if tallFont && h <= 576 && sx >= 2 {
		sy = 2
	}
	if sx == 1 && sy == 1 {
		return
	}
	buf := make([]byte, 512*32*4)
	get := consoleFontOp{op: kdFontOpGet, width: 32, height: 32, charcount: 512, data: &buf[0]}
	if _, _, e := unix.Syscall(unix.SYS_IOCTL, tty.Fd(), kdFontOp, uintptr(unsafe.Pointer(&get))); e != 0 {
		logf("reading the console font: %v", e)
		return
	}
	if get.width > 8 || get.height*uint32(sy) > 32 {
		logf("font %dx%d can't be widened", get.width, get.height)
		return
	}
	nw := 8 * sx
	bpr := (nw + 7) / 8 // bytes per row
	out := make([]byte, int(get.charcount)*32*bpr)
	for c := 0; c < int(get.charcount); c++ {
		for r := 0; r < int(get.height)*sy; r++ {
			src := buf[c*32+r/sy] // vpitch 32, one byte per row for width <= 8
			var row uint32
			for i := 0; i < 8; i++ {
				if src&(0x80>>i) != 0 {
					for k := 0; k < sx; k++ {
						row |= 1 << (31 - (i*sx + k))
					}
				}
			}
			var be [4]byte
			binary.BigEndian.PutUint32(be[:], row)
			copy(out[(c*32+r)*bpr:], be[:bpr])
		}
	}
	set := consoleFontOp{op: kdFontOpSet, width: uint32(nw), height: get.height * uint32(sy),
		charcount: get.charcount, data: &out[0]}
	if _, _, e := unix.Syscall(unix.SYS_IOCTL, tty.Fd(), kdFontOp, uintptr(unsafe.Pointer(&set))); e != 0 {
		logf("setting a %dx%d font: %v", set.width, set.height, e)
		return
	}
	logf("framebuffer %dx%d, console font %dx%d", w, h, set.width, set.height)
}

func termSize(tty *os.File) (int, int) {
	ws, err := unix.IoctlGetWinsize(int(tty.Fd()), unix.TIOCGWINSZ)
	if err != nil || ws.Col == 0 || ws.Row == 0 {
		return 80, 25
	}
	return int(ws.Col), int(ws.Row)
}

// quiet turns off echo and line mode on the console, so key presses (read
// from evdev) don't print over the menu. It returns a function that undoes it.
func quiet(tty *os.File) func() {
	t, err := unix.IoctlGetTermios(int(tty.Fd()), unix.TCGETS)
	if err != nil {
		return func() {}
	}
	old := *t
	t.Lflag &^= unix.ECHO | unix.ICANON
	unix.IoctlSetTermios(int(tty.Fd()), unix.TCSETS, t)
	return func() { unix.IoctlSetTermios(int(tty.Fd()), unix.TCSETS, &old) }
}

// ---------------------------------------------------------------------------
// Input: every /dev/input/event* device, so keyboards and gamepads both work
// (and ones plugged in while the menu is up).

type key int

const (
	keyUp key = iota
	keyDown
	keyEnter
	keyDigit // + n
)

const (
	evKey = 0x01
	evAbs = 0x03

	absY     = 0x01
	absHat0Y = 0x11

	btnSouth    = 0x130
	btnStart    = 0x13b
	btnDpadUp   = 0x220
	btnDpadDown = 0x221
)

func watchInput(keys chan<- key) {
	seen := map[string]bool{}
	for {
		paths, _ := filepath.Glob("/dev/input/event*")
		for _, p := range paths {
			if !seen[p] {
				seen[p] = true
				go readInput(p, keys)
			}
		}
		time.Sleep(500 * time.Millisecond)
	}
}

func readInput(path string, keys chan<- key) {
	f, err := os.Open(path)
	if err != nil {
		return
	}
	defer f.Close()
	var ev [24]byte // struct input_event on 64-bit
	stick := 0
	for {
		if _, err := f.Read(ev[:]); err != nil {
			return
		}
		typ := binary.LittleEndian.Uint16(ev[16:])
		code := binary.LittleEndian.Uint16(ev[18:])
		val := int32(binary.LittleEndian.Uint32(ev[20:]))
		send := func(k key) {
			select {
			case keys <- k:
			default:
			}
		}
		switch typ {
		case evKey:
			if val == 0 {
				continue
			}
			switch {
			case code == 103 || code == btnDpadUp: // KEY_UP
				send(keyUp)
			case code == 108 || code == btnDpadDown: // KEY_DOWN
				send(keyDown)
			case val != 1:
				// Auto-repeat only moves the selection.
			case code == 28 || code == 96 || code == 57 || code == btnSouth || code == btnStart: // ENTER, KPENTER, SPACE
				send(keyEnter)
			case code >= 2 && code <= 10: // KEY_1 .. KEY_9
				send(keyDigit + key(code-1))
			}
		case evAbs:
			switch code {
			case absHat0Y:
				if val < 0 {
					send(keyUp)
				} else if val > 0 {
					send(keyDown)
				}
			case absY: // Xbox-style sticks (+-32767); smaller ranges never trip it
				s := 0
				if val < -16000 {
					s = -1
				} else if val > 16000 {
					s = 1
				}
				if s != stick && s != 0 {
					if s < 0 {
						send(keyUp)
					} else {
						send(keyDown)
					}
				}
				stick = s
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Finding Crimson: FAT partitions with EFI/BOOT/BOOTX64.EFI and a sion/ folder.

type esp struct {
	dev       string // sda1
	dir       string // where it is mounted
	removable bool
}

func removable(dev string) bool {
	p, err := filepath.EvalSymlinks("/sys/class/block/" + dev)
	if err != nil {
		return false
	}
	if strings.Contains(p, "/usb") {
		return true
	}
	disk := p
	if _, err := os.Stat(p + "/partition"); err == nil {
		disk = filepath.Dir(p)
	}
	b, _ := os.ReadFile(disk + "/removable")
	return strings.TrimSpace(string(b)) == "1"
}

func isFile(p string) bool {
	st, err := os.Stat(p)
	return err == nil && st.Mode().IsRegular()
}

func isDir(p string) bool {
	st, err := os.Stat(p)
	return err == nil && st.IsDir()
}

// scanESPs mounts (read-only) any FAT partition not looked at yet and keeps
// the ones that hold a Crimson image.
func scanESPs(known map[string]*esp, tried map[string]bool) {
	devs, _ := os.ReadDir("/sys/class/block")
	for _, d := range devs {
		name := d.Name()
		if tried[name] || strings.HasPrefix(name, "loop") || strings.HasPrefix(name, "ram") {
			continue
		}
		// Whole disks only count when they have no partitions (superfloppy sticks).
		if _, err := os.Stat("/sys/class/block/" + name + "/partition"); err != nil {
			if m, _ := filepath.Glob("/sys/class/block/" + name + "/" + name + "*"); len(m) > 0 {
				continue
			}
		}
		tried[name] = true
		dir := filepath.Join(mountRoot, name)
		os.MkdirAll(dir, 0o755)
		if err := unix.Mount("/dev/"+name, dir, "vfat", unix.MS_RDONLY|unix.MS_NOSUID|unix.MS_NODEV, ""); err != nil {
			os.Remove(dir)
			continue
		}
		if isFile(dir+"/EFI/BOOT/BOOTX64.EFI") && isDir(dir+"/sion") {
			known[name] = &esp{dev: name, dir: dir, removable: removable(name)}
			logf("Crimson on %s (removable %v)", name, known[name].removable)
			continue
		}
		unix.Unmount(dir, 0)
		os.Remove(dir)
	}
}

// ---------------------------------------------------------------------------
// Menu entries.

type entry struct {
	label string
	run   func() error
}

type bootError struct{ msg string }

func (e *bootError) Error() string { return e.msg }

func crimsonEntries(e *esp) []entry {
	where := ""
	if e.removable {
		where = " (USB)"
	}
	var out []entry
	out = append(out, entry{"Crimson" + where, func() error {
		return kexecCrimson(e, e.dir+"/EFI/BOOT/BOOTX64.EFI")
	}})
	if isFile(e.dir + "/EFI/sion/previous.efi") {
		out = append(out, entry{"Crimson, previous image" + where, func() error {
			return kexecCrimson(e, e.dir+"/EFI/sion/previous.efi")
		}})
	}
	return out
}

// kexecCrimson starts a Crimson image (an EFI-stub bzImage with the system
// built in) the way the Sion's UEFI boot entry does: with
// sion/video-boot.cpio and its video= option when that kit is installed,
// plus anything in sion/linuxboot-cmdline.txt.
func kexecCrimson(e *esp, kernel string) error {
	if err := loadCrimson(e, kernel, nil); err != nil {
		return err
	}
	return startLoaded()
}

// startLoaded boots the kernel loadCrimson loaded.
func startLoaded() error {
	unmountAll()
	unix.Sync()
	return unix.Reboot(unix.LINUX_REBOOT_CMD_KEXEC)
}

// loadCrimson loads a Crimson image for kexec, with extra kernel options.
func loadCrimson(e *esp, kernel string, extra []string) error {
	k, err := os.Open(kernel)
	if err != nil {
		return err
	}
	defer k.Close()
	cmdline := []string{"sion.loader=linuxboot"}
	// Which SIONBOOT this is: Crimson's early init picks its ESP by it
	// (there is no BootCurrent without UEFI), not by a label a backup stick
	// can share.
	if u := partUUID(e.dev); u != "" {
		cmdline = append(cmdline, "sion.esp="+u)
	}
	initrd := -1
	flags := unix.KEXEC_FILE_NO_INITRAMFS
	if cpio := e.dir + "/sion/video-boot.cpio"; isFile(cpio) {
		f, err := os.Open(cpio)
		if err != nil {
			return err
		}
		defer f.Close()
		initrd = int(f.Fd())
		flags = 0
		cmdline = append(cmdline, "video=HDMI-A-1:2880x480M@30i")
	}
	cmdline = append(cmdline, extra...)
	if b, err := os.ReadFile(e.dir + "/sion/linuxboot-cmdline.txt"); err == nil {
		for _, l := range strings.Split(string(b), "\n") {
			if l = strings.TrimSpace(l); l != "" && !strings.HasPrefix(l, "#") {
				cmdline = append(cmdline, l)
			}
		}
	}
	c := strings.Join(cmdline, " ")
	logf("kexec %s (%s)", kernel, c)
	if err := unix.KexecFileLoad(int(k.Fd()), initrd, c, flags); err != nil {
		return &bootError{fmt.Sprintf("loading %s failed: %v", filepath.Base(kernel), err)}
	}
	return nil
}

// partUUID is a partition's GPT (or MBR) partition UUID, from sysfs.
func partUUID(dev string) string {
	b, err := os.ReadFile("/sys/class/block/" + dev + "/uevent")
	if err != nil {
		return ""
	}
	for _, l := range strings.Split(string(b), "\n") {
		if v, ok := strings.CutPrefix(l, "PARTUUID="); ok {
			return strings.ToLower(v)
		}
	}
	return ""
}

func unmountAll() {
	ds, _ := os.ReadDir(mountRoot)
	for _, d := range ds {
		unix.Unmount(filepath.Join(mountRoot, d.Name()), 0)
	}
}

func uefiOnce() error {
	if err := cmosWrite(cmosNormalOnce); err != nil {
		return &bootError{fmt.Sprintf("writing CMOS failed: %v", err)}
	}
	unmountAll()
	unix.Sync()
	return unix.Reboot(unix.LINUX_REBOOT_CMD_RESTART)
}

// ---------------------------------------------------------------------------
// Drawing.

type screen struct {
	tty        *os.File
	cols, rows int
}

func (s *screen) at(row int, text string, attr string) {
	col := (s.cols - len(text)) / 2
	if col < 0 {
		col = 0
	}
	fmt.Fprintf(s.tty, "\x1b[%d;%dH%s%s\x1b[0m", row+1, col+1, attr, text)
}

func (s *screen) clear() { fmt.Fprint(s.tty, "\x1b[0m\x1b[2J\x1b[H\x1b[?25l") }

func (s *screen) draw(entries []entry, sel int, footer, status string) {
	s.clear()
	// Keep clear of the TV's overscan: a line top and bottom.
	top := 1
	if s.rows >= 20 {
		top = 2
	}
	s.at(top, "C R I M S O N", "\x1b[1;31m")
	width := 0
	for _, e := range entries {
		if len(e.label) > width {
			width = len(e.label)
		}
	}
	width += 4
	first := top + 2
	if s.rows-first-3 < len(entries) {
		first = top + 1
	}
	for i, e := range entries {
		label := fmt.Sprintf("%-*s", width-2, e.label)
		if i == sel {
			s.at(first+i, " "+label+" ", "\x1b[1;37;41m")
		} else {
			s.at(first+i, " "+label+" ", "\x1b[37m")
		}
	}
	if status != "" {
		s.at(s.rows-3, status, "\x1b[1;33m")
	}
	s.at(s.rows-2, footer, "\x1b[37m")
}

// ---------------------------------------------------------------------------

// bootHidden is crimsonboot.show=hold: with no button held it kexecs the
// default Crimson without drawing any menu. When SIONBOOT has bootani, the
// boot animation plays meanwhile: it starts as soon as the GPU is up,
// Crimson is loaded while it runs, and the kexec happens when it ends. Its
// last frame stays on the screen through the kexec (bootani copies it into
// the console framebuffer, and i915 here leaves the pipe running), and
// Crimson gets sion.bootani=done so it keeps that frame instead of playing
// the animation again. A button pressed during the animation stops it and
// brings up the menu. bootHidden returns only when the menu should come up,
// with the reason.
func bootHidden() string {
	// Find Crimson first: the animation lives next to it, in SIONBOOT's
	// sion/bootani/. SATA is there at once; a USB stick gets until 2 s past
	// the hold window.
	known := map[string]*esp{}
	tried := map[string]bool{}
	var def *esp
	for {
		scanESPs(known, tried)
		for _, e := range known {
			if def == nil || (def.removable && !e.removable) || (def.removable == e.removable && e.dev < def.dev) {
				def = e
			}
		}
		if def != nil && !def.removable {
			break
		}
		var ts unix.Timespec
		unix.ClockGettime(unix.CLOCK_BOOTTIME, &ts)
		if time.Duration(ts.Nano()) >= holdWait+2*time.Second {
			break
		}
		if def != nil && time.Duration(ts.Nano()) >= holdWait {
			break
		}
		time.Sleep(50 * time.Millisecond)
	}

	var ani *exec.Cmd
	if def != nil {
		ani = startBootani(def.dir + "/sion/bootani")
	}
	aniDone := make(chan error, 1)
	aniRunning := ani != nil
	if ani != nil {
		go func() { aniDone <- ani.Wait() }()
	}
	stopAni := func() {
		if !aniRunning {
			return
		}
		ani.Process.Signal(unix.SIGTERM)
		select {
		case <-aniDone:
		case <-time.After(3 * time.Second):
			ani.Process.Kill()
			<-aniDone
		}
		aniRunning = false
	}
	toMenu := func(why string) string {
		stopAni()
		unmountAll()
		return why
	}

	if menuWanted() {
		return toMenu("button held")
	}
	if def == nil {
		return toMenu("no Crimson boot partition found")
	}
	clearPendingMark()
	kernel := def.dir + "/EFI/BOOT/BOOTX64.EFI"
	var extra []string
	if aniRunning {
		extra = []string{"sion.bootani=done"}
	}
	if err := loadCrimson(def, kernel, extra); err != nil {
		logf("loading Crimson: %v", err)
		return toMenu("loading Crimson failed")
	}
	if aniRunning {
		// Let the animation finish; a button pressed meanwhile opens the menu.
		tick := time.NewTicker(50 * time.Millisecond)
	wait:
		for {
			select {
			case err := <-aniDone:
				aniRunning = false
				if err != nil {
					// It didn't play through: let Crimson play its own.
					logf("bootani: %v", err)
					if err := loadCrimson(def, kernel, nil); err != nil {
						logf("loading Crimson: %v", err)
						tick.Stop()
						return toMenu("loading Crimson failed")
					}
				}
				break wait
			case <-tick.C:
				if buttonHeld() {
					tick.Stop()
					return toMenu("button pressed during the animation")
				}
			}
		}
		tick.Stop()
	}
	if err := startLoaded(); err != nil {
		logf("kexec: %v", err)
	}
	return toMenu("booting Crimson failed")
}

// startBootani starts the boot animation from dir (SIONBOOT's sion/bootani/,
// installed by bootani-esp/sion-install-bootani.sh) once the GPU is up, or
// returns nil when there is none to play. dir holds bootani with its own
// loader, libraries (lib/) and Mesa driver (dri/); it runs through that
// loader, so nothing needs execute permission or a fixed path on FAT.
func startBootani(dir string) *exec.Cmd {
	ld := dir + "/ld-linux-x86-64.so.2"
	if !playAni || !isFile(ld) || !isFile(dir+"/bootani") {
		return nil
	}
	// i915 probes in the background of the kernel's boot.
	for i := 0; i < 60 && !(exists("/dev/dri/card0") && exists("/dev/fb0")); i++ {
		time.Sleep(50 * time.Millisecond)
	}
	args := append([]string{"--library-path", dir + "/lib", dir + "/bootani",
		"--kms", "--to-fbdev", "/dev/fb0"}, aniArgs...)
	cmd := exec.Command(ld, args...)
	cmd.Env = append(os.Environ(), "LIBGL_DRIVERS_PATH="+dir+"/dri", "GBM_DRIVERS_PATH="+dir+"/dri")
	if log, err := os.Create("/run/bootani.log"); err == nil {
		cmd.Stdout, cmd.Stderr = log, log
	}
	if err := cmd.Start(); err != nil {
		logf("starting bootani: %v", err)
		return nil
	}
	logf("bootani started from %s", dir)
	return cmd
}

func exists(p string) bool {
	_, err := os.Stat(p)
	return err == nil
}

// clearPendingMark tells coreboot that LinuxBoot works.
func clearPendingMark() {
	if v, err := cmosRead(); err != nil {
		logf("reading CMOS: %v", err)
	} else if v == cmosAltPending {
		if err := cmosWrite(0); err != nil {
			logf("clearing the CMOS mark: %v", err)
		}
	}
}

// initSystem stands in for u-root's init when crimsonboot is PID 1 (the
// ROM's /init): the same root filesystem setup, but nothing on the screen.
// u-root's init prints a banner, its log lines and lets the kernel's
// warnings through while it mounts things.
func initSystem() {
	// Only emergencies reach the console from here on (quiet loglevel= is
	// for the kernel's own boot; this covers what init and kexec print).
	unix.Syscall(unix.SYS_SYSLOG, unix.SYSLOG_ACTION_CONSOLE_LEVEL, 0, 1)
	log.SetOutput(io.Discard)
	libinit.SetEnv()
	libinit.CreateRootfs()
	log.SetOutput(os.Stderr)
	// No reaper for orphans: a wait(-1) would also take bootani's and gosh's
	// exit status away from exec.Cmd.Wait. The few orphans a session here
	// can leave stay zombies until the kexec or reboot.
}

func main() {
	if os.Getpid() == 1 {
		initSystem()
	}
	parseCmdline()
	os.MkdirAll(mountRoot, 0o755)

	tty, err := os.OpenFile(ttyPath, os.O_RDWR, 0)
	if err != nil {
		tty = os.Stdout
	}
	restore := quiet(tty)

	if hideMenu {
		if quick := bootHidden(); quick != "" {
			logf("%s: showing the menu", quick)
		}
		timeout = -1
	}

	// i915 (or bochs in QEMU) needs a moment to probe; the menu needs its
	// framebuffer to pick the font.
	for i := 0; i < 50; i++ {
		if _, _, ok := fbSize(); ok {
			break
		}
		time.Sleep(100 * time.Millisecond)
	}
	fitFont(tty)
	fbW, fbH, _ := fbSize()

	keys := make(chan key, 8)
	go watchInput(keys)

	known := map[string]*esp{}
	tried := map[string]bool{}
	// SATA shows up at once; give USB sticks a moment.
	for i := 0; i < 20 && len(known) == 0; i++ {
		scanESPs(known, tried)
		if len(known) == 0 {
			time.Sleep(100 * time.Millisecond)
		}
	}

	// The menu is up: tell coreboot LinuxBoot works.
	clearPendingMark()

	build := func() []entry {
		var es []entry
		names := make([]string, 0, len(known))
		for n := range known {
			names = append(names, n)
		}
		// Internal disks first, then USB, each by name.
		sort.Slice(names, func(i, j int) bool {
			a, b := known[names[i]], known[names[j]]
			if a.removable != b.removable {
				return !a.removable
			}
			return a.dev < b.dev
		})
		for _, n := range names {
			es = append(es, crimsonEntries(known[n])...)
		}
		es = append(es,
			entry{"UEFI (edk2)", uefiOnce},
			entry{"LinuxBoot shell", func() error {
				restore()
				fmt.Fprint(tty, "\x1b[0m\x1b[2J\x1b[H\x1b[?25h")
				fmt.Fprintln(tty, "LinuxBoot shell. Type exit to go back to the menu.")
				cmd := exec.Command("gosh")
				cmd.Stdin, cmd.Stdout, cmd.Stderr = tty, tty, tty
				// Its own session with the console as controlling tty, so
				// Ctrl-C reaches what it runs.
				cmd.SysProcAttr = &unix.SysProcAttr{Setsid: true, Setctty: true, Ctty: 0}
				cmd.Run()
				restore = quiet(tty)
				// Keys typed into the shell also reached the menu's evdev
				// readers; drop them so a digit doesn't pick an entry.
				for len(keys) > 0 {
					<-keys
				}
				return nil
			}},
			entry{"Reboot", func() error { unmountAll(); unix.Sync(); return unix.Reboot(unix.LINUX_REBOOT_CMD_RESTART) }},
			entry{"Power off", func() error { unmountAll(); unix.Sync(); return unix.Reboot(unix.LINUX_REBOOT_CMD_POWER_OFF) }},
		)
		return es
	}

	entries := build()
	sel := 0
	left := timeout
	if len(known) == 0 {
		// No Crimson: hand over to edk2 unless someone picks something.
		sel = 0
		left = 10
	}
	status := ""
	if len(known) == 0 {
		status = "No Crimson boot partition found"
	}
	scr := &screen{tty: tty}

	tick := time.NewTicker(time.Second)
	defer tick.Stop()
	for {
		// The firmware's framebuffer (efifb/simpledrm) can be fb0 first and
		// i915's 2880x480i one replace it later: widen the font again then.
		if w, h, ok := fbSize(); ok && (w != fbW || h != fbH) {
			fbW, fbH = w, h
			fitFont(tty)
		}
		scr.cols, scr.rows = termSize(tty)
		footer := "Up/Down to choose, Enter or A to start"
		if left >= 0 {
			footer = fmt.Sprintf("%s in %d", entries[sel].label, left)
		}
		if left == 0 {
			left = -1
			if err := entries[sel].run(); err != nil {
				status = err.Error()
			}
			entries = build()
			continue
		}
		scr.draw(entries, sel, footer, status)

		var pick = -1
		select {
		case k := <-keys:
			left = -1 // any input stops the countdown
			switch {
			case k == keyUp:
				sel = (sel + len(entries) - 1) % len(entries)
			case k == keyDown:
				sel = (sel + 1) % len(entries)
			case k == keyEnter:
				pick = sel
			case k > keyDigit && int(k-keyDigit) <= len(entries):
				sel = int(k-keyDigit) - 1
				pick = sel
			}
		case <-tick.C:
			if left > 0 {
				left--
			}
			// Pick up USB sticks plugged in while the menu is up.
			n := len(known)
			scanESPs(known, tried)
			if len(known) != n {
				label := entries[sel].label
				entries = build()
				for i, e := range entries {
					if e.label == label {
						sel = i
					}
				}
				if n == 0 {
					status = ""
					if left >= 0 {
						sel, left = 0, timeout
					}
				}
			}
		}
		if pick >= 0 {
			scr.draw(entries, pick, "Starting "+entries[pick].label, "")
			if err := entries[pick].run(); err != nil {
				status = err.Error()
			} else {
				status = ""
			}
			entries = build()
			if sel >= len(entries) {
				sel = 0
			}
		}
	}
}
