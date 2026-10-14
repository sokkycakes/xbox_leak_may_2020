/*
 * glslcheck: compile a GLSL shader with the system's OpenGL and print the
 * driver's log.  Used to test the NV2A shader translators without running
 * a title.
 *
 *   glslcheck vert file.glsl        compile a vertex shader
 *   glslcheck frag file.glsl        compile a fragment shader
 *   glslcheck link vert.glsl frag.glsl   compile both and link them
 *
 * Needs a display (run under xvfb-run on a headless machine).
 */
#include <SDL.h>
#include <SDL_opengl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static GLuint (APIENTRY *p_glCreateShader)(GLenum);
static void (APIENTRY *p_glShaderSource)(GLuint, GLsizei, const char *const *, const GLint *);
static void (APIENTRY *p_glCompileShader)(GLuint);
static void (APIENTRY *p_glGetShaderiv)(GLuint, GLenum, GLint *);
static void (APIENTRY *p_glGetShaderInfoLog)(GLuint, GLsizei, GLsizei *, char *);
static GLuint (APIENTRY *p_glCreateProgram)(void);
static void (APIENTRY *p_glAttachShader)(GLuint, GLuint);
static void (APIENTRY *p_glLinkProgram)(GLuint);
static void (APIENTRY *p_glGetProgramiv)(GLuint, GLenum, GLint *);
static void (APIENTRY *p_glGetProgramInfoLog)(GLuint, GLsizei, GLsizei *, char *);
static void (APIENTRY *p_glBindAttribLocation)(GLuint, GLuint, const char *);

static char *slurp(const char *path)
{
    FILE *f = strcmp(path, "-") ? fopen(path, "rb") : stdin;
    if (!f) { perror(path); exit(1); }
    size_t cap = 65536, n = 0;
    char *buf = malloc(cap);
    size_t got;
    while ((got = fread(buf + n, 1, cap - n - 1, f)) > 0) {
        n += got;
        if (n + 1 >= cap) buf = realloc(buf, cap *= 2);
    }
    buf[n] = 0;
    if (f != stdin) fclose(f);
    return buf;
}

static GLuint compile(GLenum type, const char *path)
{
    char *src = slurp(path);
    GLuint sh = p_glCreateShader(type);
    const char *s = src;
    p_glShaderSource(sh, 1, &s, NULL);
    p_glCompileShader(sh);
    GLint ok = 0, len = 0;
    p_glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    p_glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &len);
    char *log = malloc(len + 1);
    p_glGetShaderInfoLog(sh, len + 1, NULL, log);
    printf("%s: %s\n%s", path, ok ? "OK" : "FAILED", log);
    free(log);
    free(src);
    if (!ok) exit(1);
    return sh;
}

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: glslcheck vert|frag file | link vert frag\n"); return 2; }
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 2; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_Window *w = SDL_CreateWindow("glslcheck", 0, 0, 64, 64, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!w || !SDL_GL_CreateContext(w)) { fprintf(stderr, "GL: %s\n", SDL_GetError()); return 2; }
#define LOAD(n) p_##n = SDL_GL_GetProcAddress(#n)
    LOAD(glCreateShader); LOAD(glShaderSource); LOAD(glCompileShader); LOAD(glGetShaderiv);
    LOAD(glGetShaderInfoLog); LOAD(glCreateProgram); LOAD(glAttachShader); LOAD(glLinkProgram);
    LOAD(glGetProgramiv); LOAD(glGetProgramInfoLog); LOAD(glBindAttribLocation);
    if (!p_glCreateShader) { fprintf(stderr, "no GLSL support\n"); return 2; }

    if (!strcmp(argv[1], "vert")) compile(GL_VERTEX_SHADER, argv[2]);
    else if (!strcmp(argv[1], "frag")) compile(GL_FRAGMENT_SHADER, argv[2]);
    else if (!strcmp(argv[1], "link") && argc >= 4) {
        GLuint vs = compile(GL_VERTEX_SHADER, argv[2]), fs = compile(GL_FRAGMENT_SHADER, argv[3]);
        GLuint prog = p_glCreateProgram();
        p_glAttachShader(prog, vs);
        p_glAttachShader(prog, fs);
        for (int i = 0; i < 16; i++) {
            char name[8];
            snprintf(name, sizeof name, "v%d", i);
            p_glBindAttribLocation(prog, i, name);
        }
        p_glLinkProgram(prog);
        GLint ok = 0, len = 0;
        p_glGetProgramiv(prog, GL_LINK_STATUS, &ok);
        p_glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
        char *log = malloc(len + 1);
        p_glGetProgramInfoLog(prog, len + 1, NULL, log);
        printf("link: %s\n%s", ok ? "OK" : "FAILED", log);
        if (!ok) return 1;
    } else {
        fprintf(stderr, "usage: glslcheck vert|frag file | link vert frag\n");
        return 2;
    }
    return 0;
}
