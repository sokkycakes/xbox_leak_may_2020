package gles2

/*
#include <GLES2/gl2.h>
*/
import "C"

// Additions for the XUI renderer (internal/xui/xuigl).

const (
	ONE                 = uint32(C.GL_ONE)
	SCISSOR_TEST        = uint32(C.GL_SCISSOR_TEST)
	ONE_MINUS_DST_ALPHA = uint32(C.GL_ONE_MINUS_DST_ALPHA)
	FUNC_ADD            = uint32(C.GL_FUNC_ADD)
)

func Scissor(x, y, w, h int32) { C.glScissor(C.GLint(x), C.GLint(y), C.GLsizei(w), C.GLsizei(h)) }

func BlendFuncSeparate(srcRGB, dstRGB, srcA, dstA uint32) {
	C.glBlendFuncSeparate(C.GLenum(srcRGB), C.GLenum(dstRGB), C.GLenum(srcA), C.GLenum(dstA))
}
