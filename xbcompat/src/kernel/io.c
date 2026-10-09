/*
 * Io/Nt file services on top of host directories.
 *
 * Xbox object paths are mapped like this:
 *   \??\X:                       symbolic link (created by the kernel or XAPI)
 *   \Device\CdRom0               the game disc: the directory holding the XBE,
 *                                --dvd DIR, or an Xbox disc read by dvd.c
 *                                (--dvd IMAGE, or a real drive)
 *   \Device\Harddisk0\PartitionN <hdd root>/partitionN
 *   \Device\GameCard0            a Kazeta game card: --card DIR, or else the
 *                                first SD card or USB drive mounted under
 *                                /media or /run/media with a .kzi or .kzp cart
 *                                at its top (looked up on every open, so cards
 *                                can come and go)
 * FATX and the DVD file system are case-insensitive, so each path component
 * is matched case-insensitively against the host directory.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <ftw.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "../xbcompat.h"

#define STATUS_NO_MEDIA_IN_DEVICE       ((NTSTATUS)0xC0000013)
#define STATUS_MEDIA_WRITE_PROTECTED    ((NTSTATUS)0xC00000A2)
#ifndef STATUS_INVALID_DEVICE_REQUEST
#define STATUS_INVALID_DEVICE_REQUEST   ((NTSTATUS)0xC0000010)
#endif

typedef struct xfile {
    int fd;
    DIR *dir;
    char *pattern;          /* NtQueryDirectoryFile mask */
    char *host;
    char *xbox;
    bool is_dir;
    bool is_device;         /* raw partition/drive opened for IOCTLs */
    bool delete_on_close;
    bool async;             /* opened without FILE_SYNCHRONOUS_IO_*: reads and writes pend */
    bool is_dvd;            /* a file, directory or the volume of an XDVDFS disc (dvd.c) */
    dvd_node *dvd;          /* ...that file or directory (NULL: the volume of an empty drive) */
    int dir_index;          /* next entry NtQueryDirectoryFile returns from a disc directory */
    LONGLONG pos;
} xfile;

static char cdrom_dir[PATH_MAX];
static char hdd_dir[PATH_MAX];
static char card_dir[PATH_MAX];

void fs_set_card(const char *dir)
{
    snprintf(card_dir, sizeof(card_dir), "%s", dir);
}

static bool has_cart(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return false;
    struct dirent *e;
    bool found = false;
    while (!found && (e = readdir(d))) {
        size_t n = strlen(e->d_name);
        found = n > 4 && (!strcasecmp(e->d_name + n - 4, ".kzi") || !strcasecmp(e->d_name + n - 4, ".kzp"));
    }
    closedir(d);
    return found;
}

/* The mounted game card, the way Kazeta finds one (find /media -maxdepth 2
   -name '*.kzi'); desktop automounters add a /run/media/<user> level. */
static bool find_card(char *out, size_t outlen)
{
    if (card_dir[0]) {
        snprintf(out, outlen, "%s", card_dir);
        return has_cart(card_dir);
    }
    static const char *roots[] = { "/media", "/run/media" };
    for (size_t r = 0; r < sizeof(roots) / sizeof(roots[0]); r++) {
        DIR *d = opendir(roots[r]);
        if (!d) continue;
        struct dirent *e;
        bool found = false;
        while (!found && (e = readdir(d))) {
            if (e->d_name[0] == '.') continue;
            char p[PATH_MAX];
            snprintf(p, sizeof(p), "%s/%s", roots[r], e->d_name);
            if (has_cart(p)) {
                snprintf(out, outlen, "%s", p);
                found = true;
                break;
            }
            DIR *d2 = opendir(p);
            if (!d2) continue;
            struct dirent *e2;
            while (!found && (e2 = readdir(d2))) {
                if (e2->d_name[0] == '.') continue;
                char p2[PATH_MAX];
                snprintf(p2, sizeof(p2), "%s/%s", p, e2->d_name);
                if (has_cart(p2)) {
                    snprintf(out, outlen, "%s", p2);
                    found = true;
                }
            }
            closedir(d2);
        }
        closedir(d);
        if (found) return true;
    }
    return false;
}

char *symlink_lookup(const char *name);

static void mkdir_p(const char *path)
{
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

void fs_init(const char *xbe_path, const char *hdd_root, const char *dvd_root, const char *dvd_drive)
{
    struct stat dsb;
    if (dvd_root && stat(dvd_root, &dsb) == 0 && !S_ISDIR(dsb.st_mode)) {
        /* An image or a drive: dvd.c reads it. */
        dvd_init(dvd_root, dvd_drive);
    } else {
        char *dup = realpath(dvd_root ? dvd_root : xbe_path, NULL);
        if (!dup) fatal("cannot resolve %s", dvd_root ? dvd_root : xbe_path);
        char *slash = strrchr(dup, '/');
        if (!dvd_root) *slash = 0;   /* the DVD is the XBE's directory */
        snprintf(cdrom_dir, sizeof(cdrom_dir), "%s", dup);
        free(dup);
        dvd_init(cdrom_dir, dvd_drive);
    }
    char xbe_real[PATH_MAX];
    if (realpath(xbe_path, xbe_real)) dvd_title_started(xbe_real);
    snprintf(hdd_dir, sizeof(hdd_dir), "%s", hdd_root);
    for (int i = 1; i <= 7; i++) {
        char p[PATH_MAX];
        snprintf(p, sizeof(p), "%s/partition%d", hdd_dir, i);
        mkdir_p(p);
    }
    xlog("D: is %s%s%s, hard disk partitions are under %s", cdrom_dir[0] ? cdrom_dir : dvd_root,
         dvd_drive ? " or the disc in " : "", dvd_drive ? dvd_drive : "", hdd_dir);
}

/* Find `name` in host directory `dir` ignoring case; writes the real name. */
static bool match_component(const char *dir, const char *name, char *out, size_t outlen)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    if (access(path, F_OK) == 0) {
        snprintf(out, outlen, "%s", name);
        return true;
    }
    DIR *d = opendir(dir);
    if (!d) return false;
    struct dirent *e;
    bool found = false;
    while ((e = readdir(d))) {
        if (!strcasecmp(e->d_name, name)) {
            snprintf(out, outlen, "%s", e->d_name);
            found = true;
            break;
        }
    }
    closedir(d);
    return found;
}

/*
 * Translate an Xbox object path into a host path.  Missing trailing
 * components are kept as given so creates work; *is_device is set when the
 * path names a raw device rather than something in its file system.
 */
static NTSTATUS translate_path(const char *xpath, char *host, size_t hostlen, int *is_device)
{
    char path[1024];
    /* The object manager treats a bare "X:" or "CdRom0:" prefix as a \??\ link. */
    const char *colon = strchr(xpath, ':'), *bslash = strchr(xpath, '\\');
    if (xpath[0] != '\\' && colon && (!bslash || colon < bslash))
        snprintf(path, sizeof(path), "\\??\\%s", xpath);
    else
        snprintf(path, sizeof(path), "%s", xpath);
    *is_device = 0;
    host[0] = 0;

    /* Resolve \??\X: symbolic links (possibly chained). */
    for (int depth = 0; depth < 8; depth++) {
        if (strncasecmp(path, "\\??\\", 4) != 0) break;
        char *rest = strchr(path + 4, '\\');
        char link[256];
        size_t n = rest ? (size_t)(rest - path) : strlen(path);
        snprintf(link, sizeof(link), "%.*s", (int)n, path);
        char *target = symlink_lookup(link);
        if (!target) return STATUS_OBJECT_PATH_NOT_FOUND;
        char tmp[1024];
        snprintf(tmp, sizeof(tmp), "%s%s", target, rest ? rest : "");
        free(target);
        snprintf(path, sizeof(path), "%s", tmp);
    }

    const char *rest;
    char base[PATH_MAX];
    if (!strncasecmp(path, "\\Device\\CdRom0", 14)) {
        rest = path + 14;
        const char *dir = dvd_is_media() ? dvd_host_dir() : cdrom_dir;
        if (!dir) {
            /* An Xbox disc (or none): its files are named, not mapped. */
            *is_device = 3;
            snprintf(host, hostlen, "dvd:%s", rest);
            return STATUS_SUCCESS;
        }
        snprintf(base, sizeof(base), "%s", dir);
    } else if (!strncasecmp(path, "\\Device\\GameCard0", 17)) {
        if (!find_card(base, sizeof(base))) return STATUS_OBJECT_PATH_NOT_FOUND;
        rest = path + 17;
    } else if (!strncasecmp(path, "\\Device\\Harddisk0\\Partition", 27)) {
        int part = atoi(path + 27);
        if (part == 0) {
            *is_device = 1;
            snprintf(host, hostlen, "%s", hdd_dir);
            return STATUS_SUCCESS;
        }
        snprintf(base, sizeof(base), "%s/partition%d", hdd_dir, part);
        rest = path + 27;
        while (*rest >= '0' && *rest <= '9') rest++;
    } else {
        return STATUS_OBJECT_PATH_NOT_FOUND;
    }
    if (*rest == 0) {
        /* The volume itself: a directory handle that also takes IOCTLs. */
        *is_device = 2;
        snprintf(host, hostlen, "%s", base);
        return STATUS_SUCCESS;
    }

    char cur[PATH_MAX];
    snprintf(cur, sizeof(cur), "%s", base);
    char comp[256];
    while (*rest) {
        while (*rest == '\\') rest++;
        if (!*rest) break;
        const char *end = strchr(rest, '\\');
        size_t n = end ? (size_t)(end - rest) : strlen(rest);
        snprintf(comp, sizeof(comp), "%.*s", (int)n, rest);
        rest += n;
        char real[256];
        if (!strcmp(comp, ".")) continue;
        if (match_component(cur, comp, real, sizeof(real))) {
            size_t l = strlen(cur);
            snprintf(cur + l, sizeof(cur) - l, "/%s", real);
        } else {
            bool last = *rest == 0 || (rest[0] == '\\' && rest[1] == 0);
            if (!last) {
                snprintf(host, hostlen, "%s/%s%s", cur, comp, rest);
                return STATUS_OBJECT_PATH_NOT_FOUND;
            }
            size_t l = strlen(cur);
            snprintf(cur + l, sizeof(cur) - l, "/%s", comp);
        }
    }
    snprintf(host, hostlen, "%s", cur);
    return STATUS_SUCCESS;
}

static char *full_xbox_path(OBJECT_ATTRIBUTES *oa)
{
    char name[1024];
    snprintf(name, sizeof(name), "%.*s", oa->ObjectName ? oa->ObjectName->Length : 0,
             oa->ObjectName ? oa->ObjectName->Buffer : "");
    if (oa->RootDirectory) {
        xobject *o = handle_lookup(oa->RootDirectory);
        if (o && o->kind == OBJ_FILE) {
            char *r;
            if (asprintf(&r, "%s\\%s", o->file->xbox, name) < 0) return NULL;
            return r;
        }
    }
    return strdup(name);
}

/* The host path of an Xbox object path (\Device\..., \??\X:..., X:\...). */
NTSTATUS fs_host_path(const char *xpath, char *host, size_t hostlen)
{
    int dev;
    NTSTATUS st = translate_path(xpath, host, hostlen, &dev);
    if (NT_SUCCESS(st) && dev == 3) {
        /* A file on an Xbox disc: a host copy of it (the image to launch). */
        dvd_node *n;
        st = dvd_lookup(host + 4, &n);
        if (NT_SUCCESS(st) && !dvd_node_is_dir(n) && !dvd_extract(n, host, hostlen))
            st = STATUS_UNSUCCESSFUL;
    }
    return st;
}

const char *fs_hdd_root(void) { return hdd_dir; }

NTSTATUS fs_translate(const OBJECT_ATTRIBUTES *oa, char *host, size_t hostlen, int *is_device)
{
    char *x = full_xbox_path((OBJECT_ATTRIBUTES *)oa);
    NTSTATUS st = translate_path(x, host, hostlen, is_device);
    free(x);
    return st;
}

void file_close(xfile *f)
{
    if (f->fd >= 0) close(f->fd);
    if (f->dir) closedir(f->dir);
    if (f->delete_on_close && !f->is_dvd) {
        if (f->is_dir) rmdir(f->host); else unlink(f->host);
    }
    free(f->pattern);
    free(f->host);
    free(f->xbox);
    free(f);
}

static NTSTATUS errno_status(int e)
{
    switch (e) {
    case ENOENT: return STATUS_OBJECT_NAME_NOT_FOUND;
    case EEXIST: return STATUS_OBJECT_NAME_COLLISION;
    case EACCES: case EPERM: return STATUS_ACCESS_DENIED;
    case ENOTDIR: return STATUS_NOT_A_DIRECTORY;
    case EISDIR: return STATUS_FILE_IS_A_DIRECTORY;
    case ENOTEMPTY: return STATUS_DIRECTORY_NOT_EMPTY;
    case ENOMEM: return STATUS_NO_MEMORY;
    default: return STATUS_UNSUCCESSFUL;
    }
}

#define FILE_SUPERSEDE      0
#define FILE_OPEN           1
#define FILE_CREATE         2
#define FILE_OPEN_IF        3
#define FILE_OVERWRITE      4
#define FILE_OVERWRITE_IF   5
#define FILE_OPENED         1
#define FILE_CREATED        2
#define FILE_DIRECTORY_FILE     0x00000001
#define FILE_NON_DIRECTORY_FILE 0x00000040
#define FILE_DELETE_ON_CLOSE    0x00001000
#define FILE_SYNCHRONOUS_IO_ALERT    0x00000010
#define FILE_SYNCHRONOUS_IO_NONALERT 0x00000020

#define FILE_ATTRIBUTE_READONLY  0x01
#define FILE_ATTRIBUTE_DIRECTORY 0x10
#define FILE_ATTRIBUTE_NORMAL    0x80

NTSTATUS NTAPI NtCreateFile(HANDLE *FileHandle, ACCESS_MASK DesiredAccess, OBJECT_ATTRIBUTES *oa,
                            IO_STATUS_BLOCK *iosb, LARGE_INTEGER *AllocationSize,
                            ULONG FileAttributes, ULONG ShareAccess, ULONG CreateDisposition,
                            ULONG CreateOptions)
{
    (void)AllocationSize; (void)FileAttributes; (void)ShareAccess;
    char *xpath = full_xbox_path(oa);
    char host[PATH_MAX];
    int is_device;
    NTSTATUS st = translate_path(xpath, host, sizeof(host), &is_device);
    ULONG info = FILE_OPENED;
    xfile *f = NULL;

    if (!NT_SUCCESS(st)) goto out;

    f = calloc(1, sizeof(*f));
    f->fd = -1;
    f->host = strdup(host);
    f->xbox = strdup(xpath);

    if (is_device == 3) {
        f->is_dvd = true;
        bool volume = host[4] == 0;
        st = dvd_lookup(host + 4, &f->dvd);
        if (volume) {
            /* The drive itself takes IOCTLs with or without a disc. */
            st = STATUS_SUCCESS;
            f->is_device = f->is_dir = true;
            goto made;
        }
        if (st == STATUS_OBJECT_NAME_NOT_FOUND && CreateDisposition != FILE_OPEN && CreateDisposition != FILE_OVERWRITE)
            st = STATUS_MEDIA_WRITE_PROTECTED;
        if (!NT_SUCCESS(st)) goto out;
        f->is_dir = dvd_node_is_dir(f->dvd);
        if (f->is_dir && (CreateOptions & FILE_NON_DIRECTORY_FILE)) { st = STATUS_FILE_IS_A_DIRECTORY; goto out; }
        if (!f->is_dir && (CreateOptions & FILE_DIRECTORY_FILE)) { st = STATUS_NOT_A_DIRECTORY; goto out; }
        if (CreateDisposition == FILE_CREATE) { st = STATUS_OBJECT_NAME_COLLISION; goto out; }
        if (CreateDisposition != FILE_OPEN && CreateDisposition != FILE_OPEN_IF) { st = STATUS_MEDIA_WRITE_PROTECTED; goto out; }
        goto made;
    }

    if (is_device == 1) {
        /* The raw disk: XAPI keeps its cache partition database in sector 4,
           so back the first megabyte with a file. */
        char img[PATH_MAX];
        snprintf(img, sizeof(img), "%s/partition0.img", host);
        f->fd = open(img, O_RDWR | O_CREAT, 0644);
        struct stat isb;
        if (f->fd >= 0 && fstat(f->fd, &isb) == 0 && isb.st_size < (1 << 20) && ftruncate(f->fd, 1 << 20) != 0)
            xlog("NtCreateFile: cannot size %s", img);
        f->is_device = true;
        goto made;
    }

    struct stat sb;
    bool exists = stat(host, &sb) == 0;
    bool want_dir = CreateOptions & FILE_DIRECTORY_FILE;

    if (is_device == 2 || (exists && S_ISDIR(sb.st_mode))) {
        if (!exists) { st = STATUS_OBJECT_NAME_NOT_FOUND; goto out; }
        if (is_device != 2 && (CreateOptions & FILE_NON_DIRECTORY_FILE)) { st = STATUS_FILE_IS_A_DIRECTORY; goto out; }
        if (CreateDisposition == FILE_CREATE) { st = STATUS_OBJECT_NAME_COLLISION; goto out; }
        f->is_dir = true;
        f->is_device = is_device == 2;
        goto made;
    }
    if (want_dir) {
        if (exists) { st = STATUS_NOT_A_DIRECTORY; goto out; }
        if (CreateDisposition == FILE_OPEN || CreateDisposition == FILE_OVERWRITE) {
            st = STATUS_OBJECT_NAME_NOT_FOUND;
            goto out;
        }
        if (mkdir(host, 0755) != 0) { st = errno_status(errno); goto out; }
        f->is_dir = true;
        info = FILE_CREATED;
        goto made;
    }

    int flags = O_RDWR;
    if (!(DesiredAccess & (0x40000000 /* GENERIC_WRITE */ | 0x2 /* FILE_WRITE_DATA */ |
                           0x4 /* FILE_APPEND_DATA */ | 0x10000000 /* GENERIC_ALL */ |
                           0x00010000 /* DELETE */)))
        flags = O_RDONLY;
    switch (CreateDisposition) {
    case FILE_SUPERSEDE: flags |= O_CREAT | O_TRUNC; info = exists ? 0 : FILE_CREATED; break;
    case FILE_OPEN: break;
    case FILE_CREATE: flags |= O_CREAT | O_EXCL; info = FILE_CREATED; break;
    case FILE_OPEN_IF: flags |= O_CREAT; info = exists ? FILE_OPENED : FILE_CREATED; break;
    case FILE_OVERWRITE: flags |= O_TRUNC; info = 3; break;
    case FILE_OVERWRITE_IF: flags |= O_CREAT | O_TRUNC; info = exists ? 3 : FILE_CREATED; break;
    }
    if (flags & (O_CREAT | O_TRUNC)) flags = (flags & ~O_RDONLY) | O_RDWR;
    f->fd = open(host, flags, 0644);
    if (f->fd < 0 && errno == EACCES && (flags & O_ACCMODE) == O_RDWR)
        f->fd = open(host, (flags & ~O_ACCMODE) | O_RDONLY);  /* read-only media */
    if (f->fd < 0) { st = errno_status(errno); goto out; }

made:
    if (CreateOptions & FILE_DELETE_ON_CLOSE) f->delete_on_close = true;
    f->async = !(CreateOptions & (FILE_SYNCHRONOUS_IO_ALERT | FILE_SYNCHRONOUS_IO_NONALERT));
    {
        xobject *o = object_new(OBJ_FILE);
        o->file = f;
        *FileHandle = handle_insert(o);
    }
    f = NULL;
out:
    if (f) file_close(f);
    iosb->Status = st;
    iosb->Information = NT_SUCCESS(st) ? info : 0;
    TRACE("NtCreateFile(%s -> %s, disp %u, opt %#x) = %#x", xpath, host, CreateDisposition,
          CreateOptions, st);
    free(xpath);
    return st;
}

NTSTATUS NTAPI NtOpenFile(HANDLE *FileHandle, ACCESS_MASK DesiredAccess, OBJECT_ATTRIBUTES *oa,
                          IO_STATUS_BLOCK *iosb, ULONG ShareAccess, ULONG OpenOptions)
{
    return NtCreateFile(FileHandle, DesiredAccess, oa, iosb, NULL, 0, ShareAccess, FILE_OPEN,
                        OpenOptions);
}

static xfile *file_of(HANDLE h)
{
    xobject *o = handle_lookup(h);
    return o && o->kind == OBJ_FILE ? o->file : NULL;
}

extern LONG NTAPI KeSetEvent(KEVENT *, LONG, BOOLEAN);
extern NTSTATUS NTAPI NtSetEvent(HANDLE, LONG *);

/* I/O finishes at once here.  As in IopCompleteRequest, a request that fails
   without pending still writes the status block but signals nothing; any
   other one sets the event and queues the caller's completion APC, which runs
   at the thread's next alertable wait (ReadFileEx + SleepEx). */
static NTSTATUS complete(HANDLE Event, PVOID ApcRoutine, PVOID ApcContext, IO_STATUS_BLOCK *iosb,
                         NTSTATUS st, ULONG info)
{
    iosb->Status = st;
    iosb->Information = info;
    if (NT_ERROR(st)) return st;
    if (Event) NtSetEvent(Event, NULL);
    if (ApcRoutine) apc_queue(thread_current(), ApcRoutine, ApcContext, iosb, NULL, NULL);
    return st;
}

/* ReadFileEx/WriteFileEx pass this as the APC routine and their
   LPOVERLAPPED_COMPLETION_ROUTINE as its context (io/misc.c). */
void NTAPI NtUserIoApcDispatcher(PVOID ApcContext, IO_STATUS_BLOCK *iosb, ULONG Reserved)
{
    (void)Reserved;
    ULONG err = 0, n = 0;
    if (NT_ERROR(iosb->Status)) err = RtlNtStatusToDosError(iosb->Status);
    else n = (ULONG)iosb->Information;
    /* The status block is OVERLAPPED.Internal, the structure's first field. */
    ((void (NTAPI *)(ULONG, ULONG, PVOID))ApcContext)(err, n, iosb);
}

/* Empty a formatted volume's host directory (but not the directory itself). */
static int remove_entry(const char *path, const struct stat *sb, int type, struct FTW *ftw)
{
    (void)sb; (void)type;
    if (ftw->level > 0) remove(path);
    return 0;
}

/* A partition opened as a volume has no backing file: sector reads return
   zeros and sector writes are dropped, except that rewriting the volume
   header of a cache partition (3 and up) is a format, which empties it. */
static NTSTATUS volume_io(xfile *f, HANDLE Event, PVOID ApcRoutine, PVOID ApcContext, IO_STATUS_BLOCK *iosb, PVOID Buffer,
                          ULONG Length, LARGE_INTEGER *ByteOffset, bool write)
{
    LONGLONG off = ByteOffset ? ByteOffset->QuadPart : f->pos;
    const char *p = strrchr(f->host, '/');
    int part = p && !strncmp(p, "/partition", 10) ? atoi(p + 10) : 0;
    if (!write) {
        memset(Buffer, 0, Length);
    } else if (off == 0 && part >= 3) {
        xlog("formatting cache partition %d (%s)", part, f->host);
        nftw(f->host, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
    }
    f->pos = off + Length;
    return complete(Event, ApcRoutine, ApcContext, iosb, STATUS_SUCCESS, Length);
}

NTSTATUS NTAPI NtReadFile(HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine, PVOID ApcContext,
                          IO_STATUS_BLOCK *iosb, PVOID Buffer, ULONG Length, LARGE_INTEGER *ByteOffset)
{
    (void)ApcContext;
    xfile *f = file_of(FileHandle);
    if (f && f->is_dvd) {
        LONGLONG off = ByteOffset ? ByteOffset->QuadPart : f->pos;
        ssize_t n = f->is_device ? dvd_read_volume(Buffer, Length, off)
                  : f->dvd && !f->is_dir ? dvd_read(f->dvd, Buffer, Length, off) : -2;
        if (n == -2) return complete(Event, ApcRoutine, ApcContext, iosb, STATUS_INVALID_DEVICE_REQUEST, 0);
        if (n < 0) return complete(Event, ApcRoutine, ApcContext, iosb, STATUS_NO_MEDIA_IN_DEVICE, 0);
        f->pos = off + n;
        if (n == 0 && Length > 0) return complete(Event, ApcRoutine, ApcContext, iosb, STATUS_END_OF_FILE, 0);
        complete(Event, ApcRoutine, ApcContext, iosb, STATUS_SUCCESS, (ULONG)n);
        return f->async ? STATUS_PENDING : STATUS_SUCCESS;
    }
    if (f && f->fd < 0 && f->is_device) return volume_io(f, Event, ApcRoutine, ApcContext, iosb, Buffer, Length, ByteOffset, false);
    if (!f || f->fd < 0) return STATUS_INVALID_HANDLE;
    LONGLONG off = ByteOffset ? ByteOffset->QuadPart : f->pos;
    ssize_t n = pread(f->fd, Buffer, Length, off);
    if (n < 0) return complete(Event, ApcRoutine, ApcContext, iosb, errno_status(errno), 0);
    f->pos = off + n;
    if (n == 0 && Length > 0) return complete(Event, ApcRoutine, ApcContext, iosb, STATUS_END_OF_FILE, 0);
    complete(Event, ApcRoutine, ApcContext, iosb, STATUS_SUCCESS, (ULONG)n);
    /* The read is done, but a handle opened for overlapped I/O reports it
       the way the disk driver would, as pending: titles wait on the I/O
       status block, and some only advance after an ERROR_IO_PENDING. */
    return f->async ? STATUS_PENDING : STATUS_SUCCESS;
}

NTSTATUS NTAPI NtWriteFile(HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine, PVOID ApcContext,
                           IO_STATUS_BLOCK *iosb, PVOID Buffer, ULONG Length, LARGE_INTEGER *ByteOffset)
{
    (void)ApcContext;
    xfile *f = file_of(FileHandle);
    if (f && f->is_dvd) return complete(Event, ApcRoutine, ApcContext, iosb, STATUS_MEDIA_WRITE_PROTECTED, 0);
    if (f && f->fd < 0 && f->is_device) return volume_io(f, Event, ApcRoutine, ApcContext, iosb, Buffer, Length, ByteOffset, true);
    if (!f || f->fd < 0) return STATUS_INVALID_HANDLE;
    LONGLONG off = ByteOffset && ByteOffset->QuadPart >= 0 ? ByteOffset->QuadPart : f->pos;
    ssize_t n = pwrite(f->fd, Buffer, Length, off);
    if (n < 0) return complete(Event, ApcRoutine, ApcContext, iosb, errno_status(errno), 0);
    f->pos = off + n;
    complete(Event, ApcRoutine, ApcContext, iosb, STATUS_SUCCESS, (ULONG)n);
    return f->async ? STATUS_PENDING : STATUS_SUCCESS;
}

NTSTATUS NTAPI NtFlushBuffersFile(HANDLE FileHandle, IO_STATUS_BLOCK *iosb)
{
    xfile *f = file_of(FileHandle);
    if (f && f->fd >= 0) fsync(f->fd);
    iosb->Status = STATUS_SUCCESS;
    iosb->Information = 0;
    return STATUS_SUCCESS;
}

static LONGLONG to_nt_time(struct timespec ts)
{
    return 116444736000000000LL + (LONGLONG)ts.tv_sec * 10000000LL + ts.tv_nsec / 100;
}

typedef struct {
    LARGE_INTEGER CreationTime, LastAccessTime, LastWriteTime, ChangeTime;
    ULONG FileAttributes;
} FILE_BASIC_INFORMATION;

typedef struct {
    LARGE_INTEGER AllocationSize, EndOfFile;
    ULONG NumberOfLinks;
    BOOLEAN DeletePending, Directory;
} FILE_STANDARD_INFORMATION;

typedef struct {
    LARGE_INTEGER CreationTime, LastAccessTime, LastWriteTime, ChangeTime;
    LARGE_INTEGER AllocationSize, EndOfFile;
    ULONG FileAttributes;
} FILE_NETWORK_OPEN_INFORMATION;

static ULONG attributes_of(const struct stat *sb)
{
    ULONG a = S_ISDIR(sb->st_mode) ? FILE_ATTRIBUTE_DIRECTORY : 0;
    if (!(sb->st_mode & S_IWUSR)) a |= FILE_ATTRIBUTE_READONLY;
    return a ? a : FILE_ATTRIBUTE_NORMAL;
}

NTSTATUS NTAPI NtQueryInformationFile(HANDLE FileHandle, IO_STATUS_BLOCK *iosb, PVOID Info,
                                      ULONG Length, ULONG Class)
{
    xfile *f = file_of(FileHandle);
    if (!f) return STATUS_INVALID_HANDLE;
    struct stat sb;
    if (f->is_dvd) {
        if (f->dvd) dvd_stat(f->dvd, &sb);
        else { memset(&sb, 0, sizeof(sb)); sb.st_mode = S_IFDIR | 0555; }
    } else if ((f->fd >= 0 ? fstat(f->fd, &sb) : stat(f->host, &sb)) != 0)
        memset(&sb, 0, sizeof(sb));
    ULONG size = 0;
    switch (Class) {
    case 4: {  /* FileBasicInformation */
        FILE_BASIC_INFORMATION *b = Info;
        b->CreationTime.QuadPart = to_nt_time(sb.st_ctim);
        b->LastAccessTime.QuadPart = to_nt_time(sb.st_atim);
        b->LastWriteTime.QuadPart = b->ChangeTime.QuadPart = to_nt_time(sb.st_mtim);
        b->FileAttributes = attributes_of(&sb);
        size = sizeof(*b);
        break;
    }
    case 5: {  /* FileStandardInformation */
        FILE_STANDARD_INFORMATION *s = Info;
        s->AllocationSize.QuadPart = (sb.st_size + 4095) & ~4095LL;
        s->EndOfFile.QuadPart = sb.st_size;
        s->NumberOfLinks = 1;
        s->DeletePending = f->delete_on_close;
        s->Directory = S_ISDIR(sb.st_mode);
        size = sizeof(*s);
        break;
    }
    case 6:    /* FileInternalInformation: a stable file id */
        if (Length < 8) return STATUS_BUFFER_TOO_SMALL;
        ((LARGE_INTEGER *)Info)->QuadPart = (LONGLONG)sb.st_ino;
        size = 8;
        break;
    case 7:    /* FileEaInformation: no extended attributes */
    case 17:   /* FileAlignmentInformation: byte aligned */
        if (Length < 4) return STATUS_BUFFER_TOO_SMALL;
        *(ULONG *)Info = 0;
        size = 4;
        break;
    case 8:    /* FileAccessInformation */
        if (Length < 4) return STATUS_BUFFER_TOO_SMALL;
        *(ULONG *)Info = 0x001F01FF;   /* FILE_ALL_ACCESS */
        size = 4;
        break;
    case 16:   /* FileModeInformation */
        if (Length < 4) return STATUS_BUFFER_TOO_SMALL;
        *(ULONG *)Info = 0;
        size = 4;
        break;
    case 14:   /* FilePositionInformation */
        ((LARGE_INTEGER *)Info)->QuadPart = f->pos;
        size = 8;
        break;
    case 34: { /* FileNetworkOpenInformation */
        FILE_NETWORK_OPEN_INFORMATION *n = Info;
        n->CreationTime.QuadPart = to_nt_time(sb.st_ctim);
        n->LastAccessTime.QuadPart = to_nt_time(sb.st_atim);
        n->LastWriteTime.QuadPart = n->ChangeTime.QuadPart = to_nt_time(sb.st_mtim);
        n->AllocationSize.QuadPart = (sb.st_size + 4095) & ~4095LL;
        n->EndOfFile.QuadPart = sb.st_size;
        n->FileAttributes = attributes_of(&sb);
        size = sizeof(*n);
        break;
    }
    default:
        xlog("NtQueryInformationFile: class %u not implemented", Class);
        return STATUS_INVALID_PARAMETER;
    }
    if (size > Length) return STATUS_BUFFER_TOO_SMALL;
    iosb->Status = STATUS_SUCCESS;
    iosb->Information = size;
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI NtSetInformationFile(HANDLE FileHandle, IO_STATUS_BLOCK *iosb, PVOID Info,
                                    ULONG Length, ULONG Class)
{
    (void)Length;
    xfile *f = file_of(FileHandle);
    if (!f) return STATUS_INVALID_HANDLE;
    NTSTATUS st = STATUS_SUCCESS;
    switch (Class) {
    case 4: break;   /* FileBasicInformation: timestamps/attributes, ignored */
    case 10: {       /* FileRenameInformation */
        struct { BOOLEAN Replace; HANDLE Root; OBJECT_STRING Name; } *r = Info;
        OBJECT_ATTRIBUTES oa = { r->Root, &r->Name, 0 };
        char host[PATH_MAX];
        int dev;
        st = fs_translate(&oa, host, sizeof(host), &dev);
        if (NT_SUCCESS(st) && rename(f->host, host) != 0) st = errno_status(errno);
        if (NT_SUCCESS(st)) { free(f->host); f->host = strdup(host); }
        break;
    }
    case 13:         /* FileDispositionInformation */
        f->delete_on_close = *(BOOLEAN *)Info;
        break;
    case 14:         /* FilePositionInformation */
        f->pos = ((LARGE_INTEGER *)Info)->QuadPart;
        break;
    case 19:         /* FileAllocationInformation */
        break;
    case 20:         /* FileEndOfFileInformation */
        if (f->fd < 0 || ftruncate(f->fd, ((LARGE_INTEGER *)Info)->QuadPart) != 0)
            st = STATUS_INVALID_PARAMETER;
        break;
    default:
        xlog("NtSetInformationFile: class %u not implemented", Class);
        st = STATUS_INVALID_PARAMETER;
    }
    iosb->Status = st;
    iosb->Information = 0;
    return st;
}

NTSTATUS NTAPI NtQueryVolumeInformationFile(HANDLE FileHandle, IO_STATUS_BLOCK *iosb, PVOID Info,
                                            ULONG Length, ULONG Class)
{
    xfile *f = file_of(FileHandle);
    if (!f) return STATUS_INVALID_HANDLE;
    ULONG size = 0;
    struct statvfs vs;
    if (statvfs(f->host, &vs) != 0) memset(&vs, 0, sizeof(vs));
    switch (Class) {
    case 1: {   /* FileFsVolumeInformation */
        struct { LARGE_INTEGER Created; ULONG Serial, LabelLength; BOOLEAN SupportsObjects; } *v = Info;
        memset(v, 0, sizeof(*v));
        v->Serial = 0x58424f58;
        size = sizeof(*v);
        break;
    }
    case 3:     /* FileFsSizeInformation */
    case 7: {   /* FileFsFullSizeInformation */
        LARGE_INTEGER *li = Info;
        ULONG *u;
        /* Report a 16 KB cluster FATX-like volume, capped to what a console has. */
        ULONGLONG total = (ULONGLONG)vs.f_blocks * vs.f_frsize, avail = (ULONGLONG)vs.f_bavail * vs.f_frsize;
        if (total > 8ULL << 30) total = 8ULL << 30;
        if (avail > total) avail = total;
        li[0].QuadPart = total / 16384;
        li[1].QuadPart = avail / 16384;
        if (Class == 7) { li[2].QuadPart = li[1].QuadPart; u = (ULONG *)&li[3]; size = 32; }
        else { u = (ULONG *)&li[2]; size = 24; }
        u[0] = 32;
        u[1] = 512;
        break;
    }
    case 4: {   /* FileFsDeviceInformation */
        ULONG *d = Info;
        const char *cd = dvd_is_media() ? dvd_host_dir() : cdrom_dir;
        d[0] = f->is_dvd || (cd && strstr(f->host, cd) == f->host) ? 0x02 /* CD_ROM */ : 0x07 /* DISK */;
        d[1] = 0;
        size = 8;
        break;
    }
    case 5: {   /* FileFsAttributeInformation */
        struct { ULONG Attrs; LONG MaxComp; ULONG NameLen; char Name[4]; } *a = Info;
        a->Attrs = 0;
        a->MaxComp = 42;
        a->NameLen = 4;
        memcpy(a->Name, "FATX", 4);
        size = sizeof(*a);
        break;
    }
    default:
        xlog("NtQueryVolumeInformationFile: class %u not implemented", Class);
        return STATUS_INVALID_PARAMETER;
    }
    if (size > Length) return STATUS_BUFFER_TOO_SMALL;
    iosb->Status = STATUS_SUCCESS;
    iosb->Information = size;
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI NtQueryDirectoryFile(HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine, PVOID ApcContext,
                                    IO_STATUS_BLOCK *iosb, PVOID Info, ULONG Length, ULONG Class,
                                    OBJECT_STRING *FileMask, BOOLEAN RestartScan)
{
    (void)ApcContext;
    xfile *f = file_of(FileHandle);
    if (!f || !f->is_dir) return STATUS_INVALID_HANDLE;
    if (Class != 1) {
        xlog("NtQueryDirectoryFile: class %u not implemented", Class);
        return STATUS_INVALID_PARAMETER;
    }
    if (!f->pattern || RestartScan) {
        if (f->dir) closedir(f->dir);
        f->dir = f->is_dvd ? NULL : opendir(f->host);
        f->dir_index = 0;
        free(f->pattern);
        f->pattern = FileMask && FileMask->Length ? strndup(FileMask->Buffer, FileMask->Length)
                                                  : strdup("*");
    }
    const char *name = NULL;
    struct stat sb;
    if (f->is_dvd) {
        dvd_node *k;
        while (f->dvd && (k = dvd_child(f->dvd, f->dir_index))) {
            f->dir_index++;
            if (fnmatch(f->pattern, dvd_node_name(k), FNM_CASEFOLD) == 0) {
                name = dvd_node_name(k);
                dvd_stat(k, &sb);
                break;
            }
        }
    } else {
        struct dirent *e = NULL;
        while (f->dir && (e = readdir(f->dir))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            if (fnmatch(f->pattern, e->d_name, FNM_CASEFOLD) == 0) break;
        }
        if (f->dir && e) {
            name = e->d_name;
            char path[PATH_MAX];
            snprintf(path, sizeof(path), "%s/%s", f->host, e->d_name);
            stat(path, &sb);
        }
    }
    if (!name) return complete(Event, ApcRoutine, ApcContext, iosb, STATUS_NO_MORE_FILES, 0);

    size_t namelen = strlen(name);
    struct dirinfo {
        ULONG NextEntryOffset, FileIndex;
        LARGE_INTEGER CreationTime, LastAccessTime, LastWriteTime, ChangeTime, EndOfFile, AllocationSize;
        ULONG FileAttributes, FileNameLength;
        char FileName[];
    } *d = Info;
    if (offsetof(struct dirinfo, FileName) + namelen > Length)
        return STATUS_BUFFER_TOO_SMALL;
    memset(d, 0, offsetof(struct dirinfo, FileName));
    d->CreationTime.QuadPart = to_nt_time(sb.st_ctim);
    d->LastAccessTime.QuadPart = to_nt_time(sb.st_atim);
    d->LastWriteTime.QuadPart = d->ChangeTime.QuadPart = to_nt_time(sb.st_mtim);
    d->EndOfFile.QuadPart = sb.st_size;
    d->AllocationSize.QuadPart = (sb.st_size + 4095) & ~4095LL;
    d->FileAttributes = attributes_of(&sb);
    d->FileNameLength = namelen;
    memcpy(d->FileName, name, namelen);
    return complete(Event, ApcRoutine, ApcContext, iosb, STATUS_SUCCESS,
                    offsetof(struct dirinfo, FileName) + namelen);
}

NTSTATUS NTAPI NtQueryFullAttributesFile(OBJECT_ATTRIBUTES *oa, FILE_NETWORK_OPEN_INFORMATION *n)
{
    char host[PATH_MAX];
    int dev;
    NTSTATUS st = fs_translate(oa, host, sizeof(host), &dev);
    if (!NT_SUCCESS(st)) return st;
    struct stat sb;
    if (dev == 3) {
        dvd_node *dn;
        if (!NT_SUCCESS(st = dvd_lookup(host + 4, &dn))) return st;
        dvd_stat(dn, &sb);
    } else if (stat(host, &sb) != 0) return errno_status(errno);
    n->CreationTime.QuadPart = to_nt_time(sb.st_ctim);
    n->LastAccessTime.QuadPart = to_nt_time(sb.st_atim);
    n->LastWriteTime.QuadPart = n->ChangeTime.QuadPart = to_nt_time(sb.st_mtim);
    n->AllocationSize.QuadPart = (sb.st_size + 4095) & ~4095LL;
    n->EndOfFile.QuadPart = sb.st_size;
    n->FileAttributes = attributes_of(&sb);
    return STATUS_SUCCESS;
}

NTSTATUS NTAPI NtDeleteFile(OBJECT_ATTRIBUTES *oa)
{
    char host[PATH_MAX];
    int dev;
    NTSTATUS st = fs_translate(oa, host, sizeof(host), &dev);
    if (!NT_SUCCESS(st)) return st;
    if (dev == 3) return STATUS_MEDIA_WRITE_PROTECTED;
    if (unlink(host) != 0 && rmdir(host) != 0) return errno_status(errno);
    return STATUS_SUCCESS;
}

/* Device and file system controls: the raw disk and volume IOCTLs XAPI
   issues while mounting and formatting the cache partition. */
#define IOCTL_DISK_GET_DRIVE_GEOMETRY   0x00070000
#define IOCTL_DISK_GET_PARTITION_INFO   0x00074004
#define IOCTL_CDROM_GET_DRIVE_GEOMETRY  0x0002404C
#define FSCTL_DISMOUNT_VOLUME           0x00090020

NTSTATUS NTAPI NtDeviceIoControlFile(HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine, PVOID ApcContext,
                                     IO_STATUS_BLOCK *iosb, ULONG Code, PVOID In, ULONG InLen,
                                     PVOID Out, ULONG OutLen)
{
    (void)ApcContext; (void)In; (void)InLen;
    TRACE("NtDeviceIoControlFile(%p, %#x, out %u)", FileHandle, Code, OutLen);
    switch (Code) {
    case IOCTL_DISK_GET_DRIVE_GEOMETRY:
    case IOCTL_CDROM_GET_DRIVE_GEOMETRY: {
        /* DISK_GEOMETRY: Cylinders(8), MediaType, TracksPerCylinder, SectorsPerTrack, BytesPerSector */
        if (OutLen < 24) return STATUS_BUFFER_TOO_SMALL;
        ULONG *g = Out;
        g[0] = 0x100000; g[1] = 0;
        g[2] = 12; g[3] = 1; g[4] = 1; g[5] = Code == IOCTL_DISK_GET_DRIVE_GEOMETRY ? 512 : 2048;
        return complete(Event, ApcRoutine, ApcContext, iosb, STATUS_SUCCESS, 24);
    }
    case IOCTL_DISK_GET_PARTITION_INFO: {
        /* PARTITION_INFORMATION: StartingOffset, PartitionLength, HiddenSectors,
           PartitionNumber, PartitionType, BootIndicator, RecognizedPartition, RewritePartition */
        if (OutLen < 32) return STATUS_BUFFER_TOO_SMALL;
        memset(Out, 0, 32);
        LARGE_INTEGER *li = Out;
        li[0].QuadPart = 0x80000;
        li[1].QuadPart = 750ULL << 20;
        ((UCHAR *)Out)[24] = 1;  /* PartitionType */
        ((UCHAR *)Out)[26] = 1;  /* RecognizedPartition */
        return complete(Event, ApcRoutine, ApcContext, iosb, STATUS_SUCCESS, 32);
    }
    case 0x24800:     /* IOCTL_CDROM_CHECK_VERIFY: is there a disc in the tray? */
        return complete(Event, ApcRoutine, ApcContext, iosb, dvd_check_verify(), 0);
    case 0x24000:     /* IOCTL_CDROM_READ_TOC: the DVD directory is a data disc, never audio */
    case 0x2403E: {   /* IOCTL_CDROM_RAW_READ (audio sectors) */
        return complete(Event, ApcRoutine, ApcContext, iosb,
                        dvd_tray_empty() ? STATUS_NO_MEDIA_IN_DEVICE : STATUS_INVALID_DEVICE_REQUEST, 0);
    }
    default:
        xlog("NtDeviceIoControlFile: IOCTL %#x not implemented, reporting success", Code);
        return complete(Event, ApcRoutine, ApcContext, iosb, STATUS_SUCCESS, 0);
    }
}

NTSTATUS NTAPI NtFsControlFile(HANDLE FileHandle, HANDLE Event, PVOID ApcRoutine, PVOID ApcContext,
                               IO_STATUS_BLOCK *iosb, ULONG Code, PVOID In, ULONG InLen,
                               PVOID Out, ULONG OutLen)
{
    (void)FileHandle; (void)ApcContext; (void)In; (void)InLen; (void)Out; (void)OutLen;
    TRACE("NtFsControlFile(%p, %#x)", FileHandle, Code);
    return complete(Event, ApcRoutine, ApcContext, iosb, STATUS_SUCCESS, 0);
}

NTSTATUS NTAPI IoDismountVolume(PVOID DeviceObject) { (void)DeviceObject; return STATUS_SUCCESS; }

NTSTATUS NTAPI IoDismountVolumeByName(OBJECT_STRING *Name)
{
    TRACE("IoDismountVolumeByName(%.*s)", Name->Length, Name->Buffer);
    return STATUS_SUCCESS;
}
