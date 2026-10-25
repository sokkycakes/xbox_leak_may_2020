/*
 * vsh_test: translate compiled Xbox vertex shaders (.xvu) to GLSL and
 * compile the result with the real GL driver.
 *
 *   vsh_test [-o outdir] [-g glslcheck] file.xvu...
 *
 * An .xvu file is exactly the blob a title passes to
 * D3DDevice_CreateVertexShader: a WORD shader type (0x2078 ordinary,
 * 0x7378 state shader, 0x7778 read/write shader), a WORD instruction count,
 * then count 16-byte instructions.  For each file the test copies the .xvu
 * (and the .vsh source next to it, when there is one) to outdir, writes the
 * GLSL beside them, and, with -g, runs "glslcheck vert" on it.  glslcheck
 * needs a display, so run the whole test under xvfb-run.
 *
 * Build (any word size):
 *   gcc -std=gnu11 -Wall -Isrc/hle -o vsh_test tests/vsh_test.c src/hle/vsh.c
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include "nv2a_shaders.h"

static unsigned char *slurp(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc(n > 0 ? n : 1);
    if (n > 0 && fread(buf, 1, n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
    fclose(f);
    *size = n > 0 ? n : 0;
    return buf;
}

static int spit(const char *path, const void *data, size_t size)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (size && fwrite(data, 1, size, f) != size) { fclose(f); return -1; }
    return fclose(f);
}

static int copy_file(const char *from, const char *to)
{
    size_t n;
    unsigned char *d = slurp(from, &n);
    if (!d) return -1;
    int r = spit(to, d, n);
    free(d);
    return r;
}

/* A file name for the output that keeps enough of the path to be unique:
   everything after "samples/", with separators turned into '_'. */
static void output_stem(const char *path, char *out, size_t n)
{
    const char *p = strstr(path, "samples/");
    p = p ? p + 8 : (strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
    size_t i = 0;
    for (; *p && i + 1 < n; p++) {
        char ch = *p;
        if (ch == '/' || ch == ' ' || ch == '\\') ch = '_';
        out[i++] = ch;
    }
    out[i] = 0;
    char *dot = strrchr(out, '.');
    if (dot && !strcasecmp(dot, ".xvu")) *dot = 0;
}

/* Shell-quote a path. */
static void quote(const char *s, char *out, size_t n)
{
    size_t i = 0;
    if (i + 1 < n) out[i++] = '\'';
    for (; *s && i + 5 < n; s++) {
        if (*s == '\'') { memcpy(out + i, "'\\''", 4); i += 4; }
        else out[i++] = *s;
    }
    if (i + 1 < n) out[i++] = '\'';
    out[i] = 0;
}

int main(int argc, char **argv)
{
    const char *outdir = "/tmp/claude-0/xbe/vsh";
    const char *glslcheck = NULL;
    int argi = 1;
    while (argi < argc && argv[argi][0] == '-') {
        if (!strcmp(argv[argi], "-o") && argi + 1 < argc) outdir = argv[argi + 1], argi += 2;
        else if (!strcmp(argv[argi], "-g") && argi + 1 < argc) glslcheck = argv[argi + 1], argi += 2;
        else { fprintf(stderr, "usage: vsh_test [-o outdir] [-g glslcheck] file.xvu...\n"); return 2; }
    }
    if (argi >= argc) { fprintf(stderr, "usage: vsh_test [-o outdir] [-g glslcheck] file.xvu...\n"); return 2; }
    if (mkdir(outdir, 0777) && errno != EEXIST) { perror(outdir); return 2; }

    int failed = 0, total = 0;
    for (; argi < argc; argi++) {
        const char *path = argv[argi];
        total++;
        size_t size;
        unsigned char *blob = slurp(path, &size);
        if (!blob) { printf("%s: cannot read\n", path); failed++; continue; }
        if (size < 4 + 16) { printf("%s: too short (%zu bytes)\n", path, size); failed++; free(blob); continue; }
        unsigned type = blob[0] | blob[1] << 8;
        unsigned count = blob[2] | blob[3] << 8;
        if ((type != 0x2078 && type != 0x7378 && type != 0x7778) || !count || 4 + 16u * count > size) {
            printf("%s: bad header type %#x count %u size %zu\n", path, type, count, size);
            failed++;
            free(blob);
            continue;
        }
        uint32_t *code = malloc(16 * count);
        memcpy(code, blob + 4, 16 * count);   /* the blob is not necessarily aligned */
        char *glsl = vsh_translate(code, count);
        free(code);

        char stem[256], base[512], buf[1024];
        output_stem(path, stem, sizeof stem);
        snprintf(base, sizeof base, "%s/%s", outdir, stem);
        snprintf(buf, sizeof buf, "%s.xvu", base);
        spit(buf, blob, size);
        free(blob);
        /* The assembly source, when it sits next to the .xvu. */
        snprintf(buf, sizeof buf, "%s", path);
        char *dot = strrchr(buf, '.');
        if (dot) {
            strcpy(dot, ".vsh");
            char to[600];
            snprintf(to, sizeof to, "%s.vsh", base);
            if (copy_file(buf, to)) { strcpy(dot, ".VSH"); copy_file(buf, to); }
        }

        if (!glsl) {
            printf("%s: type %#x, %u instructions: NOT TRANSLATED\n", path, type, count);
            failed++;
            continue;
        }
        snprintf(buf, sizeof buf, "%s.glsl", base);
        spit(buf, glsl, strlen(glsl));
        free(glsl);
        if (!glslcheck) {
            printf("%s: type %#x, %u instructions -> %s\n", path, type, count, buf);
            continue;
        }
        char q1[600], q2[1200], cmd[2048];
        quote(glslcheck, q1, sizeof q1);
        quote(buf, q2, sizeof q2);
        snprintf(cmd, sizeof cmd, "%s vert %s", q1, q2);
        fflush(stdout);
        int rc = system(cmd);
        if (rc != 0) {
            printf("%s: type %#x, %u instructions: GLSL FAILED (%s)\n", path, type, count, buf);
            failed++;
        }
    }
    printf("%d of %d shaders OK\n", total - failed, total);
    return failed ? 1 : 0;
}
