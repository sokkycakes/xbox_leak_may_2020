/* Soft reset: back to the dashboard from a running title.

   The Xbox itself has none: titles quit to the dashboard on their own, and
   the in-game reset players know (L + R + Back + Start, the power button
   tapped instead of turning the console off) came from modded BIOSes. Here
   both leave the title the way HalReturnToFirmware(HalRebootRoutine) does
   with no title to launch, so whoever started xbcompat (the Sion's
   xbox-session) brings the dashboard back.

   The power button is any input device that reports KEY_POWER (the ACPI
   "Power Button" on the Sion). With nothing else on the system listening
   to it, a tap did nothing before; holding it down still powers off in the
   firmware. The dashboard itself ignores both, so the console can't be
   reset into itself. XBCOMPAT_RESET=0 turns both off. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "../xbcompat.h"

#define DASHBOARD_TITLE_ID 0xFFFE0000
#define MAX_BUTTONS 8

static volatile int requested;
static bool enabled;

bool reset_enabled(void) { return enabled; }

void reset_request(const char *why)
{
    if (!enabled || requested) return;
    xlog("soft reset (%s): back to the dashboard", why);
    __atomic_store_n(&requested, 1, __ATOMIC_SEQ_CST);
}

static void leave(void)
{
    av_hand_over();
    if (g_log) fflush(g_log);
    fflush(stderr);
}

/* From Present, on the thread that owns the display. */
void reset_check(void)
{
    if (!__atomic_load_n(&requested, __ATOMIC_SEQ_CST)) return;
    leave();
    exit(0);
}

static bool has_key(int fd, int key)
{
    unsigned long bits[KEY_MAX / (8 * sizeof(long)) + 1] = { 0 };
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof bits), bits) < 0) return false;
    return bits[key / (8 * sizeof(long))] >> (key % (8 * sizeof(long))) & 1;
}

static void *power_thread(void *arg)
{
    struct pollfd *pf = arg;
    int n = 0;
    while (pf[n].fd >= 0) n++;
    for (;;) {
        if (poll(pf, (nfds_t)n, -1) < 0) {
            if (errno == EINTR) continue;
            return NULL;
        }
        for (int i = 0; i < n; i++) {
            if (!(pf[i].revents & POLLIN)) continue;
            struct input_event ev[16];
            ssize_t r = read(pf[i].fd, ev, sizeof ev);
            for (ssize_t j = 0; j < r / (ssize_t)sizeof ev[0]; j++)
                if (ev[j].type == EV_KEY && ev[j].code == KEY_POWER && ev[j].value == 1)
                    reset_request("power button");
        }
        if (!__atomic_load_n(&requested, __ATOMIC_SEQ_CST)) continue;
        /* A title that has stopped presenting (stuck loading) never reaches
           reset_check: leave from here after a moment. */
        struct timespec ts = { 3, 0 };
        while (nanosleep(&ts, &ts) && errno == EINTR) {}
        xlog("soft reset: the title isn't presenting; leaving anyway");
        leave();
        _exit(0);
    }
}

void reset_init(ULONG title_id)
{
    const char *e = getenv("XBCOMPAT_RESET");
    if (title_id == DASHBOARD_TITLE_ID || (e && !strcmp(e, "0"))) return;
    enabled = true;

    static struct pollfd pf[MAX_BUTTONS + 1];
    int n = 0;
    DIR *d = opendir("/dev/input");
    struct dirent *de;
    while (d && (de = readdir(d)) && n < MAX_BUTTONS) {
        if (strncmp(de->d_name, "event", 5)) continue;
        char path[300], name[64] = "";
        snprintf(path, sizeof path, "/dev/input/%s", de->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        if (!has_key(fd, KEY_POWER)) { close(fd); continue; }
        ioctl(fd, EVIOCGNAME(sizeof name), name);
        xlog("soft reset: %s (%s) is the power button", path, name);
        pf[n].fd = fd;
        pf[n++].events = POLLIN;
    }
    if (d) closedir(d);
    pf[n].fd = -1;
    if (!n) return;
    pthread_t t;
    if (pthread_create(&t, NULL, power_thread, pf)) {
        xlog("soft reset: no power button thread");
        return;
    }
    pthread_detach(t);
}
