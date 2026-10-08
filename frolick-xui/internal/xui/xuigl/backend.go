// Package xuigl draws XUI scenes with GLES2 into the currently bound
// framebuffer (Frolick's panel FBO).
package xuigl

import (
	"fmt"
	"image"
	"strings"
	"unsafe"

	"emulauncher-rmlui-starter/internal/gles2"
	"emulauncher-rmlui-starter/internal/xui"
)

// Backend implements xui.Backend on GLES2.
type Backend struct {
	prog        uint32
	uProj, uTex int32
	aPos, aUV   uint32
	aCol        uint32
	vbo, ibo    uint32
	white       uint32
	texs        []uint32
	w, h        int
	verts       []float32
	clipOn      bool
}

// New compiles the shader and creates buffers. Needs a current GL context.
func New() (*Backend, error) {
	const vs = `
		attribute vec2 a_pos;
		attribute vec2 a_uv;
		attribute vec4 a_col;
		uniform vec4 u_proj; // 2/w, -2/h, -1, 1
		varying vec2 v_uv;
		varying vec4 v_col;
		void main() {
			v_uv = a_uv;
			v_col = a_col;
			gl_Position = vec4(a_pos.x * u_proj.x + u_proj.z, a_pos.y * u_proj.y + u_proj.w, 0.0, 1.0);
		}`
	const fs = `
		precision mediump float;
		varying vec2 v_uv;
		varying vec4 v_col;
		uniform sampler2D u_tex;
		void main() {
			gl_FragColor = texture2D(u_tex, v_uv) * vec4(v_col.rgb * v_col.a, v_col.a);
		}`
	prog, err := link(vs, fs)
	if err != nil {
		return nil, err
	}
	b := &Backend{
		prog:  prog,
		uProj: gles2.GetUniformLocation(prog, "u_proj"),
		uTex:  gles2.GetUniformLocation(prog, "u_tex"),
		aPos:  uint32(gles2.GetAttribLocation(prog, "a_pos")),
		aUV:   uint32(gles2.GetAttribLocation(prog, "a_uv")),
		aCol:  uint32(gles2.GetAttribLocation(prog, "a_col")),
	}
	gles2.GenBuffers(1, &b.vbo)
	gles2.GenBuffers(1, &b.ibo)
	b.white = b.upload(image.NewRGBA(image.Rect(0, 0, 1, 1)), true)
	return b, nil
}

// Begin prepares to draw a w×h scene into the bound framebuffer.
func (b *Backend) Begin(w, h int) {
	b.w, b.h = w, h
	gles2.Viewport(0, 0, int32(w), int32(h))
	gles2.Disable(gles2.DEPTH_TEST)
	gles2.Disable(gles2.CULL_FACE)
	gles2.Disable(gles2.SCISSOR_TEST)
	gles2.Enable(gles2.BLEND)
	gles2.UseProgram(b.prog)
	// Same orientation as the RmlUi pass: scene y=0 at the top of the FBO.
	gles2.Uniform4f(b.uProj, 2/float32(w), -2/float32(h), -1, 1)
	gles2.Uniform1i(b.uTex, 0)
	gles2.ActiveTexture(gles2.TEXTURE0)
	gles2.BindBuffer(gles2.ARRAY_BUFFER, b.vbo)
	gles2.BindBuffer(gles2.ELEMENT_ARRAY_BUFFER, b.ibo)
	stride := int32(8 * 4)
	gles2.EnableVertexAttribArray(b.aPos)
	gles2.VertexAttribPointer(b.aPos, 2, gles2.FLOAT, false, stride, gles2.PtrOffset(0))
	gles2.EnableVertexAttribArray(b.aUV)
	gles2.VertexAttribPointer(b.aUV, 2, gles2.FLOAT, false, stride, gles2.PtrOffset(2*4))
	gles2.EnableVertexAttribArray(b.aCol)
	gles2.VertexAttribPointer(b.aCol, 4, gles2.FLOAT, false, stride, gles2.PtrOffset(4*4))
}

// End restores the state the rest of the frame expects.
func (b *Backend) End() {
	gles2.DisableVertexAttribArray(b.aPos)
	gles2.DisableVertexAttribArray(b.aUV)
	gles2.DisableVertexAttribArray(b.aCol)
	gles2.BindBuffer(gles2.ARRAY_BUFFER, 0)
	gles2.BindBuffer(gles2.ELEMENT_ARRAY_BUFFER, 0)
	gles2.BindTexture(gles2.TEXTURE_2D, 0)
	gles2.Disable(gles2.SCISSOR_TEST)
	gles2.BlendFunc(gles2.SRC_ALPHA, gles2.ONE_MINUS_SRC_ALPHA)
	gles2.UseProgram(0)
}

func (b *Backend) Upload(img *image.RGBA) xui.Texture {
	b.texs = append(b.texs, b.upload(img, false))
	return xui.Texture(len(b.texs))
}

func (b *Backend) upload(img *image.RGBA, white bool) uint32 {
	var t uint32
	gles2.GenTextures(1, &t)
	gles2.BindTexture(gles2.TEXTURE_2D, t)
	gles2.TexParameteri(gles2.TEXTURE_2D, gles2.TEXTURE_MIN_FILTER, gles2.LINEAR)
	gles2.TexParameteri(gles2.TEXTURE_2D, gles2.TEXTURE_MAG_FILTER, gles2.LINEAR)
	gles2.TexParameteri(gles2.TEXTURE_2D, gles2.TEXTURE_WRAP_S, gles2.CLAMP_TO_EDGE)
	gles2.TexParameteri(gles2.TEXTURE_2D, gles2.TEXTURE_WRAP_T, gles2.CLAMP_TO_EDGE)
	pix := img.Pix
	w, h := img.Bounds().Dx(), img.Bounds().Dy()
	if white {
		pix = []byte{255, 255, 255, 255}
	} else if img.Stride != w*4 {
		pix = make([]byte, 0, w*h*4)
		for y := 0; y < h; y++ {
			pix = append(pix, img.Pix[y*img.Stride:y*img.Stride+w*4]...)
		}
	}
	gles2.TexImage2D(gles2.TEXTURE_2D, 0, gles2.RGBA, int32(w), int32(h), 0, gles2.RGBA, gles2.UNSIGNED_BYTE, gles2.Ptr(pix))
	return t
}

func (b *Backend) Clip(r *[4]float32) {
	if r == nil {
		gles2.Disable(gles2.SCISSOR_TEST)
		return
	}
	x0, y0 := int32(r[0]), int32(r[1])
	x1, y1 := int32(r[2]+0.999), int32(r[3]+0.999)
	if x1 < x0 {
		x1 = x0
	}
	if y1 < y0 {
		y1 = y0
	}
	gles2.Enable(gles2.SCISSOR_TEST)
	gles2.Scissor(x0, int32(b.h)-y1, x1-x0, y1-y0)
}

func (b *Backend) Triangles(tex xui.Texture, blend xui.Blend, v []xui.Vertex, idx []uint16) {
	if len(idx) == 0 {
		return
	}
	t := b.white
	if tex > 0 && int(tex) <= len(b.texs) {
		t = b.texs[tex-1]
	}
	gles2.BindTexture(gles2.TEXTURE_2D, t)
	switch blend {
	case xui.BlendAdd:
		gles2.BlendFuncSeparate(gles2.ONE, gles2.ONE, gles2.ZERO, gles2.ONE)
	case xui.BlendMultiply:
		gles2.BlendFuncSeparate(gles2.DST_COLOR, gles2.ONE_MINUS_SRC_ALPHA, gles2.ZERO, gles2.ONE)
	default: // premultiplied over
		gles2.BlendFunc(gles2.ONE, gles2.ONE_MINUS_SRC_ALPHA)
	}
	// xui.Vertex is 8 float32s laid out as the shader expects.
	fl := unsafe.Slice((*float32)(unsafe.Pointer(&v[0])), len(v)*8)
	gles2.BufferData(gles2.ARRAY_BUFFER, len(fl)*4, gles2.Ptr(fl), gles2.DYNAMIC_DRAW)
	gles2.BufferData(gles2.ELEMENT_ARRAY_BUFFER, len(idx)*2, gles2.Ptr(idx), gles2.DYNAMIC_DRAW)
	gles2.DrawElements(gles2.TRIANGLES, int32(len(idx)), gles2.UNSIGNED_SHORT, gles2.PtrOffset(0))
}

// Destroy frees GL objects.
func (b *Backend) Destroy() {
	for i := range b.texs {
		gles2.DeleteTextures(1, &b.texs[i])
	}
	gles2.DeleteTextures(1, &b.white)
	gles2.DeleteBuffers(1, &b.vbo)
	gles2.DeleteBuffers(1, &b.ibo)
	gles2.DeleteProgram(b.prog)
	b.texs = nil
}

func link(vsSrc, fsSrc string) (uint32, error) {
	compile := func(typ uint32, src string) (uint32, error) {
		sh := gles2.CreateShader(typ)
		gles2.ShaderSource(sh, src)
		gles2.CompileShader(sh)
		var ok int32
		gles2.GetShaderiv(sh, gles2.COMPILE_STATUS, &ok)
		if ok == 0 {
			buf := make([]byte, 2048)
			n := gles2.GetShaderInfoLog(sh, buf)
			gles2.DeleteShader(sh)
			return 0, fmt.Errorf("xuigl shader: %s", strings.TrimSpace(string(buf[:n])))
		}
		return sh, nil
	}
	vs, err := compile(gles2.VERTEX_SHADER, vsSrc)
	if err != nil {
		return 0, err
	}
	fs, err := compile(gles2.FRAGMENT_SHADER, fsSrc)
	if err != nil {
		gles2.DeleteShader(vs)
		return 0, err
	}
	prog := gles2.CreateProgram()
	gles2.AttachShader(prog, vs)
	gles2.AttachShader(prog, fs)
	gles2.LinkProgram(prog)
	gles2.DeleteShader(vs)
	gles2.DeleteShader(fs)
	var ok int32
	gles2.GetProgramiv(prog, gles2.LINK_STATUS, &ok)
	if ok == 0 {
		buf := make([]byte, 2048)
		n := gles2.GetProgramInfoLog(prog, buf)
		gles2.DeleteProgram(prog)
		return 0, fmt.Errorf("xuigl link: %s", strings.TrimSpace(string(buf[:n])))
	}
	return prog, nil
}
