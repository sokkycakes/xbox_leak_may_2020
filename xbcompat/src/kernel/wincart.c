/* Windows carts on the game card.

   A Kazeta cart whose Runtime is windows (windows-1.1, windows-proton-ge-9,
   ...) holds a Windows game: on the Sion it runs under Wine, outside
   xbcompat (tools/wincart-run).  The dashboards only know how to launch
   Xbox titles, so the card the dashboard sees (\Device\GameCard0, io.c) is
   a shim of the real one: every entry is a symlink to the card's, except
   that each Windows cart's .kzi says Runtime=xbox and Exec=<cart>/default.xbe,
   and <cart>/ holds a stub XBE whose certificate carries the cart's Name
   and a title ID made from its Id, plus a .wincart file naming the real
   .kzi.  The dashboard reads the stub's certificate (name, title ID for the
   saves under UDATA) and launches D:\default.xbe like any other cart; the
   xbcompat that is started for it finds .wincart next to the XBE and execs
   wincart-run instead of loading a title (wincart_exec, from main).

   The shim is rebuilt whenever the card's set of .kzi files changes
   (names and mtimes), under /tmp/xbcompat-card-<uid>/<card>. */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../xbcompat.h"

/* PNG only, for a cart's icon (stb_image, public domain / MIT). */
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#include "../third_party/stb_image.h"
#pragma GCC diagnostic pop

typedef struct {
    char kzi[NAME_MAX + 1];     /* the .kzi's file name */
    char dir[NAME_MAX + 1];     /* the cart's directory on the card */
    char name[128], id[NAME_MAX + 1], exec[256], runtime[64], icon[256];
    time_t mtime;
} wincart;

static char shim_dir[PATH_MAX];
static char shim_sig[4096];

/* key=value lines; a UTF-8 BOM and CRs are ignored. */
static void kzi_read(const char *path, wincart *c)
{
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[1024];
    bool first = true;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        if (first && !strncmp(p, "\xEF\xBB\xBF", 3)) p += 3;
        first = false;
        p[strcspn(p, "\r\n")] = 0;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == ';' || !*p) continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *v = eq + 1;
        while (*v == ' ' || *v == '\t') v++;
        for (char *e = eq; e > p && (e[-1] == ' ' || e[-1] == '\t'); e--) e[-1] = 0;
        if (!strcasecmp(p, "Name")) snprintf(c->name, sizeof c->name, "%s", v);
        else if (!strcasecmp(p, "Id")) snprintf(c->id, sizeof c->id, "%s", v);
        else if (!strcasecmp(p, "Exec")) snprintf(c->exec, sizeof c->exec, "%s", v);
        else if (!strcasecmp(p, "Runtime")) snprintf(c->runtime, sizeof c->runtime, "%s", v);
        else if (!strcasecmp(p, "Icon")) snprintf(c->icon, sizeof c->icon, "%s", v);
    }
    fclose(f);
}

static bool is_windows(const wincart *c)
{
    return !strncasecmp(c->runtime, "windows", 7);
}

/* 'SN' (Sion) + CRC-16/CCITT of the cart's Id: a title ID the dashboard's
   Memory screen can file the saves under. */
ULONG wincart_title_id(const char *id)
{
    unsigned crc = 0xFFFF;
    for (const unsigned char *p = (const unsigned char *)id; *p; p++) {
        crc ^= (unsigned)(*p | 0x20) << 8;    /* case-insensitive */
        for (int i = 0; i < 8; i++) crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
        crc &= 0xFFFF;
    }
    return 0x534E0000u | crc;
}

/* UTF-8 to UTF-16, truncated to n-1 units. */
static void utf16(WCHAR *out, int n, const char *s)
{
    int i = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p && i < n - 1) {
        unsigned c = *p++;
        if (c >= 0xF0 && p[0] && p[1] && p[2]) { c = ((c & 7) << 18) | ((p[0] & 0x3F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3; }
        else if (c >= 0xE0 && p[0] && p[1]) { c = ((c & 0xF) << 12) | ((p[0] & 0x3F) << 6) | (p[1] & 0x3F); p += 2; }
        else if (c >= 0xC0 && p[0]) { c = ((c & 0x1F) << 6) | (p[0] & 0x3F); p += 1; }
        if (c >= 0x10000) c = '?';
        out[i++] = (WCHAR)c;
    }
    while (i < n) out[i++] = 0;
}

/* A cart's icon as the dashboard's title image (an XBE's $$XTIMAGE): an XPR0
   texture, 128x128 swizzled A8R8G8B8, the picture scaled to fit, centred on
   transparent.  NULL when there is no readable PNG. */
enum { TI_SIDE = 128, TI_HDR = 0x800 };

static uint8_t *title_image(const char *png, ULONG *size)
{
    int w, h, n;
    uint8_t *rgba = png ? stbi_load(png, &w, &h, &n, 4) : NULL;
    if (!rgba) {
        if (png) xlog("wincart: icon %s: %s", png, stbi_failure_reason());
        return NULL;
    }
    *size = TI_HDR + TI_SIDE * TI_SIDE * 4;
    uint8_t *x = calloc(1, *size);
    ULONG hd[9] = {
        0x30525058, *size, TI_HDR,      /* XPR0, total and header sizes */
        0x00040001, 0, 0,               /* a texture: Common, Data, Lock */
        0x07710629, 0,                  /* Format: 2D, A8R8G8B8, 128x128, 1 level; Size */
        0xFFFFFFFF,                     /* end of the resources */
    };
    memcpy(x, hd, sizeof hd);
    memset(x + sizeof hd, 0xAD, TI_HDR - sizeof hd);
    float s = (float)TI_SIDE / (w > h ? w : h);
    float ox = (TI_SIDE - w * s) / 2, oy = (TI_SIDE - h * s) / 2;
    for (int y = 0; y < TI_SIDE; y++)
        for (int xx = 0; xx < TI_SIDE; xx++) {
            /* 4x4 samples per texel: fine for shrinking and growing alike */
            unsigned acc[4] = { 0 }, cnt = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    int px = (int)((xx + (sx + 0.5f) / 4 - ox) / s), py = (int)((y + (sy + 0.5f) / 4 - oy) / s);
                    cnt++;
                    if (px < 0 || py < 0 || px >= w || py >= h) continue;
                    const uint8_t *p = rgba + ((size_t)py * w + px) * 4;
                    acc[0] += p[2] * p[3]; acc[1] += p[1] * p[3]; acc[2] += p[0] * p[3]; acc[3] += p[3];
                }
            uint8_t *d = x + TI_HDR;
            unsigned at = 0;
            for (int b = 0; b < 7; b++) at |= ((xx >> b) & 1) << (2 * b) | ((y >> b) & 1) << (2 * b + 1);
            d += at * 4;
            if (acc[3]) for (int k = 0; k < 3; k++) d[k] = (uint8_t)(acc[k] / acc[3]);
            d[3] = (uint8_t)(acc[3] / cnt);
        }
    stbi_image_free(rgba);
    return x;
}

/* A stub XBE: headers, a certificate, one empty section and, given a cart
   icon, its $$XTIMAGE.  Nothing in it ever runs; the dashboard reads the
   certificate and title image and xbrun.py its (empty) library list. */
static bool write_stub_xbe(const char *path, const wincart *c, const char *icon)
{
    enum { HDR = 0x1000, TEXT_VA = XBE_BASE + 0x1000 };
    ULONG tisize = 0;
    uint8_t *ti = title_image(icon, &tisize);
    ULONG tiraw = (tisize + 0xFFF) & ~0xFFFu;
    size_t len = HDR + 0x1000 + tiraw;
    unsigned char *img = calloc(1, len);
    XBE_HEADER *h = (XBE_HEADER *)img;
    time_t stamp = c->mtime;
    struct stat ib;
    if (ti && stat(icon, &ib) == 0 && ib.st_mtime > stamp) stamp = ib.st_mtime;  /* new icon, new cache name */
    h->Signature = 0x48454258;      /* XBEH */
    h->BaseAddress = XBE_BASE;
    h->SizeOfHeaders = HDR;
    h->SizeOfImage = len;
    h->SizeOfImageHeader = sizeof *h;
    h->TimeDateStamp = (ULONG)stamp;
    h->Certificate = XBE_BASE + sizeof *h;
    h->NumberOfSections = ti ? 2 : 1;
    h->SectionHeaders = h->Certificate + sizeof(XBE_CERTIFICATE);
    h->AddressOfEntryPoint = (TEXT_VA + 0x10) ^ 0xA8FC57AB;     /* retail keys */
    h->XboxKernelThunkData = TEXT_VA ^ 0x5B6D40B6;
    h->SizeOfStackCommit = 0x10000;
    h->SizeOfHeapReserve = 0x100000;
    h->SizeOfHeapCommit = 0x1000;
    h->NtBaseOfDll = 0x10000;
    h->NtSizeOfImage = len;
    ULONG names = h->SectionHeaders + 2 * sizeof(XBE_SECTION);   /* ".text", "$$XTIMAGE", then an empty string */
    h->DebugPathName = h->DebugFileName = h->DebugUnicodeFileName = names + 32;
    h->NumberOfLibraryVersions = 0;
    h->LibraryVersions = names + 32;

    XBE_CERTIFICATE *cert = (XBE_CERTIFICATE *)(img + sizeof *h);
    cert->SizeOfCertificate = sizeof *cert;
    cert->TimeDateStamp = (ULONG)stamp;
    cert->TitleID = wincart_title_id(c->id);
    utf16(cert->TitleName, 40, c->name);
    cert->AllowedMediaTypes = 0x000000FF;
    cert->GameRegion = 0xFFFFFFFF;
    cert->GameRatings = 0xFFFFFFFF;
    cert->Version = 1;

    XBE_SECTION *sec = (XBE_SECTION *)(img + (h->SectionHeaders - XBE_BASE));
    sec->SectionFlags = XBE_SECTION_PRELOAD | 0x1 /* writable */ | 0x4 /* executable */;
    sec->VirtualAddress = TEXT_VA;
    sec->VirtualSize = 0x1000;
    sec->PointerToRawData = HDR;
    sec->SizeOfRawData = 0x1000;
    sec->SectionName = names;
    memcpy(img + (names - XBE_BASE), ".text", 6);
    img[HDR + 0x10] = 0xC3;         /* ret, never reached */
    if (ti) {                       /* not preloaded: only the dashboards read it, from the file */
        sec[1].VirtualAddress = TEXT_VA + 0x1000;
        sec[1].VirtualSize = tisize;
        sec[1].PointerToRawData = HDR + 0x1000;
        sec[1].SizeOfRawData = tisize;
        sec[1].SectionName = names + 16;
        memcpy(img + (names + 16 - XBE_BASE), "$$XTIMAGE", 10);
        memcpy(img + HDR + 0x1000, ti, tisize);
        free(ti);
    }

    char tmp[PATH_MAX];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    bool ok = f && fwrite(img, 1, len, f) == len;
    if (f) ok = fclose(f) == 0 && ok;
    free(img);
    if (ok && rename(tmp, path)) ok = false;
    if (!ok) unlink(tmp);
    return ok;
}

static bool write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (!f) return false;
    fputs(text, f);
    return fclose(f) == 0;
}

/* The cart's .kzi as the dashboard sees it: an Xbox cart of the stub
   (icon: the cart's icon as linked into the stub's folder, or NULL). */
static bool write_shim_kzi(const char *path, const char *orig, const wincart *c, const char *icon)
{
    FILE *in = fopen(orig, "r"), *out = fopen(path, "w");
    if (!in || !out) { if (in) fclose(in); if (out) fclose(out); return false; }
    char line[1024];
    while (fgets(line, sizeof line, in)) {
        char key[32];
        const char *p = line;
        if (!strncmp(p, "\xEF\xBB\xBF", 3)) p += 3;
        size_t n = strspn(p, " \t");
        size_t k = strcspn(p + n, "= \t\r\n");
        snprintf(key, sizeof key, "%.*s", (int)(k < sizeof key ? k : sizeof key - 1), p + n);
        if (!strcasecmp(key, "Runtime") || !strcasecmp(key, "Exec") || (icon && !strcasecmp(key, "Icon"))) continue;
        fputs(line, out);
    }
    fprintf(out, "\nRuntime=xbox\nExec=%s/default.xbe\n", c->dir);
    if (icon) fprintf(out, "Icon=%s/%s\n", c->dir, icon);
    fclose(in);
    return fclose(out) == 0;
}

/* The cart's Icon, resolved the way Kazeta does (beside the .kzi), else in
   the cart's folder, linked into the stub's folder (which hides the real
   one) so the dashboards can show it.  Returns its name there, or NULL. */
static const char *link_icon(const char *card, const wincart *c, const char *stub)
{
    if (!c->icon[0] || strstr(c->icon, "..")) return NULL;
    const char *base = strrchr(c->icon, '/');
    base = base ? base + 1 : c->icon;
    if (!*base || !strcmp(base, "default.xbe") || !strcmp(base, ".wincart")) return NULL;
    char from[PATH_MAX], to[PATH_MAX];
    struct stat sb;
    snprintf(from, sizeof from, "%s/%s", card, c->icon);
    if (stat(from, &sb) || !S_ISREG(sb.st_mode)) {
        snprintf(from, sizeof from, "%s/%s/%s", card, c->dir, c->icon);
        if (stat(from, &sb) || !S_ISREG(sb.st_mode)) return NULL;
    }
    snprintf(to, sizeof to, "%s/%s", stub, base);
    if (symlink(from, to)) { xlog("wincart: cannot link %s: %s", to, strerror(errno)); return NULL; }
    return base;
}

static int rm_entry(const char *path, const struct stat *sb, int type, struct FTW *ftw)
{
    (void)sb; (void)type; (void)ftw;
    return remove(path);
}

static void rm_tree(const char *path)
{
    nftw(path, rm_entry, 16, FTW_DEPTH | FTW_PHYS);
}

/* The card the dashboard gets for the one mounted at card: the card itself
   unless it holds a Windows cart, then its shim. */
void wincart_shim(const char *card, char *out, size_t outlen)
{
    wincart carts[16];
    int n = 0;
    DIR *d = opendir(card);
    if (!d) return;
    struct dirent *e;
    char sig[sizeof shim_sig];
    int sn = snprintf(sig, sizeof sig, "%s\n", card);
    while ((e = readdir(d)) && n < 16) {
        size_t len = strlen(e->d_name);
        if (len <= 4 || len > NAME_MAX || strcasecmp(e->d_name + len - 4, ".kzi")) continue;
        char path[PATH_MAX];
        struct stat sb;
        snprintf(path, sizeof path, "%s/%s", card, e->d_name);
        if (stat(path, &sb)) continue;
        wincart *c = &carts[n];
        memset(c, 0, sizeof *c);
        snprintf(c->kzi, sizeof c->kzi, "%s", e->d_name);
        c->mtime = sb.st_mtime;
        kzi_read(path, c);
        if (!is_windows(c)) continue;
        /* The cart's folder: named after the .kzi, as Kazeta lays a card out,
           or after its Id. */
        snprintf(c->dir, sizeof c->dir, "%.*s", (int)(len - 4), e->d_name);
        snprintf(path, sizeof path, "%s/%s", card, c->dir);
        if (stat(path, &sb) || !S_ISDIR(sb.st_mode)) {
            snprintf(path, sizeof path, "%s/%s", card, c->id);
            if (c->id[0] && stat(path, &sb) == 0 && S_ISDIR(sb.st_mode)) snprintf(c->dir, sizeof c->dir, "%s", c->id);
        }
        if (!c->id[0]) snprintf(c->id, sizeof c->id, "%s", c->dir);
        if (!c->name[0]) snprintf(c->name, sizeof c->name, "%s", c->id);
        if (sn < (int)sizeof sig) sn += snprintf(sig + sn, sizeof sig - sn, "%s %lld\n", e->d_name, (long long)c->mtime);
        n++;
    }
    closedir(d);
    if (!n) return;
    if (shim_dir[0] && !strcmp(sig, shim_sig)) {
        struct stat sb;
        if (stat(shim_dir, &sb) == 0) { snprintf(out, outlen, "%s", shim_dir); return; }
    }

    /* (Re)build the shim. */
    const char *base = strrchr(card, '/');
    char root[64], dir[PATH_MAX];
    snprintf(root, sizeof root, "/tmp/xbcompat-card-%u", (unsigned)getuid());
    mkdir(root, 0700);
    snprintf(dir, sizeof dir, "%s/%s", root, base && base[1] ? base + 1 : "card");
    rm_tree(dir);
    if (mkdir(dir, 0700) && errno != EEXIST) { xlog("wincart: cannot make %s: %s", dir, strerror(errno)); return; }
    d = opendir(card);
    if (!d) return;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        bool skip = false;
        for (int i = 0; i < n; i++)
            if (!strcmp(e->d_name, carts[i].kzi) || !strcmp(e->d_name, carts[i].dir)) skip = true;
        if (skip) continue;
        char from[PATH_MAX], to[PATH_MAX];
        snprintf(from, sizeof from, "%s/%s", card, e->d_name);
        snprintf(to, sizeof to, "%s/%s", dir, e->d_name);
        if (symlink(from, to)) xlog("wincart: cannot link %s: %s", to, strerror(errno));
    }
    closedir(d);
    for (int i = 0; i < n; i++) {
        wincart *c = &carts[i];
        char orig[PATH_MAX], path[PATH_MAX], text[PATH_MAX + 256];
        snprintf(orig, sizeof orig, "%s/%s", card, c->kzi);
        snprintf(path, sizeof path, "%s/%s", dir, c->kzi);
        char stub[PATH_MAX];
        snprintf(stub, sizeof stub, "%s/%s", dir, c->dir);
        mkdir(stub, 0700);
        const char *icon = link_icon(card, c, stub);
        write_shim_kzi(path, orig, c, icon);
        char iconpath[PATH_MAX];
        if (icon) snprintf(iconpath, sizeof iconpath, "%s/%s", stub, icon);
        snprintf(path, sizeof path, "%s/%s/default.xbe", dir, c->dir);
        if (!write_stub_xbe(path, c, icon ? iconpath : NULL)) xlog("wincart: cannot write %s: %s", path, strerror(errno));
        snprintf(path, sizeof path, "%s/%s/.wincart", dir, c->dir);
        snprintf(text, sizeof text, "kzi=%s\ntitle=%08x\nname=%s\nid=%s\nruntime=%s\nexec=%s\n",
                 orig, (unsigned)wincart_title_id(c->id), c->name, c->id, c->runtime, c->exec);
        write_text(path, text);
        xlog("wincart: %s (%s) is title %08x, runtime %s", c->name, c->kzi, (unsigned)wincart_title_id(c->id), c->runtime);
    }
    snprintf(shim_dir, sizeof shim_dir, "%s", dir);
    snprintf(shim_sig, sizeof shim_sig, "%s", sig);
    snprintf(out, outlen, "%s", dir);
}

/* Started on a Windows cart's stub XBE (a .wincart beside it): run the cart
   with wincart-run instead, in this process.  Returns only on failure. */
void wincart_exec(const char *xbe)
{
    char path[PATH_MAX];
    const char *slash = strrchr(xbe, '/');
    snprintf(path, sizeof path, "%.*s.wincart", slash ? (int)(slash - xbe + 1) : 0, xbe);
    FILE *f = fopen(path, "r");
    if (!f) return;
    static char kzi[PATH_MAX], title[16];
    char line[PATH_MAX + 64];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "kzi=", 4)) snprintf(kzi, sizeof kzi, "%s", line + 4);
        else if (!strncmp(line, "title=", 6)) snprintf(title, sizeof title, "%s", line + 6);
    }
    fclose(f);
    if (!kzi[0]) { xlog("wincart: %s names no .kzi", path); return; }
    const char *run = getenv("XBCOMPAT_WINCART_RUN");
    if (!run || !*run) run = "/opt/xbcompat/tools/wincart-run";
    xlog("wincart: running %s with %s (title %s)", kzi, run, title);
    if (g_log) fflush(g_log);
    fflush(stderr);
    char *args[] = { (char *)run, kzi, "--title-id", title, NULL };
    execv(run, args);
    xlog("wincart: %s: %s", run, strerror(errno));
}
