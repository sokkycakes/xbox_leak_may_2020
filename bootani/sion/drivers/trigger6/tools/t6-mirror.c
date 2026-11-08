// t6-mirror: shows the Sion's main screen on every connected output of an
// MCT Trigger 6 USB display adapter (trigger6 driver, j5create JUA365).
//
// It never touches the main display: it only reads which framebuffer the GPU
// card (i915) is scanning out, imports it into OpenGL ES (any tiling or
// compression the GPU used), scales it into the adapter's mode and hands the
// result to the adapter. If OpenGL can't import a buffer and the buffer is
// linear (QEMU, dumb buffers), it scales it on the CPU instead.
//
//   t6-mirror [-m WxH] [-r fps] [-a W:H | -a fill] [-v]
//
//   -m  adapter mode (default 720x480p, the same picture the Sion sends to the
//       TV; the monitor's preferred mode if it doesn't offer that)
//   -r  frames per second sent to the adapter (default 60)
//   -a  shape of the main picture (default: 720x480 / 720x576 modes are filled,
//       as on the TV; other modes get a 4:3 picture with black bars);
//       "fill" stretches it to the whole monitor
//
// Exits when the adapter goes away; the init script starts it again.
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_mode.h>
#include <linux/dma-buf.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#ifndef EGL_PLATFORM_GBM_KHR
#define EGL_PLATFORM_GBM_KHR 0x31D7
#endif

static int verbose;
static volatile sig_atomic_t quit;

static void logf_(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	fputs("t6-mirror: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}
#define LOG(...) logf_(__VA_ARGS__)
#define DBG(...) do { if (verbose) logf_(__VA_ARGS__); } while (0)

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;
	do r = ioctl(fd, req, arg); while (r < 0 && (errno == EINTR || errno == EAGAIN));
	return r;
}

static int drm_driver_name(int fd, char *name, size_t len)
{
	struct drm_version v = {0};
	v.name = name;
	v.name_len = len - 1;
	memset(name, 0, len);
	return xioctl(fd, DRM_IOCTL_VERSION, &v);
}

/* Opens the first /dev/dri/<prefix>N whose driver is <drv>. */
static int open_by_driver(const char *prefix, const char *drv, char *path, size_t plen)
{
	for (int i = 0; i < 16; i++) {
		char p[64], name[64];
		snprintf(p, sizeof(p), "/dev/dri/%s%d", prefix, prefix[0] == 'r' ? 128 + i : i);
		int fd = open(p, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (!drm_driver_name(fd, name, sizeof(name)) && !strcmp(name, drv)) {
			if (path)
				snprintf(path, plen, "%s", p);
			return fd;
		}
		close(fd);
	}
	return -1;
}

/* ------------------------------------------------------------------------ */
/* OpenGL ES, loaded at run time so the binary only needs libc              */

static struct {
	void *gbm_dev;
	EGLDisplay dpy;
	EGLContext ctx;
	PFNEGLCREATEIMAGEKHRPROC CreateImage;
	PFNEGLDESTROYIMAGEKHRPROC DestroyImage;
	PFNGLEGLIMAGETARGETTEXTURE2DOESPROC ImageTargetTexture2D;
	EGLBoolean (*MakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
#define GLF(ret, name, args) ret (*name) args;
#define GLFUNCS \
	GLF(void, GenTextures, (GLsizei, GLuint *)) \
	GLF(void, DeleteTextures, (GLsizei, const GLuint *)) \
	GLF(void, BindTexture, (GLenum, GLuint)) \
	GLF(void, TexParameteri, (GLenum, GLenum, GLint)) \
	GLF(void, GenFramebuffers, (GLsizei, GLuint *)) \
	GLF(void, DeleteFramebuffers, (GLsizei, const GLuint *)) \
	GLF(void, BindFramebuffer, (GLenum, GLuint)) \
	GLF(void, GenRenderbuffers, (GLsizei, GLuint *)) \
	GLF(void, DeleteRenderbuffers, (GLsizei, const GLuint *)) \
	GLF(void, BindRenderbuffer, (GLenum, GLuint)) \
	GLF(void, RenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei)) \
	GLF(void, FramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint)) \
	GLF(GLenum, CheckFramebufferStatus, (GLenum)) \
	GLF(GLuint, CreateShader, (GLenum)) \
	GLF(void, ShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
	GLF(void, CompileShader, (GLuint)) \
	GLF(void, GetShaderiv, (GLuint, GLenum, GLint *)) \
	GLF(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
	GLF(GLuint, CreateProgram, (void)) \
	GLF(void, AttachShader, (GLuint, GLuint)) \
	GLF(void, BindAttribLocation, (GLuint, GLuint, const GLchar *)) \
	GLF(void, LinkProgram, (GLuint)) \
	GLF(void, GetProgramiv, (GLuint, GLenum, GLint *)) \
	GLF(void, UseProgram, (GLuint)) \
	GLF(GLint, GetUniformLocation, (GLuint, const GLchar *)) \
	GLF(void, Uniform1i, (GLint, GLint)) \
	GLF(void, VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
	GLF(void, EnableVertexAttribArray, (GLuint)) \
	GLF(void, Viewport, (GLint, GLint, GLsizei, GLsizei)) \
	GLF(void, ClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
	GLF(void, Clear, (GLbitfield)) \
	GLF(void, DrawArrays, (GLenum, GLint, GLsizei)) \
	GLF(void, ReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)) \
	GLF(void, PixelStorei, (GLenum, GLint)) \
	GLF(GLenum, GetError, (void))
	GLFUNCS
#undef GLF
	GLuint prog;
	int ok;
} gl;

static int gl_init(int render_fd)
{
	void *egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_GLOBAL);
	void *gles = dlopen("libGLESv2.so.2", RTLD_NOW | RTLD_GLOBAL);
	void *gbm = dlopen("libgbm.so.1", RTLD_NOW | RTLD_GLOBAL);
	if (!egl || !gles || !gbm) {
		LOG("no OpenGL ES libraries (EGL %s, GLESv2 %s, gbm %s)", egl ? "ok" : "missing", gles ? "ok" : "missing", gbm ? "ok" : "missing");
		return -1;
	}
	PFNEGLGETPROCADDRESSPROC GetProc = (PFNEGLGETPROCADDRESSPROC)dlsym(egl, "eglGetProcAddress");
	void *(*gbm_create_device)(int) = dlsym(gbm, "gbm_create_device");
	if (!GetProc || !gbm_create_device)
		return -1;
#define EGLF(name) __typeof__(&name) name##_ = (__typeof__(&name))dlsym(egl, #name)
	EGLF(eglInitialize); EGLF(eglBindAPI); EGLF(eglCreateContext); EGLF(eglQueryString);
	EGLF(eglChooseConfig); EGLF(eglMakeCurrent);
	PFNEGLGETPLATFORMDISPLAYEXTPROC GetPlatformDisplay =
		(PFNEGLGETPLATFORMDISPLAYEXTPROC)GetProc("eglGetPlatformDisplayEXT");
	if (!eglInitialize_ || !eglCreateContext_ || !GetPlatformDisplay || !eglMakeCurrent_)
		return -1;
	gl.MakeCurrent = eglMakeCurrent_;
#define GLF(ret, name, args) \
	if (!(gl.name = (ret (*) args)dlsym(gles, "gl" #name)) && \
	    !(gl.name = (ret (*) args)GetProc("gl" #name))) { LOG("missing gl" #name); return -1; }
	GLFUNCS
#undef GLF

	gl.gbm_dev = gbm_create_device(render_fd);
	if (!gl.gbm_dev)
		return -1;
	gl.dpy = GetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gl.gbm_dev, NULL);
	EGLint maj, min;
	if (gl.dpy == EGL_NO_DISPLAY || !eglInitialize_(gl.dpy, &maj, &min)) {
		LOG("eglInitialize failed");
		return -1;
	}
	const char *ext = eglQueryString_(gl.dpy, EGL_EXTENSIONS);
	DBG("EGL %d.%d %s", maj, min, eglQueryString_(gl.dpy, EGL_VENDOR));
	if (!ext || !strstr(ext, "EGL_EXT_image_dma_buf_import") ||
	    !strstr(ext, "EGL_KHR_surfaceless_context")) {
		LOG("EGL lacks dma-buf import or surfaceless contexts");
		return -1;
	}
	int mods = strstr(ext, "EGL_EXT_image_dma_buf_import_modifiers") != NULL;
	eglBindAPI_(EGL_OPENGL_ES_API);
	EGLConfig cfg = 0;
	if (!strstr(ext, "EGL_KHR_no_config_context")) {
		static const EGLint ca[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, 0, EGL_NONE };
		EGLint n = 0;
		if (!eglChooseConfig_(gl.dpy, ca, &cfg, 1, &n) || n < 1)
			return -1;
	}
	static const EGLint cta[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
	gl.ctx = eglCreateContext_(gl.dpy, cfg, EGL_NO_CONTEXT, cta);
	if (gl.ctx == EGL_NO_CONTEXT || !gl.MakeCurrent(gl.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, gl.ctx)) {
		LOG("no GLES2 context");
		return -1;
	}
	gl.CreateImage = (PFNEGLCREATEIMAGEKHRPROC)GetProc("eglCreateImageKHR");
	gl.DestroyImage = (PFNEGLDESTROYIMAGEKHRPROC)GetProc("eglDestroyImageKHR");
	gl.ImageTargetTexture2D = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)GetProc("glEGLImageTargetTexture2DOES");
	if (!gl.CreateImage || !gl.DestroyImage || !gl.ImageTargetTexture2D)
		return -1;
	if (!mods)
		DBG("no dma-buf modifier support: only linear buffers can be imported");

	/* The texture is sampled .bgra so that the RGBA read-back lands as
	 * B,G,R,A bytes = DRM XRGB8888. */
	static const char *vs =
		"attribute vec2 pos; attribute vec2 uv; varying vec2 v;\n"
		"void main() { v = uv; gl_Position = vec4(pos, 0.0, 1.0); }\n";
	static const char *fs =
		"precision mediump float; varying vec2 v; uniform sampler2D t;\n"
		"void main() { gl_FragColor = vec4(texture2D(t, v).bgr, 1.0); }\n";
	GLuint s[2];
	const char *src[2] = { vs, fs };
	GLenum type[2] = { GL_VERTEX_SHADER, GL_FRAGMENT_SHADER };
	gl.prog = gl.CreateProgram();
	for (int i = 0; i < 2; i++) {
		GLint ok = 0;
		s[i] = gl.CreateShader(type[i]);
		gl.ShaderSource(s[i], 1, &src[i], NULL);
		gl.CompileShader(s[i]);
		gl.GetShaderiv(s[i], GL_COMPILE_STATUS, &ok);
		if (!ok) {
			char log[512] = "";
			gl.GetShaderInfoLog(s[i], sizeof(log), NULL, log);
			LOG("shader: %s", log);
			return -1;
		}
		gl.AttachShader(gl.prog, s[i]);
	}
	gl.BindAttribLocation(gl.prog, 0, "pos");
	gl.BindAttribLocation(gl.prog, 1, "uv");
	gl.LinkProgram(gl.prog);
	GLint linked = 0;
	gl.GetProgramiv(gl.prog, GL_LINK_STATUS, &linked);
	if (!linked)
		return -1;
	gl.UseProgram(gl.prog);
	gl.Uniform1i(gl.GetUniformLocation(gl.prog, "t"), 0);
	gl.ok = 1;
	return 0;
}

/* ------------------------------------------------------------------------ */
/* Source: whatever the main GPU is scanning out                             */

struct src_buf {
	ino_t ino;		/* dma-buf identity */
	int fd;			/* keeps the dma-buf (and so its inode) alive */
	uint32_t w, h, fmt, pitch, offset;
	uint64_t mod;
	EGLImageKHR img;
	GLuint tex;
	void *map;		/* CPU fallback mapping (linear only) */
	size_t map_len;
	unsigned last_used;
	int failed;
};

#define NCACHE 8
static struct src_buf cache[NCACHE];
static unsigned frame_no;

static void src_free(struct src_buf *b)
{
	if (b->tex)
		gl.DeleteTextures(1, &b->tex);
	if (b->img)
		gl.DestroyImage(gl.dpy, b->img);
	if (b->map)
		munmap(b->map, b->map_len);
	if (b->fd >= 0)
		close(b->fd);
	memset(b, 0, sizeof(*b));
	b->fd = -1;
}

static int is_rgb32(uint32_t f)
{
	return f == DRM_FORMAT_XRGB8888 || f == DRM_FORMAT_ARGB8888 ||
	       f == DRM_FORMAT_XBGR8888 || f == DRM_FORMAT_ABGR8888;
}

static void src_import(struct src_buf *b, const struct drm_mode_fb_cmd2 *fb, int fds[4], int nplanes)
{
	if (gl.ok) {
		EGLint a[64], n = 0;
		static const EGLint keys[4][5] = {
			{ EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE0_PITCH_EXT,
			  EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT },
			{ EGL_DMA_BUF_PLANE1_FD_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
			  EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT },
			{ EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE2_PITCH_EXT,
			  EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT },
			{ EGL_DMA_BUF_PLANE3_FD_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT,
			  EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT },
		};
		a[n++] = EGL_WIDTH; a[n++] = fb->width;
		a[n++] = EGL_HEIGHT; a[n++] = fb->height;
		a[n++] = EGL_LINUX_DRM_FOURCC_EXT; a[n++] = fb->pixel_format;
		for (int p = 0; p < nplanes; p++) {
			a[n++] = keys[p][0]; a[n++] = fds[p];
			a[n++] = keys[p][1]; a[n++] = fb->offsets[p];
			a[n++] = keys[p][2]; a[n++] = fb->pitches[p];
			if (fb->flags & DRM_MODE_FB_MODIFIERS) {
				a[n++] = keys[p][3]; a[n++] = (EGLint)(fb->modifier[p] & 0xffffffff);
				a[n++] = keys[p][4]; a[n++] = (EGLint)(fb->modifier[p] >> 32);
			}
		}
		a[n++] = EGL_NONE;
		b->img = gl.CreateImage(gl.dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, a);
		if (b->img) {
			gl.GenTextures(1, &b->tex);
			gl.BindTexture(GL_TEXTURE_2D, b->tex);
			gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
			gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
			gl.ImageTargetTexture2D(GL_TEXTURE_2D, b->img);
			if (gl.GetError() == GL_NO_ERROR) {
				DBG("imported %ux%u %.4s mod %#llx on the GPU", fb->width, fb->height,
				    (char *)&fb->pixel_format, (unsigned long long)fb->modifier[0]);
				return;
			}
			gl.DeleteTextures(1, &b->tex);
			b->tex = 0;
			gl.DestroyImage(gl.dpy, b->img);
			b->img = NULL;
		}
		DBG("GPU import of %ux%u %.4s mod %#llx failed", fb->width, fb->height,
		    (char *)&fb->pixel_format, (unsigned long long)fb->modifier[0]);
	}
	/* CPU fallback: linear, and Intel X / Y tiling (no bit-6 swizzle on
	 * gen8+); compressed (CCS) buffers can't be read this way. */
	uint64_t mod = (fb->flags & DRM_MODE_FB_MODIFIERS) ? fb->modifier[0] : DRM_FORMAT_MOD_LINEAR;
	if (nplanes == 1 && is_rgb32(fb->pixel_format) &&
	    (mod == DRM_FORMAT_MOD_LINEAR || mod == I915_FORMAT_MOD_X_TILED || mod == I915_FORMAT_MOD_Y_TILED)) {
		off_t len = lseek(fds[0], 0, SEEK_END);
		b->map_len = len > 0 ? (size_t)len : (size_t)fb->offsets[0] + (size_t)fb->pitches[0] * fb->height;
		b->map = mmap(NULL, b->map_len, PROT_READ, MAP_SHARED, fds[0], 0);
		if (b->map == MAP_FAILED)
			b->map = NULL;
		else {
			b->mod = mod;
			DBG("imported %ux%u %.4s mod %#llx on the CPU", fb->width, fb->height,
			    (char *)&fb->pixel_format, (unsigned long long)mod);
			return;
		}
	}
	LOG("can't read a %ux%u %.4s buffer (modifier %#llx)", fb->width, fb->height,
	    (char *)&fb->pixel_format, (unsigned long long)fb->modifier[0]);
	b->failed = 1;
}

/* Returns the cached buffer behind framebuffer <fb_id> on the main card. */
static struct src_buf *src_get(int fd, uint32_t fb_id)
{
	struct drm_mode_fb_cmd2 fb = { .fb_id = fb_id };
	int fds[4] = { -1, -1, -1, -1 }, nplanes = 0;
	struct src_buf *b = NULL;
	struct stat st;

	if (xioctl(fd, DRM_IOCTL_MODE_GETFB2, &fb) || !fb.handles[0]) {
		DBG("GETFB2 %u: %s", fb_id, strerror(errno));
		return NULL;
	}
	for (int p = 0; p < 4 && fb.handles[p]; p++) {
		struct drm_prime_handle ph = { .handle = fb.handles[p], .flags = DRM_CLOEXEC };
		for (int q = 0; q < p; q++)
			if (fb.handles[q] == fb.handles[p] && fds[q] >= 0)
				ph.fd = fds[q], ph.handle = 0;
		if (ph.handle && xioctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &ph)) {
			DBG("PRIME export: %s", strerror(errno));
			break;
		}
		fds[p] = ph.handle ? ph.fd : dup(ph.fd);
		nplanes = p + 1;
	}
	for (int p = 0; p < 4 && fb.handles[p]; p++) {
		int dup_h = 0;
		for (int q = 0; q < p; q++)
			dup_h |= fb.handles[q] == fb.handles[p];
		if (!dup_h) {
			struct drm_gem_close gc = { .handle = fb.handles[p] };
			ioctl(fd, DRM_IOCTL_GEM_CLOSE, &gc);
		}
	}
	if (!nplanes || fstat(fds[0], &st))
		goto out;

	for (int i = 0; i < NCACHE; i++)
		if (cache[i].fd >= 0 && cache[i].ino == st.st_ino && cache[i].w == fb.width &&
		    cache[i].h == fb.height && cache[i].fmt == fb.pixel_format &&
		    cache[i].pitch == fb.pitches[0] && cache[i].offset == fb.offsets[0])
			b = &cache[i];
	if (!b) {
		struct src_buf *victim = &cache[0];
		for (int i = 0; i < NCACHE; i++) {
			if (cache[i].fd < 0) { victim = &cache[i]; break; }
			if (cache[i].last_used < victim->last_used)
				victim = &cache[i];
		}
		src_free(victim);
		b = victim;
		b->ino = st.st_ino;
		b->w = fb.width; b->h = fb.height; b->fmt = fb.pixel_format;
		b->pitch = fb.pitches[0]; b->offset = fb.offsets[0];
		b->mod = (fb.flags & DRM_MODE_FB_MODIFIERS) ? fb.modifier[0] : DRM_FORMAT_MOD_INVALID;
		src_import(b, &fb, fds, nplanes);
		b->fd = fds[0];
		fds[0] = -1;
	}
	b->last_used = frame_no;
out:
	for (int p = 0; p < 4; p++)
		if (fds[p] >= 0)
			close(fds[p]);
	return b && !b->failed ? b : NULL;
}

/* ------------------------------------------------------------------------ */
/* Adapter outputs                                                           */

struct output {
	uint32_t conn_id, crtc_id, plane_id;
	int connected, active;
	struct drm_mode_modeinfo mode;
	uint32_t fb_id, handle, pitch;
	uint8_t *map;
	size_t size;
	GLuint fbo, rb;
};

#define MAXOUT 4
static struct output outs[MAXOUT];
static int nouts;
static int want_w = 720, want_h = 480, want_default = 1;
static double aspect = -1;	/* 0 = fill, -1 = 4:3 except SD modes (already 4:3 TV frames) */

static void out_release(int fd, struct output *o)
{
	if (o->fbo) { gl.DeleteFramebuffers(1, &o->fbo); o->fbo = 0; }
	if (o->rb) { gl.DeleteRenderbuffers(1, &o->rb); o->rb = 0; }
	if (o->map) { munmap(o->map, o->size); o->map = NULL; }
	if (o->fb_id) { ioctl(fd, DRM_IOCTL_MODE_RMFB, &o->fb_id); o->fb_id = 0; }
	if (o->handle) {
		struct drm_mode_destroy_dumb dd = { .handle = o->handle };
		ioctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
		o->handle = 0;
	}
	o->active = 0;
}

static int get_conn(int fd, uint32_t id, int probe, struct drm_mode_get_connector *c,
		    struct drm_mode_modeinfo *modes, int maxmodes, uint32_t *encs, int maxenc)
{
	struct drm_mode_modeinfo dummy;

	/* count_modes != 0 returns the cached state without probing the
	 * monitor again; count_modes == 0 probes (EDID read). */
	memset(c, 0, sizeof(*c));
	c->connector_id = id;
	c->count_modes = probe ? 0 : 1;
	c->modes_ptr = (uintptr_t)&dummy;
	if (xioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, c))
		return -1;
	if (!modes)
		return 0;
	uint32_t n = c->count_modes, ne = c->count_encoders;
	memset(c, 0, sizeof(*c));
	c->connector_id = id;
	c->count_modes = n < (uint32_t)maxmodes ? n : (uint32_t)maxmodes;
	c->modes_ptr = (uintptr_t)(c->count_modes ? modes : &dummy);
	if (!c->count_modes)
		c->count_modes = 1;	/* still no probe */
	c->count_encoders = ne < (uint32_t)maxenc ? ne : (uint32_t)maxenc;
	c->encoders_ptr = (uintptr_t)encs;
	if (xioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, c))
		return -1;
	if (!n)
		c->count_modes = 0;
	return 0;
}

static int out_setup(int fd, struct output *o)
{
	struct drm_mode_modeinfo modes[128];
	uint32_t encs[4];
	struct drm_mode_get_connector c;
	int pick = -1;

	if (get_conn(fd, o->conn_id, 1, &c, NULL, 0, NULL, 0) ||
	    get_conn(fd, o->conn_id, 0, &c, modes, 128, encs, 4))
		return -1;
	if (c.connection != 1 || !c.count_modes)
		return -1;
	for (uint32_t i = 0; i < c.count_modes && pick < 0; i++)
		if (modes[i].hdisplay == want_w && modes[i].vdisplay == want_h &&
		    !(modes[i].flags & DRM_MODE_FLAG_INTERLACE) &&
		    (modes[i].vrefresh == 60 || modes[i].vrefresh == 59))
			pick = i;
	for (uint32_t i = 0; i < c.count_modes && pick < 0; i++)
		if (modes[i].hdisplay == want_w && modes[i].vdisplay == want_h &&
		    !(modes[i].flags & DRM_MODE_FLAG_INTERLACE))
			pick = i;
	if (pick < 0 && !want_default) {
		LOG("the monitor doesn't offer %dx%d; using its preferred mode", want_w, want_h);
	}
	for (uint32_t i = 0; i < c.count_modes && pick < 0; i++)
		if (modes[i].type & DRM_MODE_TYPE_PREFERRED)
			pick = i;
	if (pick < 0)
		pick = 0;
	o->mode = modes[pick];

	/* CRTC: the one the encoder can drive (one per port on trigger6). */
	struct drm_mode_card_res res = {0};
	uint32_t crtcs[8];
	if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res))
		return -1;
	res.count_fbs = res.count_connectors = res.count_encoders = 0;
	res.count_crtcs = res.count_crtcs > 8 ? 8 : res.count_crtcs;
	res.crtc_id_ptr = (uintptr_t)crtcs;
	if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) || !c.count_encoders)
		return -1;
	struct drm_mode_get_encoder e = { .encoder_id = encs[0] };
	if (xioctl(fd, DRM_IOCTL_MODE_GETENCODER, &e))
		return -1;
	int ci = __builtin_ffs(e.possible_crtcs) - 1;
	if (ci < 0 || ci >= (int)res.count_crtcs)
		return -1;
	o->crtc_id = crtcs[ci];

	struct drm_mode_get_plane_res pr = {0};
	uint32_t planes[16];
	o->plane_id = 0;
	if (!xioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr)) {
		pr.count_planes = pr.count_planes > 16 ? 16 : pr.count_planes;
		pr.plane_id_ptr = (uintptr_t)planes;
		xioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr);
		for (uint32_t i = 0; i < pr.count_planes && !o->plane_id; i++) {
			struct drm_mode_get_plane p = { .plane_id = planes[i] };
			if (!xioctl(fd, DRM_IOCTL_MODE_GETPLANE, &p) && (p.possible_crtcs & (1u << ci)))
				o->plane_id = planes[i];
		}
	}

	struct drm_mode_create_dumb cd = { .width = o->mode.hdisplay, .height = o->mode.vdisplay, .bpp = 32 };
	if (xioctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &cd))
		return -1;
	o->handle = cd.handle;
	o->pitch = cd.pitch;
	o->size = cd.size;
	struct drm_mode_map_dumb md = { .handle = cd.handle };
	if (xioctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &md))
		goto fail;
	o->map = mmap(NULL, o->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, md.offset);
	if (o->map == MAP_FAILED) {
		o->map = NULL;
		goto fail;
	}
	memset(o->map, 0, o->size);
	struct drm_mode_fb_cmd2 f = { .width = cd.width, .height = cd.height,
				      .pixel_format = DRM_FORMAT_XRGB8888 };
	f.handles[0] = cd.handle;
	f.pitches[0] = cd.pitch;
	if (xioctl(fd, DRM_IOCTL_MODE_ADDFB2, &f))
		goto fail;
	o->fb_id = f.fb_id;

	struct drm_mode_crtc sc = { .crtc_id = o->crtc_id, .fb_id = o->fb_id, .mode_valid = 1,
				    .set_connectors_ptr = (uintptr_t)&o->conn_id, .count_connectors = 1 };
	sc.mode = o->mode;
	if (xioctl(fd, DRM_IOCTL_MODE_SETCRTC, &sc)) {
		LOG("can't set %s on connector %u: %s", o->mode.name, o->conn_id, strerror(errno));
		goto fail;
	}
	if (gl.ok) {
		gl.GenRenderbuffers(1, &o->rb);
		gl.BindRenderbuffer(GL_RENDERBUFFER, o->rb);
		gl.RenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8_OES, cd.width, cd.height);
		gl.GenFramebuffers(1, &o->fbo);
		gl.BindFramebuffer(GL_FRAMEBUFFER, o->fbo);
		gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, o->rb);
		if (gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
			LOG("GL framebuffer incomplete");
			goto fail;
		}
	}
	o->active = 1;
	LOG("connector %u: showing the main screen at %s %u Hz", o->conn_id, o->mode.name, o->mode.vrefresh);
	return 0;
fail:
	out_release(fd, o);
	return -1;
}

static int scan_outputs(int fd)
{
	struct drm_mode_card_res res = {0};
	uint32_t conns[MAXOUT];

	if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res))
		return -1;
	res.count_fbs = res.count_crtcs = res.count_encoders = 0;
	res.count_connectors = res.count_connectors > MAXOUT ? MAXOUT : res.count_connectors;
	res.connector_id_ptr = (uintptr_t)conns;
	if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res))
		return -1;
	nouts = res.count_connectors;
	for (int i = 0; i < nouts; i++)
		outs[i].conn_id = conns[i];
	return 0;
}

/* Re-reads the cached connection state (no EDID traffic) and sets up or
 * drops outputs. Returns -1 when the adapter is gone. */
static int check_outputs(int fd)
{
	for (int i = 0; i < nouts; i++) {
		struct output *o = &outs[i];
		struct drm_mode_get_connector c;
		if (get_conn(fd, o->conn_id, 0, &c, NULL, 0, NULL, 0))
			return errno == ENODEV ? -1 : 0;
		int conn = c.connection == 1;
		if (conn && !o->active && (!o->connected || frame_no % 600 == 0))
			out_setup(fd, o);
		else if (!conn && o->active) {
			LOG("connector %u: monitor unplugged", o->conn_id);
			out_release(fd, o);
		}
		o->connected = conn;
	}
	return 0;
}

/* ------------------------------------------------------------------------ */
/* Drawing                                                                   */

struct rect { int x, y, w, h; };

static struct rect fit(int ow, int oh)
{
	struct rect r = { 0, 0, ow, oh };
	double a = aspect;
	if (a < 0)
		a = (ow == 720 || ow == 704) && (oh == 480 || oh == 576) ? 0 : 4.0 / 3.0;
	if (a > 0) {
		if ((double)ow / oh > a) {
			r.w = (int)(oh * a + 0.5) & ~1;
			r.x = (ow - r.w) / 2;
		} else {
			r.h = (int)(ow / a + 0.5) & ~1;
			r.y = (oh - r.h) / 2;
		}
	}
	return r;
}

/* src rect (sx,sy,sw,sh) inside buffer b -> output o */
static void draw_gl(struct output *o, struct src_buf *b, int sx, int sy, int sw, int sh)
{
	int ow = o->mode.hdisplay, oh = o->mode.vdisplay;
	struct rect r = fit(ow, oh);
	float u0 = (float)sx / b->w, u1 = (float)(sx + sw) / b->w;
	float v0 = (float)sy / b->h, v1 = (float)(sy + sh) / b->h;
	/* Drawn upside down so glReadPixels (bottom row first) returns the
	 * picture top row first. */
	const GLfloat v[] = {
		-1, -1, u0, v0,
		 1, -1, u1, v0,
		-1,  1, u0, v1,
		 1,  1, u1, v1,
	};
	gl.BindFramebuffer(GL_FRAMEBUFFER, o->fbo);
	gl.Viewport(0, 0, ow, oh);
	gl.ClearColor(0, 0, 0, 1);
	gl.Clear(GL_COLOR_BUFFER_BIT);
	gl.Viewport(r.x, oh - r.y - r.h, r.w, r.h);
	gl.BindTexture(GL_TEXTURE_2D, b->tex);
	gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, v);
	gl.VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, v + 2);
	gl.EnableVertexAttribArray(0);
	gl.EnableVertexAttribArray(1);
	gl.DrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	gl.PixelStorei(GL_PACK_ALIGNMENT, 4);
	if (o->pitch == (uint32_t)ow * 4)
		gl.ReadPixels(0, 0, ow, oh, GL_RGBA, GL_UNSIGNED_BYTE, o->map);
	else
		for (int y = 0; y < oh; y++)
			gl.ReadPixels(0, y, ow, 1, GL_RGBA, GL_UNSIGNED_BYTE, o->map + (size_t)y * o->pitch);
}

static void draw_cpu(int fd_unused, struct output *o, struct src_buf *b, int sx, int sy, int sw, int sh)
{
	(void)fd_unused;
	int ow = o->mode.hdisplay, oh = o->mode.vdisplay;
	struct rect r = fit(ow, oh);
	int swap_rb = b->fmt == DRM_FORMAT_XBGR8888 || b->fmt == DRM_FORMAT_ABGR8888;
	int xs[4096];
	struct dma_buf_sync sync = { DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };

	if (r.w > 4096)
		return;
	ioctl(b->fd, DMA_BUF_IOCTL_SYNC, &sync);
	for (int x = 0; x < r.w; x++)
		xs[x] = sx + (int)(((int64_t)x * sw + sw / 2) / r.w);
	for (int y = 0; y < oh; y++) {
		uint32_t *d = (uint32_t *)(o->map + (size_t)y * o->pitch);
		if (y < r.y || y >= r.y + r.h) {
			memset(d, 0, ow * 4);
			continue;
		}
		int syy = sy + (int)(((int64_t)(y - r.y) * sh + sh / 2) / r.h);
		const uint8_t *base = (const uint8_t *)b->map + b->offset;
		const uint32_t *s = (const uint32_t *)(base + (size_t)syy * b->pitch);
		size_t tiles_per_row, row_off = 0;
		if (b->mod == I915_FORMAT_MOD_X_TILED) {	/* 512 B x 8 rows */
			tiles_per_row = b->pitch / 512;
			row_off = (size_t)(syy / 8) * tiles_per_row * 4096 + (syy % 8) * 512;
		} else if (b->mod == I915_FORMAT_MOD_Y_TILED) {	/* 128 B x 32 rows of 16 B columns */
			tiles_per_row = b->pitch / 128;
			row_off = (size_t)(syy / 32) * tiles_per_row * 4096 + (syy % 32) * 16;
		}
		memset(d, 0, r.x * 4);
		for (int x = 0; x < r.w; x++) {
			uint32_t p, bx = xs[x] * 4;
			if (b->mod == I915_FORMAT_MOD_X_TILED)
				p = *(const uint32_t *)(base + row_off + (bx / 512) * 4096 + bx % 512);
			else if (b->mod == I915_FORMAT_MOD_Y_TILED)
				p = *(const uint32_t *)(base + row_off + (bx / 128) * 4096 + ((bx % 128) / 16) * 512 + bx % 16);
			else
				p = s[xs[x]];
			if (swap_rb)
				p = (p & 0xff00ff00) | ((p >> 16) & 0xff) | ((p & 0xff) << 16);
			d[r.x + x] = p | 0xff000000;
		}
		memset(d + r.x + r.w, 0, (ow - r.x - r.w) * 4);
	}
	sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
	ioctl(b->fd, DMA_BUF_IOCTL_SYNC, &sync);
}

/* Hands the finished picture to the adapter. SETPLANE is a blocking commit,
 * so the driver converts the picture in this (low-priority) thread. */
static int present(int fd, struct output *o)
{
	if (o->plane_id) {
		struct drm_mode_set_plane sp = {
			.plane_id = o->plane_id, .crtc_id = o->crtc_id, .fb_id = o->fb_id,
			.crtc_w = o->mode.hdisplay, .crtc_h = o->mode.vdisplay,
			.src_w = (uint32_t)o->mode.hdisplay << 16, .src_h = (uint32_t)o->mode.vdisplay << 16,
		};
		if (!xioctl(fd, DRM_IOCTL_MODE_SETPLANE, &sp))
			return 0;
	}
	struct drm_mode_fb_dirty_cmd dc = { .fb_id = o->fb_id };
	return xioctl(fd, DRM_IOCTL_MODE_DIRTYFB, &dc);
}

/* Finds the main card's lit CRTC (its scanout framebuffer and viewport). */
static int main_crtc(int fd, uint32_t *crtc_id, struct drm_mode_crtc *out)
{
	if (*crtc_id) {
		out->crtc_id = *crtc_id;
		if (!xioctl(fd, DRM_IOCTL_MODE_GETCRTC, out) && out->mode_valid && out->fb_id)
			return 0;
		*crtc_id = 0;
	}
	struct drm_mode_card_res res = {0};
	uint32_t crtcs[8];
	if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res))
		return -1;
	res.count_fbs = res.count_connectors = res.count_encoders = 0;
	res.count_crtcs = res.count_crtcs > 8 ? 8 : res.count_crtcs;
	res.crtc_id_ptr = (uintptr_t)crtcs;
	if (xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res))
		return -1;
	for (uint32_t i = 0; i < res.count_crtcs; i++) {
		memset(out, 0, sizeof(*out));
		out->crtc_id = crtcs[i];
		if (!xioctl(fd, DRM_IOCTL_MODE_GETCRTC, out) && out->mode_valid && out->fb_id) {
			*crtc_id = crtcs[i];
			DBG("main screen: crtc %u %s", crtcs[i], out->mode.name);
			return 0;
		}
	}
	return -1;
}

static void on_signal(int s) { (void)s; quit = 1; }

int main(int argc, char **argv)
{
	int fps = 60, opt;

	while ((opt = getopt(argc, argv, "m:r:a:v")) != -1) {
		switch (opt) {
		case 'm':
			if (sscanf(optarg, "%dx%d", &want_w, &want_h) != 2)
				goto usage;
			want_default = 0;
			break;
		case 'r':
			fps = atoi(optarg);
			if (fps < 1 || fps > 120)
				goto usage;
			break;
		case 'a': {
			int a, b;
			if (!strcmp(optarg, "fill"))
				aspect = 0;
			else if (sscanf(optarg, "%d:%d", &a, &b) == 2 && a > 0 && b > 0)
				aspect = (double)a / b;
			else
				goto usage;
			break;
		}
		case 'v':
			verbose = 1;
			break;
		default:
usage:
			fprintf(stderr, "usage: t6-mirror [-m WxH] [-r fps] [-a W:H|fill] [-v]\n");
			return 2;
		}
	}
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	signal(SIGPIPE, SIG_IGN);
	setpriority(PRIO_PROCESS, 0, 10);	/* games come first */

	char t6path[64], mainpath[64];
	int t6 = open_by_driver("card", "trigger6", t6path, sizeof(t6path));
	if (t6 < 0) {
		LOG("no trigger6 adapter");
		return 1;
	}
	struct drm_set_client_cap cap = { DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1 };
	ioctl(t6, DRM_IOCTL_SET_CLIENT_CAP, &cap);
	ioctl(t6, DRM_IOCTL_SET_MASTER, 0);

	/* The main card: anything that isn't the adapter (i915 on the Sion). */
	int mainfd = -1;
	for (int i = 0; i < 16 && mainfd < 0; i++) {
		char p[64], name[64];
		snprintf(p, sizeof(p), "/dev/dri/card%d", i);
		if (!strcmp(p, t6path))
			continue;
		int fd = open(p, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			continue;
		/* Opening a card nobody holds makes us its master; give that
		 * back at once so the dashboard can take the screen. */
		ioctl(fd, DRM_IOCTL_DROP_MASTER, 0);
		if (!drm_driver_name(fd, name, sizeof(name)) && strcmp(name, "trigger6") &&
		    strcmp(name, "udl") && strcmp(name, "gm12u320")) {
			mainfd = fd;
			snprintf(mainpath, sizeof(mainpath), "%s", p);
			LOG("mirroring %s (%s) to %s", p, name, t6path);
		} else
			close(fd);
	}
	if (mainfd < 0) {
		LOG("no main display card");
		return 1;
	}

	/* GL on the main GPU's render node. */
	char mname[64] = "";
	drm_driver_name(mainfd, mname, sizeof(mname));
	int rfd = open_by_driver("renderD", mname, NULL, 0);
	if (rfd < 0 || gl_init(rfd) < 0)
		LOG("OpenGL not available: copying on the CPU (linear, X and Y tiled buffers)");
	for (int i = 0; i < NCACHE; i++)
		cache[i].fd = -1;

	if (scan_outputs(t6) < 0)
		return 1;

	uint32_t main_id = 0;
	struct timespec next;
	clock_gettime(CLOCK_MONOTONIC, &next);
	long period = 1000000000L / fps;
	unsigned warned = 0;

	while (!quit) {
		if (frame_no % (fps > 2 ? fps / 2 : 1) == 0 && check_outputs(t6) < 0) {
			LOG("adapter gone");
			break;
		}
		int any = 0;
		for (int i = 0; i < nouts; i++)
			any |= outs[i].active;

		struct drm_mode_crtc mc = {0};
		struct src_buf *b = NULL;
		if (any && main_crtc(mainfd, &main_id, &mc)) DBG("main crtc: none"); else if (any)
			b = src_get(mainfd, mc.fb_id);
		if (b) {
			int sw = mc.mode.hdisplay, sh = mc.mode.vdisplay;
			if ((int)mc.x + sw > (int)b->w) sw = b->w - mc.x;
			if ((int)mc.y + sh > (int)b->h) sh = b->h - mc.y;
			for (int i = 0; i < nouts; i++) {
				struct output *o = &outs[i];
				if (!o->active)
					continue;
				if (b->tex)
					draw_gl(o, b, mc.x, mc.y, sw, sh);
				else
					draw_cpu(mainfd, o, b, mc.x, mc.y, sw, sh);
				if (present(t6, o)) {
					DBG("connector %u: present: %s", o->conn_id, strerror(errno));
					if (errno == ENODEV)
						quit = 2;
				}
			}
		} else if (any && !warned++)
			DBG("nothing on the main screen yet");

		/* Drop buffers the main screen stopped using, so their memory
		 * isn't kept alive by us. */
		for (int i = 0; i < NCACHE; i++)
			if (cache[i].fd >= 0 && frame_no - cache[i].last_used > (unsigned)fps * 2)
				src_free(&cache[i]);

		frame_no++;
		next.tv_nsec += period;
		while (next.tv_nsec >= 1000000000L) {
			next.tv_nsec -= 1000000000L;
			next.tv_sec++;
		}
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		if (now.tv_sec > next.tv_sec + 1)
			next = now;	/* fell far behind: don't try to catch up */
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
	}
	for (int i = 0; i < NCACHE; i++)
		if (cache[i].fd >= 0)
			src_free(&cache[i]);
	return quit == 2;
}
