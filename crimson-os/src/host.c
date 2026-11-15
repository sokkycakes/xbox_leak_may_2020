/* crimson-host: the host OS side of the Crimson three-partition prototype.
 *
 * Plays the part of the Xbox One host OS:
 *   - owns the physical controller and the Xbox (Guide) button,
 *   - routes input to whichever partition has focus,
 *   - composites the Game OS and System OS outputs (Full, Fill/Snap, Guide
 *     overlay, Home) into one full screen window,
 *   - tells each partition where it sits so it can react (resize, open Guide).
 *
 * Partitions connect over TCP (default port 7700) and, when --hv is given,
 * over Hyper-V sockets, which is how the VMs reach the host.
 *
 * Controls (controller / keyboard):
 *   Xbox button tap        / Home    Guide overlay on/off; in Snap, swap focus
 *   Xbox button hold       / End     Home (System OS full screen, game constrained)
 *   Xbox button + RB       / F2      Snap on/off
 *   arrows Enter Backspace X Y       sent to the focused partition as a pad
 *   F12  HUD on/off       Shift+Esc  quit
 */
#include "net.h"
#include <xinput.h>
#include <stdlib.h>
#include <stdint.h>

#define SNAP_FRACTION 0.25 /* the 2013 snap strip took about a quarter of the screen */
#define GUIDE_HOLD_MS 700
#define XINPUT_GUIDE  0x0400

enum mode { MODE_FULL, MODE_OVERLAY, MODE_SNAP, MODE_HOME };
static const char *mode_name[] = { "Full", "Guide", "Snap", "Home" };

typedef struct {
    CRITICAL_SECTION lock;     /* guards everything below */
    SOCKET sock;
    int connected;
    char name[33];
    HBITMAP dib;               /* latest frame, BGRA top-down */
    uint8_t *bits;
    int w, h;
    uint32_t frames, fps;      /* frames this second / last second */
    CRITICAL_SECTION send_lock;
} partition;

static partition parts[3]; /* index by cr_role */
static HWND g_wnd;
static int g_mode = MODE_FULL, g_prev_game_mode = MODE_FULL;
static int g_focus = CR_ROLE_GAME;
static int g_hud = 1;
static int g_port = CR_TCP_PORT;
static int g_hv = 0;
static int g_windowed = 0;
static GUID g_hv_service;

/* ---------- networking ---------- */

static void send_to(int role, uint16_t type, const void *body, uint32_t len)
{
    partition *p = &parts[role];
    EnterCriticalSection(&p->send_lock);
    if (p->connected) cr_send_msg(p->sock, type, body, len, NULL, 0);
    LeaveCriticalSection(&p->send_lock);
}

static void layout_for(int role, RECT *r, uint32_t *loc);

static void push_layout(void)
{
    for (int role = CR_ROLE_GAME; role <= CR_ROLE_SYSTEM; role++) {
        RECT r; uint32_t loc;
        layout_for(role, &r, &loc);
        cr_layout l = { loc, (uint32_t)(g_focus == role), (uint32_t)(r.right - r.left),
                        (uint32_t)(r.bottom - r.top) };
        send_to(role, CR_LAYOUT, &l, sizeof l);
    }
    InvalidateRect(g_wnd, NULL, FALSE);
}

static DWORD WINAPI conn_thread(LPVOID arg)
{
    SOCKET s = (SOCKET)(uintptr_t)arg;
    cr_hdr h;
    cr_hello hello;
    cr_tune_socket(s);
    if (!cr_recv_all(s, &h, sizeof h) || h.magic != CR_MAGIC || h.type != CR_HELLO ||
        h.len != sizeof hello || !cr_recv_all(s, &hello, sizeof hello) ||
        (hello.role != CR_ROLE_GAME && hello.role != CR_ROLE_SYSTEM)) {
        closesocket(s);
        return 0;
    }
    int role = (int)hello.role;
    partition *p = &parts[role];

    EnterCriticalSection(&p->send_lock);
    EnterCriticalSection(&p->lock);
    if (p->connected) closesocket(p->sock); /* newest connection wins */
    p->sock = s;
    p->connected = 1;
    memcpy(p->name, hello.name, 32);
    p->name[32] = 0;
    LeaveCriticalSection(&p->lock);
    LeaveCriticalSection(&p->send_lock);
    PostMessage(g_wnd, WM_APP, 0, 0); /* resend layouts from the UI thread */

    uint8_t *rowbuf = NULL;
    size_t rowcap = 0;
    for (;;) {
        if (!cr_recv_all(s, &h, sizeof h) || h.magic != CR_MAGIC) break;
        if (h.type == CR_FRAME && h.len >= sizeof(cr_frame)) {
            cr_frame f;
            if (!cr_recv_all(s, &f, sizeof f)) break;
            size_t need = (size_t)h.len - sizeof f;
            if (f.width == 0 || f.width > 7680 || f.height == 0 || f.height > 4320 ||
                f.y0 + f.rows > f.height || need != (size_t)f.rows * f.width * 4)
                break;
            if (need > rowcap) {
                free(rowbuf);
                rowcap = need;
                rowbuf = malloc(rowcap);
                if (!rowbuf) break;
            }
            if (need && !cr_recv_all(s, rowbuf, (int)need)) break;

            EnterCriticalSection(&p->lock);
            if (!p->dib || p->w != (int)f.width || p->h != (int)f.height) {
                if (p->dib) DeleteObject(p->dib);
                BITMAPINFO bi = {0};
                bi.bmiHeader.biSize = sizeof bi.bmiHeader;
                bi.bmiHeader.biWidth = (LONG)f.width;
                bi.bmiHeader.biHeight = -(LONG)f.height;
                bi.bmiHeader.biPlanes = 1;
                bi.bmiHeader.biBitCount = 32;
                p->dib = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, (void **)&p->bits, NULL, 0);
                p->w = (int)f.width;
                p->h = (int)f.height;
            }
            if (p->dib && need)
                memcpy(p->bits + (size_t)f.y0 * f.width * 4, rowbuf, need);
            p->frames++;
            LeaveCriticalSection(&p->lock);
            InvalidateRect(g_wnd, NULL, FALSE);
        } else {
            /* skip anything we do not understand */
            char tmp[1024];
            uint32_t left = h.len;
            while (left) {
                int n = left > sizeof tmp ? (int)sizeof tmp : (int)left;
                if (!cr_recv_all(s, tmp, n)) goto done;
                left -= (uint32_t)n;
            }
        }
    }
done:
    free(rowbuf);
    EnterCriticalSection(&p->send_lock);
    EnterCriticalSection(&p->lock);
    if (p->sock == s) p->connected = 0;
    LeaveCriticalSection(&p->lock);
    LeaveCriticalSection(&p->send_lock);
    closesocket(s);
    InvalidateRect(g_wnd, NULL, FALSE);
    return 0;
}

static DWORD WINAPI listen_thread(LPVOID arg)
{
    SOCKET ls = (SOCKET)(uintptr_t)arg;
    for (;;) {
        SOCKET s = accept(ls, NULL, NULL);
        if (s == INVALID_SOCKET) { Sleep(100); continue; }
        CloseHandle(CreateThread(NULL, 0, conn_thread, (LPVOID)(uintptr_t)s, 0, NULL));
    }
    return 0;
}

static int start_listeners(void)
{
    int ok = 0;
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)g_port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (ls != INVALID_SOCKET && bind(ls, (struct sockaddr *)&a, sizeof a) == 0 && listen(ls, 4) == 0) {
        CloseHandle(CreateThread(NULL, 0, listen_thread, (LPVOID)(uintptr_t)ls, 0, NULL));
        ok = 1;
    }
    if (g_hv) {
        SOCKET hs = socket(AF_HYPERV, SOCK_STREAM, HV_PROTOCOL_RAW);
        CR_SOCKADDR_HV ha = {0};
        ha.Family = AF_HYPERV;
        ha.VmId = CR_HV_GUID_WILDCARD;
        ha.ServiceId = g_hv_service;
        if (hs != INVALID_SOCKET && bind(hs, (struct sockaddr *)&ha, sizeof ha) == 0 && listen(hs, 4) == 0) {
            CloseHandle(CreateThread(NULL, 0, listen_thread, (LPVOID)(uintptr_t)hs, 0, NULL));
            ok = 1;
        } else {
            MessageBoxA(NULL, "Could not listen on the Hyper-V socket. Is Hyper-V on, and was "
                        "New-CrimsonPartitions.ps1 run (it registers the service id)?",
                        "crimson-host", MB_ICONWARNING);
        }
    }
    return ok;
}

/* ---------- layout ---------- */

static void client_size(int *w, int *h)
{
    RECT rc;
    GetClientRect(g_wnd, &rc);
    *w = rc.right;
    *h = rc.bottom;
}

static void layout_for(int role, RECT *r, uint32_t *loc)
{
    int W, H;
    client_size(&W, &H);
    int snap_w = (int)(W * SNAP_FRACTION), gap = W / 160;
    SetRect(r, 0, 0, W, H);
    if (role == CR_ROLE_GAME) {
        switch (g_mode) {
        case MODE_SNAP: SetRect(r, 0, 0, W - snap_w - gap, H); *loc = CR_LOC_FILL; break;
        case MODE_HOME: *loc = CR_LOC_BACKGROUND; break;
        default:        *loc = CR_LOC_FULL; break;
        }
    } else {
        switch (g_mode) {
        case MODE_SNAP:    SetRect(r, W - snap_w, 0, W, H); *loc = CR_LOC_SNAP; break;
        case MODE_OVERLAY: *loc = CR_LOC_OVERLAY; break;
        case MODE_HOME:    *loc = CR_LOC_FULL; break;
        default:           *loc = CR_LOC_HIDDEN; break;
        }
    }
}

static void set_mode(int mode, int focus)
{
    if (mode == MODE_FULL || mode == MODE_SNAP) g_prev_game_mode = mode;
    g_mode = mode;
    g_focus = focus;
    push_layout();
}

/* The three host-reserved gestures. */
static void guide_tap(void)
{
    switch (g_mode) {
    case MODE_FULL:    set_mode(MODE_OVERLAY, CR_ROLE_SYSTEM); break;
    case MODE_OVERLAY: set_mode(MODE_FULL, CR_ROLE_GAME); break;
    case MODE_SNAP:    set_mode(MODE_SNAP, g_focus == CR_ROLE_GAME ? CR_ROLE_SYSTEM : CR_ROLE_GAME); break;
    case MODE_HOME:    set_mode(g_prev_game_mode, g_prev_game_mode == MODE_SNAP ? CR_ROLE_SYSTEM : CR_ROLE_GAME); break;
    }
}
static void guide_hold(void) { if (g_mode != MODE_HOME) set_mode(MODE_HOME, CR_ROLE_SYSTEM); }
static void snap_toggle(void)
{
    if (g_mode == MODE_SNAP) set_mode(MODE_FULL, CR_ROLE_GAME);
    else set_mode(MODE_SNAP, CR_ROLE_SYSTEM);
}

/* ---------- input ---------- */

typedef DWORD (WINAPI *xinput_get_t)(DWORD, XINPUT_STATE *);
static xinput_get_t xget;
static uint16_t g_key_buttons; /* virtual pad driven by the host keyboard */
static uint16_t g_key_latch;   /* keys pressed since the last poll, so quick taps are not lost */
static cr_input g_last_sent;
static uint32_t g_input_seq;

static void load_xinput(void)
{
    HMODULE m = LoadLibraryA("xinput1_4.dll");
    if (!m) m = LoadLibraryA("xinput1_3.dll");
    if (!m) return;
    /* Ordinal 100 is XInputGetStateEx, the only call that reports the Xbox button. */
    xget = (xinput_get_t)GetProcAddress(m, (LPCSTR)100);
    if (!xget) xget = (xinput_get_t)GetProcAddress(m, "XInputGetState");
}

static void poll_input(void)
{
    static DWORD guide_down_at;
    static int guide_down, guide_used;
    XINPUT_GAMEPAD pad = {0};
    if (xget) {
        for (DWORD i = 0; i < 4; i++) {
            struct { XINPUT_STATE s; BYTE pad_[16]; } st; /* Ex struct is a little larger */
            memset(&st, 0, sizeof st);
            if (xget(i, &st.s) == ERROR_SUCCESS) { pad = st.s.Gamepad; break; }
        }
    }
    uint16_t buttons = pad.wButtons | g_key_buttons | g_key_latch;
    g_key_latch = 0;

    int guide = (buttons & XINPUT_GUIDE) != 0;
    if (guide && !guide_down) { guide_down = 1; guide_used = 0; guide_down_at = GetTickCount(); }
    if (guide_down && guide && !guide_used) {
        if (buttons & XINPUT_GAMEPAD_RIGHT_SHOULDER) { snap_toggle(); guide_used = 1; }
        else if (GetTickCount() - guide_down_at >= GUIDE_HOLD_MS) { guide_hold(); guide_used = 1; }
    }
    if (!guide && guide_down) {
        guide_down = 0;
        if (!guide_used) guide_tap();
    }

    /* The Xbox button belongs to the host, as on the console. While it is held
     * the partitions see an idle pad so chords do not leak through. */
    cr_input in = {0};
    if (!guide_down) {
        in.buttons = buttons & ~XINPUT_GUIDE;
        in.lt = pad.bLeftTrigger; in.rt = pad.bRightTrigger;
        in.lx = pad.sThumbLX; in.ly = pad.sThumbLY;
        in.rx = pad.sThumbRX; in.ry = pad.sThumbRY;
    }
    in.seq = g_last_sent.seq;
    if (memcmp(&in, &g_last_sent, sizeof in) != 0) {
        in.seq = ++g_input_seq;
        g_last_sent = in;
        send_to(g_focus, CR_INPUT, &in, sizeof in);
    }
}

static uint16_t key_to_button(WPARAM vk)
{
    switch (vk) {
    case VK_UP: return XINPUT_GAMEPAD_DPAD_UP;
    case VK_DOWN: return XINPUT_GAMEPAD_DPAD_DOWN;
    case VK_LEFT: return XINPUT_GAMEPAD_DPAD_LEFT;
    case VK_RIGHT: return XINPUT_GAMEPAD_DPAD_RIGHT;
    case VK_RETURN: case VK_SPACE: return XINPUT_GAMEPAD_A;
    case VK_BACK: case VK_ESCAPE: return XINPUT_GAMEPAD_B;
    case 'X': return XINPUT_GAMEPAD_X;
    case 'Y': return XINPUT_GAMEPAD_Y;
    case VK_TAB: return XINPUT_GAMEPAD_START;
    }
    return 0;
}

/* ---------- drawing ---------- */

static RECT fit(RECT box, int w, int h)
{
    int bw = box.right - box.left, bh = box.bottom - box.top;
    RECT r = box;
    if (w <= 0 || h <= 0) return r;
    if ((long long)bw * h > (long long)bh * w) { /* box is wider: pillarbox */
        int nw = (int)((long long)bh * w / h);
        r.left += (bw - nw) / 2; r.right = r.left + nw;
    } else {
        int nh = (int)((long long)bw * h / w);
        r.top += (bh - nh) / 2; r.bottom = r.top + nh;
    }
    return r;
}

static void draw_text(HDC dc, RECT r, const char *s, COLORREF c, UINT fmt)
{
    SetTextColor(dc, c);
    DrawTextA(dc, s, -1, &r, fmt);
}

static void draw_partition(HDC dc, HDC src, int role, RECT box, int keyed)
{
    partition *p = &parts[role];
    EnterCriticalSection(&p->lock);
    if (p->connected && p->dib) {
        RECT r = fit(box, p->w, p->h);
        HGDIOBJ old = SelectObject(src, p->dib);
        SetStretchBltMode(dc, HALFTONE);
        if (keyed)
            TransparentBlt(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, src, 0, 0, p->w, p->h,
                           RGB(CR_KEY_R, CR_KEY_G, CR_KEY_B));
        else
            StretchBlt(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, src, 0, 0, p->w, p->h, SRCCOPY);
        SelectObject(src, old);
    } else if (!keyed) {
        HBRUSH b = CreateSolidBrush(RGB(16, 16, 16));
        FillRect(dc, &box, b);
        DeleteObject(b);
        draw_text(dc, box, role == CR_ROLE_GAME ? "Game OS partition is not running"
                                                : "System OS partition is not running",
                  RGB(140, 140, 140), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    LeaveCriticalSection(&p->lock);
}

static void dim(HDC dc, HDC src, RECT r, BYTE alpha)
{
    static HBITMAP black;
    if (!black) {
        BITMAPINFO bi = {0};
        void *bits;
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = 1; bi.bmiHeader.biHeight = 1;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        black = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        memset(bits, 0, 4);
    }
    HGDIOBJ old = SelectObject(src, black);
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, alpha, 0 };
    AlphaBlend(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, src, 0, 0, 1, 1, bf);
    SelectObject(src, old);
}

static void frame_rect(HDC dc, RECT r, int t, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    RECT e;
    SetRect(&e, r.left, r.top, r.right, r.top + t); FillRect(dc, &e, b);
    SetRect(&e, r.left, r.bottom - t, r.right, r.bottom); FillRect(dc, &e, b);
    SetRect(&e, r.left, r.top, r.left + t, r.bottom); FillRect(dc, &e, b);
    SetRect(&e, r.right - t, r.top, r.right, r.bottom); FillRect(dc, &e, b);
    DeleteObject(b);
}

static void paint(HDC wdc)
{
    int W, H;
    client_size(&W, &H);
    if (W <= 0 || H <= 0) return;
    HDC dc = CreateCompatibleDC(wdc), src = CreateCompatibleDC(wdc);
    HBITMAP back = CreateCompatibleBitmap(wdc, W, H);
    HGDIOBJ oldb = SelectObject(dc, back);
    RECT all = {0, 0, W, H}, gr, sr;
    uint32_t gl, sl;
    FillRect(dc, &all, (HBRUSH)GetStockObject(BLACK_BRUSH));
    SetBkMode(dc, TRANSPARENT);
    layout_for(CR_ROLE_GAME, &gr, &gl);
    layout_for(CR_ROLE_SYSTEM, &sr, &sl);

    if (gl != CR_LOC_BACKGROUND) draw_partition(dc, src, CR_ROLE_GAME, gr, 0);
    switch (sl) {
    case CR_LOC_OVERLAY:
        dim(dc, src, all, 110);
        draw_partition(dc, src, CR_ROLE_SYSTEM, sr, 1);
        break;
    case CR_LOC_SNAP: {
        HBRUSH b = CreateSolidBrush(RGB(24, 24, 24));
        FillRect(dc, &sr, b);
        DeleteObject(b);
        draw_partition(dc, src, CR_ROLE_SYSTEM, sr, 1);
        frame_rect(dc, g_focus == CR_ROLE_GAME ? gr : sr, 4, RGB(240, 240, 240));
        break;
    }
    case CR_LOC_FULL:
        draw_partition(dc, src, CR_ROLE_SYSTEM, sr, 0);
        break;
    }

    if (g_hud) {
        char line[256];
        partition *g = &parts[CR_ROLE_GAME], *s = &parts[CR_ROLE_SYSTEM];
        snprintf(line, sizeof line,
                 "HOST  mode %s  |  GAME OS %s %u fps%s  |  SYSTEM OS %s %u fps%s  |  F12 hides",
                 mode_name[g_mode],
                 g->connected ? g->name : "offline", g->fps, g_focus == CR_ROLE_GAME ? " [focus]" : "",
                 s->connected ? s->name : "offline", s->fps, g_focus == CR_ROLE_SYSTEM ? " [focus]" : "");
        RECT hr = {0, H - 28, W, H};
        dim(dc, src, hr, 160);
        hr.left += 12;
        draw_text(dc, hr, line, RGB(16, 220, 16), DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }

    BitBlt(wdc, 0, 0, W, H, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldb);
    DeleteObject(back);
    DeleteDC(dc);
    DeleteDC(src);
}

/* ---------- window ---------- */

static LRESULT CALLBACK wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_TIMER:
        if (wp == 1) poll_input();
        if (wp == 2) {
            for (int r = CR_ROLE_GAME; r <= CR_ROLE_SYSTEM; r++) {
                EnterCriticalSection(&parts[r].lock);
                parts[r].fps = parts[r].frames;
                parts[r].frames = 0;
                LeaveCriticalSection(&parts[r].lock);
            }
            InvalidateRect(w, NULL, FALSE);
        }
        return 0;
    case WM_APP:
        push_layout();
        return 0;
    case WM_SIZE:
        push_layout();
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE && (GetKeyState(VK_SHIFT) & 0x8000)) { DestroyWindow(w); return 0; }
        if (wp == VK_F12) { g_hud = !g_hud; InvalidateRect(w, NULL, FALSE); return 0; }
        if (wp == VK_HOME) { if (!(lp & (1 << 30))) guide_tap(); return 0; }
        if (wp == VK_END) { guide_hold(); return 0; }
        if (wp == VK_F2) { snap_toggle(); return 0; }
        g_key_buttons |= key_to_button(wp);
        g_key_latch |= key_to_button(wp);
        return 0;
    case WM_KEYUP:
        g_key_buttons &= ~key_to_button(wp);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(w, &ps);
        paint(dc);
        EndPaint(w, &ps);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(w, m, wp, lp);
}

static void usage(void)
{
    MessageBoxA(NULL,
                "crimson-host [--port N] [--hv] [--hv-service GUID] [--windowed]\n\n"
                "--hv        also accept partitions over Hyper-V sockets (VM mode)\n"
                "--windowed  1280x720 window instead of full screen",
                "crimson-host", MB_OK);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    (void)prev; (void)cmdline; (void)show;
    cr_parse_guid(CR_HV_SERVICE_STR, &g_hv_service);
    for (int i = 1; i < __argc; i++) {
        const char *a = __argv[i];
        if (!strcmp(a, "--port") && i + 1 < __argc) g_port = atoi(__argv[++i]);
        else if (!strcmp(a, "--hv")) g_hv = 1;
        else if (!strcmp(a, "--hv-service") && i + 1 < __argc) { g_hv = 1; cr_parse_guid(__argv[++i], &g_hv_service); }
        else if (!strcmp(a, "--windowed")) g_windowed = 1;
        else { usage(); return 1; }
    }

    SetProcessDPIAware();
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    for (int r = 0; r < 3; r++) {
        InitializeCriticalSection(&parts[r].lock);
        InitializeCriticalSection(&parts[r].send_lock);
    }
    load_xinput();

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "CrimsonHost";
    RegisterClassA(&wc);
    if (g_windowed) {
        RECT r = {0, 0, 1280, 720};
        AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
        g_wnd = CreateWindowA("CrimsonHost", "Crimson host", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                              CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                              NULL, NULL, inst, NULL);
    } else {
        g_wnd = CreateWindowA("CrimsonHost", "Crimson host", WS_POPUP | WS_VISIBLE, 0, 0,
                              GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN),
                              NULL, NULL, inst, NULL);
    }
    if (!start_listeners()) {
        MessageBoxA(NULL, "Could not open the partition port.", "crimson-host", MB_ICONERROR);
        return 1;
    }
    SetTimer(g_wnd, 1, 8, NULL);    /* input poll, ~120 Hz */
    SetTimer(g_wnd, 2, 1000, NULL); /* fps counters */
    push_layout();

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    WSACleanup();
    return 0;
}
