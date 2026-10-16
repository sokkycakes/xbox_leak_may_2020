// demo.c: a stand-in for FFCC's GameCube, so every screen of the GBA client
// can be tried on a DS without the game.
//
// Each DS gets a caravan member with a full bag, a map from the disc
// (m*.mcd, sent as the GameCube sends it) with party members, monsters and
// treasure moving on the radar, letters from home, a shop, a blacksmith and
// character creation. The client is the real one, so what it shows and
// sends is exactly what the game would see; this file answers the way the
// game does (joybus.cpp and gbaque.cpp in the decompilation), with made-up
// numbers where the game's item tables would supply them.
//
// Buttons, as on the real game: SELECT switches the DS between steering the
// menus and being a controller. As a controller:
//   D-pad   walk the caravan around the map
//   A       next map            B       take a hit (the screen shakes)
//   L / R   radar role (map, monsters, scouter, treasure)
//   START   next scene: field, character creation, shop, blacksmith,
//           "look at the TV", then back to the field (finishing or leaving a
//           scene on the DS also moves on)
// Lines typed at ffhost do the same from the Sion; help lists them.

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>

#include "ffhost.h"

// GBA keys in the client's pad words
#define KEY_A      0x001
#define KEY_B      0x002
#define KEY_SELECT 0x004
#define KEY_START  0x008
#define KEY_RIGHT  0x010
#define KEY_LEFT   0x020
#define KEY_UP     0x040
#define KEY_DOWN   0x080
#define KEY_R      0x100
#define KEY_L      0x200

enum { MODE_FIELD, MODE_CMAKE, MODE_SHOP, MODE_SMITH, MODE_TV };
static const char *const mode_names[] = { "field", "character creation", "shop", "blacksmith", "TV" };

// LINK_EVENT sub-commands
#define EVT_TAKE_ATTACHMENT 0
#define EVT_ADD_LETTER      1
#define EVT_CMAKE_LOOK      2
#define EVT_CMAKE_JOB       3
#define EVT_CMAKE_CANCEL    4
#define EVT_CMAKE_END       5
#define EVT_SHOP_LEAVE      7
#define EVT_SHOP_SELL       8
#define EVT_SHOP_BUY        9
#define EVT_SMITH_FORGE     10
#define EVT_SMITH_LEAVE     11
#define EVT_RADAR_TYPE      13
#define EVT_RADAR_MODE      14
#define EVT_OPEN_MENU       15
#define EVT_ITEM_USE_FLAGS  16
#define EVT_SP_MODE         17
#define EVT_GIL_RESEND      21
#define EVT_LANGUAGE        22

// Messages
#define MSG_PLAYER_STAT   1
#define MSG_ITEM_ALL      2
#define MSG_MAP_OBJ       3
#define MSG_FAVORITE      4
#define MSG_COMPATIBILITY 5
#define MSG_EQUIP_LIST    6
#define MSG_BONUS_STR     7
#define MSG_TMP_ARTIFACTS 9
#define MSG_MARKER_KINDS  10
#define MSG_SCOUTER_INFO  11
#define MSG_CMD_LIST      12

// Downloads
#define XFER_OBJ         0
#define XFER_MAP         1
#define XFER_LETTER      2
#define XFER_LETTER_LIST 3
#define XFER_SELL_LIST   6
#define XFER_BUY_LIST    7
#define XFER_SMITH_LIST  8
#define XFER_ARTIFACTS   9

// demo_done tags
enum { T_OBJ = 1, T_MAP, T_READY };

#define EMPTY (-1)

// Disc data
// =========

typedef struct {
    int stage, map;
    uint8_t *data;
    int len;
    int x, y, w, h; // map units covered (MapInfo, header size)
    uint8_t *floor; // a byte per 8x8 tile, set where the radar draws ground
    int tw, th;
} mapfile;

static uint8_t *objdat;
static int objdat_len;
static mapfile maps[128];
static int nmaps;

static uint8_t *load(const char *path, int *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    *len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(*len ? *len : 1);
    if (fread(d, 1, *len, f) != (size_t)*len)
    {
        free(d);
        d = NULL;
    }
    fclose(f);
    return d;
}

static int rd16(const uint8_t *p) { return (int16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static int map_cmp(const void *a, const void *b)
{
    const mapfile *x = a, *y = b;
    return x->stage != y->stage ? x->stage - y->stage : x->map - y->map;
}

// The radar map's rows (radarmap.c): a length byte per row after MapInfo,
// then u16 entries per row, either a tile or 0x8000 | value << 10 | count
// for a run of the plain tile 0x1000 | value. 0x1000 is empty space.
static void load_floor(mapfile *m, uint32_t info)
{
    m->tw = m->w >> 3;
    m->th = m->h >> 3;
    if (m->tw <= 0 || m->th <= 0 || info + 8 + m->th > (uint32_t)m->len)
        return;
    m->floor = calloc(m->tw * m->th, 1);
    const uint8_t *rows = m->data + info + 8;
    uint32_t at = info + rd32(m->data + info + 4);
    for (int y = 0; y < m->th; y++)
    {
        int x = 0;
        for (int i = 0; i < rows[y] && at + 2 <= (uint32_t)m->len; i++, at += 2)
        {
            int e = m->data[at] | m->data[at + 1] << 8;
            int n = e & 0x8000 ? e & 0x3FF : 1;
            bool ground = e & 0x8000 && (e >> 10 & 3) != 0;
            for (; n > 0 && x < m->tw; n--, x++)
                m->floor[y * m->tw + x] = ground;
        }
    }
}

static bool on_floor(const mapfile *m, int x, int y)
{
    if (!m || !m->floor)
        return true;
    int tx = (x - m->x) >> 3, ty = (y - m->y) >> 3;
    return tx >= 0 && ty >= 0 && tx < m->tw && ty < m->th && m->floor[ty * m->tw + tx];
}

// The ground tile nearest (x, y), searched in growing squares
static void nearest_floor(const mapfile *m, int *x, int *y)
{
    for (int r = 0; r < 64; r++)
        for (int dy = -r; dy <= r; dy++)
            for (int dx = -r; dx <= r; dx++)
                if ((abs(dx) == r || abs(dy) == r) && on_floor(m, *x + dx * 8, *y + dy * 8))
                {
                    *x += dx * 8;
                    *y += dy * 8;
                    return;
                }
}

bool demo_init(const char *dir)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/objdat.spt", dir);
    objdat = load(path, &objdat_len);
    if (!objdat)
    {
        logf_("%s is missing: ffhost needs the disc's gba folder (objdat.spt and the m*.mcd maps); "
              "give it with --data", path);
        return false;
    }
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d)) && nmaps < (int)(sizeof(maps) / sizeof(maps[0])))
    {
        mapfile m = { 0 };
        if (sscanf(e->d_name, "m%d_%d.mcd", &m.stage, &m.map) != 2)
            continue;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        m.data = load(path, &m.len);
        if (!m.data || m.len < 0x30 || memcmp(m.data, "MCD ", 4))
            continue;
        uint32_t info = rd32(m.data + 12);
        if (info + 8 > (uint32_t)m.len)
            continue;
        m.x = rd16(m.data + info);
        m.y = rd16(m.data + info + 2);
        m.w = m.data[20] | m.data[21] << 8;
        m.h = m.data[22] | m.data[23] << 8;
        load_floor(&m, info);
        maps[nmaps++] = m;
    }
    if (d)
        closedir(d);
    qsort(maps, nmaps, sizeof(maps[0]), map_cmp);
    logf_("demo: objdat.spt (%d bytes) and %d radar maps from %s", objdat_len, nmaps, dir);
    return true;
}

// Items
// =====
//
// Names and icons are the client's own; what the game's item table would
// add (who can equip it, its strength, prices) is invented here.

typedef struct __attribute__((packed)) {
    uint16_t flags, count, kind, pad;
} iteminfo;

static iteminfo item_info(int id)
{
    iteminfo i = { 0 };
    if (id >= 1 && id <= 67)
    {
        int tribe = id < 18 ? 0 : id < 36 ? 1 : id < 52 ? 2 : 3;
        i.flags = 0x100 | 1 << tribe;
        i.count = 3 + id % 7;
    }
    else if (id >= 68 && id <= 87)
        i = (iteminfo){ 0x40F, 1 + id % 5, 0, 0 };
    else if (id >= 88 && id <= 106)
        i = (iteminfo){ 0x20F, 1 + id % 3, 0, 0 };
    else if (id >= 107 && id <= 126)
        i = (iteminfo){ 0x80F, 1 + id % 3, 0, 0 };
    else if (id >= 127 && id <= 158)
        i = (iteminfo){ 0x100F, 1, 1 + id % 19, 0 };
    return i;
}

static int item_price(int id)
{
    if (id <= 158)
        return 200 + id * 10;
    if (id == 293)
        return 500;
    if (id <= 300)
        return 80;
    if (id <= 380)
        return 40;
    if (id <= 392)
        return 10;
    return 300;
}

// Recipes the blacksmith knows: design -> materials and results per tribe
typedef struct {
    int design, price;
    int mat[3], cnt[3];
    int result[4];
} recipe;

static const recipe recipes[] = {
    { 401, 100, { 294, 0, 0 }, { 1, 0, 0 }, { 2, 18, 36, 52 } },          // Novice's Wpn.
    { 412, 150, { 294, 301, 0 }, { 1, 1, 0 }, { 70, 70, 70, 70 } },      // Bronze Armor
    { 426, 300, { 295, 302, 0 }, { 1, 1, 0 }, { 89, 89, 89, 89 } },      // Iron Shield
    { 452, 120, { 294, 0, 0 }, { 2, 0, 0 }, { 118, 118, 118, 118 } },    // Bronze Belt
};

static const recipe *find_recipe(int design)
{
    for (size_t i = 0; i < sizeof(recipes) / sizeof(recipes[0]); i++)
        if (recipes[i].design == design)
            return &recipes[i];
    return NULL;
}

// What the shop sells
static const struct { int id; const char *desc; } goods[] = {
    { 293, "Brings a fallen\nmember back." },
    { 381, "A sweet, striped\napple." },
    { 387, "Hearty meat for\nthe road." },
    { 294, "Metal for\nforging." },
    { 301, "A shard of\nbronze." },
    { 412, "Plans for\nbronze armor." },
};

// Letters from home
typedef struct {
    const char *subject, *sender, *body, *answers;
    int attach;   // item id, or gil with gil set
    bool gil;
    uint8_t flags;
} letter;

#define L_OPENED   0x01
#define L_TAKEN    0x02
#define L_REPLIED  0x04
#define L_ITEM     0x08
#define L_GIL      0x10
#define L_ANSWERS  0x20

static const letter letters_init[] = {
    { "Safe travels", "Mother",
      "Dear Sol,\nThe village misses\nyou already. I packed\nsome apples for the\nroad.\n\x1E" "Come home safe!",
      "I will, Mom.\nThe apples are great.", 381, false, L_ITEM | L_ANSWERS },
    { "Your wages", "Father",
      "Sol,\nHere is a little\nsomething from the\nsmithy. Spend it on\ngood armor, not on\nsweets!",
      "", 500, true, L_GIL },
    { "Hello!", "Stiltzkin",
      "Kupo! I saw your\ncaravan pass by the\nriver. The myrrh\ntree is close.\n\x1E" "Good luck, kupo!",
      "Thanks, Stiltzkin!\nSee you around.", 0, false, L_ANSWERS },
};
#define NLETTERS (int)(sizeof(letters_init) / sizeof(letters_init[0]))

// A player's game
// ===============

typedef struct { int x, y, dx, dy, kind; } mover;

struct demo {
    int no;                  // the player's number in the caravan (0-3)
    int mode;
    bool ready;              // the field is set up
    bool ctrl;               // the DS steers its menus (LINK_CTRL_MODE 1)
    int screen;              // as the client last reported it
    int radar_type;
    int trail[64][2];   // where the player walked, for the others to follow
    int trail_at;
    int map;                 // index into maps[]
    int x, y;                // the player's map position
    int hp, max_hp;
    bool miasma;             // outside the crystal's protection
    uint16_t keys;
    uint32_t frame;

    int16_t items[64];
    uint32_t artifacts[3];
    int16_t tmp_artifacts[4];
    uint8_t equip[4];
    uint16_t cmd_slots[8];
    int cmd_count;
    uint32_t gil;
    uint8_t stats[3];

    mover party[4], enemies[8], treasures[4];
    uint8_t letter_flags[NLETTERS];
    int nletters;           // letters received so far (the letter command brings the next)
    bool letter_pending;

    // words the client sends in parts
    uint32_t gil_word, name_words[6], reply_words[3];
    int name_n, reply_n;
    char cmake_name[17];
};

static demo games[MAX_PLAYERS + 1];

static int free_slot(demo *g)
{
    for (int i = 0; i < 64; i++)
        if (g->items[i] == EMPTY)
            return i;
    return -1;
}

static void set_item(player *p, int slot, int id)
{
    p->game->items[slot] = id;
    q_word(p, WORD16(LINK_ITEM, slot, (uint16_t)id));
}

static int add_item(player *p, int id)
{
    int slot = free_slot(p->game);
    if (slot >= 0)
        set_item(p, slot, id);
    return slot;
}

static void send_gil(player *p)
{
    uint32_t g = p->game->gil;
    q_word(p, WORD(LINK_GIL, 0, g >> 24, g >> 16));
    q_word(p, WORD(LINK_GIL | LINK_KIND_SECOND, g >> 8, g, 0));
}

static void reply_event(player *p, bool ok, int sub)
{
    q_word(p, WORD(ok ? LINK_ACK : LINK_NAK, LINK_EVENT, sub, 0));
}

static void event(player *p, int sub, int a, int b)
{
    q_word(p, WORD(LINK_EVENT, sub, a, b));
}

static void new_game(player *p)
{
    demo *g = p->game;
    memset(g, 0, sizeof(*g));
    g->no = (p->id - 1) & 3;
    g->max_hp = 4;
    g->hp = 4;
    g->radar_type = 0;
    g->gil = 1234;
    g->stats[0] = 5, g->stats[1] = 3, g->stats[2] = 2;
    g->screen = -1;
    for (int i = 0; i < 64; i++)
        g->items[i] = EMPTY;
    static const int16_t bag[] = { 1, 69, 88, 127, 256, 257, 261, 293, 381, 387, 294, 294, 294,
                                   301, 295, 302, 299, 397, 401, 412, 452, 18, 70 };
    for (size_t i = 0; i < sizeof(bag) / sizeof(bag[0]); i++)
        g->items[i] = bag[i];
    g->artifacts[0] = 1 << 0 | 1 << 16;  // Shuriken, Onion Sword
    g->artifacts[2] = 1 << 0;            // Fire Ring (223)
    g->tmp_artifacts[0] = 203;           // Ribbon
    g->tmp_artifacts[1] = g->tmp_artifacts[2] = g->tmp_artifacts[3] = EMPTY;
    g->equip[0] = 0, g->equip[1] = 1, g->equip[2] = 2, g->equip[3] = 3;
    for (int i = 0; i < 8; i++)
        g->cmd_slots[i] = 0xFFFF;
    g->cmd_slots[2] = 4;    // Fire
    g->cmd_slots[3] = 6;    // Cure
    g->cmd_slots[4] = 223 - 95;
    g->cmd_count = 6;
    for (int i = 0; i < NLETTERS; i++)
        g->letter_flags[i] = letters_init[i].flags;
    g->nletters = NLETTERS - 1;
}

// The map and what moves on it
static void place_movers(demo *g)
{
    const mapfile *m = &maps[g->map];
    g->x = m->x + m->w / 2;
    g->y = m->y + m->h / 2;
    nearest_floor(m, &g->x, &g->y);
    for (int i = 0; i < 4; i++)
        g->party[i] = (mover){ g->x, g->y, 0, 0, 0 };
    for (int i = 0; i < 64; i++)
        g->trail[i][0] = g->x, g->trail[i][1] = g->y;
    for (int i = 0; i < 8; i++)
    {
        mover *e = &g->enemies[i];
        *e = (mover){ g->x - 60 + (i * 37) % 120, g->y - 50 + (i * 53) % 100,
                      i % 2 ? 1 : -1, i % 3 ? 1 : -1, 1 + i % 3 };
        nearest_floor(m, &e->x, &e->y);
    }
    for (int i = 0; i < 4; i++)
    {
        mover *t = &g->treasures[i];
        *t = (mover){ g->x - 40 + i * 27, g->y - 30 + (i % 2) * 55, 0, 0, 4 + i % 2 };
        nearest_floor(m, &t->x, &t->y);
    }
}

// Messages
// ========

static int put_compat(uint8_t *b)
{
    int n = 0;
    b[n++] = 0;   // the family runs the smithy
    b[n++] = 3;
    b[n++] = 1, b[n++] = 80;   // father
    b[n++] = 2, b[n++] = 60;   // mother
    b[n++] = 4, b[n++] = 35;   // sister
    for (const char *s = "Gareth\0Elena\0Rina\0"; *s; s += strlen(s) + 1)
    {
        strcpy((char *)b + n, s);
        n += strlen(s) + 1;
    }
    return n;
}

static const uint8_t favorites[8] = { 50, 20, 10, 70, 30, 0, 60, 40 };

static void msg_player_stat(player *p)
{
    demo *g = p->game;
    static const char *const caravan[4] = { "Sol", "Ria", "Kip", "Mira" };
    uint8_t b[300] = { MSG_PLAYER_STAT };
    int n = 1;
    for (int i = 0; i < 8; i++, n += 16)
        if (i < 4)
            strcpy((char *)b + n, caravan[i]);
    // save slot i belongs to party member i
    b[n++] = 0x01, b[n++] = 0x23, b[n++] = 0xFF, b[n++] = 0xFF;
    for (int i = 0; i < 4; i++)
    {
        b[n++] = g->max_hp;
        b[n++] = i == g->no ? g->hp : g->max_hp;
    }
    b[n++] = 0;   // Clavat, male
    memcpy(b + n, g->stats, 3);
    n += 3;
    b[n++] = 12, b[n++] = 0;   // memories
    n += put_compat(b + n);
    memcpy(b + n, favorites, 8);
    n += 8;
    for (int i = 0; i < 4; i++)
        b[n++] = g->gil >> (8 * i);
    q_msg(p, b, n);
}

static void msg_item_all(player *p)
{
    demo *g = p->game;
    uint8_t b[200] = { MSG_ITEM_ALL };
    int n = 1;
    for (int i = 0; i < 64; i++, n += 2)
        b[n] = g->items[i], b[n + 1] = g->items[i] >> 8;
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 4; k++)
            b[n++] = g->artifacts[i] >> (8 * k);
    for (int i = 0; i < 4; i++, n += 2)
        b[n] = g->tmp_artifacts[i], b[n + 1] = g->tmp_artifacts[i] >> 8;
    memcpy(b + n, g->equip, 4);
    n += 4;
    for (int i = 0; i < 8; i++, n += 2)
        b[n] = g->cmd_slots[i], b[n + 1] = g->cmd_slots[i] >> 8;
    b[n++] = g->cmd_count;
    q_msg(p, b, n);
}

static void msg_equip_list(player *p)
{
    demo *g = p->game;
    uint8_t b[700] = { MSG_EQUIP_LIST };
    int n = 1;
    memcpy(b + n, g->equip, 4);
    n += 4;
    int cands[64], nc = 0;
    for (int i = 0; i < 64; i++)
        if (g->items[i] >= 1 && g->items[i] <= 158)
            cands[nc++] = i;
    int start = n;
    b[n++] = nc;
    for (int i = 0; i < nc; i++)
        b[n++] = cands[i];
    while ((n - start) & 3)
        b[n++] = 0xFF;
    for (int i = 0; i < nc; i++, n += 8)
    {
        iteminfo info = item_info(g->items[cands[i]]);
        memcpy(b + n, &info, 8);
    }
    q_msg(p, b, n);
}

static void msg_cmd_list(player *p)
{
    demo *g = p->game;
    uint8_t b[600] = { MSG_CMD_LIST };
    int n = 1, count = 0;
    n += 4;
    for (int i = 0; i < 64; i++)
    {
        int id = g->items[i];
        if (id >= 1 && id <= 67 && (item_info(id).flags & 0xF) == 1)   // Clavat weapons
        {
            iteminfo info = item_info(id);
            memcpy(b + n, &info, 8);
            n += 8;
            count++;
        }
    }
    b[1] = count;
    q_msg(p, b, n);
}

static void msg_tmp_artifacts(player *p)
{
    uint8_t b[9] = { MSG_TMP_ARTIFACTS };
    for (int i = 0; i < 4; i++)
        b[1 + i * 2] = p->game->tmp_artifacts[i], b[2 + i * 2] = p->game->tmp_artifacts[i] >> 8;
    q_msg(p, b, sizeof(b));
}

static void msg_favorite(player *p)
{
    uint8_t b[9] = { MSG_FAVORITE };
    memcpy(b + 1, favorites, 8);
    q_msg(p, b, sizeof(b));
}

static void msg_compat(player *p)
{
    uint8_t b[64] = { MSG_COMPATIBILITY };
    q_msg(p, b, 1 + put_compat(b + 1));
}

static void msg_bonus(player *p)
{
    static const char s[] = "Defeat 10 monsters\0Take no damage";
    uint8_t b[64] = { MSG_BONUS_STR };
    memcpy(b + 1, s, sizeof(s));
    q_msg(p, b, 1 + sizeof(s));
}

static void msg_marker_kinds(player *p)
{
    uint8_t b[81] = { MSG_MARKER_KINDS };
    for (int i = 0; i < 8; i++)
        b[1 + i] = p->game->enemies[i].kind;
    for (int i = 0; i < 4; i++)
        b[1 + 64 + i] = p->game->treasures[i].kind;
    q_msg(p, b, sizeof(b));
}

// Lines around the player: a fence, a bridge and a gate
static void msg_map_obj(player *p)
{
    demo *g = p->game;
    static const int seg[3][5] = { { 0, -50, -30, 50, -30 }, { 1, -40, 30, -40, 70 }, { 2, 30, 20, 70, 20 } };
    uint8_t b[64] = { MSG_MAP_OBJ, 3, 0xFF, 0xFF, 0xFF, 0xFF };
    int n = 6;
    for (int i = 0; i < 3; i++)
    {
        int v[4] = { g->x + seg[i][1], g->y + seg[i][2], g->x + seg[i][3], g->y + seg[i][4] };
        b[n++] = seg[i][0];
        for (int k = 0; k < 4; k++, n += 2)
            b[n] = v[k], b[n + 1] = v[k] >> 8;
    }
    q_msg(p, b, n);
}

// Monster records for the scouter, and the last one hit
static void msg_scouter(player *p)
{
    uint8_t b[1 + 0x200] = { MSG_SCOUTER_INFO };
    memset(b + 1, 0xFF, 0x200);
    static const uint8_t mon[8][8] = {
        { 43, 3, 0xFF, 0xFF, 80, 0, 5, 1 },    // Goblin: weak to magic, drops Cure
        { 1, 5, 0xFF, 0xFF, 40, 0, 0x81, 1 },  // Flan: weak to fire, drops an apple
        { 4, 0, 0xFF, 0xFF, 120, 0, 0, 0 },    // Bomb
        { 16, 4, 6, 0xFF, 60, 0, 0x26, 1 },    // Mu
    };
    for (int i = 0; i < 8; i++)
        memcpy(b + 1 + i * 8, mon[i % 4], 8);
    q_msg(p, b, sizeof(b));
    q_word(p, WORD16(LINK_HIT_ENEMY, 0, 50));
}

// Downloads
// =========

static void send_letter_list(player *p)
{
    demo *g = p->game;
    uint8_t b[600] = { 0 };
    int n = 16;
    b[0] = g->nletters, b[4] = g->nletters, b[8] = g->nletters, b[12] = 1;
    for (int i = 0; i < g->nletters; i++, n += 8)
    {
        const letter *l = &letters_init[i];
        uint32_t a = l->gil ? l->attach * 100 : l->attach;
        for (int k = 0; k < 4; k++)
            b[n + k] = a >> (8 * k);
        b[n + 4] = i;
        b[n + 5] = i;
        b[n + 6] = g->letter_flags[i];
    }
    for (int i = 0; i < g->nletters; i++, n += 24)
        snprintf((char *)b + n, 24, "%s", letters_init[i].subject);
    for (int i = 0; i < g->nletters; i++, n += 16)
        snprintf((char *)b + n, 16, "%s", letters_init[i].sender);
    q_word(p, WORD(LINK_CANCEL, 0, 0, 0));
    q_bulk(p, XFER_LETTER_LIST, b, n);
}

static void send_letter(player *p, int idx)
{
    if (idx < 0 || idx >= p->game->nletters)
        return;
    const letter *l = &letters_init[idx];
    uint8_t b[600];
    int n = 0;
    n += sprintf((char *)b, "%s", l->body) + 1;
    n += sprintf((char *)b + n, "%s", l->answers) + 1;
    p->game->letter_flags[idx] |= L_OPENED;
    q_word(p, WORD(LINK_CANCEL, 0, 0, 0));
    q_bulk(p, XFER_LETTER, b, n);
}

static void send_artifacts(player *p)
{
    uint8_t b[12];
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 4; k++)
            b[i * 4 + k] = p->game->artifacts[i] >> (8 * k);
    q_word(p, WORD(LINK_CANCEL, 0, 0, 0));
    q_bulk(p, XFER_ARTIFACTS, b, sizeof(b));
}

static void send_buy_list(player *p)
{
    int count = sizeof(goods) / sizeof(goods[0]);
    uint8_t b[1024] = { count };
    int n = 4;
    for (int i = 0; i < count; i++, n += 2)
        b[n] = goods[i].id, b[n + 1] = goods[i].id >> 8;
    if (count & 1)
        n += 2;
    for (int i = 0; i < count; i++)
        for (int k = 0; k < 4; k++)
            b[n++] = item_price(goods[i].id) >> (8 * k);
    for (int i = 0; i < count; i++)
        n += sprintf((char *)b + n, "%s", goods[i].desc) + 1;
    q_word(p, WORD(LINK_CANCEL, 0, 0, 0));
    q_bulk(p, XFER_BUY_LIST, b, n);
}

static void send_sell_list(player *p)
{
    demo *g = p->game;
    uint8_t b[0x300 + 64 * 32] = { 0 };
    int n = 0x300;
    for (int i = 0; i < 64; i++)
    {
        int id = g->items[i];
        iteminfo info = item_info(id);
        memcpy(b + i * 8, &info, 8);
        uint32_t price = id > 0 ? (item_price(id) + 3) / 4 : 0;
        for (int k = 0; k < 4; k++)
            b[0x200 + i * 4 + k] = price >> (8 * k);
        if (id >= 293 && id <= 300)
            n += sprintf((char *)b + n, "Useful for\nforging.") + 1;
        else if (id >= 381 && id <= 392)
            n += sprintf((char *)b + n, "Something to eat.") + 1;
        else if (id > 400)
            n += sprintf((char *)b + n, "A blacksmith's\ndesign.") + 1;
        else
            b[n++] = 0;
    }
    q_word(p, WORD(LINK_CANCEL, 0, 0, 0));
    q_bulk(p, XFER_SELL_LIST, b, n);
}

static void send_smith_list(player *p)
{
    demo *g = p->game;
    uint8_t b[1024] = { 0 };
    int slots[64], ns = 0;
    for (int i = 0; i < 64; i++)
        if (g->items[i] > 400 && find_recipe(g->items[i]))
            slots[ns++] = i;
    int n = 0;
    b[n++] = ns;
    for (int i = 0; i < ns; i++)
        b[n++] = slots[i];
    n = (n + 3) & ~3;
    uint32_t known[4] = { 0 };
    for (int i = 0; i < ns; i++, n += 56)
    {
        const recipe *r = find_recipe(g->items[slots[i]]);
        uint8_t *q = b + n;
        for (int k = 0; k < 4; k++)
            q[k] = r->price >> (8 * k);
        for (int k = 0; k < 3; k++)
        {
            q[4 + k * 2] = r->mat[k], q[5 + k * 2] = r->mat[k] >> 8;
            q[10 + k * 2] = r->cnt[k], q[11 + k * 2] = r->cnt[k] >> 8;
        }
        for (int k = 0; k < 4; k++)
        {
            q[16 + k * 2] = r->result[k], q[17 + k * 2] = r->result[k] >> 8;
            iteminfo info = item_info(r->result[k]);
            memcpy(q + 24 + k * 8, &info, 8);
        }
    }
    for (size_t i = 0; i < sizeof(recipes) / sizeof(recipes[0]); i++)
    {
        int bit = recipes[i].design - 401;
        known[bit / 32] |= 1u << (bit % 32);
    }
    for (int i = 0; i < 4; i++, n += 4)
        for (int k = 0; k < 4; k++)
            b[n + k] = known[i] >> (8 * k);
    q_word(p, WORD(LINK_CANCEL, 0, 0, 0));
    q_bulk(p, XFER_SMITH_LIST, b, n);
}

// Scenes
// ======

static void state_map(player *p)
{
    const mapfile *m = &maps[p->game->map];
    if (p->game->mode == MODE_TV)
        q_word(p, WORD(LINK_STATE, 1, 33, 0));
    else
        q_word(p, WORD(LINK_STATE, 1, m->stage, m->map));
}

// What the GameCube does on a mode change: cancel, the mode, and the
// sprite data check (SendMType, then ThreadMain's state 0x14)
static void set_mode(player *p, int mode)
{
    demo *g = p->game;
    if (g->ready && mode != g->mode)
        q_word(p, WORD(LINK_START, 0, 0, 0));
    g->mode = mode;
    g->ready = false;
    g->screen = -1;
    q_word(p, WORD(LINK_CANCEL, 0, 0, 0));
    q_word(p, WORD(LINK_MODE, mode, 0, 0));
    if (nmaps)
        state_map(p);
    event(p, EVT_LANGUAGE, 0, 0);
    if (objdat)
        q_crc(p, XFER_OBJ, objdat, objdat_len, T_OBJ);
    logf_("player %d: %s", p->id, mode_names[mode]);
}

static void load_map(player *p)
{
    if (!nmaps)
    {
        demo_done(p, T_MAP, true);
        return;
    }
    const mapfile *m = &maps[p->game->map];
    place_movers(p->game);
    state_map(p);
    q_crc(p, XFER_MAP, m->data, m->len, T_MAP);
}

void demo_done(player *p, int tag, bool ok)
{
    demo *g = p->game;
    switch (tag)
    {
    case T_OBJ:
        if (!ok)
        {
            q_word(p, WORD(LINK_CANCEL, 0, 0, 0));
            q_word(p, WORD(LINK_CTRL_MODE, 0, 0, 0));
            q_bulk(p, XFER_OBJ, objdat, objdat_len);
        }
        if (g->mode != MODE_FIELD)
        {
            q_word(p, WORD(LINK_START, 1, 0, 0));
            send_gil(p);
            event(p, EVT_SP_MODE, 0, 0);
            q_tag(p, T_READY);
            break;
        }
        q_word(p, WORD(LINK_START, 0, 0, 0));
        msg_bonus(p);
        msg_player_stat(p);
        msg_item_all(p);
        q_word(p, WORD(LINK_START, 1, 0, 0));
        load_map(p);
        break;
    case T_MAP:
        if (!ok)
        {
            const mapfile *m = &maps[g->map];
            logf_("player %d: sending map m%02d_%d (%d bytes)", p->id, m->stage, m->map, m->len);
            q_word(p, WORD(LINK_CANCEL, 0, 0, 0));
            q_word(p, WORD(LINK_CTRL_MODE, 0, 0, 0));
            q_bulk(p, XFER_MAP, m->data, m->len);
        }
        event(p, EVT_RADAR_TYPE, g->radar_type, 0);
        event(p, EVT_RADAR_MODE, 1, 0);
        msg_marker_kinds(p);
        msg_map_obj(p);
        q_word(p, WORD(LINK_CTRL_MODE, g->ctrl, 0, 0));
        event(p, EVT_ITEM_USE_FLAGS, 3, 0);
        event(p, EVT_SP_MODE, 0, 0);
        // The client comes out of setup on the screen it saved when the
        // mode arrived (the waiting notice); the GameCube's map load puts
        // it on the radar
        event(p, EVT_OPEN_MENU, 0, 0);
        q_tag(p, T_READY);
        break;
    case T_READY:
        g->ready = true;
        logf_("player %d: %s ready", p->id, mode_names[g->mode]);
        break;
    }
}

static void next_map(player *p)
{
    demo *g = p->game;
    if (!nmaps || g->mode != MODE_FIELD || !g->ready)
        return;
    g->map = (g->map + 1) % nmaps;
    g->ready = false;
    logf_("player %d: map m%02d_%d", p->id, maps[g->map].stage, maps[g->map].map);
    load_map(p);
}

void demo_start(player *p)
{
    p->game = &games[p->id];
    new_game(p);
    if (nmaps)
        place_movers(p->game);
    set_mode(p, MODE_FIELD);
}

void demo_end(player *p)
{
    if (p->game)
        p->game->ready = false;
}

// The radar, once a frame
// =======================

static int clamp8(int v, bool *vis, int lim)
{
    if (v < -lim || v > lim)
        *vis = false;
    return v < -127 ? -127 : v > 127 ? 127 : v;
}

static void move(const mapfile *map, mover *m, int x0, int y0, int x1, int y1)
{
    if (!on_floor(map, m->x + m->dx * 4, m->y))
        m->dx = -m->dx;
    if (!on_floor(map, m->x, m->y + m->dy * 4))
        m->dy = -m->dy;
    m->x += m->dx;
    m->y += m->dy;
    if (m->x < x0 || m->x > x1)
        m->dx = -m->dx;
    if (m->y < y0 || m->y > y1)
        m->dy = -m->dy;
}

static void radar_frame(player *p)
{
    demo *g = p->game;
    uint32_t f = g->frame;
    const mapfile *m = nmaps ? &maps[g->map] : NULL;

    // Walk with the D-pad while the DS is a controller
    if (!g->ctrl)
    {
        int dx = (g->keys & KEY_RIGHT ? 1 : 0) - (g->keys & KEY_LEFT ? 1 : 0);
        int dy = (g->keys & KEY_DOWN ? 1 : 0) - (g->keys & KEY_UP ? 1 : 0);
        int ox = g->x, oy = g->y;
        if (on_floor(m, g->x + dx * 2, g->y))
            g->x += dx * 2;
        if (on_floor(m, g->x, g->y + dy * 2))
            g->y += dy * 2;
        // the rest of the caravan follows in the player's steps
        if (g->x != ox || g->y != oy)
        {
            g->trail_at = (g->trail_at + 1) % 64;
            g->trail[g->trail_at][0] = g->x;
            g->trail[g->trail_at][1] = g->y;
        }
        for (int i = 0, k = 0; i < 4; i++)
            if (i != g->no)
            {
                int *t = g->trail[(g->trail_at + 64 - 7 * ++k) % 64];
                g->party[i].x = t[0];
                g->party[i].y = t[1];
            }
    }
    if (f % 2 == 0)
        for (int i = 0; i < 8; i++)
            move(m, &g->enemies[i], g->x - 90, g->y - 70, g->x + 90, g->y + 70);

    if (g->screen != 0 || g->radar_type == 2)
        return;
    if (f % 3 == 0)
    {
        q_word(p, WORD16(LINK_BASE_POS, 0, (uint16_t)g->x));
        q_word(p, WORD16(LINK_BASE_POS | LINK_KIND_SECOND, 0, (uint16_t)g->y));
    }
    if (f % 4 == 1)
    {
        int8_t d[8];
        uint8_t vis = 0;
        for (int i = 0; i < 4; i++)
        {
            bool v = true;
            d[i * 2] = i == g->no ? 0 : clamp8(g->party[i].x - g->x, &v, 0x50);
            d[i * 2 + 1] = i == g->no ? 0 : clamp8(g->party[i].y - g->y, &v, 0x40);
            if (v)
                vis |= 1 << i;
        }
        q_word(p, WORD(LINK_PARTY_POS, vis, d[0], d[1]));
        q_word(p, WORD(LINK_PARTY_POS | LINK_KIND_SECOND, d[2], d[3], d[4]));
        q_word(p, WORD(LINK_PARTY_POS | LINK_KIND_DATA, d[5], d[6], d[7]));
    }
    if (f % 8 == 2 && (g->radar_type == 1 || g->radar_type == 3))
        for (int i = 0; i < 8; i++)
        {
            bool v = true;
            int dx = clamp8(g->enemies[i].x - g->x, &v, 0x50);
            int dy = clamp8(g->enemies[i].y - g->y, &v, 0x40);
            q_word(p, WORD(LINK_ENEMY_POS, i | v << 7, dx, dy));
        }
    if (f % 16 == 5 && g->radar_type == 3)
        for (int i = 0; i < 4; i++)
        {
            bool v = true;
            int dx = clamp8(g->treasures[i].x - g->x, &v, 0x50);
            int dy = clamp8(g->treasures[i].y - g->y, &v, 0x40);
            q_word(p, WORD(LINK_TREASURE_POS, (64 + i) | v << 7, dx, dy));
        }
    if (f % 30 == 7)
    {
        uint8_t hp = 0x80;
        for (int i = 0; i < 4; i++)
            if (i != g->no || g->hp)
                hp |= 1 << i;
        q_word(p, WORD(LINK_PARTY_HP, hp, g->hp, g->miasma));
    }
}

void demo_frame(player *p, uint32_t now)
{
    demo *g = p->game;
    if (!g)
        return;
    g->frame++;
    if (!g->ready || !q_idle(p))
        return;
    if (g->mode == MODE_FIELD)
    {
        radar_frame(p);
        if (g->letter_pending)
        {
            g->letter_pending = false;
            event(p, EVT_ADD_LETTER, 0, 0);
        }
    }
}

// What the client sends
// =====================

static void hit(player *p)
{
    demo *g = p->game;
    if (g->hp > 1)
        g->hp--;
    else
        g->hp = g->max_hp;
    q_word(p, WORD(LINK_PARTY_HP, 0x8F, g->hp, g->miasma));
    logf_("player %d: hit, HP %d/%d", p->id, g->hp, g->max_hp);
}

static void set_radar(player *p, int type)
{
    demo *g = p->game;
    g->radar_type = (type + 4) % 4;
    event(p, EVT_RADAR_TYPE, g->radar_type, 0);
    if (g->radar_type == 2 && g->screen == 0)
        msg_scouter(p);
    static const char *const names[] = { "map", "monsters", "scouter", "monsters and treasure" };
    logf_("player %d: radar shows %s", p->id, names[g->radar_type]);
}

static void pad(player *p, uint16_t keys)
{
    demo *g = p->game;
    uint16_t down = keys & ~g->keys;
    g->keys = keys;
    if (!g->ready)
        return;
    if (g->mode == MODE_TV)
    {
        if (down & KEY_START)
            set_mode(p, MODE_FIELD);
        return;
    }
    if (g->mode != MODE_FIELD)
        return;
    if (down & KEY_SELECT)
    {
        g->ctrl = !g->ctrl;
        q_word(p, WORD(LINK_CTRL_MODE, g->ctrl, 0, 0));
        logf_("player %d: the DS %s", p->id, g->ctrl ? "steers its menus" : "is a controller");
        return;
    }
    if (g->ctrl)
        return;
    if (down & KEY_A)
        next_map(p);
    if (down & KEY_B)
        hit(p);
    if (down & KEY_R)
        set_radar(p, g->radar_type + 1);
    if (down & KEY_L)
        set_radar(p, g->radar_type - 1);
    if (down & KEY_START)
        set_mode(p, MODE_CMAKE);
}

static void screen_report(player *p, int screen)
{
    demo *g = p->game;
    g->screen = screen;
    logf_("player %d: screen %d", p->id, screen);
    if (g->mode != MODE_FIELD || !g->ready)
        return;
    switch (screen)
    {
    case 0:
        if (g->radar_type == 2)
            msg_scouter(p);
        break;
    case 1: msg_cmd_list(p); break;
    case 3: msg_equip_list(p); break;
    case 5: msg_tmp_artifacts(p); break;
    case 7: msg_favorite(p); break;
    case 8: msg_compat(p); break;
    }
}

static void request(player *p, int type, int arg)
{
    switch (type)
    {
    case 2: send_letter(p, arg); break;
    case 3: send_letter_list(p); break;
    case 6: send_sell_list(p); break;
    case 7: send_buy_list(p); break;
    case 8: send_smith_list(p); break;
    case 9: send_artifacts(p); break;
    case 14:
        break; // only the GameCube asks for this one
    default:
        logf_("player %d: request %d (%d)?", p->id, type, arg);
    }
}

static int count_items(demo *g, int id)
{
    int n = 0;
    for (int i = 0; i < 64; i++)
        n += g->items[i] == id;
    return n;
}

static void remove_items(player *p, int id, int n)
{
    for (int i = 0; i < 64 && n > 0; i++)
        if (p->game->items[i] == id)
        {
            set_item(p, i, EMPTY);
            n--;
        }
}

static void forge(player *p, int slot, int tribe)
{
    demo *g = p->game;
    const recipe *r = slot < 64 ? find_recipe(g->items[slot]) : NULL;
    bool ok = r && g->gil >= (uint32_t)r->price;
    for (int k = 0; ok && k < 3 && r->mat[k]; k++)
        ok = count_items(g, r->mat[k]) >= r->cnt[k];
    if (!ok)
    {
        reply_event(p, false, EVT_SMITH_FORGE);
        return;
    }
    set_item(p, slot, EMPTY);
    for (int k = 0; k < 3 && r->mat[k]; k++)
        remove_items(p, r->mat[k], r->cnt[k]);
    add_item(p, r->result[tribe & 3]);
    g->gil -= r->price;
    send_gil(p);
    reply_event(p, true, EVT_SMITH_FORGE);
    logf_("player %d: forged item %d", p->id, r->result[tribe & 3]);
}

static void buy(player *p, int idx, int qty)
{
    demo *g = p->game;
    int count = sizeof(goods) / sizeof(goods[0]);
    if (idx >= count || qty < 1)
    {
        reply_event(p, false, EVT_SHOP_BUY);
        return;
    }
    uint32_t cost = item_price(goods[idx].id) * qty;
    if (cost > g->gil)
    {
        reply_event(p, false, EVT_SHOP_BUY);
        return;
    }
    for (int i = 0; i < qty; i++)
        if (add_item(p, goods[idx].id) < 0)
            break;
    g->gil -= cost;
    send_gil(p);
    reply_event(p, true, EVT_SHOP_BUY);
    logf_("player %d: bought %d of item %d", p->id, qty, goods[idx].id);
}

static void sell(player *p, int slot)
{
    demo *g = p->game;
    if (slot >= 64 || g->items[slot] <= 158)
    {
        reply_event(p, false, EVT_SHOP_SELL);
        return;
    }
    g->gil += (item_price(g->items[slot]) + 3) / 4;
    logf_("player %d: sold item %d", p->id, g->items[slot]);
    set_item(p, slot, EMPTY);
    send_gil(p);
    reply_event(p, true, EVT_SHOP_SELL);
}

static void take_attachment(player *p, int idx)
{
    demo *g = p->game;
    if (idx >= g->nletters || g->letter_flags[idx] & L_TAKEN)
    {
        reply_event(p, false, EVT_TAKE_ATTACHMENT);
        return;
    }
    const letter *l = &letters_init[idx];
    if (l->gil)
    {
        g->gil += l->attach;
        send_gil(p);
    }
    else if (add_item(p, l->attach) < 0)
    {
        reply_event(p, false, EVT_TAKE_ATTACHMENT);
        return;
    }
    g->letter_flags[idx] |= L_TAKEN;
    logf_("player %d: took %s from letter %d", p->id, l->gil ? "the gil" : "the item", idx);
    reply_event(p, true, EVT_TAKE_ATTACHMENT);
}

static void letter_reply(player *p)
{
    demo *g = p->game;
    uint8_t b[9];
    uint32_t *w = g->reply_words;
    uint16_t crc = (w[0] >> 8 & 0xFF) << 8 | (w[0] >> 16 & 0xFF);
    b[0] = w[0] >> 24;
    for (int i = 0; i < 3; i++)
        b[1 + i] = w[1] >> (8 + 8 * i), b[4 + i] = w[2] >> (8 + 8 * i);
    if (ff_crc16(b, 7) != crc || b[0] >= g->nletters)
    {
        q_word(p, WORD(LINK_NAK, LINK_LETTER_REPLY, 0, 0));
        return;
    }
    int idx = b[0], answer = b[1];
    bool gil = b[2];
    uint32_t value = (uint32_t)b[3] << 24 | b[4] << 16 | b[5] << 8 | b[6];
    g->letter_flags[idx] |= L_REPLIED;
    q_word(p, WORD(LINK_ACK, LINK_LETTER_REPLY, 0, 0));
    if (gil && value <= g->gil && value)
    {
        g->gil -= value;
        send_gil(p);
    }
    else if (!gil && value && (value >> 16) < 64)
        set_item(p, value >> 16, EMPTY);
    logf_("player %d: answered letter %d with answer %d, sending %s %u", p->id, idx, answer,
          gil ? "gil" : "item", gil ? value : value & 0xFFFF);
}

static void item_op(player *p, int op, int slot)
{
    demo *g = p->game;
    if (slot >= 64 || g->items[slot] == EMPTY)
        return;
    static const char *const ops[] = { "", "used", "dropped", "threw away" };
    logf_("player %d: %s item %d", p->id, op <= 3 ? ops[op] : "?", g->items[slot]);
    if (op == 1 && g->hp < g->max_hp)
    {
        g->hp++;
        q_word(p, WORD(LINK_PARTY_HP, 0x8F, g->hp, g->miasma));
    }
    set_item(p, slot, EMPTY);
}

static void cmake_name(player *p)
{
    demo *g = p->game;
    uint8_t name[16];
    uint32_t *w = g->name_words;
    uint16_t crc = (w[0] >> 8 & 0xFF) << 8 | (w[0] >> 16 & 0xFF);
    name[0] = w[0] >> 24;
    for (int i = 0; i < 5; i++)
        for (int k = 0; k < 3; k++)
            name[1 + i * 3 + k] = w[1 + i] >> (8 + 8 * k);
    memcpy(g->cmake_name, name, 16);
    g->cmake_name[16] = 0;
    // A name already in the caravan is taken, as in the game
    bool taken = ff_crc16(name, 16) != crc || !strcasecmp(g->cmake_name, "Sol") || !strcasecmp(g->cmake_name, "Ria");
    q_word(p, WORD(taken ? LINK_NAK : LINK_ACK, LINK_CMAKE_NAME, 0, 0));
    logf_("player %d: name \"%s\" %s", p->id, g->cmake_name, taken ? "is taken" : "is free");
}

static void game_event(player *p, int sub, int a, int b)
{
    demo *g = p->game;
    switch (sub)
    {
    case EVT_TAKE_ATTACHMENT: take_attachment(p, a); break;
    case EVT_CMAKE_LOOK:
        if (a != 0xFF)
        {
            // the first look of each tribe is taken, to show the notice
            bool taken = ((a >> 2) & 3) == 0 && a != 0;
            reply_event(p, !taken, sub);
        }
        break;
    case EVT_CMAKE_JOB:
        reply_event(p, a != 7, sub);
        break;
    case EVT_CMAKE_END:
        logf_("player %d: created \"%s\"", p->id, g->cmake_name);
        set_mode(p, MODE_SHOP);
        break;
    case EVT_CMAKE_CANCEL:
        set_mode(p, MODE_SHOP);
        break;
    case EVT_SHOP_LEAVE: set_mode(p, MODE_SMITH); break;
    case EVT_SHOP_SELL: sell(p, a); break;
    case EVT_SHOP_BUY: buy(p, a, b); break;
    case EVT_SMITH_FORGE: forge(p, a, b); break;
    case EVT_SMITH_LEAVE: set_mode(p, MODE_TV); break;
    case EVT_GIL_RESEND: send_gil(p); break;
    default: logf_("player %d: event %d (%d, %d)", p->id, sub, a, b);
    }
}

void demo_word(player *p, uint32_t w)
{
    demo *g = p->game;
    if (!g)
        return;
    int cmd = w & 0x3F, kind = (w >> 6) & 3;
    int b1 = (w >> 8) & 0xFF, b2 = (w >> 16) & 0xFF, b3 = w >> 24;
    switch (cmd)
    {
    case LINK_PAD: pad(p, w >> 8); break;
    case LINK_STATE:
        if (b1 == 0)
            screen_report(p, b2);
        break;
    case LINK_REQUEST: request(p, b1, b2); break;
    case LINK_EVENT: game_event(p, b1, b2, b3); break;
    case LINK_ITEM: item_op(p, b1, b2); break;
    case LINK_GIL:
        if (kind == 0)
            g->gil_word = w;
        else if (g->gil_word && ((g->gil_word >> 8) & 0xFF) == 1)
        {
            uint32_t amount = (uint32_t)(g->gil_word >> 16 & 0xFF) << 24 | (g->gil_word >> 24) << 16 | b1 << 8 | b2;
            g->gil_word = 0;
            bool ok = amount <= g->gil;
            q_word(p, WORD(ok ? LINK_ACK : LINK_NAK, LINK_GIL | LINK_KIND_SECOND, 1, 0));
            if (ok)
            {
                g->gil -= amount;
                send_gil(p);
                logf_("player %d: dropped %u gil", p->id, amount);
            }
        }
        else
            q_word(p, WORD(LINK_NAK, LINK_GIL | LINK_KIND_SECOND, 1, 0));
        break;
    case LINK_EQUIP_SLOT:
        if (b1 < 4)
        {
            g->equip[b1] = b2;
            g->stats[0] = 5 + (b2 != 0xFF && b1 == 0 && b2 < 64 ? item_info(g->items[b2]).count / 2 : 0);
            q_word(p, WORD(LINK_STRENGTH, g->stats[0], g->stats[1], g->stats[2]));
        }
        break;
    case LINK_CMD_SLOT:
        if (b1 < 8)
            g->cmd_slots[b1] = w >> 16;
        break;
    case LINK_LETTER_REPLY:
        if (kind == 0)
            g->reply_n = 0;
        if (g->reply_n < 3)
            g->reply_words[g->reply_n++] = w;
        if (g->reply_n == 3)
        {
            g->reply_n = 0;
            letter_reply(p);
        }
        break;
    case LINK_CMAKE_NAME:
        if (kind == 0)
            g->name_n = 0;
        if (g->name_n < 6)
            g->name_words[g->name_n++] = w;
        if (g->name_n == 6)
        {
            g->name_n = 0;
            cmake_name(p);
        }
        break;
    default:
        logf_("player %d: word %08x", p->id, w);
    }
}

void demo_ack(player *p, uint32_t w)
{
    // LINK_MASK and stray answers need nothing
}

void demo_command(const char *line)
{
    char cmd[32] = "";
    sscanf(line, "%31s", cmd);
    for (int id = 1; id <= MAX_PLAYERS; id++)
    {
        player *p = ffhost_player(id);
        if (!p || !p->game)
            continue;
        if (!strcmp(cmd, "field")) set_mode(p, MODE_FIELD);
        else if (!strcmp(cmd, "shop")) set_mode(p, MODE_SHOP);
        else if (!strcmp(cmd, "smith")) set_mode(p, MODE_SMITH);
        else if (!strcmp(cmd, "cmake")) set_mode(p, MODE_CMAKE);
        else if (!strcmp(cmd, "tv")) set_mode(p, MODE_TV);
        else if (!strcmp(cmd, "map")) next_map(p);
        else if (!strcmp(cmd, "hit")) hit(p);
        else if (!strcmp(cmd, "radar")) set_radar(p, p->game->radar_type + 1);
        else if (!strcmp(cmd, "letter"))
        {
            if (p->game->nletters < NLETTERS)
            {
                p->game->nletters++;
                p->game->letter_pending = true;
                logf_("player %d: letter from %s", p->id, letters_init[p->game->nletters - 1].sender);
            }
            else
                logf_("player %d: no more letters", p->id);
        }
        else if (!strcmp(cmd, "menu")) { p->game->ctrl = true; q_word(p, WORD(LINK_CTRL_MODE, 1, 0, 0)); }
        else if (!strcmp(cmd, "pad")) { p->game->ctrl = false; q_word(p, WORD(LINK_CTRL_MODE, 0, 0, 0)); }
        else if (!strncmp(cmd, "screen", 6)) event(p, EVT_OPEN_MENU, atoi(line + 6), 0);
    }
    if (!strcmp(cmd, "help") || !cmd[0])
    {
        logf_("commands, for every linked DS:");
        logf_("  field, cmake, shop, smith, tv   go to that scene");
        logf_("  map     next radar map          radar   next radar role");
        logf_("  hit     take a hit              letter  a new letter arrives");
        logf_("  menu    the DS steers its menus pad     the DS is a controller");
        logf_("  screen N   open menu screen N (0 radar .. 9 letters)");
    }
}
