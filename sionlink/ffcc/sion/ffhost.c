// ffhost: plays the GameCube's side of FFCC's GBA link for DSes running the
// client (ffcc/ds), over Sion Link.
//
// It registers with sionlinkd, which sends the client to every DS that
// links. For each player it writes the multiboot header values, runs the
// JoyBus handshake the client expects (see the client's link.c), then reads
// the client's words every frame and logs them. It doesn't run the game:
// this is the link, with the client's real code at the far end.
//
// usage: ffhost [--client ffcc-gba.nds] [--sock PATH]

#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "ffproto.h"
#include "slgame.h"

#define MAX_PLAYERS 15
#define RESEND_MS   60
#define READS       6 // words read from each GBA per frame

// Client link.h
#define LINK_CONTEXT 1
#define LINK_PAD     4

static volatile sig_atomic_t quit;

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000ull + ts.tv_nsec / 1000000);
}

static void logf_(const char *fmt, ...)
{
    time_t t = time(NULL);
    char ts[16];
    strftime(ts, sizeof(ts), "%H:%M:%S", localtime(&t));
    printf("%s ", ts);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
}

// The handshake, as the GameCube runs it (client link.c, Link_Recv/Send):
// read the idle word, write the game code, read the context word, read the
// host ID, write the host ID back, write the player number.
enum step {
    HS_RESET, HS_READ_IDLE, HS_WRITE_GAME, HS_READ_CONTEXT, HS_READ_HOST,
    HS_WRITE_HOST, HS_WRITE_PLAYER, CONNECTED,
};

typedef struct {
    bool here;
    char name[21];
    enum step step;
    uint32_t host_id;
    uint16_t seq;
    bool waiting;           // a batch is out
    uint32_t sent_at;
    uint8_t batch[sizeof(ff_joy) + FF_MAX_OPS * sizeof(ff_op)];
    int batch_len;
    uint32_t boot_at;
    uint16_t keys;
    uint32_t words, pads, batches, resends;
} player;

static player players[MAX_PLAYERS + 1];
static int sock = -1;
static const char *sock_path = SLGAME_SOCKET;
static char client_path[256];

static void link_send(int to, const void *body, int len)
{
    uint8_t m[2 + 1024];
    m[0] = SLGAME_SEND;
    m[1] = to;
    memcpy(m + 2, body, len);
    send(sock, m, 2 + len, MSG_DONTWAIT | MSG_NOSIGNAL);
}

static void send_boot(int id)
{
    player *p = &players[id];
    ff_boot b = { .kind = FF_BOOT, .boot_mode = 1, .player_no = (id - 1) & 3,
                  .idle_word = 0x43434646, .host_id = p->host_id };
    link_send(id, &b, sizeof(b));
    p->boot_at = now_ms();
}

// Builds this frame's batch for a player, by where its handshake is
static void next_batch(int id)
{
    player *p = &players[id];
    ff_joy *j = (ff_joy *)p->batch;
    ff_op *ops = (ff_op *)(p->batch + sizeof(ff_joy));
    int n = 0;
#define OP(c, d) (ops[n].cmd = (c), memset(ops[n].reserved, 0, 3), ops[n].data = (d), n++)
    switch (p->step)
    {
    case HS_RESET:        OP(FF_CMD_RESET, 0); break;
    case HS_READ_IDLE:    OP(FF_CMD_READ, 0); break;
    case HS_WRITE_GAME:   OP(FF_CMD_WRITE, 0x45434347); break; // "GCCE"
    case HS_READ_CONTEXT: OP(FF_CMD_READ, 0); break;
    case HS_READ_HOST:    OP(FF_CMD_READ, 0); break;
    case HS_WRITE_HOST:   OP(FF_CMD_WRITE, p->host_id); break;
    case HS_WRITE_PLAYER: OP(FF_CMD_WRITE, ((id - 1) & 3) << 8); break;
    case CONNECTED:
        for (int i = 0; i < READS; i++)
            OP(FF_CMD_READ, 0);
        break;
    }
#undef OP
    j->kind = FF_JOY;
    j->count = n;
    j->seq = ++p->seq;
    p->batch_len = sizeof(ff_joy) + n * sizeof(ff_op);
}

static void send_batch(int id, bool resend)
{
    player *p = &players[id];
    link_send(id, p->batch, p->batch_len);
    p->waiting = true;
    p->sent_at = now_ms();
    if (resend)
        p->resends++;
    else
        p->batches++;
}

static void word_from_gba(int id, uint32_t w)
{
    player *p = &players[id];
    uint8_t cmd = w & 0x3F;
    p->words++;
    if (cmd == LINK_PAD)
    {
        uint16_t keys = (w >> 8) & 0xFFFF;
        p->pads++;
        if (keys != p->keys)
            logf_("player %d pad %04x", id, keys);
        p->keys = keys;
    }
    else
    {
        logf_("player %d word %08x (command %u)", id, w, cmd);
    }
}

static void reply(int id, const uint8_t *b, int len)
{
    player *p = &players[id];
    ff_joy j;
    memcpy(&j, b, sizeof(j));
    if (!p->waiting || j.seq != p->seq || len < (int)(sizeof(ff_joy) + j.count * sizeof(ff_result)))
        return;
    p->waiting = false;
    const ff_result *r = (const ff_result *)(b + sizeof(ff_joy));
    for (int i = 0; i < j.count; i++)
    {
        ff_result res;
        memcpy(&res, &r[i], sizeof(res));
        switch (p->step)
        {
        case HS_RESET:
            p->step = HS_READ_IDLE;
            break;
        case HS_READ_IDLE:
            logf_("player %d idle word %08x", id, res.data);
            p->step = HS_WRITE_GAME;
            break;
        case HS_WRITE_GAME:
            p->step = HS_READ_CONTEXT;
            break;
        case HS_READ_CONTEXT:
            if ((res.data & 0x3F) != LINK_CONTEXT)
            {
                logf_("player %d: expected the context word, got %08x; starting over", id, res.data);
                p->step = HS_RESET;
                break;
            }
            logf_("player %d context %08x", id, res.data);
            p->step = HS_READ_HOST;
            break;
        case HS_READ_HOST:
            p->step = HS_WRITE_HOST;
            break;
        case HS_WRITE_HOST:
            p->step = HS_WRITE_PLAYER;
            break;
        case HS_WRITE_PLAYER:
            p->step = CONNECTED;
            logf_("player %d (%s): the FFCC client is connected", id, p->name);
            break;
        case CONNECTED:
            if (res.cmd == FF_CMD_READ && (res.stat & 8))
                word_from_gba(id, res.data);
            break;
        }
    }
}

static void join(int id, const char *name)
{
    if (id < 1 || id > MAX_PLAYERS)
        return;
    player *p = &players[id];
    memset(p, 0, sizeof(*p));
    p->here = true;
    snprintf(p->name, sizeof(p->name), "%.20s", name);
    p->host_id = (uint32_t)time(NULL) ^ (id << 24);
    logf_("player %d (%s) joined", id, p->name);
    send_boot(id);
}

static void leave(int id)
{
    if (id >= 1 && id <= MAX_PLAYERS && players[id].here)
    {
        player *p = &players[id];
        logf_("player %d (%s) left: %u batches, %u resent, %u words, %u pad reports", id, p->name,
              p->batches, p->resends, p->words, p->pads);
        p->here = false;
    }
}

static bool link_open(void)
{
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", sock_path);
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0 || connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0)
    {
        if (fd >= 0)
            close(fd);
        return false;
    }
    uint8_t m[5 + sizeof(client_path)];
    uint32_t app = FF_APP_ID;
    m[0] = SLGAME_REGISTER;
    memcpy(m + 1, &app, 4);
    int n = strlen(client_path);
    memcpy(m + 5, client_path, n);
    send(fd, m, 5 + n, MSG_NOSIGNAL);
    sock = fd;
    logf_("registered with sionlinkd; DSes that link get %s", client_path);
    return true;
}

static void link_poll(void)
{
    while (sock >= 0)
    {
        uint8_t m[SLGAME_MAX + 64];
        ssize_t n = recv(sock, m, sizeof(m), MSG_DONTWAIT);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return;
        if (n <= 0)
        {
            logf_("sionlinkd went away");
            close(sock);
            sock = -1;
            for (int id = 1; id <= MAX_PLAYERS; id++)
                leave(id);
            return;
        }
        if (n < 2)
            continue;
        int id = m[1];
        switch (m[0])
        {
        case SLGAME_JOIN:
        {
            char name[21] = { 0 };
            memcpy(name, m + 2, n - 2 < 20 ? n - 2 : 20);
            join(id, name);
            break;
        }
        case SLGAME_LEAVE:
            leave(id);
            break;
        case SLGAME_PACKET:
            if (id >= 1 && id <= MAX_PLAYERS && players[id].here && n >= 2 + (int)sizeof(ff_joy)
                && m[2] == FF_REPLY)
                reply(id, m + 2, n - 2);
            break;
        case SLGAME_NOTE:
            logf_("sionlinkd: %.*s", (int)n - 2, (const char *)m + 2);
            break;
        }
    }
}

static void on_signal(int sig)
{
    quit = 1;
}

int main(int argc, char **argv)
{
    char self[200];
    snprintf(client_path, sizeof(client_path), "ffcc-gba.nds");
    ssize_t sl = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (sl > 0)
    {
        self[sl] = 0;
        char *slash = strrchr(self, '/');
        if (slash)
        {
            *slash = 0;
            snprintf(client_path, sizeof(client_path), "%s/ffcc-gba.nds", self);
        }
    }
    for (int i = 1; i < argc; i++)
    {
        if (!strcmp(argv[i], "--client") && i + 1 < argc)
            snprintf(client_path, sizeof(client_path), "%s", argv[++i]);
        else if (!strcmp(argv[i], "--sock") && i + 1 < argc)
            sock_path = argv[++i];
        else
        {
            fprintf(stderr, "usage: ffhost [--client ffcc-gba.nds] [--sock PATH]\n");
            return 2;
        }
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    uint32_t last_try = 0, last_log = now_ms();
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    while (!quit)
    {
        uint32_t now = now_ms();
        if (sock < 0 && now - last_try >= 1000)
        {
            last_try = now;
            link_open();
        }
        link_poll();
        for (int id = 1; id <= MAX_PLAYERS && sock >= 0; id++)
        {
            player *p = &players[id];
            if (!p->here)
                continue;
            // The client reads the header values once, as it starts
            if (p->step == HS_RESET && now - p->boot_at > 1000)
                send_boot(id);
            if (p->waiting)
            {
                if (now - p->sent_at > RESEND_MS)
                    send_batch(id, true);
                continue;
            }
            next_batch(id);
            send_batch(id, false);
        }
        if (now - last_log >= 5000)
        {
            last_log = now;
            for (int id = 1; id <= MAX_PLAYERS; id++)
                if (players[id].here && players[id].step == CONNECTED)
                    logf_("player %d: %u batches, %u resent, %u words, %u pad reports", id,
                          players[id].batches, players[id].resends, players[id].words,
                          players[id].pads);
        }

        next.tv_nsec += 16666667;
        if (next.tv_nsec >= 1000000000)
        {
            next.tv_nsec -= 1000000000;
            next.tv_sec++;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }
    for (int id = 1; id <= MAX_PLAYERS; id++)
        leave(id);
    return 0;
}
