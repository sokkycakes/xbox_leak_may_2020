/* crimson-agent: runs inside a partition (Game OS or System OS).
 *
 * It is the partition's half of the host link:
 *   - captures the partition's output (whole desktop in VM mode, one window
 *     in process mode, or a built-in test pattern) and streams changed rows
 *     to the host,
 *   - receives the controller from the host when this partition has focus
 *     and feeds it to the partition's apps,
 *   - receives layout changes (Full / Fill / Snap / Guide overlay / Home).
 *
 *   crimson-agent --role game|system [--host 127.0.0.1] [--port 7700] [--hv]
 *                 [--desktop | --window "title" | --test] [--size 1280x720]
 *                 [--fps 30] [--guide-key F1] [--follow-layout] [--no-input]
 */
#include "net.h"
#include <xinput.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>
#include <ctype.h>

enum source { SRC_TEST, SRC_DESKTOP, SRC_WINDOW };

static int g_role = CR_ROLE_GAME;
static const char *g_name = NULL;
static const char *g_host = "127.0.0.1";
static int g_port = CR_TCP_PORT;
static int g_hv = 0;
static GUID g_hv_service;
static int g_src = SRC_TEST;
static const char *g_window_title = NULL;
static int g_w = 1280, g_h = 720, g_fps = 30;
static int g_base_w = 1280, g_base_h = 720; /* --size; g_w/g_h follow the slot's shape */
static WORD g_guide_key = 0;
static int g_follow_layout = 0, g_input = 1;
static const char *g_run = NULL;   /* started hidden at boot (Era images) */
static const char *g_log = NULL;

static volatile LONG g_location = CR_LOC_HIDDEN, g_focused = 0;
static volatile LONG g_layout_w, g_layout_h;
static cr_input g_pad; /* last pad from the host (for the test pattern) */
static HWND g_target;

/* ---------- target window ---------- */

typedef struct { const char *needle; HWND found; } find_ctx;
static BOOL CALLBACK find_cb(HWND w, LPARAM lp)
{
    find_ctx *c = (find_ctx *)lp;
    char t[256];
    if (!IsWindowVisible(w)) return TRUE;
    GetWindowTextA(w, t, sizeof t);
    if (t[0] && strstr(t, c->needle)) { c->found = w; return FALSE; }
    return TRUE;
}
static HWND find_target(void)
{
    if (g_target && IsWindow(g_target)) return g_target;
    find_ctx c = { g_window_title, NULL };
    EnumWindows(find_cb, (LPARAM)&c);
    g_target = c.found;
    return g_target;
}

/* ---------- input ---------- */

static void key(WORD vk, int down)
{
    if (!g_input) return;
    UINT sc = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
    int ext = (vk >= VK_PRIOR && vk <= VK_DOWN) || vk == VK_INSERT || vk == VK_DELETE;
    if (g_src == SRC_WINDOW) {
        HWND w = find_target();
        if (!w) return;
        LPARAM lp = 1 | (sc << 16) | (ext ? 1 << 24 : 0) | (down ? 0 : (3u << 30));
        PostMessageA(w, down ? WM_KEYDOWN : WM_KEYUP, vk, lp);
    } else if (g_src == SRC_DESKTOP) {
        INPUT in = {0};
        in.type = INPUT_KEYBOARD;
        in.ki.wVk = vk;
        in.ki.wScan = (WORD)sc;
        in.ki.dwFlags = (down ? 0 : KEYEVENTF_KEYUP) | (ext ? KEYEVENTF_EXTENDEDKEY : 0);
        SendInput(1, &in, sizeof in);
    }
}

/* Pad to keyboard. A virtual pad driver (ViGEmBus) in the guest is the next
 * step; keys are enough to drive the dashboard and most PC games. */
static void apply_pad(const cr_input *now, const cr_input *was)
{
    struct { uint16_t bit; WORD vk_game, vk_system; } map[] = {
        { XINPUT_GAMEPAD_DPAD_UP, VK_UP, VK_UP },
        { XINPUT_GAMEPAD_DPAD_DOWN, VK_DOWN, VK_DOWN },
        { XINPUT_GAMEPAD_DPAD_LEFT, VK_LEFT, VK_LEFT },
        { XINPUT_GAMEPAD_DPAD_RIGHT, VK_RIGHT, VK_RIGHT },
        { XINPUT_GAMEPAD_A, VK_RETURN, VK_RETURN },
        { XINPUT_GAMEPAD_B, VK_ESCAPE, VK_BACK },
        { XINPUT_GAMEPAD_X, 'X', 'X' },
        { XINPUT_GAMEPAD_Y, 'Y', 'Y' },
        { XINPUT_GAMEPAD_START, VK_ESCAPE, VK_RETURN },
        { XINPUT_GAMEPAD_BACK, VK_TAB, VK_TAB },
        { XINPUT_GAMEPAD_LEFT_SHOULDER, VK_PRIOR, VK_PRIOR },
        { XINPUT_GAMEPAD_RIGHT_SHOULDER, VK_NEXT, VK_NEXT },
    };
    /* left stick acts as a d-pad */
    uint16_t nb = now->buttons, wb = was->buttons;
    const int dz = 16000;
    if (now->ly > dz) nb |= XINPUT_GAMEPAD_DPAD_UP;
    if (now->ly < -dz) nb |= XINPUT_GAMEPAD_DPAD_DOWN;
    if (now->lx < -dz) nb |= XINPUT_GAMEPAD_DPAD_LEFT;
    if (now->lx > dz) nb |= XINPUT_GAMEPAD_DPAD_RIGHT;
    if (was->ly > dz) wb |= XINPUT_GAMEPAD_DPAD_UP;
    if (was->ly < -dz) wb |= XINPUT_GAMEPAD_DPAD_DOWN;
    if (was->lx < -dz) wb |= XINPUT_GAMEPAD_DPAD_LEFT;
    if (was->lx > dz) wb |= XINPUT_GAMEPAD_DPAD_RIGHT;
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) {
        int n = (nb & map[i].bit) != 0, w = (wb & map[i].bit) != 0;
        if (n != w) key(g_role == CR_ROLE_GAME ? map[i].vk_game : map[i].vk_system, n);
    }
}

static void tap(WORD vk) { key(vk, 1); Sleep(30); key(vk, 0); }

static void apply_layout(const cr_layout *l)
{
    LONG was = InterlockedExchange(&g_location, (LONG)l->location);
    InterlockedExchange(&g_focused, (LONG)l->focused);
    InterlockedExchange(&g_layout_w, (LONG)l->width);
    InterlockedExchange(&g_layout_h, (LONG)l->height);
    static const char *names[] = { "hidden", "full", "fill", "snap", "guide overlay", "background" };
    printf("layout: %s%s (%ux%u)\n", l->location < 6 ? names[l->location] : "?",
           l->focused ? ", focused" : "", l->width, l->height);

    /* System OS: open/close the shell's Guide as the overlay comes and goes. */
    if (g_role == CR_ROLE_SYSTEM && g_guide_key) {
        int now_ov = l->location == CR_LOC_OVERLAY, was_ov = was == CR_LOC_OVERLAY;
        if (now_ov != was_ov) tap(g_guide_key);
    }
    /* Process mode: make the window match its slot so it renders at Fill/Snap size. */
    if (g_follow_layout && g_src == SRC_WINDOW && l->width && l->height &&
        (l->location == CR_LOC_FULL || l->location == CR_LOC_FILL || l->location == CR_LOC_SNAP)) {
        HWND w = find_target();
        if (w) {
            RECT r = {0, 0, (LONG)l->width, (LONG)l->height};
            AdjustWindowRectEx(&r, (DWORD)GetWindowLongA(w, GWL_STYLE), FALSE,
                               (DWORD)GetWindowLongA(w, GWL_EXSTYLE));
            SetWindowPos(w, NULL, 0, 0, r.right - r.left, r.bottom - r.top,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
}

static DWORD WINAPI recv_thread(LPVOID arg)
{
    SOCKET s = (SOCKET)(uintptr_t)arg;
    cr_hdr h;
    cr_input prev = {0};
    for (;;) {
        if (!cr_recv_all(s, &h, sizeof h) || h.magic != CR_MAGIC || h.len > 4096) break;
        char body[4096];
        if (h.len && !cr_recv_all(s, body, (int)h.len)) break;
        if (h.type == CR_INPUT && h.len == sizeof(cr_input)) {
            cr_input in;
            memcpy(&in, body, sizeof in);
            apply_pad(&in, &prev);
            prev = in;
            g_pad = in;
        } else if (h.type == CR_LAYOUT && h.len == sizeof(cr_layout)) {
            cr_layout l;
            memcpy(&l, body, sizeof l);
            if (!l.focused && memcmp(&prev, &(cr_input){0}, sizeof prev)) {
                cr_input idle = {0};          /* lost focus: release held keys */
                apply_pad(&idle, &prev);
                prev = idle;
            }
            apply_layout(&l);
        }
    }
    shutdown(s, SD_BOTH);
    return 0;
}

/* ---------- capture ---------- */

static HDC g_dc;
static HBITMAP g_dib;
static uint8_t *g_bits;

static void make_target(void)
{
    if (g_dib) { DeleteObject(g_dib); DeleteDC(g_dc); }
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = g_w;
    bi.bmiHeader.biHeight = -g_h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    g_dc = CreateCompatibleDC(NULL);
    g_dib = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)&g_bits, NULL, 0);
    SelectObject(g_dc, g_dib);
}

static void fill(HDC dc, int x, int y, int w, int h, COLORREF c)
{
    RECT r = {x, y, x + w, y + h};
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

static void text(HDC dc, int x, int y, int size, COLORREF c, const char *s)
{
    HFONT f = CreateFontA(size, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
                          NONANTIALIASED_QUALITY, 0, "Segoe UI");
    HGDIOBJ old = SelectObject(dc, f);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    TextOutA(dc, x, y, s, (int)strlen(s));
    SelectObject(dc, old);
    DeleteObject(f);
}

static void draw_test(uint32_t n)
{
    static const char *names[] = { "hidden", "Full", "Fill", "Snap", "Guide overlay", "Background" };
    LONG loc = g_location;
    char buf[160];
    if (g_role == CR_ROLE_GAME) {
        /* a moving scene so frame rate and Fill resizing are visible */
        for (int i = 0; i < 8; i++)
            fill(g_dc, i * g_w / 8, 0, g_w / 8 + 1, g_h,
                 RGB((i & 1) * 200 + 30, (i & 2) * 90 + 30, (i & 4) * 45 + 30));
        int bx = (int)((sin(n * 0.05) * 0.4 + 0.5) * (g_w - 160));
        int by = (int)((cos(n * 0.037) * 0.4 + 0.5) * (g_h - 160));
        fill(g_dc, bx, by, 160, 160, RGB(16, 124, 16));
        text(g_dc, 40, 30, 56, RGB(255, 255, 255), "GAME OS  (exclusive partition)");
        snprintf(buf, sizeof buf, "frame %u   location %s%s   pad 0x%04x  LX %d", n,
                 loc < 6 ? names[loc] : "?", g_focused ? " [focus]" : "", g_pad.buttons, g_pad.lx);
        text(g_dc, 40, 100, 30, RGB(255, 255, 255), buf);
    } else {
        /* System OS: magenta is see-through. A Guide-like blade sits on the left
         * when overlaid; in Snap the whole strip is used. */
        fill(g_dc, 0, 0, g_w, g_h, RGB(CR_KEY_R, CR_KEY_G, CR_KEY_B));
        int pw = loc == CR_LOC_OVERLAY ? g_w * 2 / 5 : g_w;
        fill(g_dc, 0, 0, pw, g_h, RGB(20, 20, 20));
        fill(g_dc, 0, 0, pw, 8, RGB(16, 124, 16));
        text(g_dc, 40, 40, 48, RGB(255, 255, 255), "SYSTEM OS");
        text(g_dc, 40, 100, 26, RGB(180, 180, 180), "(shared partition)");
        snprintf(buf, sizeof buf, "location: %s%s", loc < 6 ? names[loc] : "?", g_focused ? " [focus]" : "");
        text(g_dc, 40, 160, 26, RGB(230, 230, 230), buf);
        static const char *items[] = { "Home", "Friends", "Achievements", "Snap an app", "Settings" };
        static int sel;
        static uint16_t last;
        uint16_t b = g_pad.buttons;
        if ((b & XINPUT_GAMEPAD_DPAD_DOWN) && !(last & XINPUT_GAMEPAD_DPAD_DOWN)) sel = (sel + 1) % 5;
        if ((b & XINPUT_GAMEPAD_DPAD_UP) && !(last & XINPUT_GAMEPAD_DPAD_UP)) sel = (sel + 4) % 5;
        last = b;
        int step = (g_h - 300) / 5;
        for (int i = 0; i < 5; i++) {
            if (i == sel) fill(g_dc, 30, 220 + i * step, pw - 60, step - 8, RGB(16, 124, 16));
            text(g_dc, 50, 222 + i * step + (step - 8 - 32) / 2, 32, RGB(255, 255, 255), items[i]);
        }
        snprintf(buf, sizeof buf, "frame %u  pad 0x%04x", n, g_pad.buttons);
        text(g_dc, 40, g_h - 60, 24, RGB(140, 140, 140), buf);
    }
}

static int capture(uint32_t n)
{
    /* Exact colours for the System OS so the magenta key survives scaling. */
    SetStretchBltMode(g_dc, g_role == CR_ROLE_SYSTEM ? COLORONCOLOR : HALFTONE);
    if (g_src == SRC_TEST) {
        draw_test(n);
        return 1;
    }
    if (g_src == SRC_DESKTOP) {
        HDC s = GetDC(NULL);
        int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
        StretchBlt(g_dc, 0, 0, g_w, g_h, s, 0, 0, sw, sh, SRCCOPY | CAPTUREBLT);
        ReleaseDC(NULL, s);
        return 1;
    }
    HWND w = find_target();
    if (!w) return 0;
    RECT rc;
    GetClientRect(w, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return 0;
    HDC wdc = GetDC(w);
    HDC tmp = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
    HGDIOBJ old = SelectObject(tmp, bmp);
    /* PW_CLIENTONLY | PW_RENDERFULLCONTENT: works for DirectX windows on Windows 8.1+ */
    if (!PrintWindow(w, tmp, 1 | 2))
        BitBlt(tmp, 0, 0, rc.right, rc.bottom, wdc, 0, 0, SRCCOPY);
    StretchBlt(g_dc, 0, 0, g_w, g_h, tmp, 0, 0, rc.right, rc.bottom, SRCCOPY);
    SelectObject(tmp, old);
    DeleteObject(bmp);
    DeleteDC(tmp);
    ReleaseDC(w, wdc);
    return 1;
}

/* ---------- connection ---------- */

static SOCKET connect_host(void)
{
    SOCKET s;
    if (g_hv) {
        s = socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW);
        CR_SOCKADDR_HV a = {0};
        a.Family = AF_HYPERV;
        a.VmId = CR_HV_GUID_PARENT;
        a.ServiceId = g_hv_service;
        if (s != INVALID_SOCKET && connect(s, (struct sockaddr *)&a, sizeof a) == 0) return s;
    } else {
        struct addrinfo hints = {0}, *res = NULL;
        char port[16];
        snprintf(port, sizeof port, "%d", g_port);
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(g_host, port, &hints, &res) != 0) return INVALID_SOCKET;
        s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        int ok = s != INVALID_SOCKET && connect(s, res->ai_addr, (int)res->ai_addrlen) == 0;
        freeaddrinfo(res);
        if (ok) return s;
    }
    if (s != INVALID_SOCKET) closesocket(s);
    return INVALID_SOCKET;
}

static void run_session(SOCKET s)
{
    cr_tune_socket(s);
    cr_hello hello = { (uint32_t)g_role, (uint32_t)g_w, (uint32_t)g_h, {0} };
    snprintf(hello.name, sizeof hello.name, "%s", g_name ? g_name : (g_role == CR_ROLE_GAME ? "game" : "system"));
    if (!cr_send_msg(s, CR_HELLO, &hello, sizeof hello, NULL, 0)) return;
    HANDLE rt = CreateThread(NULL, 0, recv_thread, (LPVOID)(uintptr_t)s, 0, NULL);

    size_t stride = (size_t)g_w * 4;
    uint8_t *prev = calloc((size_t)g_h, stride);
    int first = 1;
    LONG last_lw = 0, last_lh = 0;
    uint32_t n = 0, seq = 0;
    LARGE_INTEGER freq, t0, t;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    timeBeginPeriod(1);
    for (;; n++) {
        if (WaitForSingleObject(rt, 0) == WAIT_OBJECT_0) break; /* host went away */
        /* Snap is a tall strip: render at the slot's shape instead of squeezing 16:9 into it. */
        LONG lw = g_layout_w, lh = g_layout_h;
        if ((g_src == SRC_TEST || g_follow_layout) && lw > 0 && lh > 0 && (lw != last_lw || lh != last_lh)) {
            last_lw = lw; last_lh = lh;
            int nw = (int)((long long)g_base_h * lw / lh) & ~3, nh = g_base_h;
            if (nw > g_base_w) { nw = g_base_w; nh = (int)((long long)g_base_w * lh / lw); }
            if (nw >= 64 && nh >= 64 && (nw != g_w || nh != g_h)) {
                g_w = nw; g_h = nh;
                make_target();
                stride = (size_t)g_w * 4;
                free(prev);
                prev = calloc((size_t)g_h, stride);
                first = 1;
            }
        }
        if (capture(n)) {
            if (g_role == CR_ROLE_SYSTEM) {
                /* snap near-magenta (JPEG wallpaper, scaling) to the exact key */
                uint32_t *px = (uint32_t *)g_bits;
                for (size_t i = 0, e = (size_t)g_w * g_h; i < e; i++) {
                    uint32_t c = px[i];
                    int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
                    if (r > 240 && g < 16 && b > 240)
                        px[i] = (CR_KEY_R << 16) | (CR_KEY_G << 8) | CR_KEY_B;
                }
            }
            /* send only the band of rows that changed */
            int y0 = -1, y1 = -1;
            for (int y = 0; y < g_h; y++) {
                if (first || memcmp(prev + y * stride, g_bits + y * stride, stride)) {
                    if (y0 < 0) y0 = y;
                    y1 = y;
                }
            }
            if (y0 >= 0) {
                cr_frame f = { (uint32_t)g_w, (uint32_t)g_h, (uint32_t)y0, (uint32_t)(y1 - y0 + 1), ++seq };
                if (!cr_send_msg(s, CR_FRAME, &f, sizeof f, g_bits + y0 * stride, f.rows * (uint32_t)stride)) break;
                memcpy(prev + y0 * stride, g_bits + y0 * stride, f.rows * stride);
                first = 0;
            }
        }
        /* pace to the target rate */
        LONGLONG next = t0.QuadPart + (LONGLONG)(n + 1) * freq.QuadPart / g_fps;
        for (;;) {
            QueryPerformanceCounter(&t);
            if (t.QuadPart >= next) break;
            Sleep((DWORD)((next - t.QuadPart) * 1000 / freq.QuadPart));
        }
    }
    timeEndPeriod(1);
    closesocket(s);
    WaitForSingleObject(rt, 2000);
    CloseHandle(rt);
    free(prev);
}

static WORD parse_key(const char *s)
{
    if ((s[0] == 'F' || s[0] == 'f') && atoi(s + 1) >= 1 && atoi(s + 1) <= 24) return (WORD)(VK_F1 + atoi(s + 1) - 1);
    if (!strcmp(s, "Home")) return VK_HOME;
    if (strlen(s) == 1) return (WORD)toupper((unsigned char)s[0]);
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
            "crimson-agent --role game|system [options]\n"
            "  --name NAME        label shown on the host HUD\n"
            "  --host ADDR        host address for TCP (default 127.0.0.1)\n"
            "  --port N           TCP port (default %d)\n"
            "  --hv               connect to the parent partition over a Hyper-V socket\n"
            "  --hv-service GUID  Hyper-V socket service id\n"
            "  --test             built-in test pattern (default)\n"
            "  --desktop          capture this machine's whole screen (VM mode)\n"
            "  --window TITLE     capture one window whose title contains TITLE (process mode)\n"
            "  --size WxH         stream size (default 1280x720)\n"
            "  --fps N            stream rate (default 30)\n"
            "  --guide-key KEY    System OS: key that toggles the shell's Guide (e.g. F1)\n"
            "  --follow-layout    resize the captured window to its Fill/Snap slot\n"
            "  --no-input         do not inject the host's controller as keys\n"
            "  --run CMDLINE      start this with no window (boot script in Era images)\n"
            "  --log FILE         write status lines to FILE\n",
            CR_TCP_PORT);
}

int main(int argc, char **argv)
{
    cr_parse_guid(CR_HV_SERVICE_STR, &g_hv_service);
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--role") && v) { g_role = !strcmp(v, "system") ? CR_ROLE_SYSTEM : CR_ROLE_GAME; i++; }
        else if (!strcmp(a, "--name") && v) { g_name = v; i++; }
        else if (!strcmp(a, "--host") && v) { g_host = v; i++; }
        else if (!strcmp(a, "--port") && v) { g_port = atoi(v); i++; }
        else if (!strcmp(a, "--hv")) g_hv = 1;
        else if (!strcmp(a, "--hv-service") && v) { g_hv = 1; cr_parse_guid(v, &g_hv_service); i++; }
        else if (!strcmp(a, "--test")) g_src = SRC_TEST;
        else if (!strcmp(a, "--desktop")) g_src = SRC_DESKTOP;
        else if (!strcmp(a, "--window") && v) { g_src = SRC_WINDOW; g_window_title = v; i++; }
        else if (!strcmp(a, "--size") && v) { sscanf(v, "%dx%d", &g_w, &g_h); i++; }
        else if (!strcmp(a, "--fps") && v) { g_fps = atoi(v); i++; }
        else if (!strcmp(a, "--guide-key") && v) { g_guide_key = parse_key(v); i++; }
        else if (!strcmp(a, "--follow-layout")) g_follow_layout = 1;
        else if (!strcmp(a, "--no-input")) g_input = 0;
        else if (!strcmp(a, "--run") && v) { g_run = v; i++; }
        else if (!strcmp(a, "--log") && v) { g_log = v; i++; }
        else { usage(); return 1; }
    }
    if (g_w < 64 || g_h < 64 || g_w > 3840 || g_h > 2160) g_w = 1280, g_h = 720;
    g_base_w = g_w; g_base_h = g_h;
    if (g_fps < 1 || g_fps > 120) g_fps = 30;

    if (g_log) {
        freopen(g_log, "a", stdout);
        setvbuf(stdout, NULL, _IONBF, 0);
    }
    if (g_run) {
        /* The partition's own boot script runs without a console window, so the
         * only thing on screen is what the game draws. */
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        memset(&si, 0, sizeof si);
        si.cb = sizeof si;
        char *cmd = _strdup(g_run);
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        } else {
            printf("could not start %s (%lu)\n", g_run, GetLastError());
        }
        free(cmd);
    }
    if (g_src == SRC_DESKTOP) {
        /* Inside a partition the desktop is the backdrop: black for the game,
         * the see-through key colour for the system. (WinPE draws no wallpaper.) */
        int elem = COLOR_DESKTOP;
        COLORREF c = g_role == CR_ROLE_SYSTEM ? RGB(CR_KEY_R, CR_KEY_G, CR_KEY_B) : RGB(0, 0, 0);
        SetSysColors(1, &elem, &c);
    }
    SetProcessDPIAware();
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    make_target();
    printf("crimson-agent: %s partition, %dx%d @ %d fps, %s\n",
           g_role == CR_ROLE_GAME ? "Game OS" : "System OS", g_w, g_h, g_fps,
           g_hv ? "Hyper-V socket to the parent partition" : g_host);
    for (;;) {
        SOCKET s = connect_host();
        if (s == INVALID_SOCKET) { Sleep(1000); continue; }
        printf("connected to host\n");
        run_session(s);
        printf("host link lost, retrying\n");
        InterlockedExchange(&g_location, CR_LOC_HIDDEN);
        Sleep(1000);
    }
}
