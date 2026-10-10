/*
 * The DVD drive (\Device\CdRom0) and its tray.
 *
 * The tray holds one of:
 *   - a directory (--dvd DIR): an extracted game disc, or nothing when the
 *     directory is empty.  Read once at startup.
 *   - an Xbox disc image (--dvd game.iso): an XDVDFS ("XISO") image, or a
 *     full image of a pressed disc with its game partition at 0x18300000.
 *   - a real optical drive (--dvd-drive /dev/sr0, or --dvd naming a block
 *     device).  While the drive is there it is the tray and the directory
 *     is ignored: it is polled twice a second, so discs can go in and come
 *     out while a title runs, the way the Xbox's SMC reports its tray.
 *
 * A disc in the drive is read as XDVDFS straight from the device: burned
 * XISO discs have it at the start; pressed discs (XGD1) only show it to
 * drives with Kreon firmware, which xbcompat unlocks and then reads with
 * SG_IO past the capacity the drive first reported.  A disc that isn't
 * XDVDFS but has an ISO 9660 file system with a default.xbe at its root
 * is mounted (as root) and served as a directory.  Anything else is an
 * unrecognized disc, as the dashboard calls it.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/cdrom.h>
#include <pthread.h>
#include <scsi/sg.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "../xbcompat.h"

#define SECTOR          2048
#define XGD1_BASE       0x18300000ULL   /* game partition of a pressed disc (LBA 0x30600) */
#define XDVDFS_MAGIC    "MICROSOFT*XBOX*MEDIA"

#define SMC_TRAY_STATE_OPEN         0x10
#define SMC_TRAY_STATE_NO_MEDIA     0x40
#define SMC_TRAY_STATE_CLOSING      0x50
#define SMC_TRAY_STATE_MEDIA_DETECT 0x60

#define STATUS_NO_MEDIA_IN_DEVICE   ((NTSTATUS)0xC0000013)
#define STATUS_UNRECOGNIZED_MEDIA   ((NTSTATUS)0xC0000014)

enum { DISC_NONE, DISC_XDVDFS, DISC_DIR, DISC_BAD };

struct dvd_node {
    char *name;
    uint32_t sector, size;
    uint8_t attr;
    bool loaded;
    int nkids;
    struct dvd_node *kids;
    struct disc *disc;
};

typedef struct disc {
    int kind;
    int fd;                 /* XDVDFS source; -1 once the disc is gone */
    bool sg;                /* read with SG_IO (an unlocked pressed disc) */
    uint64_t base;          /* byte offset of the XDVDFS volume */
    LONGLONG time;          /* the volume's FILETIME */
    struct dvd_node root;
    char dir[PATH_MAX];     /* DISC_DIR: where its file system is mounted */
    bool mounted_here;      /* ...by us, so we unmount it when it goes */
    pthread_mutex_t lock;   /* device reads */
} disc;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char dir_src[PATH_MAX];      /* --dvd DIR */
static char img_src[PATH_MAX];      /* --dvd IMAGE */
static char drive_src[PATH_MAX];    /* --dvd-drive DEV */
static disc *g_disc;                /* what's in the tray (drive or image) */
static ULONG g_tray = SMC_TRAY_STATE_NO_MEDIA;
static bool g_drive_present;
static bool g_dir_empty;
static bool g_title_on_disc;        /* the running title came off the drive */
static disc *g_title_disc;

/* ---- reading the medium -------------------------------------------------------------------- */

static bool sg_cmd(int fd, uint8_t *cdb, int cdblen, void *buf, unsigned len)
{
    uint8_t sense[32];
    struct sg_io_hdr h;
    memset(&h, 0, sizeof(h));
    h.interface_id = 'S';
    h.cmd_len = cdblen;
    h.cmdp = cdb;
    h.dxfer_direction = len ? SG_DXFER_FROM_DEV : SG_DXFER_NONE;
    h.dxferp = buf;
    h.dxfer_len = len;
    h.sbp = sense;
    h.mx_sb_len = sizeof(sense);
    h.timeout = 20000;
    return ioctl(fd, SG_IO, &h) == 0 && (h.info & SG_INFO_OK_MASK) == SG_INFO_OK;
}

static bool sg_read(int fd, void *buf, uint32_t lba, uint32_t count)
{
    uint8_t cdb[12] = { 0xA8 /* READ(12) */, 0,
                        lba >> 24, lba >> 16, lba >> 8, lba,
                        count >> 24, count >> 16, count >> 8, count, 0, 0 };
    return sg_cmd(fd, cdb, sizeof(cdb), buf, count * SECTOR);
}

/* Raw bytes of the volume, `off` from its start. */
static bool disc_read_raw(disc *d, void *buf, size_t len, uint64_t off)
{
    if (d->fd < 0) return false;
    off += d->base;
    if (!d->sg) {
        while (len) {
            ssize_t n = pread(d->fd, buf, len, (off_t)off);
            if (n <= 0) return false;
            buf = (char *)buf + n;
            len -= n;
            off += n;
        }
        return true;
    }
    /* SG_IO reads whole sectors, at most 32 (64 KB) at a time. */
    static uint8_t bounce[32 * SECTOR];
    while (len) {
        uint32_t lba = off / SECTOR, skip = off % SECTOR;
        uint32_t count = (skip + len + SECTOR - 1) / SECTOR;
        if (count > 32) count = 32;
        if (!sg_read(d->fd, bounce, lba, count)) return false;
        size_t n = count * SECTOR - skip;
        if (n > len) n = len;
        memcpy(buf, bounce + skip, n);
        buf = (char *)buf + n;
        len -= n;
        off += n;
    }
    return true;
}

static bool disc_read(disc *d, void *buf, size_t len, uint64_t off)
{
    pthread_mutex_lock(&d->lock);
    bool ok = disc_read_raw(d, buf, len, off);
    pthread_mutex_unlock(&d->lock);
    return ok;
}

static bool has_xdvdfs(disc *d, uint64_t base)
{
    char vd[SECTOR];
    d->base = base;
    if (!disc_read_raw(d, vd, sizeof(vd), 32 * SECTOR)) return false;
    if (memcmp(vd, XDVDFS_MAGIC, 20) || memcmp(vd + 0x7EC, XDVDFS_MAGIC, 20)) return false;
    memset(&d->root, 0, sizeof(d->root));
    memcpy(&d->root.sector, vd + 20, 4);
    memcpy(&d->root.size, vd + 24, 4);
    memcpy(&d->time, vd + 28, 8);
    d->root.attr = 0x10;
    d->root.name = "";
    d->root.disc = d;
    return true;
}

/* ---- XDVDFS directories ------------------------------------------------------------------- */

/* A directory is a binary tree of entries: left and right child offsets (in
   dwords, 0 = none), start sector, size, attributes, name length, name. */
static void walk(const uint8_t *tab, uint32_t size, uint32_t off, int depth,
                 struct dvd_node *out, int *n, int max)
{
    if (depth > 64 || off + 14 > size || *n >= max) return;
    const uint8_t *e = tab + off;
    uint16_t left, right;
    memcpy(&left, e, 2);
    memcpy(&right, e + 2, 2);
    if (left == 0xFFFF && right == 0xFFFF) return;   /* padding: an empty directory */
    uint8_t namelen = e[13];
    if (!namelen || off + 14 + namelen > size) return;
    if (left) walk(tab, size, left * 4u, depth + 1, out, n, max);
    if (*n < max) {
        struct dvd_node *k = &out[(*n)++];
        memset(k, 0, sizeof(*k));
        memcpy(&k->sector, e + 4, 4);
        memcpy(&k->size, e + 8, 4);
        k->attr = e[12];
        k->name = strndup((const char *)e + 14, namelen);
    }
    if (right) walk(tab, size, right * 4u, depth + 1, out, n, max);
}

/* Called with the disc's lock held. */
static void load_dir(struct dvd_node *dn)
{
    if (dn->loaded) return;
    dn->loaded = true;
    if (!dn->size || dn->size > (16u << 20)) return;
    uint32_t len = (dn->size + SECTOR - 1) & ~(SECTOR - 1);
    uint8_t *tab = malloc(len);
    if (!disc_read_raw(dn->disc, tab, len, (uint64_t)dn->sector * SECTOR)) {
        xlog("DVD: cannot read a directory at sector %u", dn->sector);
        free(tab);
        return;
    }
    int max = dn->size / 16 + 1;
    dn->kids = calloc(max, sizeof(*dn->kids));
    walk(tab, dn->size, 0, 0, dn->kids, &dn->nkids, max);
    for (int i = 0; i < dn->nkids; i++) dn->kids[i].disc = dn->disc;
    free(tab);
}

/* ---- the tray ------------------------------------------------------------------------------ */

static void disc_drop(disc *d)
{
    if (!d) return;
    pthread_mutex_lock(&d->lock);
    if (d->fd >= 0) close(d->fd);
    d->fd = -1;
    if (d->mounted_here) umount2(d->dir, MNT_DETACH);
    d->mounted_here = false;
    pthread_mutex_unlock(&d->lock);
    /* The disc itself stays allocated: open handles still point at its
       nodes, and reads through them now fail as "no media". */
}

static bool dir_has_xbe(const char *dir)
{
    DIR *dd = opendir(dir);
    if (!dd) return false;
    struct dirent *e;
    bool found = false;
    while (!found && (e = readdir(dd))) found = !strcasecmp(e->d_name, "default.xbe");
    closedir(dd);
    return found;
}

/* A data disc with a plain file system: mount it, unless it already is. */
static bool try_mount(disc *d, const char *dev)
{
    char real[PATH_MAX];
    if (!realpath(dev, real)) return false;
    /* Our own mount point is mounted afresh: what's there may be a disc
       that has since come out (left by a title that ended without
       unmounting it). Someone else's mount of the drive is used as is. */
    static const char ours[] = "/run/xbcompat-disc";
    umount2(ours, MNT_DETACH);
    FILE *m = fopen("/proc/mounts", "r");
    char line[1024];
    while (m && fgets(line, sizeof(line), m)) {
        char src[PATH_MAX], dst[PATH_MAX];
        if (sscanf(line, "%4095s %4095s", src, dst) == 2 && !strcmp(src, real)) {
            snprintf(d->dir, sizeof(d->dir), "%s", dst);
            fclose(m);
            return true;
        }
    }
    if (m) fclose(m);
    snprintf(d->dir, sizeof(d->dir), "%s", ours);
    mkdir(d->dir, 0755);
    if (mount(real, d->dir, "iso9660", MS_RDONLY | MS_NOATIME, "") != 0) return false;
    d->mounted_here = true;
    return true;
}

/* What's on the medium at `path` (an image file or a drive with a disc). */
/* Sector 32 of the disc as the log shows it, to tell what was burned. */
static void log_sector32(disc *d, const char *how)
{
    uint8_t b[24];
    d->base = 0;
    if (!disc_read_raw(d, b, sizeof(b), 32 * SECTOR)) {
        xlog("DVD: sector 32 can't be read %s: %s", how, strerror(errno));
        return;
    }
    char hex[80], txt[32];
    for (int i = 0; i < 24; i++) {
        snprintf(hex + i * 3, 4, "%02x ", b[i]);
        txt[i] = isprint(b[i]) ? b[i] : '.';
    }
    txt[24] = 0;
    xlog("DVD: sector 32 %s: %s|%s|", how, hex, txt);
}

/* Look for XDVDFS at the start and at a pressed disc's game partition, read
   through the block device and then with SG_IO (which doesn't depend on
   the capacity the block device has settled on yet). */
static bool find_xdvdfs(disc *d, bool drive)
{
    for (int sg = 0; sg <= (drive ? 1 : 0); sg++) {
        d->sg = sg;
        if (has_xdvdfs(d, 0) || has_xdvdfs(d, XGD1_BASE)) return true;
    }
    d->sg = false;
    return false;
}

/* What's on the medium at `path` (an image file or a drive with a disc). */
static disc *probe(const char *path, bool drive)
{
    disc *d = calloc(1, sizeof(*d));
    pthread_mutex_init(&d->lock, NULL);
    d->kind = DISC_BAD;
    if (drive) {
        /* A plain open checks the disc and sets the device's capacity (an
           O_NONBLOCK open doesn't); closing it again leaves the tray free. */
        int t = open(path, O_RDONLY);
        if (t >= 0) close(t);
    }
    d->fd = open(path, O_RDONLY | (drive ? O_NONBLOCK : 0));
    if (d->fd < 0) {
        xlog("DVD: cannot open %s: %s", path, strerror(errno));
        return d;
    }
    /* A drive that has just closed its tray can say the disc is ready
       before the first reads work: give it a few tries. */
    bool found = false;
    for (int tries = 0; tries < (drive ? 6 : 1) && !(found = find_xdvdfs(d, drive)); tries++) {
        if (drive) {
            struct timespec ts = { 2, 0 };
            nanosleep(&ts, NULL);
            int t = open(path, O_RDONLY);
            if (t >= 0) close(t);
        }
    }
    if (found) {
        d->kind = DISC_XDVDFS;
        xlog("DVD: %s is an Xbox disc (XDVDFS%s%s)", path, d->base ? ", pressed-disc layout" : "",
             d->sg ? ", read with SG_IO" : "");
        return d;
    }
    log_sector32(d, "through the block device");
    if (drive) {
        d->sg = true;
        log_sector32(d, "with SG_IO");
        /* Kreon firmware: "set lock state" to Xtreme unlock, which shows
           the game partition of a pressed disc.  Other drives refuse it. */
        uint8_t cdb[12] = { 0xFF, 0x08, 0x01, 0x11, 0x01 };
        if (sg_cmd(d->fd, cdb, sizeof(cdb), NULL, 0)) {
            if (has_xdvdfs(d, XGD1_BASE)) {
                d->kind = DISC_XDVDFS;
                xlog("DVD: %s is a pressed Xbox game disc, unlocked by the drive's Kreon firmware", path);
                return d;
            }
        }
        d->sg = false;
        d->base = 0;
        if (try_mount(d, path) && dir_has_xbe(d->dir)) {
            d->kind = DISC_DIR;
            xlog("DVD: %s is a data disc with a default.xbe, mounted on %s", path, d->dir);
            return d;
        }
        if (d->mounted_here) umount2(d->dir, MNT_DETACH);
        d->mounted_here = false;
        d->dir[0] = 0;
        xlog("DVD: the disc in %s isn't one xbcompat can play. A pressed Xbox game disc only shows "
             "its game to a drive with Kreon firmware; burned XISO discs work in any drive.", path);
    } else {
        xlog("DVD: %s has no Xbox file system", path);
    }
    close(d->fd);
    d->fd = -1;
    return d;
}

static ULONG tray_of(const disc *d)
{
    return d && (d->kind == DISC_XDVDFS || d->kind == DISC_DIR) ? SMC_TRAY_STATE_MEDIA_DETECT
                                                                : SMC_TRAY_STATE_NO_MEDIA;
}

static int drive_fd = -1;
static char tray_img[PATH_MAX];     /* a tray file's disc image (below) */

/* For tests without a drive: --dvd-drive naming a regular file makes it a
   tray file, whose first line says what's in the drive: "open", nothing,
   or the path of a disc image. */
static int tray_file_status(bool *changed)
{
    char line[PATH_MAX] = "";
    FILE *f = fopen(drive_src, "r");
    if (!f) return -1;
    if (!fgets(line, sizeof(line), f)) line[0] = 0;
    fclose(f);
    line[strcspn(line, "\r\n")] = 0;
    if (!strcmp(line, "open")) return CDS_TRAY_OPEN;
    if (!line[0]) return CDS_NO_DISC;
    *changed = strcmp(line, tray_img) != 0;
    snprintf(tray_img, sizeof(tray_img), "%s", line);
    return CDS_DISC_OK;
}

/* One look at the drive: its CDS_* status, or -1 when there is no drive. */
static int drive_status(bool *changed)
{
    *changed = false;
    struct stat sb;
    if (stat(drive_src, &sb) == 0 && S_ISREG(sb.st_mode)) return tray_file_status(changed);
    if (drive_fd < 0) {
        drive_fd = open(drive_src, O_RDONLY | O_NONBLOCK);
        if (drive_fd < 0) return -1;
        ioctl(drive_fd, CDROM_LOCKDOOR, 0);   /* the tray opens whenever its button is pressed */
        ioctl(drive_fd, CDROM_MEDIA_CHANGED, CDSL_CURRENT);
    }
    int st = ioctl(drive_fd, CDROM_DRIVE_STATUS, CDSL_CURRENT);
    if (st < 0) {
        /* Unplugged: the next poll opens it again if it's back. */
        close(drive_fd);
        drive_fd = -1;
        return -1;
    }
    if (st == CDS_DISC_OK) *changed = ioctl(drive_fd, CDROM_MEDIA_CHANGED, CDSL_CURRENT) > 0;
    return st;
}

static void poll_drive(void)
{
    bool changed;
    int st = drive_status(&changed);
    pthread_mutex_lock(&g_lock);
    bool had = g_drive_present;
    g_drive_present = st >= 0;
    disc *cur = g_disc;
    pthread_mutex_unlock(&g_lock);
    if (!g_drive_present && had) xlog("DVD: the drive %s is gone", drive_src);
    if (g_drive_present && !had) xlog("DVD: the drive %s is here", drive_src);

    disc *next = cur;
    ULONG tray;
    if (st == CDS_DISC_OK) {
        if (!cur || changed) next = tray_img[0] ? probe(tray_img, false) : probe(drive_src, true);
        tray = tray_of(next);
    } else {
        next = NULL;
        tray = st == CDS_TRAY_OPEN ? SMC_TRAY_STATE_OPEN :
               st == CDS_DRIVE_NOT_READY ? SMC_TRAY_STATE_CLOSING : SMC_TRAY_STATE_NO_MEDIA;
    }

    pthread_mutex_lock(&g_lock);
    g_disc = next;
    if (tray != g_tray) xlog("DVD: tray state %#x -> %#x", g_tray, tray);
    g_tray = tray;
    pthread_mutex_unlock(&g_lock);
    if (cur && cur != next) {
        xlog("DVD: the disc is out");
        disc_drop(cur);
        if (g_title_on_disc && cur == g_title_disc) {
            /* The Xbox goes back to the dashboard when the disc a game is
               playing from comes out. */
            xlog("DVD: the running title's disc was ejected; back to the dashboard");
            g_title_on_disc = false;
            kill(getpid(), SIGTERM);
        }
    }
}

static void *poll_thread(void *arg)
{
    pthread_setname_np(pthread_self(), "dvd-poll");
    (void)arg;
    for (;;) {
        struct timespec ts = { 0, 500 * 1000 * 1000 };
        nanosleep(&ts, NULL);
        poll_drive();
    }
    return NULL;
}

void dvd_init(const char *src, const char *drive)
{
    struct stat sb;
    if (src && stat(src, &sb) == 0 && !S_ISDIR(sb.st_mode)) {
        if (S_ISBLK(sb.st_mode) && !drive) drive = src;
        else if (!S_ISBLK(sb.st_mode)) snprintf(img_src, sizeof(img_src), "%s", src);
    } else if (src) {
        snprintf(dir_src, sizeof(dir_src), "%s", src);
    }
    if (drive) snprintf(drive_src, sizeof(drive_src), "%s", drive);

    if (dir_src[0]) {
        g_dir_empty = true;
        DIR *d = opendir(dir_src);
        struct dirent *e;
        while (d && (e = readdir(d)))
            if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) { g_dir_empty = false; break; }
        if (d) closedir(d);
    }
    if (img_src[0]) {
        g_disc = probe(img_src, false);
        g_tray = tray_of(g_disc);
    }
    if (drive_src[0]) {
        poll_drive();   /* the disc that's in at startup is there from the first read */
        pthread_t t;
        pthread_create(&t, NULL, poll_thread, NULL);
        pthread_detach(t);
    }
}

/* Whether \Device\CdRom0 is the drive or image (true), or the directory. */
bool dvd_is_media(void)
{
    pthread_mutex_lock(&g_lock);
    bool media = img_src[0] || g_drive_present;
    pthread_mutex_unlock(&g_lock);
    return media;
}

/* The host directory behind \Device\CdRom0, when there is one: --dvd DIR,
   or a mounted data disc.  NULL for an XDVDFS disc or an empty drive. */
const char *dvd_host_dir(void)
{
    if (!dvd_is_media()) return dir_src[0] ? dir_src : NULL;
    static char dir[PATH_MAX];
    pthread_mutex_lock(&g_lock);
    const char *r = NULL;
    if (g_disc && g_disc->kind == DISC_DIR) {
        snprintf(dir, sizeof(dir), "%s", g_disc->dir);
        r = dir;
    }
    pthread_mutex_unlock(&g_lock);
    return r;
}

ULONG dvd_tray_state(void)
{
    if (!dvd_is_media()) return g_dir_empty ? SMC_TRAY_STATE_NO_MEDIA : SMC_TRAY_STATE_MEDIA_DETECT;
    pthread_mutex_lock(&g_lock);
    ULONG t = g_tray;
    pthread_mutex_unlock(&g_lock);
    return t;
}

bool dvd_tray_empty(void)
{
    return dvd_tray_state() != SMC_TRAY_STATE_MEDIA_DETECT;
}

/* IOCTL_CDROM_CHECK_VERIFY: ready, empty, or a disc it can't read. */
NTSTATUS dvd_check_verify(void)
{
    if (!dvd_is_media()) return g_dir_empty ? STATUS_NO_MEDIA_IN_DEVICE : STATUS_SUCCESS;
    pthread_mutex_lock(&g_lock);
    NTSTATUS st = g_tray == SMC_TRAY_STATE_MEDIA_DETECT ? STATUS_SUCCESS :
                  g_disc && g_disc->kind == DISC_BAD ? STATUS_UNRECOGNIZED_MEDIA : STATUS_NO_MEDIA_IN_DEVICE;
    pthread_mutex_unlock(&g_lock);
    return st;
}

/* ---- files on an XDVDFS disc --------------------------------------------------------------- */

/* The node for `rest` (a path below \Device\CdRom0, "" for the root).
   STATUS_NO_MEDIA_IN_DEVICE when there's no XDVDFS disc in the tray. */
NTSTATUS dvd_lookup(const char *rest, dvd_node **out)
{
    *out = NULL;
    pthread_mutex_lock(&g_lock);
    disc *d = g_disc && g_disc->kind == DISC_XDVDFS && g_disc->fd >= 0 ? g_disc : NULL;
    pthread_mutex_unlock(&g_lock);
    if (!d) return STATUS_NO_MEDIA_IN_DEVICE;
    pthread_mutex_lock(&d->lock);
    dvd_node *n = &d->root;
    NTSTATUS st = STATUS_SUCCESS;
    while (*rest) {
        while (*rest == '\\') rest++;
        if (!*rest) break;
        const char *end = strchr(rest, '\\');
        size_t len = end ? (size_t)(end - rest) : strlen(rest);
        if (!(n->attr & 0x10)) { st = STATUS_OBJECT_PATH_NOT_FOUND; break; }
        load_dir(n);
        dvd_node *k = NULL;
        if (len == 1 && rest[0] == '.') k = n;
        for (int i = 0; !k && i < n->nkids; i++)
            if (strlen(n->kids[i].name) == len && !strncasecmp(n->kids[i].name, rest, len)) k = &n->kids[i];
        rest += len;
        if (!k) {
            while (*rest == '\\') rest++;
            st = *rest ? STATUS_OBJECT_PATH_NOT_FOUND : STATUS_OBJECT_NAME_NOT_FOUND;
            break;
        }
        n = k;
    }
    pthread_mutex_unlock(&d->lock);
    if (NT_SUCCESS(st)) *out = n;
    return st;
}

bool dvd_node_is_dir(const dvd_node *n) { return n->attr & 0x10; }
const char *dvd_node_name(const dvd_node *n) { return n->name; }

/* A host stat of a disc file: its size, read-only, and the volume's time. */
void dvd_stat(const dvd_node *n, struct stat *sb)
{
    memset(sb, 0, sizeof(*sb));
    sb->st_mode = (n->attr & 0x10 ? S_IFDIR | 0555 : S_IFREG | 0444);
    sb->st_size = n->attr & 0x10 ? 0 : n->size;
    sb->st_ino = n->sector + 1;
    sb->st_nlink = 1;
    /* FILETIME (100 ns since 1601) to Unix time. */
    LONGLONG t = n->disc->time > 116444736000000000LL ? n->disc->time - 116444736000000000LL : 0;
    sb->st_mtim.tv_sec = sb->st_ctim.tv_sec = sb->st_atim.tv_sec = t / 10000000;
    sb->st_mtim.tv_nsec = sb->st_ctim.tv_nsec = sb->st_atim.tv_nsec = (t % 10000000) * 100;
}

/* The i-th entry of a disc directory, or NULL past its end. */
dvd_node *dvd_child(dvd_node *n, int i)
{
    pthread_mutex_lock(&n->disc->lock);
    load_dir(n);
    dvd_node *k = i < n->nkids ? &n->kids[i] : NULL;
    pthread_mutex_unlock(&n->disc->lock);
    return k;
}

/* Read from a disc file; -1 when the disc is gone or unreadable. */
ssize_t dvd_read(dvd_node *n, void *buf, size_t len, uint64_t off)
{
    if (n->attr & 0x10) return -1;
    if (off >= n->size) return 0;
    if (len > n->size - off) len = n->size - off;
    return disc_read(n->disc, buf, len, (uint64_t)n->sector * SECTOR + off) ? (ssize_t)len : -1;
}

/* Raw sectors of the volume (a read of the CdRom0 device itself). */
ssize_t dvd_read_volume(void *buf, size_t len, uint64_t off)
{
    pthread_mutex_lock(&g_lock);
    disc *d = g_disc && g_disc->kind == DISC_XDVDFS ? g_disc : NULL;
    pthread_mutex_unlock(&g_lock);
    return d && disc_read(d, buf, len, off) ? (ssize_t)len : -1;
}

/* A copy of a disc file on the host, for the loader and xbrun.py, which take
   an XBE by its path: kept under $TMPDIR/xbcompat-disc while it matches. */
static const char *cache_dir(void)
{
    static char dir[PATH_MAX];
    if (!dir[0]) {
        const char *t = getenv("TMPDIR");
        snprintf(dir, sizeof(dir), "%s/xbcompat-disc", t && *t ? t : "/tmp");
        mkdir(dir, 0755);
    }
    return dir;
}

bool dvd_extract(dvd_node *n, char *host, size_t hostlen)
{
    char safe[256];
    snprintf(safe, sizeof(safe), "%s", n->name);
    for (char *p = safe; *p; p++)
        if (*p == '/') *p = '_';
    snprintf(host, hostlen, "%s/%u-%u-%s", cache_dir(), n->sector, n->size, safe);
    struct stat sb;
    if (stat(host, &sb) == 0 && sb.st_size == n->size) return true;
    char tmp[PATH_MAX + 8];
    snprintf(tmp, sizeof(tmp), "%s.part", host);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return false;
    static char buf[1 << 16];
    uint64_t off = 0;
    bool ok = true;
    while (ok && off < n->size) {
        ssize_t r = dvd_read(n, buf, sizeof(buf), off);
        ok = r > 0 && write(fd, buf, r) == r;
        off += r > 0 ? r : 0;
    }
    close(fd);
    if (!ok || rename(tmp, host) != 0) {
        unlink(tmp);
        return false;
    }
    xlog("DVD: copied %s (%u bytes) to %s", n->name, n->size, host);
    return true;
}

/* The title being started came off the disc in the drive (a copy
   dvd_extract made, or a file on a mounted data disc): when that disc comes
   out, it goes back to the dashboard. */
void dvd_title_started(const char *xbe_host_path)
{
    const char *dir = cache_dir();
    size_t l = strlen(dir);
    pthread_mutex_lock(&g_lock);
    size_t ld = g_disc && g_disc->kind == DISC_DIR ? strlen(g_disc->dir) : 0;
    if ((g_disc && g_disc->kind == DISC_XDVDFS && !strncmp(xbe_host_path, dir, l) && xbe_host_path[l] == '/') ||
        (ld && !strncmp(xbe_host_path, g_disc->dir, ld) && xbe_host_path[ld] == '/')) {
        g_title_on_disc = true;
        g_title_disc = g_disc;
    }
    pthread_mutex_unlock(&g_lock);
}
