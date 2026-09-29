//
//  main.cpp
//
//  Host program for the portable Xbox boot animation: parses options, sets
//  up the platform (window or headless), feeds the animation its clock and
//  audio, and optionally captures frames and audio to files.
//
#include "platform/platform.h"
#include "gfx/gl_loader.h"
#include "gfx/d3d8_gl.h"
#include "sound/bootsound.h"
#include "io/image_io.h"
#include "compat/xbox_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include <algorithm>

bool Bootani_RunAnimation(int width, int height);
extern int  g_bShortVersion;
extern bool g_bBootaniLoop;

namespace {

struct Options
{
    bool        headless = false;
    bool        fullscreen = false;
    bool        vsync = true;
    bool        audio = true;
    bool        loop = false;
    bool        short_version = false;
    int         window_w = 1280;
    int         window_h = 960;
    int         msaa = 4;
    double      fps = 60.0;              // virtual frame rate when headless
    std::string frames_dir;              // write every Nth frame here
    int         capture_every = 1;
    std::vector<double> capture_at;      // seconds, headless
    std::string capture_prefix = "frame";
    std::string wav_path;                // headless audio capture
    bool        verbose = false;
};

Options      g_opt;
bool         g_quit = false;
double       g_virtual_ms = 0.0;
int          g_frame = 0;
size_t       g_next_capture = 0;
bool         g_audio_running = false;
bool         g_audio_device = false;
WavWriter*   g_wav = NULL;
uint64_t     g_wav_frames = 0;
std::vector<int16_t> g_audio_scratch;
uint64_t     g_start_ms = 0;             // windowed: wall clock at start

void Usage()
{
    printf(
"usage: bootani [options]\n"
"  --headless            render off-screen (EGL on Linux), no window\n"
"  --fullscreen          fullscreen window\n"
"  --size WxH            window size (default 1280x960)\n"
"  --no-vsync            don't wait for vertical blank\n"
"  --no-audio            silence\n"
"  --loop                repeat the animation until closed\n"
"  --short               the short variant the Xbox shows on warm boots\n"
"  --msaa N              back buffer multisampling (default 4, 1 = off)\n"
"  --fps F               headless: frames per second of animation time (default 60)\n"
"  --frames-dir DIR      write frames as PNG into DIR\n"
"  --every N             with --frames-dir, keep every Nth frame\n"
"  --capture-at T,T,...  with --frames-dir, only frames at these times (seconds)\n"
"  --wav FILE            headless: write the boot sound to a WAV file\n"
"  --verbose             print progress and debug output\n");
}

bool ParseSize(const char* s, int* w, int* h)
{
    return sscanf(s, "%dx%d", w, h) == 2 && *w > 0 && *h > 0;
}

bool ParseArgs(int argc, char** argv)
{
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        const char* next = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (a == "--headless") g_opt.headless = true;
        else if (a == "--fullscreen") g_opt.fullscreen = true;
        else if (a == "--no-vsync") g_opt.vsync = false;
        else if (a == "--no-audio") g_opt.audio = false;
        else if (a == "--loop") g_opt.loop = true;
        else if (a == "--short") g_opt.short_version = true;
        else if (a == "--verbose") g_opt.verbose = true;
        else if (a == "--size" && next) { if (!ParseSize(next, &g_opt.window_w, &g_opt.window_h)) return false; i++; }
        else if (a == "--msaa" && next) { g_opt.msaa = atoi(next); i++; }
        else if (a == "--fps" && next) { g_opt.fps = atof(next); if (g_opt.fps <= 0) return false; i++; }
        else if (a == "--frames-dir" && next) { g_opt.frames_dir = next; i++; }
        else if (a == "--every" && next) { g_opt.capture_every = std::max(1, atoi(next)); i++; }
        else if (a == "--wav" && next) { g_opt.wav_path = next; i++; }
        else if (a == "--capture-at" && next) {
            std::string list = next;
            size_t pos = 0;
            while (pos <= list.size()) {
                size_t comma = list.find(',', pos);
                if (comma == std::string::npos) comma = list.size();
                if (comma > pos) g_opt.capture_at.push_back(atof(list.substr(pos, comma - pos).c_str()));
                pos = comma + 1;
            }
            std::sort(g_opt.capture_at.begin(), g_opt.capture_at.end());
            i++;
        }
        else if (a == "--help" || a == "-h") { Usage(); exit(0); }
        else { fprintf(stderr, "bootani: unknown option %s\n", a.c_str()); return false; }
    }
    return true;
}

//------------------------------------------------------------------------------
// Audio

void AudioCallback(int16_t* out, int frames, void*)
{
    if (g_audio_running) BootSound_Render(out, frames);
    else memset(out, 0, frames * 4);
}

// Headless: render the audio that belongs to the animation time elapsed so far.
void CatchUpWavAudio()
{
    if (!g_wav) return;
    uint64_t due = (uint64_t)(g_virtual_ms * 48.0);
    if (due <= g_wav_frames) return;
    int frames = (int)(due - g_wav_frames);
    g_audio_scratch.resize(frames * 2);
    if (g_audio_running) BootSound_Render(&g_audio_scratch[0], frames);
    else std::fill(g_audio_scratch.begin(), g_audio_scratch.end(), 0);
    g_wav->Write(&g_audio_scratch[0], frames);
    g_wav_frames = due;
}

//------------------------------------------------------------------------------
// Frames

void CaptureFrame(unsigned int fbo, int w, int h, double t)
{
    std::vector<unsigned char> rgba(w * h * 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, &rgba[0]);
    char name[1024];
    snprintf(name, sizeof(name), "%s/%s_%05d_%06.3fs.png", g_opt.frames_dir.c_str(),
             g_opt.capture_prefix.c_str(), g_frame, t);
    // Row 0 of the framebuffer is the top of the picture (D3D layout).
    if (!WritePNG(name, &rgba[0], w, h))
        fprintf(stderr, "bootani: could not write %s\n", name);
    else if (g_opt.verbose)
        printf("wrote %s\n", name);
}

void OnPresent(unsigned int fbo, int w, int h, void*)
{
    double t = g_opt.headless ? g_virtual_ms / 1000.0
                              : (Platform_TicksMs() - g_start_ms) / 1000.0;

    if (!g_opt.frames_dir.empty()) {
        bool take;
        if (!g_opt.capture_at.empty()) {
            take = false;
            // Capture the first frame at or after each requested time.
            while (g_next_capture < g_opt.capture_at.size() && t + 1e-9 >= g_opt.capture_at[g_next_capture]) {
                take = true;
                g_next_capture++;
            }
        } else {
            take = (g_frame % g_opt.capture_every) == 0;
        }
        if (take) CaptureFrame(fbo, w, h, t);
    }

    if (!g_opt.headless) {
        Platform_ShowFrame(fbo, w, h);
        if (!Platform_PumpEvents()) g_quit = true;
    } else {
        g_virtual_ms += 1000.0 / g_opt.fps;
        CatchUpWavAudio();
    }
    g_frame++;
}

} // namespace

//------------------------------------------------------------------------------
// Services the animation calls (declared in anim/precomp.h).

DWORD Bootani_GetTickCount()
{
    if (g_opt.headless) return (DWORD)g_virtual_ms;
    return (DWORD)Platform_TicksMs();
}

bool Bootani_ShouldQuit() { return g_quit; }

void Bootani_AudioStart()
{
    if (!g_opt.audio) return;
    Platform_AudioLock();
    BootSound_Start();
    g_audio_running = true;
    Platform_AudioUnlock();
}

void Bootani_AudioStop()
{
    if (!g_audio_running) return;
    CatchUpWavAudio();
    Platform_AudioLock();
    g_audio_running = false;
    BootSound_Stop();
    Platform_AudioUnlock();
}

void Bootani_AudioRestart()
{
    if (!g_audio_running) return;
    Platform_AudioLock();
    BootSound_Restart();
    Platform_AudioUnlock();
}

void OutputDebugString(const char* s)
{
    if (g_opt.verbose) fputs(s, stderr);
}

int main(int argc, char** argv)
{
    if (!ParseArgs(argc, argv)) { Usage(); return 2; }
    g_bShortVersion = g_opt.short_version;
    g_bBootaniLoop = g_opt.loop && !g_opt.headless;

    PlatformConfig cfg;
    cfg.width = g_opt.window_w;
    cfg.height = g_opt.window_h;
    cfg.headless = g_opt.headless;
    cfg.fullscreen = g_opt.fullscreen;
    cfg.vsync = g_opt.vsync;
    if (!Platform_Init(cfg)) return 1;
    if (!bootani_gl_load(Platform_GetProcAddress)) { Platform_Shutdown(); return 1; }
    if (g_opt.verbose)
        printf("bootani: %s, GL %s, %s\n", Platform_Describe(),
               (const char*)glGetString(GL_VERSION), (const char*)glGetString(GL_RENDERER));

    if (!g_opt.frames_dir.empty() && !MakeDirectory(g_opt.frames_dir.c_str())) {
        fprintf(stderr, "bootani: cannot create %s\n", g_opt.frames_dir.c_str());
        return 1;
    }

    if (g_opt.audio) {
        if (g_opt.headless) {
            if (!g_opt.wav_path.empty()) {
                g_wav = new WavWriter();
                if (!g_wav->Open(g_opt.wav_path.c_str(), 48000, 2)) {
                    fprintf(stderr, "bootani: cannot write %s\n", g_opt.wav_path.c_str());
                    return 1;
                }
            } else {
                g_opt.audio = false;
            }
        } else {
            g_audio_device = Platform_AudioOpen(48000, AudioCallback, NULL);
            if (!g_audio_device) g_opt.audio = false;
        }
    }

    bootani_gl::SetBackBufferSamples(g_opt.msaa);
    bootani_gl::SetPresentHook(OnPresent, NULL);

    uint64_t t0 = Platform_TicksMs();
    g_start_ms = t0;
    bool ok = Bootani_RunAnimation(640, 480);
    uint64_t t1 = Platform_TicksMs();

    if (g_wav) {
        CatchUpWavAudio();
        g_wav->Close();
        delete g_wav;
    }
    if (g_opt.verbose || g_opt.headless)
        printf("bootani: %d frames, %.2f s animation time, %.2f s wall time\n",
               g_frame, g_opt.headless ? g_virtual_ms / 1000.0 : (t1 - t0) / 1000.0, (t1 - t0) / 1000.0);

    if (g_audio_device) Platform_AudioClose();
    Platform_Shutdown();
    return ok ? 0 : 1;
}
