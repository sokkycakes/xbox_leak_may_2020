// xuidump prints the element tree of .xur scenes or every scene in a .xzp.
package main

import (
	"fmt"
	"os"
	"sort"
	"strings"

	"emulauncher-rmlui-starter/internal/xui"
)

func main() {
	quiet := false
	args := os.Args[1:]
	if len(args) > 0 && args[0] == "-q" {
		quiet, args = true, args[1:]
	}
	ok, bad := 0, 0
	for _, path := range args {
		scenes := map[string][]byte{}
		if strings.HasSuffix(strings.ToLower(path), ".xzp") {
			pk, err := xui.OpenXZP(path)
			if err != nil {
				fmt.Println(path, err)
				bad++
				continue
			}
			for n, b := range pk.Files {
				if strings.HasSuffix(strings.ToLower(n), ".xur") {
					scenes[path+"#"+n] = b
				}
			}
		} else {
			b, err := os.ReadFile(path)
			if err != nil {
				fmt.Println(err)
				continue
			}
			scenes[path] = b
		}
		names := make([]string, 0, len(scenes))
		for n := range scenes {
			names = append(names, n)
		}
		sort.Strings(names)
		for _, n := range names {
			var root *xui.Element
			var err error
			if strings.HasSuffix(strings.ToLower(n), ".xui") {
				root, err = xui.ParseXUI(scenes[n])
			} else {
				root, err = xui.ParseXUR(scenes[n])
			}
			if err != nil {
				fmt.Println("FAIL", n, err)
				bad++
				continue
			}
			ok++
			if !quiet {
				fmt.Println("==", n)
				dump(root, 0)
			}
		}
	}
	fmt.Fprintf(os.Stderr, "%d ok, %d failed\n", ok, bad)
}

func dump(e *xui.Element, depth int) {
	ind := strings.Repeat("  ", depth)
	keys := make([]string, 0, len(e.Props))
	for k := range e.Props {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	var parts []string
	for _, k := range keys {
		parts = append(parts, fmt.Sprintf("%s=%v", k, short(e.Props[k])))
	}
	fmt.Printf("%s%s %s\n", ind, e.Class, strings.Join(parts, " "))
	for _, nf := range e.NamedFrames {
		fmt.Printf("%s  @%s t=%d cmd=%d %s\n", ind, nf.Name, nf.Time, nf.Command, nf.Target)
	}
	for _, tl := range e.Timelines {
		var ps []string
		for _, p := range tl.Props {
			ps = append(ps, strings.Join(p.Names, "."))
		}
		fmt.Printf("%s  ~%s [%s] %d keys", ind, tl.Target, strings.Join(ps, ","), len(tl.Keys))
		for i, k := range tl.Keys {
			if i >= 4 {
				fmt.Print(" ...")
				break
			}
			fmt.Printf(" %d:%v", k.Time, short(k.Values))
		}
		fmt.Println()
	}
	for _, c := range e.Children {
		dump(c, depth+1)
	}
}

func short(v any) string {
	s := fmt.Sprintf("%v", v)
	if p, ok := v.(*xui.Points); ok {
		s = fmt.Sprintf("Points(%gx%g,%d)", p.W, p.H, len(p.Pts))
	}
	if b, ok := v.([]byte); ok {
		s = fmt.Sprintf("bytes(%d)", len(b))
	}
	if len(s) > 80 {
		s = s[:80] + "…"
	}
	return s
}
