/* xbox-console-key: Ctrl + Alt + F1 (to F6) on any keyboard leaves the
   dashboard or game for a login prompt on the screen, the way a PC's console
   switch does. The display belongs to xbcompat while it runs, so a plain VT
   switch changes nothing on screen; this runs `xbox-console`, which stops
   xbcompat and lets xbox-session wait at the console (`xbox-dash` goes back).

   Reads every keyboard's events directly, so it works with a title that has
   hung, and looks for newly plugged keyboards every two seconds. Started by
   xbox-session; built for the target by the xbcompat package. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_KBD 16

static struct { int fd; char name[32]; } kbd[MAX_KBD];
static int nkbd;

static int has_key(int fd, int key)
{
    unsigned long bits[KEY_MAX / (8 * sizeof(long)) + 1] = { 0 };
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof bits), bits) < 0) return 0;
    return bits[key / (8 * sizeof(long))] >> (key % (8 * sizeof(long))) & 1;
}

static void scan(void)
{
    DIR *d = opendir("/dev/input");
    struct dirent *de;
    while (d && (de = readdir(d)) && nkbd < MAX_KBD) {
        if (strncmp(de->d_name, "event", 5)) continue;
        int known = 0;
        for (int i = 0; i < nkbd; i++) known |= !strcmp(kbd[i].name, de->d_name);
        if (known) continue;
        char path[300];
        snprintf(path, sizeof path, "/dev/input/%s", de->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        if (!has_key(fd, KEY_LEFTCTRL) || !has_key(fd, KEY_F1)) { close(fd); continue; }
        kbd[nkbd].fd = fd;
        snprintf(kbd[nkbd].name, sizeof kbd[nkbd].name, "%s", de->d_name);
        nkbd++;
    }
    if (d) closedir(d);
}

int main(int argc, char **argv)
{
    const char *action = argc > 1 ? argv[1] : "/usr/libexec/xbox-console";
    int ctrl = 0, alt = 0;
    for (;;) {
        scan();
        struct pollfd pf[MAX_KBD];
        for (int i = 0; i < nkbd; i++) pf[i] = (struct pollfd){ kbd[i].fd, POLLIN, 0 };
        int r = poll(pf, (nfds_t)nkbd, 2000);
        if (r < 0 && errno != EINTR) return 1;
        for (int i = 0; i < nkbd; i++) {
            if (pf[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {   /* unplugged */
                close(kbd[i].fd);
                kbd[i] = kbd[--nkbd];
                ctrl = alt = 0;
                i--;
                continue;
            }
            if (!(pf[i].revents & POLLIN)) continue;
            struct input_event ev[32];
            ssize_t n = read(kbd[i].fd, ev, sizeof ev);
            for (ssize_t j = 0; j < n / (ssize_t)sizeof ev[0]; j++) {
                if (ev[j].type != EV_KEY) continue;
                int down = ev[j].value != 0, code = ev[j].code;
                if (code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL) ctrl = down;
                else if (code == KEY_LEFTALT || code == KEY_RIGHTALT) alt = down;
                else if (ctrl && alt && ev[j].value == 1 && code >= KEY_F1 && code <= KEY_F6) {
                    if (!fork()) {
                        execl("/bin/sh", "sh", action, (char *)NULL);
                        _exit(127);
                    }
                }
            }
        }
        while (waitpid(-1, NULL, WNOHANG) > 0) {}
    }
}
