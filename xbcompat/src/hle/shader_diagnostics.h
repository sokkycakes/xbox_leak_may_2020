/*
 * XBCOMPAT_SHADER_DIAGNOSTICS=1: the driver's GL_KHR_debug messages, tagged
 * with the draw being issued, plus a hash of every shader source and every
 * link.  Included by d3d8.c only.  Synchronous debug output stalls the
 * driver, so this is for finding a bad shader, not for timing.
 */

static struct {
    int enabled;
    unsigned frame, draw;
    uint32_t fvf, ps;
    GLuint program;
    const char *path;
} shader_diagnostics;
/* GL_KHR_debug callback: the message, and the last draw shader_diagnostics_mark saw. */
static void APIENTRY shader_diagnostics_message(GLenum source, GLenum type, GLuint id,
                                                GLenum severity, GLsizei length,
                                                const GLchar *message, const void *data)
{
    (void)data;
    fprintf(stderr, "SHADERDIAG message frame=%u draw=%u program=%u path=%s source=%#x type=%#x severity=%#x id=%u: %.*s\n",
            shader_diagnostics.frame, shader_diagnostics.draw, shader_diagnostics.program,
            shader_diagnostics.path ? shader_diagnostics.path : "setup", source, type, severity, id,
            length > 0 ? length : 0, message);
}
static void shader_diagnostics_init(void)
{
    if (!getenv("XBCOMPAT_SHADER_DIAGNOSTICS")) return;
    typedef void (APIENTRY *callback_fn)(void (APIENTRY *)(GLenum, GLenum, GLuint, GLenum, GLsizei, const GLchar *, const void *), const void *);
    typedef void (APIENTRY *control_fn)(GLenum, GLenum, GLenum, GLsizei, const GLuint *, GLboolean);
    callback_fn callback = (callback_fn)SDL_GL_GetProcAddress("glDebugMessageCallback");
    control_fn control = (control_fn)SDL_GL_GetProcAddress("glDebugMessageControl");
    if (!callback || !control || !SDL_GL_ExtensionSupported("GL_KHR_debug")) {
        fputs("SHADERDIAG unavailable: GL_KHR_debug callback/control required\n", stderr); return;
    }
    callback(shader_diagnostics_message, NULL);
    control(GL_DONT_CARE, GL_DONT_CARE, GL_DONT_CARE, 0, NULL, GL_TRUE);
    glEnable(0x92E0 /* GL_DEBUG_OUTPUT */);
    glEnable(0x8242 /* GL_DEBUG_OUTPUT_SYNCHRONOUS */);
    shader_diagnostics.enabled = 1;
    fputs("SHADERDIAG enabled; capture changes CPU timing, keep performance runs uninstrumented\n", stderr);
}
static void shader_diagnostics_mark(unsigned frame, unsigned draw, uint32_t fvf, uint32_t ps, GLuint program, const char *path)
{
    if (!shader_diagnostics.enabled) return;
    shader_diagnostics.frame = frame; shader_diagnostics.draw = draw;
    shader_diagnostics.fvf = fvf; shader_diagnostics.ps = ps;
    shader_diagnostics.program = program; shader_diagnostics.path = path;
    fprintf(stderr, "SHADERDIAG draw frame=%u draw=%u fvf=%#x ps=%#x program=%u path=%s\n", frame, draw, fvf, ps, program, path);
}
static uint64_t shader_diagnostics_hash(const char *source)
{
    uint64_t hash = 14695981039346656037ull;   /* FNV-1a */
    for (; *source; source++) hash = (hash ^ (unsigned char)*source) * 1099511628211ull;
    return hash;
}
static void shader_diagnostics_source(GLenum kind, GLuint shader, const char *source)
{
    if (!shader_diagnostics.enabled) return;
    fprintf(stderr, "SHADERDIAG source shader=%u kind=%#x fnv=%016llx\n", shader, kind, (unsigned long long)shader_diagnostics_hash(source));
}
static void shader_diagnostics_link(GLuint program, GLuint vs, GLuint fs)
{
    if (!shader_diagnostics.enabled) return;
    fprintf(stderr, "SHADERDIAG link program=%u vs=%u fs=%u\n", program, vs, fs);
}
