/* The AV state the Xbox keeps across XLaunchNewImage's quick reboot.

   On the Xbox the picture on the TV survives a title switch: the video
   mode stays programmed, and D3DDevice_PersistDisplay leaves a copy of the
   last frame on screen (AvSetSavedDataAddress) until the next title's first
   Present; without it the screen is black. Here each title is its own
   process, so xbox-av (tools/sion/av), started with the Xbox session and
   named by XBOX_AV_SOCKET, holds the display between them: a title tells
   it when it starts (before SDL takes the display) and gives it the
   persisted frame, or black, when it leaves. With no xbox-av (a desktop),
   none of this happens. */
#include <dirent.h>
#include <drm/drm.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "../xbcompat.h"

static int av_fd = -1;
static uint8_t *persisted;
static uint32_t persisted_w, persisted_h;

static int av_command(const void *msg, size_t n)
{
    const uint8_t *p = msg;
    while (n) {
        ssize_t r = write(av_fd, p, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        p += r;
        n -= r;
    }
    char k;
    return read(av_fd, &k, 1) == 1 && k == 'k' ? 0 : -1;
}

void av_title_starting(void)
{
    const char *path = getenv("XBOX_AV_SOCKET");
    if (!path || !*path || av_fd >= 0) return;
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
    av_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (av_fd < 0 || connect(av_fd, (struct sockaddr *)&sa, sizeof sa)) {
        xlog("AV: no display keeper at %s (%s)", path, strerror(errno));
        if (av_fd >= 0) close(av_fd);
        av_fd = -1;
        return;
    }
    struct timeval tv = { 5, 0 };
    setsockopt(av_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (av_command("T", 1)) {
        xlog("AV: the display keeper didn't answer");
        close(av_fd);
        av_fd = -1;
        return;
    }
    xlog("AV: the last title's picture stays up until the first Present");
}

/* D3DDevice_PersistDisplay: w x h BGRX pixels, top row first. */
void av_persist(const unsigned char *px, unsigned w, unsigned h)
{
    free(persisted);
    persisted = malloc((size_t)w * h * 4);
    if (!persisted) { persisted_w = persisted_h = 0; return; }
    memcpy(persisted, px, (size_t)w * h * 4);
    persisted_w = w;
    persisted_h = h;
}

/* The title is leaving (a quick reboot, a return to the dashboard): let go
   of DRM master and leave the persisted frame, or black, on screen. */
void av_hand_over(void)
{
    if (av_fd < 0) return;
    /* SDL's KMSDRM descriptor: whichever /dev/dri/card* is master. */
    DIR *d = opendir("/proc/self/fd");
    struct dirent *e;
    while (d && (e = readdir(d))) {
        char link[64], target[64];
        snprintf(link, sizeof link, "/proc/self/fd/%s", e->d_name);
        ssize_t n = readlink(link, target, sizeof target - 1);
        if (n <= 0) continue;
        target[n] = 0;
        if (!strncmp(target, "/dev/dri/card", 13)) ioctl(atoi(e->d_name), DRM_IOCTL_DROP_MASTER, 0);
    }
    if (d) closedir(d);
    uint32_t hdr[2] = { persisted ? persisted_w : 0, persisted ? persisted_h : 0 };
    char cmd = 'S';
    size_t n = 1 + sizeof hdr + (size_t)hdr[0] * hdr[1] * 4;
    uint8_t *msg = malloc(n);
    if (!msg) return;
    msg[0] = cmd;
    memcpy(msg + 1, hdr, sizeof hdr);
    if (hdr[0]) memcpy(msg + 1 + sizeof hdr, persisted, (size_t)hdr[0] * hdr[1] * 4);
    if (av_command(msg, n)) xlog("AV: the display keeper didn't take the display");
    else xlog(hdr[0] ? "AV: left a %ux%u frame on screen" : "AV: left the screen black", hdr[0], hdr[1]);
    free(msg);
    close(av_fd);
    av_fd = -1;
}
