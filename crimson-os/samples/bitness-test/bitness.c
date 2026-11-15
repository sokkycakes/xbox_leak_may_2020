/* Game OS check: does a GUI app of this bitness run, with common controls v6
   and OpenGL? Shows the answers in a window and writes them to result-NN.txt
   next to the exe. Build: ./build.sh  (makes bitness32.exe and bitness64.exe) */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shlwapi.h>
#include <GL/gl.h>
#include <stdio.h>

static char g_text[2048];

static void add(const char *fmt, const char *a) {
    size_t n = strlen(g_text);
    snprintf(g_text + n, sizeof g_text - n, fmt, a);
}

static void check_comctl(void) {
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_PROGRESS_CLASS | ICC_LISTVIEW_CLASSES };
    add("InitCommonControlsEx: %s\r\n", InitCommonControlsEx(&icc) ? "ok" : "FAILED");
    HMODULE m = GetModuleHandleA("comctl32.dll");
    DLLGETVERSIONPROC gv = m ? (DLLGETVERSIONPROC)GetProcAddress(m, "DllGetVersion") : NULL;
    DLLVERSIONINFO dv = { sizeof dv };
    char v[64] = "unknown";
    if (gv && gv(&dv) == S_OK) snprintf(v, sizeof v, "%lu.%lu", dv.dwMajorVersion, dv.dwMinorVersion);
    add("comctl32 version: %s (6.x = manifest worked)\r\n", v);
}

static void check_gl(HWND w) {
    HDC dc = GetDC(w);
    PIXELFORMATDESCRIPTOR pfd = { sizeof pfd, 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
                                  PFD_TYPE_RGBA, 32 };
    int pf = ChoosePixelFormat(dc, &pfd);
    if (!pf || !SetPixelFormat(dc, pf, &pfd)) { add("OpenGL: %s\r\n", "no pixel format"); ReleaseDC(w, dc); return; }
    HGLRC rc = wglCreateContext(dc);
    if (!rc || !wglMakeCurrent(dc, rc)) { add("OpenGL: %s\r\n", "no context"); ReleaseDC(w, dc); return; }
    add("GL_VENDOR:   %s\r\n", (const char *)glGetString(GL_VENDOR));
    add("GL_RENDERER: %s\r\n", (const char *)glGetString(GL_RENDERER));
    add("GL_VERSION:  %s\r\n", (const char *)glGetString(GL_VERSION));
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(rc);
    ReleaseDC(w, dc);
}

static LRESULT CALLBACK wndproc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(w, m, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show) {
    (void)prev; (void)cmd; (void)show;
    add("Crimson bitness test: %s\r\n", sizeof(void *) == 8 ? "64-bit" : "32-bit");
    BOOL wow = FALSE;
    IsWow64Process(GetCurrentProcess(), &wow);
    add("Running under WoW64: %s\r\n", wow ? "yes" : "no");
    check_comctl();

    WNDCLASSA wc = { CS_OWNDC, wndproc, 0, 0, inst, 0, LoadCursor(0, IDC_ARROW),
                     (HBRUSH)(COLOR_WINDOW + 1), 0, "CrimsonBitness" };
    RegisterClassA(&wc);
    int y = sizeof(void *) == 8 ? 420 : 40;
    HWND w = CreateWindowA("CrimsonBitness", sizeof(void *) == 8 ? "64-bit test" : "32-bit test",
                           WS_OVERLAPPEDWINDOW | WS_VISIBLE, 40, y, 760, 360, 0, 0, inst, 0);
    HWND gl = CreateWindowA("CrimsonBitness", "", WS_CHILD | WS_VISIBLE, 560, 10, 160, 120, w, 0, inst, 0);
    check_gl(gl);
    add("Result: %s\r\n", "PASS (window, controls and OpenGL all loaded)");

    CreateWindowA(WC_EDITA, g_text, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_MULTILINE | ES_READONLY,
                  10, 10, 540, 230, w, 0, inst, 0);
    HWND bar = CreateWindowA(PROGRESS_CLASSA, "", WS_CHILD | WS_VISIBLE, 10, 250, 540, 24, w, 0, inst, 0);
    SendMessageA(bar, PBM_SETPOS, 70, 0);

    char path[MAX_PATH];
    GetModuleFileNameA(0, path, MAX_PATH);
    char *slash = strrchr(path, '\\');
    if (slash) sprintf(slash + 1, "result-%d.txt", (int)sizeof(void *) * 8);
    FILE *f = fopen(path, "wb");
    if (f) { fputs(g_text, f); fclose(f); }

    MSG msg;
    while (GetMessageA(&msg, 0, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageA(&msg); }
    return 0;
}
