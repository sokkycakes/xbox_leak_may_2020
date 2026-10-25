//go:build xuiglshot

// xuiglshot renders an XUI panel through the GLES2 backend on a headless
// EGL context and saves the framebuffer as PNG (test tool).
package main

/*
#cgo LDFLAGS: -lEGL -lGLESv2
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <stdlib.h>
static const char* initctx(void) {
	PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
	EGLDisplay d = gpd ? gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL) : EGL_NO_DISPLAY;
	if (d == EGL_NO_DISPLAY) return "no display";
	if (!eglInitialize(d, NULL, NULL)) return "init";
	EGLint ca[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
	EGLConfig cfg; EGLint n = 0;
	if (!eglChooseConfig(d, ca, &cfg, 1, &n) || n == 0) return "config";
	eglBindAPI(EGL_OPENGL_ES_API);
	EGLint cx[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
	EGLContext c = eglCreateContext(d, cfg, EGL_NO_CONTEXT, cx);
	if (c == EGL_NO_CONTEXT) return "context";
	if (!eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, c)) return "current";
	return NULL;
}
static void readpix(int w, int h, void* p) { glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, p); }
*/
import "C"

import (
	"flag"
	"fmt"
	"image"
	"image/png"
	"os"
	"runtime"
	"strings"
	"unsafe"

	"emulauncher-rmlui-starter/internal/gles2"
	"emulauncher-rmlui-starter/internal/xui/xuigl"
)

func init() { runtime.LockOSThread() }

func main() {
	skin := flag.String("skin", "", "skin")
	font := flag.String("font", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "font")
	nav := flag.String("nav", "", "nav steps")
	secs := flag.Float64("t", 1, "seconds")
	flag.Parse()
	if e := C.initctx(); e != nil {
		fmt.Println("egl:", C.GoString(e))
		os.Exit(1)
	}
	const W, H = 1024, 512
	var tex, fbo uint32
	gles2.GenTextures(1, &tex)
	gles2.BindTexture(gles2.TEXTURE_2D, tex)
	gles2.TexImage2D(gles2.TEXTURE_2D, 0, gles2.RGBA, W, H, 0, gles2.RGBA, gles2.UNSIGNED_BYTE, nil)
	gles2.GenFramebuffers(1, &fbo)
	gles2.BindFramebuffer(gles2.FRAMEBUFFER, fbo)
	gles2.FramebufferTexture2D(gles2.FRAMEBUFFER, gles2.COLOR_ATTACHMENT0, gles2.TEXTURE_2D, tex, 0)
	gles2.Clear(gles2.COLOR_BUFFER_BIT)
	fmt.Printf("fbo status 0x%x\n", gles2.CheckFramebufferStatus(gles2.FRAMEBUFFER))
	p, err := xuigl.NewPanel(xuigl.PanelConfig{Scene: flag.Arg(0), Skin: *skin, Font: *font, W: W, H: H})
	if err != nil {
		fmt.Println(err)
		os.Exit(1)
	}
	p.Scene.Root.Play("Intro")
	for _, s := range strings.Split(*nav, ",") {
		if s == "press" {
			p.Scene.Press()
		} else if s != "" {
			p.Scene.Navigate(s)
		}
		p.Update(0.25)
	}
	for t := 0.0; t < *secs; t += 1.0 / 60 {
		p.Update(1.0 / 60)
	}
	gles2.ClearColor(0.125, 0.125, 0.125, 1)
	gles2.Clear(gles2.COLOR_BUFFER_BIT)
	p.Render()
	fmt.Printf("gl error 0x%x\n", int(C.glGetError()))
	img := image.NewRGBA(image.Rect(0, 0, W, H))
	C.readpix(W, H, unsafe.Pointer(&img.Pix[0]))
	// GL rows start at the bottom.
	row := make([]byte, W*4)
	for y := 0; y < H/2; y++ {
		a, b := img.Pix[y*W*4:(y+1)*W*4], img.Pix[(H-1-y)*W*4:(H-y)*W*4]
		copy(row, a)
		copy(a, b)
		copy(b, row)
	}
	f, _ := os.Create(flag.Arg(1))
	png.Encode(f, img)
	f.Close()
	fmt.Println("focus:", p.Scene.Focus.ID())
}
