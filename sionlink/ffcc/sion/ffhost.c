// ffhost: plays the GameCube's side of FFCC's GBA link for DSes running the
// client (ffcc/ds), over Sion Link.
//
// It registers with sionlinkd, which sends the client to every DS that
// links. For each player it writes the multiboot header values and runs the
// JoyBus handshake the client expects (see the client's link.c). After that,
// every frame it reads what the client wrote and writes the words queued for
// it: plain words, messages and bulk downloads, framed as the GameCube does
// (joybus.cpp in the decompilation), each waiting for the client's ACK where
// the GameCube would. What to say is up to demo.c.
//
// usage: ffhost [--client ffcc-gba.nds] [--sock PATH] [--data DIR]

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
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

#include "ffhost.h"
#include "ffproto.h"
#include "slgame.h"

#define RESEND_MS   60
#define READS       8   // words read from each GBA per frame
#define WRITES      24  // words written to each GBA per frame (its queue holds 64)
#define ACK_MS      3000
#define TRIES       5
#define IDLE_WORD   0x43434646u // the multiboot image's idle word, "FFCC"

static volatile sig_atomic_t quit;

uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000ull + ts.tv_nsec / 1000000);
}

void logf_(const char *fmt, ...)
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


static player players[MAX_PLAYERS + 1];
static const char *data_dir = ".";

player *ffhost_player(int id)
{
    return id >= 1 && id <= MAX_PLAYERS && players[id].here && players[id].step == CONNECTED ? &players[id] : NULL;
}
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

// The queue
// =========
//
// Words for a client wait in units. A unit is written whole, then, if it
// asks for one, waits for the client's ACK before the next unit starts: a
// message waits for its ACK, each block of a download for the ACK naming
// the download's type. A NAK, or no answer, sends the unit again.

#define ACK_NONE (-1)
#define ACK_ANY  256

struct unit {
    uint32_t *w;
    int n, pos;
    int ack;        // ACK_NONE, ACK_ANY or the ACK's argument
    bool crc;       // a CHECK_CRC: a NAK answers it too
    int tag;        // demo_done() when finished
    uint32_t done_at;
    int tries;
    unit *next;
};

static unit *unit_new(player *p, int n, int ack)
{
    unit *u = calloc(1, sizeof(*u));
    u->w = calloc(n ? n : 1, sizeof(uint32_t));
    u->n = n;
    u->ack = ack;
    if (p->queue_tail)
        p->queue_tail->next = u;
    else
        p->queue = u;
    p->queue_tail = u;
    return u;
}

static void unit_pop(player *p)
{
    unit *u = p->queue;
    p->queue = u->next;
    if (!p->queue)
        p->queue_tail = NULL;
    int tag = u->tag;
    free(u->w);
    free(u);
    if (tag)
        demo_done(p, tag, true);
}

void q_clear(player *p)
{
    while (p->queue)
    {
        p->queue->tag = 0;
        unit_pop(p);
    }
}

bool q_idle(const player *p)
{
    return !p->queue;
}

static uint16_t crc_table[256];

uint16_t ff_crc16(const uint8_t *data, int len)
{
    if (!crc_table[1])
        for (int i = 0; i < 256; i++)
        {
            uint16_t c = i << 8;
            for (int b = 0; b < 8; b++)
                c = c & 0x8000 ? (c << 1) ^ 0x1021 : c << 1;
            crc_table[i] = c;
        }
    uint16_t crc = 0xFFFF;
    while (--len >= 0)
        crc = (crc << 8) ^ crc_table[(crc >> 8) ^ *data++];
    return ~crc;
}

void q_word(player *p, uint32_t w)
{
    unit *u = p->queue_tail;
    if (!u || u->ack != ACK_NONE || u->tag || u->crc || u->pos)
        u = unit_new(p, 0, ACK_NONE);
    u->w = realloc(u->w, (u->n + 1) * sizeof(uint32_t));
    u->w[u->n++] = w;
}

void q_tag(player *p, int tag)
{
    unit_new(p, 0, ACK_NONE)->tag = tag;
}

void q_wait(player *p, int ack_arg)
{
    unit_new(p, 0, ack_arg);
}

// MakeJoyData: [5][words][crc] [5|0x40][size][byte 0] [5|0x80][3 bytes]...
void q_msg(player *p, const uint8_t *data, int len)
{
    int chunks = (len - 1 + 2) / 3;
    unit *u = unit_new(p, chunks + 2, 0);
    uint16_t crc = ff_crc16(data, len);
    u->w[0] = WORD16(LINK_MESSAGE, chunks + 2, crc);
    u->w[1] = WORD(LINK_MESSAGE | LINK_KIND_SECOND, len, len >> 8, data[0]);
    for (int i = 0; i < chunks; i++)
    {
        uint8_t b[3] = { 0 };
        for (int k = 0; k < 3; k++)
            if (1 + i * 3 + k < len)
                b[k] = data[1 + i * 3 + k];
        u->w[2 + i] = WORD(LINK_MESSAGE | LINK_KIND_DATA, b[0], b[1], b[2]);
    }
}

// SendDataFile: [11][type][blocks] [11][size] [11][crc], then per block of
// up to 0x2FD bytes [11|0x40][rows][block size] [11|0x40][index][block crc]
// and its rows of three bytes, [11|0x80][3 bytes]; the client ACKs each block.
#define BLOCK 0x2FD

void q_bulk(player *p, int type, const uint8_t *data, int len)
{
    int blocks = (len + BLOCK - 1) / BLOCK;
    unit *u = unit_new(p, 3, ACK_NONE);
    u->w[0] = WORD16(LINK_DATA, type, blocks);
    u->w[1] = WORD16(LINK_DATA, 0, len);
    u->w[2] = WORD16(LINK_DATA, 0, ff_crc16(data, len));
    for (int b = 0; b < blocks; b++)
    {
        const uint8_t *d = data + b * BLOCK;
        int size = b < blocks - 1 ? BLOCK : len - b * BLOCK;
        int rows = (size + 2) / 3;
        u = unit_new(p, 2 + rows, type);
        u->w[0] = WORD16(LINK_DATA | LINK_KIND_SECOND, rows, size);
        u->w[1] = WORD16(LINK_DATA | LINK_KIND_SECOND, b, ff_crc16(d, size));
        for (int r = 0; r < rows; r++)
        {
            uint8_t x[3] = { 0 };
            for (int k = 0; k < 3; k++)
                if (r * 3 + k < size)
                    x[k] = d[r * 3 + k];
            u->w[2 + r] = WORD(LINK_DATA | LINK_KIND_DATA, x[0], x[1], x[2]);
        }
    }
}

// The client answers ACK 3 if it already holds data with this CRC
void q_crc(player *p, int type, const uint8_t *data, int len, int tag)
{
    unit *u = unit_new(p, 1, 3);
    u->w[0] = WORD16(LINK_CHECK_CRC, type, ff_crc16(data, len));
    u->crc = true;
    u->tag = tag;
}

// The words of this frame's batch, from the queue
static int queue_writes(player *p, ff_op *ops, int n, uint32_t now)
{
    int writes = 0;
    while (p->queue && writes < WRITES)
    {
        unit *u = p->queue;
        if (u->pos < u->n)
        {
            ops[n].cmd = FF_CMD_WRITE;
            memset(ops[n].reserved, 0, 3);
            ops[n].data = u->w[u->pos++];
            n++;
            writes++;
            if (u->pos == u->n)
                u->done_at = now;
            continue;
        }
        if (u->ack == ACK_NONE)
        {
            unit_pop(p);
            continue;
        }
        if (now - u->done_at > ACK_MS)
        {
            if (++u->tries >= TRIES)
            {
                logf_("player %d: no answer to a unit starting %08x; dropping it", p->id, u->w[0]);
                unit_pop(p);
                continue;
            }
            logf_("player %d: no answer to a unit starting %08x; sending it again", p->id, u->w[0]);
            u->pos = 0;
            continue;
        }
        break; // waiting for the ACK
    }
    p->written += writes;
    return writes;
}

// An ACK or NAK from the client: for the unit waiting, if it waits
static bool queue_answer(player *p, uint32_t w)
{
    unit *u = p->queue;
    if (!u || u->ack == ACK_NONE || u->pos < u->n)
        return false;
    bool ack = (w & 0x3F) == LINK_ACK;
    int arg = (w >> 8) & 0xFF;
    if (u->crc && arg == 3)
    {
        int tag = u->tag;
        u->tag = 0;
        unit_pop(p);
        demo_done(p, tag, ack);
        return true;
    }
    if (ack && (u->ack == ACK_ANY || arg == u->ack))
    {
        unit_pop(p);
        return true;
    }
    if (!ack)
    {
        logf_("player %d: NAK %08x for a unit starting %08x; sending it again", p->id, w, u->w[0]);
        if (++u->tries >= TRIES)
            unit_pop(p);
        else
            u->pos = 0;
        return true;
    }
    return false;
}

static void leave(int id);

static void send_boot(int id)
{
    player *p = &players[id];
    ff_boot b = { .kind = FF_BOOT, .boot_mode = 1, .player_no = (id - 1) & 3,
                  .idle_word = IDLE_WORD, .host_id = p->host_id };
    link_send(id, &b, sizeof(b));
    p->boot_at = now_ms();
}

// Builds this frame's batch for a player, by where its handshake is
static void next_batch(int id)
{
    player *p = &players[id];
    ff_joy *j = (ff_joy *)p->batch;
    ff_op *ops = (ff_op *)(p->batch + sizeof(ff_joy));
    _Static_assert(sizeof(ff_joy) + (READS + WRITES) * sizeof(ff_op) <= sizeof(p->batch), "batch");
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
        n += queue_writes(p, ops, n, now_ms());
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
        p->pads++;
        p->keys = (w >> 8) & 0xFFFF;
    }
    if ((cmd == LINK_ACK || cmd == LINK_NAK) && queue_answer(p, w))
        return;
    if (cmd == LINK_ACK || cmd == LINK_NAK)
        demo_ack(p, w);
    else
        demo_word(p, w);
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
            demo_start(p);
            break;
        case CONNECTED:
            if (res.cmd == FF_CMD_READ && (res.stat & 8) && res.data == IDLE_WORD)
            {
                // The client reset its link and shows its idle word again
                logf_("player %d: the client reset the link; connecting again", id);
                demo_end(p);
                q_clear(p);
                p->step = HS_RESET;
                return;
            }
            if (res.cmd == FF_CMD_READ && (res.stat & 8))
                word_from_gba(id, res.data);
            if (p->step != CONNECTED)
                return; // the game reset the link
            break;
        }
    }
}

static void join(int id, const char *name)
{
    if (id < 1 || id > MAX_PLAYERS)
        return;
    player *p = &players[id];
    if (p->here)
        leave(id);
    q_clear(p);
    memset(p, 0, sizeof(*p));
    p->id = id;
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
        logf_("player %d (%s) left: %u batches, %u resent, %u words in, %u out", id, p->name,
              p->batches, p->resends, p->words, p->written);
        if (p->step == CONNECTED)
            demo_end(p);
        q_clear(p);
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
    setvbuf(stdout, NULL, _IOLBF, 0);   // the log goes through tee on the Sion
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
        else if (!strcmp(argv[i], "--data") && i + 1 < argc)
            data_dir = argv[++i];
        else
        {
            fprintf(stderr, "usage: ffhost [--client ffcc-gba.nds] [--sock PATH] [--data DIR]\n");
            return 2;
        }
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);
    if (!demo_init(data_dir))
        return 1;
    int stdin_flags = fcntl(0, F_GETFL);
    fcntl(0, F_SETFL, stdin_flags | O_NONBLOCK);
    char line[256];
    int line_len = 0;

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
        char c;
        while (read(0, &c, 1) == 1)
        {
            if (c == '\n')
            {
                line[line_len] = 0;
                demo_command(line);
                line_len = 0;
            }
            else if (line_len < (int)sizeof(line) - 1)
                line[line_len++] = c;
        }
        for (int id = 1; id <= MAX_PLAYERS && sock >= 0; id++)
        {
            player *p = &players[id];
            if (!p->here)
                continue;
            // The client reads the header values once, as it starts
            if (p->step == HS_RESET && now - p->boot_at > 1000)
                send_boot(id);
            if (p->step == CONNECTED)
                demo_frame(p, now);
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
                    logf_("player %d: %u batches, %u resent, %u words in, %u out", id,
                          players[id].batches, players[id].resends, players[id].words,
                          players[id].written);
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
    fcntl(0, F_SETFL, stdin_flags);
    return 0;
}
