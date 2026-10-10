/*
 * Render the xbcompat-owned FF lighting shader against hand-calculated
 * color fixtures. No reference generator or GL fixed-function lighting is
 * used as the expected-value oracle.
 *
 * cc -O2 tests/ff_lighting_test.c -o build/ff_lighting_test \
 *    $(sdl2-config --cflags --libs) -lGL -lm
 * xvfb-run -a build/ff_lighting_test
 */
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/hle/ff_vsh.h"

static float state[FF_LIGHTING_VECTORS][4];
static GLint location;
static int failures, checks;

static GLuint compile(GLenum kind, const char *source)
{
    GLuint shader = glCreateShader(kind);
    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);
    GLint ok;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char error[4096];
        glGetShaderInfoLog(shader, sizeof error, NULL, error);
        fprintf(stderr, "%s\n", error);
        exit(2);
    }
    return shader;
}

static void rgb(int slot, float r, float g, float b)
{
    state[slot][0] = r; state[slot][1] = g; state[slot][2] = b;
}

static void reset(void)
{
    memset(state, 0, sizeof state);
    state[3][3] = 1;
    glColor4f(0.25f, 0.5f, 0.75f, 0.6f);
}

/* Position is in eye space. Projection recenters the sample without
   changing the position used for attenuation and the local-eye vector. */
static void check(const char *name, float x, float z, float nz,
                  float r, float g, float b, float a)
{
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(x - 1, x + 1, -1, 1, -10, 10);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glUniform4fv(location, FF_LIGHTING_VECTORS, &state[0][0]);
    glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_POINTS);
    glNormal3f(0, 0, nz);
    glVertex3f(x, 0, z);
    glEnd();
    unsigned char got[4];
    glReadPixels(16, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, got);
    const float want[4] = { r, g, b, a };
    int good = 1;
    for (int i = 0; i < 4; i++)
        if (fabsf(got[i] / 255.0f - want[i]) > 2.1f / 255.0f) good = 0;
    checks++;
    if (!good) {
        failures++;
        fprintf(stderr, "FAIL %s: got %u,%u,%u,%u expected %.4f,%.4f,%.4f,%.4f\n",
                name, got[0], got[1], got[2], got[3], r, g, b, a);
    }
}

int main(void)
{
    if (SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "%s\n", SDL_GetError()); return 2; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_Window *window = SDL_CreateWindow("FF lighting regression", 0, 0, 33, 33,
                                          SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    SDL_GLContext context = window ? SDL_GL_CreateContext(window) : NULL;
    if (!context) { fprintf(stderr, "%s\n", SDL_GetError()); SDL_Quit(); return 2; }
    GLuint vs = compile(GL_VERTEX_SHADER, ff_lighting_source);
    GLuint fs = compile(GL_FRAGMENT_SHADER,
        "#version 120\nvoid main(){gl_FragColor=vec4(gl_Color.rgb+gl_SecondaryColor.rgb,gl_Color.a);}\n");
    GLuint program = glCreateProgram();
    glAttachShader(program, vs); glAttachShader(program, fs); glLinkProgram(program);
    GLint ok;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char error[4096];
        glGetProgramInfoLog(program, sizeof error, NULL, error);
        fprintf(stderr, "%s\n", error);
        return 2;
    }
    glUseProgram(program);
    location = glGetUniformLocation(program, "ff_lighting");
    if (location < 0) return 2;
    glViewport(0, 0, 33, 33);
    glDisable(GL_DITHER);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glPointSize(1);
    glClearColor(0, 0, 0, 0);

    reset();
    rgb(3, 1, 1, 1);
    state[9][3] = 1;     /* point light at origin */
    rgb(11, 1, 1, 1);
    state[13][0] = 1;
    state[13][3] = 1;
    check("point outside range", 0, 2, -1, 0, 0, 0, 1);
    state[13][3] = 3;
    check("point inside range", 0, 2, -1, 1, 1, 1, 1);
    state[13][0] = 0; state[13][1] = 1;
    check("linear attenuation at distance two", 0, 2, -1, .5f, .5f, .5f, 1);
    state[13][1] = 0; state[13][2] = 1;
    check("quadratic attenuation at distance two", 0, 2, -1, .25f, .25f, .25f, 1);
    state[13][2] = 0; state[13][0] = 1;
    check("unnormalized normal", 0, 2, -.5f, .5f, .5f, .5f, 1);
    state[0][1] = 1;
    check("normalized normal", 0, 2, -.5f, 1, 1, 1, 1);

    reset();
    rgb(2, .5f, .5f, .5f);
    rgb(4, .5f, .25f, .125f);
    check("ambient material", 0, 2, -1, .25f, .125f, .0625f, 1);
    state[1][1] = 1;
    check("ambient primary source", 0, 2, -1, .125f, .25f, .375f, 1);
    reset();
    state[1][3] = 1;
    check("emissive primary source", 0, 2, -1, .25f, .5f, .75f, 1);
    reset();
    state[1][0] = 1;
    state[9][2] = -1; state[9][3] = 3;
    rgb(11, 1, 1, 1);
    check("diffuse primary source and alpha", 0, 2, -1, .25f, .5f, .75f, .6f);

    reset();
    state[0][0] = 1; state[0][3] = 256;
    rgb(5, 1, 1, 1);
    state[9][0] = .198751094f; state[9][2] = .98005f; state[9][3] = 3;
    rgb(12, 1, 1, 1);
    check("power above GL 128 limit", 0, 2, 1, .277146f, .277146f, .277146f, 1);
    state[1][2] = 1;
    check("specular primary source", 0, 2, 1, .0692865f, .138573f, .2078595f, 1);
    state[1][2] = 0;
    state[0][3] = 16;
    state[9][0] = 0; state[9][2] = -1;
    check("infinite viewer", 1, 2, -1, 0, 0, 0, 1);
    state[0][2] = 1;
    check("local viewer", 1, 2, -1, .6480125f, .6480125f, .6480125f, 1);
    check("back-facing light suppresses specular", 1, 2, 1, 0, 0, 0, 1);

    printf("%d/%d FF lighting checks passed\n", checks - failures, checks);
    glDeleteProgram(program); glDeleteShader(vs); glDeleteShader(fs);
    SDL_GL_DeleteContext(context); SDL_DestroyWindow(window); SDL_Quit();
    return failures ? 1 : 0;
}
