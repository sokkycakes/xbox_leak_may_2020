// xuirender draws an XUI scene to a PNG with the software backend.
//
//	xuirender [-skin pkg.xzp#skin.xur] [-pkg a.xzp,...] [-font f.ttf] [-size 1280x720]
//	          [-play Frame] [-t seconds] [-focus Id] [-nav down,down] scene out.png
package main

import (
	"flag"
	"fmt"
	"image"
	"image/color"
	"image/draw"
	"image/png"
	"os"
	"path/filepath"
	"strings"

	"emulauncher-rmlui-starter/internal/xui"
)

func main() {
	skin := flag.String("skin", "", "skin scene (path or pkg.xzp#skin.xur)")
	pkgs := flag.String("pkg", "", "comma-separated .xzp packages to resolve resources")
	fontPath := flag.String("font", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "fallback font")
	size := flag.String("size", "1280x720", "canvas size")
	play := flag.String("play", "", "named frame to play on the scene root")
	secs := flag.Float64("t", 1, "seconds to advance before drawing")
	nav := flag.String("nav", "", "comma-separated navigation steps (up/down/left/right/press)")
	bg := flag.String("bg", "202020", "background RGB hex, or 'none'")
	flag.Parse()
	if flag.NArg() != 2 {
		flag.Usage()
		os.Exit(2)
	}
	res := &xui.Resources{Fonts: map[string]string{"": *fontPath}}
	for _, p := range strings.Split(*pkgs, ",") {
		if p == "" {
			continue
		}
		pk, err := xui.OpenXZP(p)
		if err != nil {
			fail(err)
		}
		res.Packages = append(res.Packages, pk)
	}
	src := flag.Arg(0)
	res.Dirs = append(res.Dirs, filepath.Dir(strings.SplitN(src, "#", 2)[0]))
	root, err := res.Scene(src)
	if err != nil {
		fail(fmt.Errorf("%s: %w", src, err))
	}
	var skinEl *xui.Element
	if *skin != "" {
		if skinEl, err = res.Scene(*skin); err != nil {
			fail(fmt.Errorf("skin: %w", err))
		}
	}
	sc := xui.NewScene(root, skinEl)
	if *play != "" {
		sc.Root.Play(*play)
	}
	for _, step := range strings.Split(*nav, ",") {
		switch step {
		case "":
		case "press":
			sc.Press()
		default:
			sc.Navigate(step)
		}
		sc.Update(0.25)
	}
	for t := 0.0; t < *secs; t += 1.0 / 60 {
		sc.Update(1.0 / 60)
	}
	var w, h int
	fmt.Sscanf(*size, "%dx%d", &w, &h)
	img := image.NewRGBA(image.Rect(0, 0, w, h))
	if *bg != "none" {
		var r, g, b uint8
		fmt.Sscanf(*bg, "%02x%02x%02x", &r, &g, &b)
		draw.Draw(img, img.Bounds(), image.NewUniform(color.RGBA{r, g, b, 255}), image.Point{}, draw.Src)
	}
	rd := xui.NewRenderer(&xui.Soft{Dst: img}, res)
	rd.Draw(sc, 0, 0, 1)
	f, err := os.Create(flag.Arg(1))
	if err != nil {
		fail(err)
	}
	png.Encode(f, img)
	f.Close()
	if sc.Focus != nil {
		fmt.Println("focus:", sc.Focus.ID())
	}
}

func fail(err error) { fmt.Fprintln(os.Stderr, err); os.Exit(1) }
