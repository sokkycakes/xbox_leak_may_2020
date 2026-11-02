/*
 * padmap profiles on disk, chosen by title ID, and the built-in templates.
 * See padmap.h for the layout under <dir>.
 */
#include "padmap.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

bool padmap_dir(char *out, size_t size, const char *hdd_root)
{
    const char *env = getenv("XBCOMPAT_PADMAP_DIR");
    if (env && *env) return (size_t)snprintf(out, size, "%s", env) < size;
    if (!hdd_root || !*hdd_root) return false;
    return (size_t)snprintf(out, size, "%s/../input", hdd_root) < size;
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *b = NULL;
    size_t n = 0, cap = 0;
    for (;;) {
        if (n + 4096 + 1 > cap) {
            if (cap > (1u << 20)) break;            /* a profile is never this big */
            char *nb = (char *)realloc(b, cap = cap ? cap * 2 : 8192);
            if (!nb) break;
            b = nb;
        }
        size_t r = fread(b + n, 1, 4096, f);
        n += r;
        if (r < 4096) break;
    }
    fclose(f);
    if (b) b[n] = 0;
    return b;
}

bool padmap_load_file(padmap_profile *p, const char *path, char *err, size_t errsize)
{
    char *s = read_file(path);
    if (!s) {
        if (err && errsize) snprintf(err, errsize, "%s: %s", path, strerror(errno));
        padmap_profile_init(p, "Standard");
        return false;
    }
    bool ok = padmap_profile_parse(p, s, err, errsize);
    free(s);
    return ok;
}

/* A profile name as a file name: letters, digits, space, '-' and '_'. */
static void file_name(char *out, size_t size, const char *name)
{
    size_t n = 0;
    for (; *name && n + 1 < size; name++) {
        char c = *name;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_';
        out[n++] = ok ? c : '_';
    }
    if (!n && size > 1) out[n++] = '_';
    out[n] = 0;
}

static bool try_load(padmap_profile *p, const char *path, char *from, size_t fromsize)
{
    struct stat sb;
    if (stat(path, &sb) || !S_ISREG(sb.st_mode)) return false;
    char err[160];
    if (!padmap_load_file(p, path, err, sizeof err)) {
        /* a broken file shouldn't leave the pad half mapped */
        fprintf(stderr, "padmap: %s\n", err);
        padmap_profile_init(p, "Standard");
        return false;
    }
    if (from && fromsize) snprintf(from, fromsize, "%s", path);
    return true;
}

bool padmap_load_title(padmap_profile *p, const char *dir, uint32_t title_id, const char *pad_type,
                       char *from, size_t fromsize)
{
    char path[1024];
    if (from && fromsize) from[0] = 0;
    padmap_profile_init(p, "Standard");
    if (!dir) return false;
    snprintf(path, sizeof path, "%s/%08X/selected", dir, (unsigned)title_id);
    char *sel = read_file(path);
    if (sel) {
        char *e = sel + strlen(sel);
        while (e > sel && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
        char fn[PM_NAME + 8];
        file_name(fn, sizeof fn, sel);
        free(sel);
        snprintf(path, sizeof path, "%s/%08X/%s.json", dir, (unsigned)title_id, fn);
        if (try_load(p, path, from, fromsize)) return true;
    }
    if (pad_type && *pad_type) {
        snprintf(path, sizeof path, "%s/default-%s.json", dir, pad_type);
        if (try_load(p, path, from, fromsize)) return true;
    }
    snprintf(path, sizeof path, "%s/default.json", dir);
    return try_load(p, path, from, fromsize);
}

static bool make_dir(const char *path)
{
    return !mkdir(path, 0755) || errno == EEXIST;
}

static bool write_atomic(const char *path, const char *data, size_t len)
{
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len;
    ok = !fclose(f) && ok;
    if (ok) ok = !rename(tmp, path);
    if (!ok) remove(tmp);
    return ok;
}

bool padmap_save_title(const padmap_profile *p, const char *dir, uint32_t title_id)
{
    char path[1024], fn[PM_NAME + 8];
    if (!dir || !make_dir(dir)) return false;
    snprintf(path, sizeof path, "%s/%08X", dir, (unsigned)title_id);
    if (!make_dir(path)) return false;
    int n = padmap_profile_write(p, NULL, 0);
    if (n < 0) return false;
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) return false;
    padmap_profile_write(p, buf, (size_t)n + 1);
    file_name(fn, sizeof fn, p->name);
    snprintf(path, sizeof path, "%s/%08X/%s.json", dir, (unsigned)title_id, fn);
    bool ok = write_atomic(path, buf, (size_t)n);
    free(buf);
    if (!ok) return false;
    snprintf(path, sizeof path, "%s/%08X/selected", dir, (unsigned)title_id);
    char line[PM_NAME + 2];
    int len = snprintf(line, sizeof line, "%s\n", p->name);
    return write_atomic(path, line, (size_t)len);
}

bool padmap_reset_title(const char *dir, uint32_t title_id)
{
    char path[1024];
    if (!dir) return false;
    snprintf(path, sizeof path, "%s/%08X/selected", dir, (unsigned)title_id);
    return !remove(path) || errno == ENOENT;
}

/* ---- templates --------------------------------------------------------------- */

static const struct { const char *name; bool windows_only; const char *json; } templates[] = {
    { "Standard", false, "{ \"name\": \"Standard\" }" },
    { "Southpaw", false,
      "{ \"name\": \"Southpaw\", \"sources\": {"
      " \"left_stick\": { \"mode\": \"joystick\", \"output\": \"right_stick\" },"
      " \"right_stick\": { \"mode\": \"joystick\", \"output\": \"left_stick\" },"
      " \"buttons\": { \"mode\": \"buttons\", \"lthumb\": \"rthumb\", \"rthumb\": \"lthumb\" } } }" },
    { "Inverted Look", false,
      "{ \"name\": \"Inverted Look\", \"sources\": {"
      " \"right_stick\": { \"mode\": \"joystick\", \"invert_y\": true } } }" },
    { "Nintendo Layout", false,
      "{ \"name\": \"Nintendo Layout\", \"sources\": {"
      " \"buttons\": { \"mode\": \"buttons\", \"a\": \"b\", \"b\": \"a\", \"x\": \"y\", \"y\": \"x\" } } }" },
    { "Bumpers as Triggers", false,
      "{ \"name\": \"Bumpers as Triggers\", \"sources\": {"
      " \"buttons\": { \"mode\": \"buttons\", \"lb\": \"lt\", \"rb\": \"rt\" },"
      " \"left_trigger\": { \"mode\": \"button\", \"button\": \"lb\" },"
      " \"right_trigger\": { \"mode\": \"button\", \"button\": \"rb\" } } }" },
    { "Keyboard and Mouse", true,
      "{ \"name\": \"Keyboard and Mouse\", \"sources\": {"
      " \"left_stick\": { \"mode\": \"dpad\", \"threshold\": 0.4,"
      "   \"up\": \"key:w\", \"down\": \"key:s\", \"left\": \"key:a\", \"right\": \"key:d\" },"
      " \"right_stick\": { \"mode\": \"mouse\" },"
      " \"dpad\": { \"mode\": \"dpad\", \"up\": \"key:1\", \"down\": \"key:2\", \"left\": \"key:3\", \"right\": \"key:4\" },"
      " \"buttons\": { \"mode\": \"buttons\", \"a\": \"key:space\", \"b\": \"key:c\", \"x\": \"key:r\", \"y\": \"key:e\","
      "   \"lb\": \"key:q\", \"rb\": \"key:f\", \"back\": \"key:tab\", \"start\": \"key:esc\","
      "   \"lthumb\": \"key:lshift\", \"rthumb\": \"mouse:middle\" },"
      " \"left_trigger\": { \"mode\": \"button\", \"button\": \"mouse:right\" },"
      " \"right_trigger\": { \"mode\": \"button\", \"button\": \"mouse:left\" } } }" },
};
#define NTEMPLATES ((int)(sizeof templates / sizeof templates[0]))

static int template_index(int i, bool windows)
{
    for (int t = 0; t < NTEMPLATES; t++) {
        if (templates[t].windows_only && !windows) continue;
        if (i-- == 0) return t;
    }
    return -1;
}

int padmap_template_count(bool windows)
{
    int n = 0;
    for (int t = 0; t < NTEMPLATES; t++) n += !templates[t].windows_only || windows;
    return n;
}

const char *padmap_template_name(int i, bool windows)
{
    int t = template_index(i, windows);
    return t < 0 ? NULL : templates[t].name;
}

bool padmap_template(padmap_profile *p, int i, bool windows)
{
    int t = template_index(i, windows);
    if (t < 0) { padmap_profile_init(p, "Standard"); return false; }
    return padmap_profile_parse(p, templates[t].json, NULL, 0);
}
