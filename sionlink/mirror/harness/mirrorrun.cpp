// Headless melonDS runner for the screen-mirroring test.
//
//   mirrorrun ROM FRAMES [options]
//     --input FILE     key script: lines "FRAME KEYS [TOUCH X Y]", KEYS like A+START or -
//     --record FILE    write one mirror bundle per frame (see common/mirror_format.h)
//     --frames FILE    write every frame as raw 32-bit pixels, top then bottom screen
//     --shots DIR      write a PPM of both screens every --every frames (default 60)
//     --stats FILE     per-frame bundle sizes as CSV
//     --lang N         firmware language (0 Japanese, 1 English, ...)
//     --half 1         30 fps: record every other frame, the client shows each for two
//     --gxdelta 1      send 3D commands as copies from the previous bundle's where they repeat
//     --serve LINK     live: stream the bundles to the DS and play with its pad;
//                      LINK is sim:PATH (a --dspico run) or usb (the DSpico on this machine)
//     --reconnect 1    when the DS goes away (switched off), wait for it to come back
//                      instead of stopping
//     --hotplug 1      start the game at once and let the DS come and go: a thread waits
//                      for it, and until it's there no bundles are made, only the state
//                      a full one needs when it comes
//     --pads 1         the Sion's own pads play too (pads.h; built with SDL2)
//     --save FILE      the game's save memory: read at start, written back after it saves
//     --dspico PATH    run a DS client with a simulated DSpico whose USB host is the
//                      --serve run listening on PATH
//     --usbrate N      simulated USB bytes per frame (default 12000, ~700 KB/s)
//     --realtime 1     run at the DS's 59.83 frames a second instead of flat out
//     --jit 1          run the game with melonDS's JIT (faster)
//     --threaded 1     draw 3D on a second thread (faster on a multi-core host, same picture)
//     --gamecode XXXX  replace the header's game code, for prototypes whose code melonDS's
//                      cart list gives to another game (wrong cart/save type)
//     --tv OUT         the game on a TV too, both screens stacked (see tvout.h): drm (the
//                      Sion's display, taken over from xbox-av; the game then runs at the
//                      TV's pace) or dump:FILE (every 60th picture, 720x480 raw, for tests)
//     --sound OUT      the game's sound (see soundout.h): alsa ($AUDIODEV, the TV's HDMI
//                      on the Sion) or wav:FILE
//     --stretch 0      where the game runs slow (a scene too heavy for the Sion), let the
//                      sound break up instead of stretching it with the game (up to 15%)
//
// SIGTERM or SIGINT stops it cleanly (the TV goes back to xbox-av); a second one at once.
//
// melonDS is GPL; this runner links it and stays GPL with it.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <string>
#include <vector>
#include <deque>
#include <map>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <functional>
#include <utility>
#include <zlib.h>
#include <csignal>
#include <dirent.h>
#include <pthread.h>
#include <unistd.h>

#include "NDS.h"
#include "NDSCart.h"
#include "GPU.h"
#include "Platform.h"
#include "MirrorTap.h"
#include "Args.h"
#include "SPI.h"
#include "../common/mirror_format.h"
#include "hostlink.h"
#include "pads.h"
#include "saves.h"
#include "dspico_sim.h"
#include "lzpack.h"
#include "geostate.h"
#include "tvout.h"
#include "soundout.h"

using namespace melonDS;


// ---------------------------------------------------------------- 30 fps mode
//
// With --half the client gets every other frame and shows each for two. A frame
// that isn't sent still has 3D commands that leave state behind for later frames
// (matrix stacks, lights, materials, texture settings). GXFilter walks the
// geometry command stream: for a frame that is sent, the writes pass through as
// they are; for a skipped frame, only the state commands are kept, re-encoded as
// direct command port writes, and geometry, tests and SWAP_BUFFERS are dropped.
// Packets and commands that straddle the two are finished in the same form they
// were started in, so the client's FIFO always sees whole commands.

static u8 GXParamCount(u8 c)
{
    switch (c)
    {
    case 0x16: case 0x18: return 16;
    case 0x17: case 0x19: return 12;
    case 0x1A: return 9;
    case 0x1B: case 0x1C: case 0x70: return 3;
    case 0x23: case 0x71: return 2;
    case 0x34: return 32;
    case 0x11: case 0x15: case 0x41: return 0;
    default: return c >= 0x10 ? 1 : 0;
    }
}

static bool GXValid(u8 c)
{
    return (c >= 0x10 && c <= 0x1C) || (c >= 0x20 && c <= 0x2B) || (c >= 0x30 && c <= 0x34) ||
           c == 0x40 || c == 0x41 || c == 0x50 || c == 0x60 || (c >= 0x70 && c <= 0x72);
}

// state a skipped frame must still apply: matrices, vertex color, polygon/texture
// attributes, lighting and material, viewport
static bool GXKeepWhenSkipped(u8 c)
{
    return (c >= 0x10 && c <= 0x1C) || c == 0x20 || (c >= 0x29 && c <= 0x2B) ||
           (c >= 0x30 && c <= 0x34) || c == 0x60;
}

// How busy each of mirrorrun's threads has been since the last call, in % of one core:
// on the Sion's two cores the game, melonDS's 3D thread, the USB sender and the sound
// share them.
static std::string CpuUse(double seconds)
{
    static std::map<std::string, unsigned long long> before;
    std::map<std::string, unsigned long long> now;
    if (DIR* d = opendir("/proc/self/task"))
    {
        while (dirent* e = readdir(d))
        {
            if (e->d_name[0] == '.') continue;
            char path[300], line[512];
            snprintf(path, sizeof path, "/proc/self/task/%s/stat", e->d_name);
            FILE* f = fopen(path, "r");
            if (!f) continue;
            size_t n = fread(line, 1, sizeof line - 1, f);
            fclose(f);
            line[n] = 0;
            // pid (comm) state ... utime stime are fields 14 and 15
            char* open = strchr(line, '('), *close = strrchr(line, ')');
            if (!open || !close) continue;
            std::string comm(open + 1, close);
            unsigned long long ut = 0, st = 0;
            if (sscanf(close + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) == 2)
                now[comm] += ut + st;
        }
        closedir(d);
    }
    static const long hz = sysconf(_SC_CLK_TCK);
    std::string out;
    for (auto& [name, ticks] : now)
    {
        unsigned long long was = before.count(name) ? before[name] : 0;
        int pct = (int)((ticks - was) * 100.0 / hz / seconds + 0.5);
        if (pct < 1) continue;
        static const std::map<std::string, std::string> nice = {
            {"mirrorrun", "game"}, {"mirror-3d", "3D"}, {"mirror-send", "USB"}, {"mirror-sound", "sound"}};
        auto it = nice.find(name);
        char part[300];
        snprintf(part, sizeof part, "%s%s %d%%", out.empty() ? "" : ", ", it == nice.end() ? name.c_str() : it->second.c_str(), pct);
        out += part;
    }
    before = now;
    return out;
}

// MIRROR_PROF=FROM:TO (cloud tests): where the time goes in frames FROM..TO-1, per frame
struct Prof
{
    enum { Snap, Lines, Emit, Skipped, Sync, Frame, Tv, EmitFilter, EmitPack, EmitDelta, N };
    double ms[N] = {};
    u32 from = 0, to = 0, frames = 0;
    bool on = false;
    Prof()
    {
        if (const char* p = getenv("MIRROR_PROF")) on = sscanf(p, "%u:%u", &from, &to) == 2;
    }
    struct Timer
    {
        Prof& p; int what; std::chrono::steady_clock::time_point t;
        Timer(Prof& p, int what) : p(p), what(what) { if (p.on) t = std::chrono::steady_clock::now(); }
        ~Timer() { if (p.on) p.ms[what] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count(); }
    };
    bool Active(u32 f) { return on && f >= from && f < to; }
} gProf;
#define PROF(what) Prof::Timer profTimer_(gProf, Prof::what)

struct GXFilter
{
    struct IOW { u16 off; u8 size; u32 val; };

    // FIFO packet in progress
    u8 cmds[4]; int ncmds = 0, ci = 0;
    std::vector<u32> params;
    bool packetRaw = false;
    // direct port command in progress
    u8 dcmd = 0;
    std::vector<u32> dparams;
    bool dActive = false, dRaw = false;

    u32 kept = 0, dropped = 0;  // skipped-frame commands kept / dropped, for stats
    u32 badCmds = 0;            // unknown command bytes: the parse lost sync

    // The geometry state the client's commands leave it in, so that what a run of
    // left-out frames did can go as the few commands that rebuild its end state
    // (GeoState::Rebuild) instead of every state command they had: after 40-60 left
    // out in a row, those came to 600-860 KB a bundle and slowed the link down further.
    GeoState geo;
    GeoState geoBase;           // where the left-out frames' kept commands began
    bool baseSet = false;       // geoBase holds that
    size_t headLen = 0;         // writes in the carry before then: a packet the client was mid-way through
    bool noRebuild = false;     // the carry has something a rebuild can't stand in for
    u32 powcnt = 0;             // the last POWCNT write

    static void Direct(std::vector<IOW>& out, u8 c, const std::vector<u32>& p)
    {
        u16 off = 0x400 + c * 4;
        if (p.empty()) out.push_back({off, 4, 0});
        for (u32 v : p) out.push_back({off, 4, v});
    }

    u32 keptBy[256] = {};
    void Done(std::vector<IOW>& out, u8 c, const std::vector<u32>& p, bool sent)
    {
        if (sent || GXKeepWhenSkipped(c))
        {
            if (!sent && !baseSet)
            {
                geoBase = geo;
                headLen = out.size();
                baseSet = true;
            }
            Direct(out, c, p);
            geo.Exec(c, p.data(), p.size());
            if (!sent) { kept++; keptBy[c]++; }
        }
        else dropped++;
    }
    // a command that goes to the client as it was written
    void Raw(u8 c, const std::vector<u32>& p, bool sent)
    {
        if (!sent && baseSet) noRebuild = true;
        if (GXValid(c)) geo.Exec(c, p.data(), p.size());
    }

    // finish zero-parameter commands at the head of the packet
    void Advance(std::vector<IOW>& out, bool sent)
    {
        while (ci < ncmds && GXParamCount(cmds[ci]) == 0)
        {
            if (packetRaw) Raw(cmds[ci], {}, sent);
            else if (GXValid(cmds[ci])) Done(out, cmds[ci], {}, sent);
            ci++;
        }
        params.clear();
    }

    // sent: the write belongs to a frame the client gets
    void Feed(const IOW& w, bool sent, std::vector<IOW>& out)
    {
        if (w.off >= 0x400 && w.off < 0x440)  // GXFIFO and its mirrors
        {
            if (ci < ncmds)
            {
                if (packetRaw) out.push_back(w);
                params.push_back(w.val);
                u8 c = cmds[ci];
                if (params.size() >= GXParamCount(c))
                {
                    if (packetRaw) Raw(c, params, sent);
                    else Done(out, c, params, sent);
                    ci++;
                    Advance(out, sent);
                }
                return;
            }
            // packet header: up to four command bytes, the first zero byte ends the list
            ncmds = ci = 0;
            for (int i = 0; i < 4; i++)
            {
                u8 c = w.val >> (8 * i);
                if (!c) break;
                if (!GXValid(c)) badCmds++;
                cmds[ncmds++] = c;
            }
            packetRaw = sent;
            if (sent) out.push_back(w);
            Advance(out, sent);
            return;
        }
        if (w.off >= 0x440 && w.off < 0x600 && w.size == 4)
        {
            u8 c = (w.off - 0x400) / 4;
            if (!dActive || c != dcmd)
            {
                dActive = true;
                dcmd = c;
                dparams.clear();
                dRaw = sent;
            }
            if (dRaw) out.push_back(w);
            if (GXParamCount(c)) dparams.push_back(w.val);
            if (dparams.size() >= GXParamCount(c))
            {
                if (dRaw) Raw(c, dparams, sent);
                else if (GXValid(c)) Done(out, c, dparams, sent);
                dActive = false;
            }
            return;
        }
        // 3D registers outside the command FIFO (DISP3DCNT, tables, POWCNT) always go.
        // POWCNT switches the 3D engines on and off, so a write that does has to stay where
        // it was among the commands (its other bits, like the screen swap that dual-screen
        // 3D flips every frame, don't care)
        if (w.off == 0x304)
        {
            if (!sent && baseSet && ((w.val ^ powcnt) & 0x000C)) noRebuild = true;
            powcnt = w.val;
        }
        out.push_back(w);
    }

    // The carry as the commands that rebuild the state its left-out frames ended in:
    // the head (the rest of a packet the client already began), the 3D register writes,
    // then for each part of the geometry state those frames changed, the commands that
    // set it. Run through a copy of the state they began in, the result has to come out
    // as that end state, or the carry goes as it was. False: send the carry as it is.
    bool Rebuild(const std::vector<IOW>& carry, std::vector<IOW>& out)
    {
        static const bool off = getenv("MIRROR_NOREBUILD") != nullptr;
        if (off || !baseSet || noRebuild || badCmds || geo.unknownOps != geoBase.unknownOps || headLen > carry.size())
        {
            static const bool why = getenv("MIRROR_REBUILDLOG") != nullptr;
            if (why && carry.size() > 8192)
                fprintf(stderr, "rebuild: carry of %zu writes as it is: base %d, raw/POWCNT %d, bad %u, unknown ops %u -> %u, head %zu\n",
                        carry.size(), baseSet, noRebuild, badCmds, geoBase.unknownOps, geo.unknownOps, headLen);
            return false;
        }
        out.assign(carry.begin(), carry.begin() + headLen);
        for (size_t i = headLen; i < carry.size(); i++)
            if (carry[i].off < 0x400 || carry[i].off >= 0x600) out.push_back(carry[i]);
        GeoState sim = geoBase;
        std::vector<GeoCmd> cmds;
        geo.Rebuild(sim, cmds);
        for (const GeoCmd& c : cmds)
            Direct(out, c.cmd, c.params);
        if (!sim.Same(geo))
        {
            rebuildMisses++;
            return false;
        }
        if (out.size() >= carry.size()) return false;
        rebuilds++;
        rebuildSaved += carry.size() - out.size();
        return true;
    }
    u32 rebuildMisses = 0;

    // the client is part way through a command that goes as it was written
    bool MidRaw() const { return (ci < ncmds && packetRaw) || (dActive && dRaw); }

    // A full bundle: the DS may have missed everything so far (switched off and on
    // again), so set every part of the geometry state that's known. The matrix stack
    // pointer can only be moved, so it's taken to be where ours is.
    void RebuildAll(std::vector<IOW>& out)
    {
        if (MidRaw()) return;
        GeoState none;
        none.projSP = geo.projSP;
        none.posSP = geo.posSP;
        none.texSP = geo.texSP;
        std::vector<GeoCmd> cmds;
        geo.Rebuild(none, cmds);
        for (const GeoCmd& c : cmds) Direct(out, c.cmd, c.params);
        fullRebuilds++;
    }
    u32 rebuilds = 0, fullRebuilds = 0;
    u64 rebuildSaved = 0;   // writes the rebuilds saved

    // after a bundle: the next run of left-out frames starts from here
    void Sent()
    {
        baseSet = false;
        noRebuild = false;
        headLen = 0;
    }
};

// ---------------------------------------------------------------- recorder

static bool Tracked2DCalc(u32 off)
{
    if (off < 0x58) return off >= 8 || off < 4;      // DISPCNT, BG/affine/window/blend
    if (off >= 0x64 && off < 0x68) return true;      // DISPCAPCNT
    if (off == 0x6C || off == 0x6D) return true;     // MASTER_BRIGHT A
    if (off >= 0x1000 && off < 0x1058) return off >= 0x1008 || off < 0x1004;
    if (off == 0x106C || off == 0x106D) return true;
    return false;
}

// the registers above as a table, and the halfwords holding any of them: looked up
// for every I/O write and walked on every visible line
struct Tracked2DTable
{
    bool byte[0x1070] {};
    std::vector<u16> halves;
    Tracked2DTable()
    {
        for (u32 o = 0; o < 0x1070; o++) byte[o] = Tracked2DCalc(o);
        for (u32 o = 0; o < 0x1070; o += 2)
            if (byte[o] || byte[o + 1]) halves.push_back(o);
    }
};
static const Tracked2DTable gTracked2D;

static inline bool Tracked2D(u32 off) { return off < 0x1070 && gTracked2D.byte[off]; }

static bool Tracked3D(u32 off)
{
    if (off >= 0x60 && off < 0x64) return true;      // DISP3DCNT
    if (off >= 0x320 && off < 0x6A4) return !(off >= 0x600 && off < 0x608); // skip GXSTAT
    return false;
}

// registers whose write itself does something, so a rewrite of the same value matters
static bool WriteTriggers(u32 off)
{
    u32 o = off & 0xFFF;
    return (o >= 0x28 && o < 0x30) || (o >= 0x38 && o < 0x40) || (off >= 0x64 && off < 0x68);
}

static const u32 BankSize[9] = {0x20000,0x20000,0x20000,0x20000,0x10000,0x4000,0x4000,0x8000,0x4000};

class Recorder : public MirrorTap
{
public:
    NDS* nds = nullptr;
    FILE* out = nullptr;
    HostLink* link = nullptr;
    bool linkLost = false;
    u32 linkSkips = 0;   // frames left out because the link was still busy

    // Live: a thread hands bundles to the link so the game never waits on USB.
    // When bundles back up (a big VRAM upload), frames are left out the way
    // --half leaves them out, so the DS stays close to live instead of falling behind.
    // The 3D commands' state tracking and the bundle's last steps take most of the time
    // a bundle costs, so they run on that thread too, in frame order (MIRROR_SYNCGX=1 or
    // MIRROR_GEOCHECK: on the game's thread, as before): a frame's job carries its 3D
    // writes and the rest of its bundle, ready made.
    struct Sizes { u32 gx, regs, pal, oam, vram, vramBlocks, lines; } sz {};
    struct GXJob
    {
        bool send = true;           // a bundle; false: a left-out frame's 3D writes, kept as state
        u32 frame = 0, flags = 0;
        std::vector<GXFilter::IOW> gx;   // the frame's 3D writes
        std::vector<u8> post;       // the bundle after its 3D record (2D state, lines)
        std::vector<u8> built;      // the whole bundle, when it was made on the game's thread
        std::vector<GXFilter::IOW> regs3d;   // a full bundle: the 3D registers as the game left them
        Sizes sz {};
        bool cap = false;
    };
    const bool asyncGX = !getenv("MIRROR_SYNCGX") && !getenv("MIRROR_GEOCHECK");
    std::thread sender;
    std::mutex sendLock;
    std::condition_variable sendWake;
    std::deque<GXJob> sendQueue;
    u32 sendBusy = 0;    // bundles queued or being sent
    std::atomic<u32> fullPending {0};   // full bundles queued or being sent
    std::atomic<u64> sentBytes {0};
    std::atomic<u32> fullSentAt {0};   // emulated frame when the last full bundle had gone out
    std::condition_variable sendDone;
    u32 capWaits = 0;
    bool sendStop = false;

    void StartLink(HostLink* l)
    {
        link = l;
        sender = std::thread([this] {
            pthread_setname_np(pthread_self(), "mirror-send");
            for (;;)
            {
                GXJob job;
                {
                    std::unique_lock<std::mutex> g(sendLock);
                    sendWake.wait(g, [this] { return sendStop || !sendQueue.empty(); });
                    if (sendQueue.empty()) return;
                    job = std::move(sendQueue.front());
                    sendQueue.pop_front();
                }
                if (!job.send)
                {
                    BuildSkipped(job);
                    continue;
                }
                std::vector<u8> b = job.built.empty() ? BuildBundle(job) : std::move(job.built);
                auto t0 = std::chrono::steady_clock::now();
                size_t raw = b.size();
                b = LzPackBundle(b);
                // Whole 512-byte blocks only: the DS reads in 512-byte USB transfers, and a short
                // packet that ends one early leaves the DSpico's (RP2040) second packet buffer
                // armed, which loses data or hangs its firmware. The DS skips the zero padding
                // while it looks for the next bundle's header.
                b.resize((b.size() + 511) & ~size_t(511), 0);
                bool ok = !linkLost && link->Send(b.data(), b.size());
                if (ok) sentBytes += b.size();
                if (((mirror_hdr*)b.data())->flags & MIRROR_FLAG_FULL)
                {
                    fullSentAt = emuFrame;
                    fullPending--;
                    if (ok && fullRequests)
                        fprintf(stderr, "link: sent a full frame, %zu KB (%zu KB unpacked) in %.1f s\n", b.size() >> 10,
                                raw >> 10, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
                }
                std::lock_guard<std::mutex> g(sendLock);
                sendBusy--;
                sendDone.notify_all();
                if (!ok && !linkLost)
                {
                    fprintf(stderr, "link: the DS went away at frame %u\n", emuFrame);
                    linkLost = true;
                }
            }
        });
    }
    void StopLink()
    {
        if (!link) return;
        { std::lock_guard<std::mutex> g(sendLock); sendStop = true; }
        sendWake.notify_one();
        sender.join();
        link = nullptr;
    }
    // the DS came back on a new link: start over with a full bundle
    void RestartLink(HostLink* l)
    {
        dsShownValid = false;
        unshown.clear();
        sendQueue.clear();
        sendBusy = 0;
        fullPending = 0;
        sendStop = false;
        linkLost = false;
        RequestFull();
        StartLink(l);
    }
    // Frames with display capture can't be left out (the DS redoes the capture from the
    // frame before). When those pile up, the game waits for the DS instead.
    void WaitForLink()
    {
        std::unique_lock<std::mutex> g(sendLock);
        if (sendBusy < 8) return;
        if (capWaits++ < 10)
            fprintf(stderr, "link: %u bundles waiting during display capture; the game waits for the DS\n", sendBusy);
        sendDone.wait(g, [this] { return sendBusy < 4 || linkLost; });
    }
    bool LinkBehind()
    {
        std::lock_guard<std::mutex> g(sendLock);
        return sendBusy >= 2;
    }
    // Bundles sent that the DS hasn't shown yet, from its reports. Every bundle the DS
    // can't show at once waits in its 64 KB buffer, and after a slow stretch (a course
    // loading) they stayed there: the DS showed one every VBlank, as many as came in,
    // but up to a second late, and the pad felt that late (run 9). So frames are left out
    // while more than a few are on their way, or with display capture the game waits.
    static constexpr size_t MaxQueued = 4;
    std::deque<u32> unshown;   // frame numbers of the bundles sent and not shown yet
    u32 dsShown = 0;           // the frame on the DS's screen
    u32 dsShownAt = 0;         // emuFrame when that changed
    bool dsShownValid = false;
    std::function<void()> pollReports;   // reads the DS's reports while the game waits
    void Shown(u32 f)
    {
        while (!unshown.empty() && (s32)(unshown.front() - f) <= 0)
            unshown.pop_front();
    }
    bool DSBehind()
    {
        // while it waits for a full frame, or hasn't shown anything new for a second
        // (its status page is up), holding frames back wouldn't help it
        static const bool off = getenv("MIRROR_NOHOLD") != nullptr;   // cloud tests: run 9's way
        // at 30 fps each bundle stays up for two frames: half as many make the same wait
        return !off && link && dsShownValid && !full && !fullPending && unshown.size() > (half ? MaxQueued / 2 : MaxQueued) &&
               emuFrame - dsShownAt < 60;
    }
    void WaitForDS()
    {
        if (!pollReports || !DSBehind()) return;
        auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
        while (DSBehind() && !linkLost && std::chrono::steady_clock::now() < until)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            pollReports();
        }
    }
    FILE* stats = nullptr;
    u32 frame = 0;      // bundles written
    u32 emuFrame = 0;
    bool full = true;
    bool fullThis = false;   // the bundle being built is a full one
    u32 fullRequests = 0;
    // the DS lost track (MIRROR_INPUT_NEED_FULL): next bundle carries everything
    void RequestFull()
    {
        full = true;   // its bundle clears prevGX where it's built
        fullRequests++;
    }
    bool half = false;  // 30 fps: every other frame, each shown twice
    bool skipThis = false, holdNext = false, capPrev = false, capThis = false;
    u32 skipRun = 0;          // frames left out in a row
    u32 dsCaptured[4] = {};   // emuFrame of the last capture the DS did itself, per bank (A-D)
    u32 capStart = 0;         // emuFrame the current stretch of frames with capture began
    u32 lastCapture = 0;      // emuFrame of the last frame with capture
    GXFilter filt;

    using IOW = GXFilter::IOW;

    u8 shadow[0x1070] {};      // last value the game wrote, per byte
    u8 sent[0x1070] {};        // what the client's registers hold
    u8 lineCur[0x1070] {};     // register state while the visible lines run
    bool written[0x1070 / 2] {};
    bool lineWritten[0x1070 / 2] {};
    std::vector<IOW> pending, ready, carry;
    std::vector<mirror_line> lines;
    u8 prevPal[2048] {}, prevOAM[2048] {};
    std::vector<u8> prevVRAM[9];
    u8 prevVRAMCNT[9] {};
    u16 prevPow = 0;
    bool haveFrame = false;
    int captureBank = -1;
    u32 captureOff = 0, captureLen = 0;

    std::vector<u8> part2D;
    u64 totalRaw = 0, totalZ = 0;

    Recorder() { for (int i = 0; i < 9; i++) prevVRAM[i].assign(BankSize[i], 0); }

    void IOWrite(u32 addr, u32 val, int size) override
    {
        u32 off = addr - 0x04000000;
        if (addr < 0x04000000 || off >= 0x1070) return;
        if (Tracked3D(off))
        {
            pending.push_back({(u16)off, (u8)size, val});
            return;
        }
        if (off == 0x304 && size >= 2)
        {
            // POWCNT1 gates the 3D engines, so it also goes in order with the 3D writes
            // (the client keeps its own screen-swap bit there; the 2D state sets that)
            pending.push_back({(u16)off, 2, val & 0xFFFF});
            return;
        }
        for (int i = 0; i < size; i++)
        {
            u32 o = off + i;
            if (o < 0x1070 && Tracked2D(o))
            {
                shadow[o] = (val >> (8 * i)) & 0xFF;
                written[o >> 1] = true;
                lineWritten[o >> 1] = true;
            }
        }
    }

    void Scanline(u32 vcount) override
    {
        if (vcount == 0)
        {
            capPrev = capThis;
            capThis = (nds->GPU.CaptureCnt >> 31) & 1;
            if (capThis && !capPrev && emuFrame - lastCapture > 2) capStart = emuFrame;
            if (capThis) lastCapture = emuFrame;
            skipThis = holdNext;
            holdNext = false;
            // no DS yet (--hotplug) and nothing recorded: no bundles, only the state they'd leave
            dropThis = !link && !out;
            if (dropThis) skipThis = true;
            bool behind = link && !skipThis && frame > 0 && (LinkBehind() || DSBehind());
            if (link && frame > 0 && (capThis || capPrev))
            {
                // Display capture: the game shows 3D on both screens by drawing one per frame
                // and capturing it for the other, and the DS redoes each capture itself. Frames
                // are left out two at a time (at 30 fps two of every four): the frames the DS
                // gets then still take turns between the screens, and the screen it doesn't
                // draw shows its own earlier capture of that same screen. Both stay right,
                // each just changes less often, and the game never waits for the DS.
                if (!skipThis && ((skipRun & 1) || behind || (half && ((emuFrame >> 1) & 1))))
                {
                    skipThis = true;
                    if (behind) linkSkips++;
                }
            }
            else if (behind)
            {
                skipThis = true;
                linkSkips++;
            }
            if (link && !skipThis && frame > 0)
            {
                WaitForLink();
                WaitForDS();
            }
            if (!skipThis)
            {
                skipRun = 0;
                PROF(Snap);
                Snapshot2D();
                haveFrame = true;
            }
            else
            {
                skipRun++;
                // the pixels of a capture the DS doesn't redo aren't sent either: it keeps its
                // own, of the same screen. Unless it has none of its own there yet in this
                // stretch of capture: then they go as VRAM like any other change.
                NoteCapture();
                if (captureBank >= 0 && (s32)(dsCaptured[captureBank] - capStart) < 0)
                    captureBank = -1;
            }
        }
        else if (vcount < 192 && haveFrame)
        {
            PROF(Lines);
            TrackLine(vcount);
        }
        else if (vcount == 192)
        {
            if (haveFrame && (out || link))
            {
                PROF(Emit);
                Emit();
            }
            else if (skipThis)
            {
                PROF(Skipped);
                // not sent: keep the state its 3D commands leave behind for the next bundle
                GXJob j;
                j.send = false;
                j.gx.swap(ready);
                Note3DRegs(j.gx);
                if (dropThis)
                    DropFrame(j);
                else if (link && asyncGX)
                {
                    std::lock_guard<std::mutex> g(sendLock);
                    sendQueue.push_back(std::move(j));
                    sendWake.notify_one();
                }
                else
                    BuildSkipped(j);
            }
            static const bool geoCheck = getenv("MIRROR_GEOCHECK") != nullptr;
            if (geoCheck) CheckGeo();
            haveFrame = false;
            emuFrame++;
            ready.insert(ready.end(), pending.begin(), pending.end());
            pending.clear();
            if (geoCheck) melo.Take(nds->GPU.GPU3D);
        }
        else if (vcount == 193)
        {
            PROF(Sync);
            SyncCapture();
        }
    }

    // MIRROR_GEOCHECK=1 (cloud tests): the geometry state worked out from the commands
    // (filt.geo) against melonDS's own. The commands fed so far are the ones written up to
    // the last line 192, so it's compared with melonDS as it was then, when its 3D engine
    // had nothing left to do.
    struct MeloGeo
    {
        bool valid = false;
        u32 mode;
        s32 proj[16], pos[16], vec[16], tex[16], projStack[16], texStack[16], posStack[32][16], vecStack[32][16];
        s32 projSP, posSP, texSP;
        s16 lightDir[4][3];
        u8 lightColor[4][3], dif[3], amb[3], spe[3], emi[3], color[3], shin[128];
        bool useShin;
        u32 polyAttr, texParam, texPal, viewport[4];
        void Take(const GPU3D& g)
        {
            valid = g.CmdFIFO.IsEmpty() && g.CmdPIPE.IsEmpty() && g.CmdStallQueue.IsEmpty() && !g.ExecParamCount;
            if (!valid) return;
            mode = g.MatrixMode;
            memcpy(proj, g.ProjMatrix, 64); memcpy(pos, g.PosMatrix, 64); memcpy(vec, g.VecMatrix, 64); memcpy(tex, g.TexMatrix, 64);
            memcpy(projStack, g.ProjMatrixStack, 64); memcpy(texStack, g.TexMatrixStack, 64);
            memcpy(posStack, g.PosMatrixStack, sizeof posStack); memcpy(vecStack, g.VecMatrixStack, sizeof vecStack);
            projSP = g.ProjMatrixStackPointer; posSP = g.PosMatrixStackPointer; texSP = g.TexMatrixStackPointer;
            memcpy(lightDir, g.LightDirection, sizeof lightDir); memcpy(lightColor, g.LightColor, sizeof lightColor);
            memcpy(dif, g.MatDiffuse, 3); memcpy(amb, g.MatAmbient, 3); memcpy(spe, g.MatSpecular, 3); memcpy(emi, g.MatEmission, 3);
            memcpy(color, g.VertexColor, 3); memcpy(shin, g.ShininessTable, 128);
            useShin = g.UseShininessTable;
            polyAttr = g.PolygonAttr; texParam = g.TexParam; texPal = g.TexPalette;
            for (int i = 0; i < 4; i++) viewport[i] = g.Viewport[i];
        }
    } melo;
    u32 geoMismatches = 0, geoChecks = 0;
    void CheckGeo()
    {
        if (!melo.valid) return;
        const GeoState& s = filt.geo;
        geoChecks++;
        char what[160] = "";
        auto mtx = [&](const GeoState::Mtx& m, const s32* g, const char* name) {
            if (!what[0] && m.known && memcmp(m.m, g, 64)) snprintf(what, sizeof what, "%s", name);
        };
        auto rgb = [](const u8* c) { return (u32)c[0] | (u32)c[1] << 5 | (u32)c[2] << 10; };
        auto val = [&](const GeoState::Val& v, u32 g, const char* name) {
            if (!what[0] && v.known && v.v != g) snprintf(what, sizeof what, "%s %08x, melonDS %08x", name, v.v, g);
        };
        val(s.mode, melo.mode, "matrix mode");
        mtx(s.proj, melo.proj, "projection matrix");
        mtx(s.pos, melo.pos, "position matrix");
        mtx(s.vec, melo.vec, "vector matrix");
        mtx(s.tex, melo.tex, "texture matrix");
        mtx(s.projStack, melo.projStack, "projection stack");
        mtx(s.texStack, melo.texStack, "texture stack");
        for (int i = 0; i < 32; i++)
        {
            char n[32];
            snprintf(n, sizeof n, "position stack %d", i);
            mtx(s.posStack[i], melo.posStack[i], n);
            snprintf(n, sizeof n, "vector stack %d", i);
            mtx(s.vecStack[i], melo.vecStack[i], n);
        }
        if (!what[0] && (s.projSP != melo.projSP || s.posSP != melo.posSP || s.texSP != melo.texSP))
            snprintf(what, sizeof what, "stack pointers %d %d %d, melonDS %d %d %d", s.projSP, s.posSP, s.texSP,
                     melo.projSP, melo.posSP, melo.texSP);
        for (int l = 0; l < 4 && !what[0]; l++)
        {
            const GeoState::Light& L = s.light[l];
            if (L.known)
            {
                // melonDS GPU3D.cpp, light direction
                s16 d[3] = {(s16)((s16)((L.param & 0x3FF) << 6) >> 6), (s16)((s16)((L.param & 0xFFC00) >> 4) >> 6),
                            (s16)((s16)((L.param & 0x3FF00000) >> 14) >> 6)};
                const s32* v = L.vec.m;
                for (int k = 0; k < 3; k++)
                {
                    s16 dir = (s16)((-((d[0] * v[k] + d[1] * v[4 + k] + d[2] * v[8 + k]) >> 12) << 21) >> 21);
                    if (dir != melo.lightDir[l][k] && !what[0])
                        snprintf(what, sizeof what, "light %d direction", l);
                }
            }
            val(s.lightColor[l], rgb(melo.lightColor[l]), "light color");
        }
        val(s.difAmb, rgb(melo.dif) | rgb(melo.amb) << 16, "diffuse/ambient");
        val(s.speEmi, rgb(melo.spe) | rgb(melo.emi) << 16 | (melo.useShin ? 0x8000 : 0), "specular/emission");
        val(s.polyAttr, melo.polyAttr, "polygon attributes");
        val(s.texParam, melo.texParam, "texture parameters");
        val(s.texPal, melo.texPal, "texture palette");
        // (not the vertex color: frames left out lose their NORMAL commands, which light it)
        if (!what[0] && s.shininessKnown && memcmp(s.shininess, melo.shin, 128)) snprintf(what, sizeof what, "shininess table");
        if (!what[0] && s.viewport.known)
        {
            u32 p = s.viewport.v;
            u32 vp[4] = {p & 0xFF, (191 - ((p >> 8) & 0xFF)) & 0xFF, (p >> 16) & 0xFF, (191 - (p >> 24)) & 0xFF};
            if (memcmp(vp, melo.viewport, sizeof vp)) snprintf(what, sizeof what, "viewport");
        }
        if (what[0] && geoMismatches++ < 30)
            fprintf(stderr, "geocheck f%u: %s differs\n", emuFrame, what);
    }

    // ---- output helpers
    template <typename T> static void Put(std::vector<u8>& b, const T& v)
    {
        const u8* p = (const u8*)&v;
        b.insert(b.end(), p, p + sizeof(T));
    }
    static void PutBytes(std::vector<u8>& b, const void* p, size_t n) { b.insert(b.end(), (const u8*)p, (const u8*)p + n); }
    static size_t BeginRec(std::vector<u8>& b, u16 type)
    {
        size_t at = b.size();
        mirror_rec r {type, 0, 0};
        Put(b, r);
        return at;
    }
    static void EndRec(std::vector<u8>& b, size_t at)
    {
        ((mirror_rec*)&b[at])->len = b.size() - at - sizeof(mirror_rec);
    }

    // ---- line 0: the 2D state this frame starts with
    void Snapshot2D()
    {
        GPU& gpu = nds->GPU;
        std::vector<u8>& b = part2D;
        b.clear();
        fullThis = full;
        if (full && frame > 0)
        {
            // a full refresh mid-stream: the DS clears its VRAM first, so
            // compare against cleared VRAM and send every register again
            for (int bank = 0; bank < 9; bank++) std::fill(prevVRAM[bank].begin(), prevVRAM[bank].end(), 0);
            memset(prevPal, 0, sizeof prevPal);
            memset(prevOAM, 0, sizeof prevOAM);
        }
        sz = {};

        if (full || memcmp(prevVRAMCNT, gpu.VRAMCNT, 9))
        {
            size_t at = BeginRec(b, MREC_VRAMCNT);
            PutBytes(b, gpu.VRAMCNT, 9);
            u8 pad[3] {};
            PutBytes(b, pad, 3);
            EndRec(b, at);
            memcpy(prevVRAMCNT, gpu.VRAMCNT, 9);
        }

        std::vector<mirror_reg> regs;
        for (u32 o : gTracked2D.halves)
        {
            u16 v = shadow[o] | (shadow[o + 1] << 8);
            u16 s = sent[o] | (sent[o + 1] << 8);
            if (full || v != s || (written[o >> 1] && WriteTriggers(o)))
            {
                regs.push_back({(u16)o, v});
                sent[o] = shadow[o];
                sent[o + 1] = shadow[o + 1];
            }
        }
        if (full || nds->PowerControl9 != prevPow)
        {
            regs.push_back({0x304, nds->PowerControl9});
            prevPow = nds->PowerControl9;
        }
        memset(written, 0, sizeof(written));
        memset(lineWritten, 0, sizeof(lineWritten));
        memcpy(lineCur, shadow, sizeof(lineCur));
        lines.clear();
        if (!regs.empty())
        {
            size_t at = BeginRec(b, MREC_REGS), s0 = b.size();
            u32 n = regs.size();
            Put(b, n);
            PutBytes(b, regs.data(), n * sizeof(mirror_reg));
            EndRec(b, at);
            sz.regs = b.size() - s0;
        }

        sz.pal = Rows(MREC_PAL, gpu.Palette, prevPal);
        sz.oam = Rows(MREC_OAM, gpu.OAM, prevOAM);

        size_t at = BeginRec(b, MREC_VRAM), s0 = b.size(), cntAt = b.size();
        u32 n = 0;
        Put(b, n);
        for (int bank = 0; bank < 9; bank++)
        {
            const u8* cur = gpu.VRAM[bank];
            u8* prev = prevVRAM[bank].data();
            for (u32 blk = 0; blk < BankSize[bank] / MIRROR_BLOCK; blk++)
            {
                const u8* c = cur + blk * MIRROR_BLOCK;
                u8* p = prev + blk * MIRROR_BLOCK;
                if (!full && !memcmp(c, p, MIRROR_BLOCK)) continue;
                if (full)
                {
                    // client VRAM starts cleared; skip all-zero blocks
                    bool zero = true;
                    for (int i = 0; i < MIRROR_BLOCK && zero; i++) zero = !c[i];
                    if (zero) continue;
                }
                u32 id = ((u32)bank << 16) | blk;
                Put(b, id);
                PutBytes(b, c, MIRROR_BLOCK);
                memcpy(p, c, MIRROR_BLOCK);
                n++;
            }
        }
        if (n)
        {
            memcpy(&b[cntAt], &n, 4);
            EndRec(b, at);
            sz.vram = b.size() - s0;
            sz.vramBlocks = n;
        }
        else
            b.resize(at);

        // a capture this frame is redone by the client's own hardware; don't send its pixels
        NoteCapture();
        if (captureBank >= 0) dsCaptured[captureBank] = emuFrame;
        full = false;
    }

    // ---- 3D commands as copies from the previous bundle's (format: MREC_GXDELTA)
    bool gxDelta = false;
    std::vector<u8> prevGX;
    std::vector<u32> gxSeen;   // DeltaGX: hash of 4 words -> last position in prevGX (direct-mapped)

    std::vector<u8> DeltaGX(const std::vector<u8>& curb)
    {
        const u32* P = (const u32*)prevGX.data();
        const u32* N = (const u32*)curb.data();
        u32 np = prevGX.size() / 4, nn = curb.size() / 4;
        const u32 K = 4;  // shortest copy, in words
        auto key = [](const u32* w) {
            u64 h = 1469598103934665603ull;
            for (u32 i = 0; i < 4; i++) h = (h ^ w[i]) * 1099511628211ull;
            return h;
        };
        // a candidate only: tryAt compares the words, so a collision just finds no copy
        u32 bits = 10;
        while ((1u << bits) < np * 2 && bits < 22) bits++;
        const u32 mask = (1u << bits) - 1;
        gxSeen.assign(mask + 1, ~0u);
        for (u32 i = 0; i + K <= np; i++)
            gxSeen[key(P + i) & mask] = i;  // keeps the last occurrence

        std::vector<u32> out, lit;
        out.push_back(nn);
        auto flush = [&]() {
            if (lit.empty()) return;
            out.push_back((u32)lit.size());
            out.insert(out.end(), lit.begin(), lit.end());
            lit.clear();
        };
        u32 expect = ~0u;  // where the last copy ended: the usual place for the next one
        for (u32 i = 0; i < nn;)
        {
            u32 src = ~0u, len = 0;
            auto tryAt = [&](u32 s) {
                u32 l = 0;
                while (i + l < nn && s + l < np && N[i + l] == P[s + l] && l < 0x7FFFFFFF) l++;
                if (l > len) { len = l; src = s; }
            };
            if (expect < np) tryAt(expect);
            if (len < K && i + K <= nn)
            {
                u32 s = gxSeen[key(N + i) & mask];
                if (s != ~0u) tryAt(s);
            }
            if (len >= K)
            {
                flush();
                out.push_back(0x80000000u | len);
                out.push_back(src);
                i += len;
                expect = src + len;
            }
            else
                lit.push_back(N[i++]);
        }
        flush();
        std::vector<u8> b(out.size() * 4);
        memcpy(b.data(), out.data(), b.size());
        return b;
    }

    u32 Rows(u16 type, const u8* cur, u8* prev)
    {
        std::vector<u8>& b = part2D;
        size_t at = BeginRec(b, type), s0 = b.size(), cntAt = b.size();
        u32 n = 0;
        Put(b, n);
        for (u32 o = 0; o < 2048; o += MIRROR_ROW)
        {
            if (!full && !memcmp(cur + o, prev + o, MIRROR_ROW)) continue;
            Put(b, o);
            PutBytes(b, cur + o, MIRROR_ROW);
            memcpy(prev + o, cur + o, MIRROR_ROW);
            n++;
        }
        if (!n) { b.resize(at); return 0; }
        memcpy(&b[cntAt], &n, 4);
        EndRec(b, at);
        return b.size() - s0;
    }

    // ---- lines 1-191: registers the game changes partway down the screen
    void TrackLine(u32 line)
    {
        for (u32 o : gTracked2D.halves)
        {
            bool changed = shadow[o] != lineCur[o] || shadow[o + 1] != lineCur[o + 1];
            if (changed || (lineWritten[o >> 1] && WriteTriggers(o)))
            {
                lines.push_back({(u16)line, (u16)o, (u16)(shadow[o] | (shadow[o + 1] << 8)), 0});
                lineCur[o] = shadow[o];
                lineCur[o + 1] = shadow[o + 1];
            }
        }
        memset(lineWritten, 0, sizeof(lineWritten));
    }

    // the VRAM a capture armed for this frame writes to
    void NoteCapture()
    {
        GPU& gpu = nds->GPU;
        captureBank = -1;
        u32 cc = gpu.CaptureCnt;
        u32 bank = (cc >> 16) & 3;
        if ((cc & (1u << 31)) && (gpu.VRAMCNT[bank] & 0x87) == 0x80)
        {
            static const u32 lens[4] = {0x8000, 0x8000, 0x10000, 0x18000};
            captureBank = bank;
            captureOff = ((cc >> 18) & 3) * 0x8000;
            captureLen = lens[(cc >> 20) & 3];
        }
    }

    void SyncCapture()
    {
        if (captureBank < 0) return;
        const u8* cur = nds->GPU.VRAM[captureBank];
        u8* prev = prevVRAM[captureBank].data();
        for (u32 i = 0; i < captureLen; i += MIRROR_BLOCK)
        {
            u32 o = (captureOff + i) & 0x1FFFF;
            memcpy(prev + o, cur + o, MIRROR_BLOCK);
        }
        captureBank = -1;
    }

    // ---- line 192: send the frame
    // the game's thread, line 192: what only this frame knows (2D state, lines, flags)
    void Emit()
    {
        // 30 fps: hold this frame for two unless the game is capturing (3D on both
        // screens, one per frame), where every frame has to go
        holdNext = half && !capThis && !capPrev;
        GXJob j;
        j.frame = emuFrame;
        j.flags = (fullThis ? MIRROR_FLAG_FULL : 0) | (holdNext ? MIRROR_FLAG_HOLD : 0);
        fullThis = false;
        j.gx.swap(ready);
        if (j.flags & MIRROR_FLAG_FULL) j.regs3d = Regs3D();
        Note3DRegs(j.gx);
        j.post.swap(part2D);
        if (!lines.empty())
        {
            size_t at = BeginRec(j.post, MREC_LINES), s0 = j.post.size();
            u32 n = lines.size();
            Put(j.post, n);
            PutBytes(j.post, lines.data(), n * sizeof(mirror_line));
            EndRec(j.post, at);
            sz.lines = j.post.size() - s0;
            // the client's registers now hold the end-of-frame values
            memcpy(sent, lineCur, sizeof(sent));
        }
        j.sz = sz;
        j.cap = capThis;
        if (link)
        {
            if (!asyncGX) j.built = BuildBundle(j);
            std::lock_guard<std::mutex> g(sendLock);
            if (j.flags & MIRROR_FLAG_FULL) fullPending++;
            sendQueue.push_back(std::move(j));   // built, packed and padded by the sender
            sendBusy++;
            sendWake.notify_one();
            unshown.push_back(emuFrame);
            if (unshown.size() > 600) unshown.pop_front();   // a DS that doesn't say what it shows
        }
        else
            BuildBundle(j);
        frame++;
    }

    // a left-out frame: keep the state its 3D commands leave behind for the next bundle
    void BuildSkipped(GXJob& j)
    {
        for (const IOW& w : j.gx) filt.Feed(w, false, carry);
    }

    // No DS: the frame's 3D commands count as sent, so the geometry state stays known and
    // nothing piles up. A DS that comes along gets a full bundle, which sets all of it.
    void DropFrame(GXJob& j)
    {
        std::vector<IOW> none;
        filt.Sent();
        for (const IOW& w : j.gx) filt.Feed(w, true, none);
    }

    // The 3D registers outside the command FIFO that hold state (DISP3DCNT, edge colors,
    // alpha test, clear color and depth, fog, toon table, 1-dot depth): games set most of
    // them once a scene, so a DS that joins later or comes back gets them with its full
    // bundle. Kept on the game's thread from each frame's 3D writes as its job is made.
    static bool State3D(u32 o) { return (o >= 0x60 && o < 0x64) || (o >= 0x330 && o < 0x3C0) || (o >= 0x610 && o < 0x612); }
    u8 reg3d[0x614] {};
    bool reg3dSet[0x614] {};
    bool dropThis = false;
    void Note3DRegs(const std::vector<IOW>& v)
    {
        for (const IOW& w : v)
            for (int i = 0; i < w.size; i++)
                if (State3D(w.off + i))
                {
                    reg3d[w.off + i] = (w.val >> (8 * i)) & 0xFF;
                    reg3dSet[w.off + i] = true;
                }
    }
    std::vector<IOW> Regs3D() const
    {
        std::vector<IOW> out;
        for (u32 o = 0x60; o < 0x614; o += 4)
        {
            if (!State3D(o)) continue;
            u8 size = o == 0x610 ? 2 : 4;
            bool any = false;
            u32 v = 0;
            for (int i = 0; i < size; i++)
            {
                any |= reg3dSet[o + i];
                v |= (u32)reg3d[o + i] << (8 * i);
            }
            if (any) out.push_back({(u16)o, size, v});
        }
        return out;
    }

    // the sender's thread (or the game's): the 3D commands as the DS gets them, and the
    // bundle around them, in frame order
    std::vector<u8> BuildBundle(GXJob& j)
    {
        std::vector<u8> buf;
        if (j.flags & MIRROR_FLAG_FULL) prevGX.clear();   // its 3D commands go literally
        mirror_hdr h {MIRROR_MAGIC, j.frame, 0, j.flags};
        Put(buf, h);
        std::vector<IOW>& ready = j.gx;
        if (half || link)
        {
            static const bool cstat = getenv("MIRROR_CARRYSTAT") != nullptr;
            if (cstat && carry.size() > 8192)
            {
                fprintf(stderr, "carry f%u: %zu writes:", j.frame, carry.size());
                for (int c = 0; c < 256; c++)
                    if (filt.keptBy[c]) fprintf(stderr, " %02x:%u", c, filt.keptBy[c]);
                fprintf(stderr, "\n");
            }
            memset(filt.keptBy, 0, sizeof filt.keptBy);
            PROF(EmitFilter);
            std::vector<IOW> gx;
            if (filt.Rebuild(carry, gx))
                carry.clear();
            else
                gx.swap(carry);
            filt.Sent();
            if (j.flags & MIRROR_FLAG_FULL)
            {
                for (const IOW& w : j.regs3d) filt.Feed(w, true, gx);
                filt.RebuildAll(gx);
            }
            for (const IOW& w : ready) filt.Feed(w, true, gx);
            ready.swap(gx);
        }

        u32 gxSize = 0;
        if (!ready.empty())
        {
            Prof::Timer packTimer(gProf, Prof::EmitPack);
            std::vector<u8> gx;
            u32 runs = 0;
            Put(gx, runs);
            for (size_t i = 0; i < ready.size();)
            {
                size_t k = i;
                while (k < ready.size() && k - i < 0x3FFF && ready[k].off == ready[i].off && ready[k].size == ready[i].size)
                    k++;
                u16 code = ready[i].size == 4 ? 0 : ready[i].size == 2 ? 1 : 2;
                mirror_run r {ready[i].off, (u16)((k - i) | (code << 14))};
                Put(gx, r);
                for (size_t m = i; m < k; m++) Put(gx, ready[m].val);
                runs++;
                i = k;
            }
            memcpy(&gx[0], &runs, 4);

            std::vector<u8> delta;
            Prof::Timer deltaTimer(gProf, Prof::EmitDelta);
            if (gxDelta && !prevGX.empty() && (delta = DeltaGX(gx), delta.size() < gx.size()))
            {
                size_t at = BeginRec(buf, MREC_GXDELTA), s0 = buf.size();
                PutBytes(buf, delta.data(), delta.size());
                EndRec(buf, at);
                gxSize = buf.size() - s0;
            }
            else
            {
                size_t at = BeginRec(buf, MREC_GX), s0 = buf.size();
                PutBytes(buf, gx.data(), gx.size());
                EndRec(buf, at);
                gxSize = buf.size() - s0;
            }
            prevGX.swap(gx);
        }
        else
            prevGX.clear();

        PutBytes(buf, j.post.data(), j.post.size());
        size_t at = BeginRec(buf, MREC_END);
        EndRec(buf, at);
        ((mirror_hdr*)buf.data())->size = buf.size();
        if (out) fwrite(buf.data(), 1, buf.size(), out);

        // what zlib would make of it, for recordings' numbers: not live, where it cost the game
        // a millisecond a frame in the course flyover
        static const bool noZ = getenv("MIRROR_NOZ") != nullptr;
        uLongf zlen = 0;
        if ((out || stats) && !noZ)
        {
            zlen = compressBound(buf.size());
            std::vector<u8> z(zlen);
            compress2(z.data(), &zlen, buf.data(), buf.size(), 1);
        }
        totalRaw += buf.size();
        totalZ += zlen;
        if (stats)
            fprintf(stats, "%u,%zu,%lu,%u,%u,%u,%u,%u,%u,%u,%d\n", j.frame, buf.size(), (unsigned long)zlen,
                    gxSize, j.sz.regs, j.sz.pal, j.sz.oam, j.sz.vram, j.sz.vramBlocks, j.sz.lines, j.cap ? 1 : 0);
        return buf;
    }
};

// ---------------------------------------------------------------- input script

struct InputStep { u32 frame; u32 mask; bool touch; u16 tx, ty; };

static std::vector<InputStep> LoadInput(const char* path)
{
    static const char* names[] = {"A","B","SELECT","START","RIGHT","LEFT","UP","DOWN","R","L","X","Y"};
    std::vector<InputStep> steps;
    FILE* f = fopen(path, "r");
    if (!f) { perror(path); exit(1); }
    char line[256];
    while (fgets(line, sizeof line, f))
    {
        if (line[0] == '#' || line[0] == '\n') continue;
        InputStep s {0, 0xFFF, false, 0, 0};
        char keys[128] = "-";
        int x = 0, y = 0;
        char t[16] = "";
        int n = sscanf(line, "%u %127s %15s %d %d", &s.frame, keys, t, &x, &y);
        if (n < 1) continue;
        if (strcmp(keys, "-"))
        {
            for (char* k = strtok(keys, "+"); k; k = strtok(nullptr, "+"))
                for (int i = 0; i < 12; i++)
                    if (!strcmp(k, names[i])) s.mask &= ~(1u << i);
        }
        if (n == 5 && !strcmp(t, "TOUCH")) { s.touch = true; s.tx = x; s.ty = y; }
        steps.push_back(s);
    }
    fclose(f);
    return steps;
}

// ---------------------------------------------------------------- main

static void WritePPM(const std::string& path, const u32* top, const u32* bot)
{
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    fprintf(f, "P6\n256 384\n255\n");
    for (int i = 0; i < 256 * 384; i++)
    {
        u32 p = i < 256 * 192 ? top[i] : bot[i - 256 * 192];
        u8 rgb[3] = {(u8)(p >> 16), (u8)(p >> 8), (u8)p};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

// What the game gets from everything that plays it: the DS over the link, a key script
// (--input) and the Sion's own pads (--pads). Buttons held anywhere count; the touch
// screen goes to the DS's stylus first, then the script's, then the pads' cursor.
struct InputMix
{
    struct Src { u16 held = 0; bool touch = false; u16 x = 0, y = 0; };   // held: bit set = pressed (libnds order)
    Src script, ds, pads;
    void Apply(NDS& nds) const
    {
        nds.SetKeyMask(~(script.held | ds.held | pads.held) & 0xFFF);   // melonDS: bit clear = pressed
        if (ds.touch) nds.TouchScreen(ds.x, ds.y);
        else if (script.touch) nds.TouchScreen(script.x, script.y);
        else if (pads.touch) nds.TouchScreen(pads.x, pads.y);
        else nds.ReleaseScreen();
    }
};

// --hotplug: waits for the DS on its own thread while the game runs
struct LinkWaiter
{
    std::thread t;
    std::mutex m;
    std::unique_ptr<HostLink> got;
    std::atomic<bool> stop {false}, done {false};
    void Start(const std::string& spec)
    {
        Stop();
        stop = false;
        done = false;
        t = std::thread([this, spec] {
            pthread_setname_np(pthread_self(), "mirror-wait");
            while (!stop && !gMirrorQuit)
            {
                if (auto l = OpenHostLink(spec, &stop))
                {
                    std::lock_guard<std::mutex> g(m);
                    got = std::move(l);
                    break;
                }
                for (int i = 0; i < 10 && !stop && !gMirrorQuit; i++)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            done = true;
        });
    }
    std::unique_ptr<HostLink> Take()
    {
        if (!done) return nullptr;
        if (t.joinable()) t.join();
        std::lock_guard<std::mutex> g(m);
        return std::move(got);
    }
    void Stop()
    {
        stop = true;
        if (t.joinable()) t.join();
        got.reset();
    }
    ~LinkWaiter() { Stop(); }
};

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: %s ROM FRAMES [--input F] [--record F] [--frames F] [--shots DIR] [--every N] [--stats F] [--boottype N] [--lang N] [--gamecode XXXX] [--half 1] [--gxdelta 1] [--serve LINK] [--reconnect 1] [--dspico PATH] [--usbrate N] [--realtime 1] [--jit 1] [--threaded 1] [--tv drm|dump:F] [--sound alsa|wav:F] [--stretch 0] [--hotplug 1] [--pads 1] [--save F]\n", argv[0]);
        return 1;
    }
    const char* rompath = argv[1];
    u32 frames = atoi(argv[2]);
    if (frames == 0) frames = ~0u;   // 0: until stopped (or until the DS goes away, with --serve)
    const char *input = nullptr, *record = nullptr, *framesOut = nullptr, *shots = nullptr, *statsPath = nullptr;
    u32 every = 60;
    int bootType = -1;
    int lang = -1;
    const char* gamecode = nullptr;
    const char* clientIdx = nullptr;
    bool half = false, gxDelta = false, reconnect = false;
    const char *serve = nullptr, *dspico = nullptr;
    u32 usbRate = 12000;
    bool realtime = false, jit = false, threaded = false;
    const char *tvSpec = nullptr, *soundSpec = nullptr;
    bool stretch = true;
    bool hotplug = false;   // the game starts at once; the DS can come and go
    bool padsOn = false;
    const char* savePath = nullptr;
    for (int i = 3; i + 1 < argc; i += 2)
    {
        std::string o = argv[i];
        if (o == "--input") input = argv[i + 1];
        else if (o == "--record") record = argv[i + 1];
        else if (o == "--frames") framesOut = argv[i + 1];
        else if (o == "--shots") shots = argv[i + 1];
        else if (o == "--every") every = atoi(argv[i + 1]);
        else if (o == "--stats") statsPath = argv[i + 1];
        else if (o == "--boottype") bootType = atoi(argv[i + 1]);
        else if (o == "--lang") lang = atoi(argv[i + 1]);
        else if (o == "--gamecode") gamecode = argv[i + 1];
        else if (o == "--client-index") clientIdx = argv[i + 1];
        else if (o == "--half") half = atoi(argv[i + 1]) != 0;
        else if (o == "--gxdelta") gxDelta = atoi(argv[i + 1]) != 0;
        else if (o == "--reconnect") reconnect = atoi(argv[i + 1]) != 0;
        else if (o == "--serve") serve = argv[i + 1];
        else if (o == "--dspico") dspico = argv[i + 1];
        else if (o == "--usbrate") usbRate = atoi(argv[i + 1]);
        else if (o == "--realtime") realtime = atoi(argv[i + 1]) != 0;
        else if (o == "--jit") jit = atoi(argv[i + 1]) != 0;
        else if (o == "--threaded") threaded = atoi(argv[i + 1]) != 0;
        else if (o == "--tv") tvSpec = argv[i + 1];
        else if (o == "--sound") soundSpec = argv[i + 1];
        else if (o == "--stretch") stretch = atoi(argv[i + 1]) != 0;
        else if (o == "--hotplug") hotplug = atoi(argv[i + 1]) != 0;
        else if (o == "--pads") padsOn = atoi(argv[i + 1]) != 0;
        else if (o == "--save") savePath = argv[i + 1];
        else { fprintf(stderr, "unknown option %s\n", o.c_str()); return 1; }
    }

    FILE* rf = fopen(rompath, "rb");
    if (!rf) { perror(rompath); return 1; }
    fseek(rf, 0, SEEK_END);
    u32 romlen = ftell(rf);
    fseek(rf, 0, SEEK_SET);
    auto rom = std::make_unique<u8[]>(romlen);
    if (fread(rom.get(), 1, romlen, rf) != romlen) { fprintf(stderr, "short read\n"); return 1; }
    fclose(rf);
    if (gamecode && strlen(gamecode) == 4 && romlen >= 0x200)
    {
        // melonDS looks the cart type and save type up by game code; prototypes can share a
        // code with an unrelated retail game (Mario Kart DS kiosk A39E is listed as a NAND cart)
        memcpy(&rom[0x0C], gamecode, 4);
        u16 crc = CRC16(rom.get(), 0x15E, 0xFFFF);
        memcpy(&rom[0x15E], &crc, 2);
    }

    NDSArgs args {};
    // JIT: I/O writes from compiled code still go through ARM9IOWrite (no fastmem for I/O)
    if (jit) args.JIT = JITArgs {};
    else args.JIT = std::nullopt;
    if (lang >= 0)
    {
        // firmware language (0 Japanese, 1 English, ...); some builds only carry some languages' files
        for (auto& u : args.Firmware.GetUserData())
        {
            u.Settings = (u.Settings & ~7) | lang;
            u.UpdateChecksum();
        }
    }
    auto nds = std::make_unique<NDS>(std::move(args));
    if (threaded)
    {
        // 3D on its own thread; the picture is the same
        RendererSettings rs {};
        rs.ScaleFactor = 1;
        rs.Threaded = true;
        nds->GPU.GetRenderer().SetRenderSettings(rs);
    }
    nds->Reset();
    SaveKeeper saves;
    std::optional<NDSCart::NDSCartArgs> cartArgs;
    if (savePath)
    {
        std::vector<u8> sav = saves.Load(savePath);
        cartArgs = NDSCart::NDSCartArgs {};
        if (!sav.empty())
        {
            cartArgs->SRAM = std::make_unique<u8[]>(sav.size());
            memcpy(cartArgs->SRAM.get(), sav.data(), sav.size());
            cartArgs->SRAMLength = sav.size();
        }
        gSaveKeeper = &saves;
    }
    auto cart = NDSCart::ParseROM(std::move(rom), romlen, nullptr, std::move(cartArgs));
    if (!cart) { fprintf(stderr, "bad ROM\n"); return 1; }
    nds->SetNDSCart(std::move(cart));
    nds->Reset();
    nds->SetupDirectBoot(rompath);
    if (bootType >= 0) nds->ARM9Write16(0x027FFC40, bootType); // 2 = Download Play child

    Recorder rec;
    rec.nds = nds.get();
    rec.half = half;
    rec.gxDelta = gxDelta;
    if (record)
    {
        rec.out = fopen(record, "wb");
        if (statsPath)
        {
            rec.stats = fopen(statsPath, "w");
            fprintf(rec.stats, "frame,bytes,zlib,gx,regs,pal,oam,vram,vramblocks,lines,capture\n");
        }
    }
    // stop cleanly when told to, so the TV goes back to xbox-av; a second time, at once
    struct sigaction sa {};
    sa.sa_handler = [](int) {
        if (gMirrorQuit.exchange(true)) _exit(1);
    };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, nullptr);   // no SA_RESTART: waiting for the DS stops
    sigaction(SIGINT, &sa, nullptr);

    // the TV and the sound first: the dashboard has gone by now, and the TV stays dark
    // (not the dashboard's last picture) until the DS is there
    TVOut tv;
    SoundOut sound;
    if (tvSpec)
    {
        if (!tv.Open(tvSpec)) return 1;
        static const u32 black[256 * 192] = {};
        tv.Show(black, black);
    }
    if (soundSpec && !sound.Open(soundSpec)) return 1;
    SionPads sionPads;
    if (padsOn) padsOn = sionPads.Open();
    std::vector<u32> cursorScreen(256 * 192);
    std::vector<s16> soundBuf(4096 * 2);
    auto lastFrameDone = std::chrono::steady_clock::now();
    double frameGap = 16715757e-9;   // seconds between frames lately

    std::unique_ptr<HostLink> link;
    LinkWaiter waiter;
    if (serve && hotplug)
        waiter.Start(serve);
    else if (serve)
    {
        link = OpenHostLink(serve);
        if (!link) return gMirrorQuit ? 0 : 1;
        rec.StartLink(link.get());
    }
    if (record || serve) gMirrorTap = &rec;

    std::unique_ptr<DSpicoSim> pico;
    if (dspico)
    {
        pico = std::make_unique<DSpicoSim>(nds.get(), dspico, usbRate);
        gCardHook = pico.get();
    }
    InputMix in;
    u32 reports = 0, padFrames = 0, lastFullReq = 0, lastReportAt = 0, reportsShown = 0;
    u64 sentShown = 0;
    u32 shownBefore = 0;
    bool firstReport = true;   // the DS's counters run on from before this link (run 11: from the run before)
    mirror_input dsSeen {};
    bool deaf = false;   // no reports for a while: the DS can't ask for a full frame
    mirror_input pad {};
    mirror_input linkSeen {};   // the DS's link counters at the last speed line
    u32 busyMax = 0;            // longest the DS went without looking at USB, since then
    u32 behindMin = ~0u, behindMax = 0;   // how far the DS's screen was behind the game, since then
    bool reportWaiting = false;   // pollReports took one while the game waited for the DS
    auto noteShown = [&] {
        if (!pad.shownFrame) return;
        // still a frame from before this run (the DS stayed on through a restart)
        if ((s32)(rec.emuFrame - pad.shownFrame) < 0) return;
        if (pad.shownFrame != rec.dsShown || !rec.dsShownValid)
        {
            rec.dsShown = pad.shownFrame;
            rec.dsShownAt = rec.emuFrame;
        }
        rec.dsShownValid = true;
        rec.Shown(pad.shownFrame);
        u32 behind = rec.emuFrame - pad.shownFrame;
        if (behind < behindMin) behindMin = behind;
        if (behind > behindMax) behindMax = behind;
        static const bool trace = getenv("MIRROR_BEHINDLOG") != nullptr;
        if (trace && rec.emuFrame % 30 == 0)
            fprintf(stderr, "behind f%u %u queued %zu\n", rec.emuFrame, behind, rec.unshown.size());
    };
    rec.pollReports = [&] {
        if (link && link->Poll(pad))
        {
            reportWaiting = true;
            noteShown();
        }
    };

    nds->Start();

    std::vector<InputStep> steps;
    if (input) steps = LoadInput(input);
    size_t si = 0;
    FILE* fo = framesOut ? fopen(framesOut, "wb") : nullptr;
    FILE* fidx = clientIdx ? fopen(clientIdx, "w") : nullptr;
    s64 statsAt = -1;

    auto tick = std::chrono::steady_clock::now();
    const auto tvFrame = std::chrono::nanoseconds((s64)(1e9 / tv.Hz()));
    const double tvFrameMs = 1000 / tv.Hz();
    double heavyMs = 0;     // how long frames take here lately, one slow frame counting as one TV frame
    bool caughtUp = false;  // the last frame wasn't shown
    u32 tvSkipped = 0, tvSkippedShown = 0;
    // 560190 cycles at 33.51 MHz; MIRROR_FRAMEHZ=60 runs at a TV's pace instead (cloud tests)
    const auto frameTime = std::chrono::nanoseconds(getenv("MIRROR_FRAMEHZ") ? (s64)(1e9 / atof(getenv("MIRROR_FRAMEHZ"))) : 16715757);
    auto speedFrom = std::chrono::steady_clock::now();
    u32 underrunsShown = 0, xrunsShown = 0;
    double longestMs = 0, workMs = 0;   // the longest a frame took here (game, bundle), all of them, since the last speed line
    for (u32 f = 0; f < frames; f++)
    {
        if (gMirrorQuit)
        {
            fprintf(stderr, "stopping at frame %u\n", f);
            break;
        }
        if (hotplug && !link)
        {
            if ((link = waiter.Take()))
            {
                // a DS: a full bundle first, then on as usual
                rec.RestartLink(link.get());
                fprintf(stderr, "link: the DS is here at frame %u\n", f);
                lastReportAt = lastFullReq = f;
                deaf = false;
                firstReport = true;
            }
            else if (waiter.done && !gMirrorQuit)
                waiter.Start(serve);
        }
        if (link && rec.linkLost && hotplug)
        {
            // the game carries on without it
            rec.StopLink();
            link.reset();
            pad = {};
            in.ds = {};
            fprintf(stderr, "link: the game carries on; waiting for the DS to come back\n");
            waiter.Start(serve);
        }
        if (link && rec.linkLost)
        {
            if (!reconnect)
                break;
            // switched off, or its DSpico stopped answering and then it was switched off:
            // keep the game going and wait for it to come back
            rec.StopLink();
            link.reset();
            pad = {};
            in.ds = {};
            in.Apply(*nds);
            firstReport = true;
            fprintf(stderr, "link: waiting for the DS to come back (switch it on and start Sion Link)\n");
            link = OpenHostLink(serve);
            if (!link)
            {
                if (gMirrorQuit) continue;   // stops at the top
                return 1;
            }
            rec.RestartLink(link.get());
            fprintf(stderr, "link: the DS is back at frame %u\n", f);
            lastReportAt = lastFullReq = f;
            deaf = false;
        }
        if ((link || hotplug) && f && f % 600 == 0)
        {
            // how fast the game runs here, and whether the link keeps up
            auto now = std::chrono::steady_clock::now();
            double s = std::chrono::duration<double>(now - speedFrom).count();
            u64 sent = rec.sentBytes;
            u32 shown = pad.applied & ~MIRROR_INPUT_NEED_FULL;
            char behind[48] = "";
            if (behindMax)
                snprintf(behind, sizeof behind, " (%u to %u frames behind)", behindMin, behindMax);
            if (!link)
                fprintf(stderr, "frame %u: %.1f fps (%.1f ms a frame, longest %.0f ms), no DS\n", f, 600 / s, workMs / 600, longestMs);
            else
                fprintf(stderr, "frame %u: %.1f fps (%.1f ms a frame, longest %.0f ms), %u bundles sent (%llu KB/s), %u frames left out, DS showed %u%s, %u reports, pad %03x%s\n",
                        f, 600 / s, workMs / 600, longestMs, rec.frame, (unsigned long long)((sent - sentShown) / 1024 / s), rec.linkSkips,
                        shown - shownBefore, behind, reports - reportsShown, pad.keys & 0xFFF,
                        pad.applied & MIRROR_INPUT_NEED_FULL ? ", DS waiting for a full frame" : "");
            longestMs = workMs = 0;
            fprintf(stderr, "  CPU: %s\n", CpuUse(s).c_str());
            if (tvSkipped != tvSkippedShown)
                fprintf(stderr, "  TV: %u frames not shown, catching up after slow ones\n", tvSkipped - tvSkippedShown);
            tvSkippedShown = tvSkipped;
            behindMin = ~0u;
            behindMax = 0;
            if (memcmp(&pad.damaged, &dsSeen.damaged, 8))
                fprintf(stderr, "  DS so far: %u damaged bundles, %u with no memory, %u full refreshes, %u stalls\n",
                        pad.damaged, pad.noMemory, pad.fulls, pad.stalls);
            dsSeen = pad;
            if (pad.magic == MIRROR_INPUT_MAGIC)
            {
                // how the DS side of the link is doing: whether the DSpico's IRQ line works,
                // how long a 512-byte block waits for its data, how long the DS is busy
                u32 blocks = pad.blocks - linkSeen.blocks;
                double waitMs = blocks ? (pad.armedTime - linkSeen.armedTime) / 32.768 / blocks : 0;
                fprintf(stderr, "  DS link: %u blocks, %.2f ms each waiting for data, busy up to %.1f ms, "
                        "events read %u on IRQ %u timed (%u found some), %u silent, %u recovered, %u restarted\n",
                        blocks, waitMs, busyMax / 32.768, (u16)(pad.irqReads - linkSeen.irqReads),
                        (u16)(pad.timedReads - linkSeen.timedReads), (u16)(pad.timedHits - linkSeen.timedHits), pad.silent - linkSeen.silent,
                        (u16)(pad.recovered - linkSeen.recovered), (u16)(pad.restarted - linkSeen.restarted));
                linkSeen = pad;
                busyMax = 0;
            }
            if (sound.Underruns() != underrunsShown || sound.Xruns() != xrunsShown)
            {
                fprintf(stderr, "  sound: ran dry %u times, the card %u times\n", sound.Underruns() - underrunsShown,
                        sound.Xruns() - xrunsShown);
                underrunsShown = sound.Underruns();
                xrunsShown = sound.Xruns();
            }
            shownBefore = shown;
            reportsShown = reports;
            sentShown = sent;
            speedFrom = now;
        }
        // On the TV each frame waits for the one before to be on screen. One frame here that
        // takes longer than the TV's (a course loading, a big bundle) would leave the game
        // behind for good, and its sound would run dry: the frame after it isn't shown, so
        // the game catches up (the TV keeps showing the last one meanwhile). Not in a scene
        // too heavy for 60 frames a second, though: there run 12 left nearly every frame
        // out and the TV stood still. There the game slows down instead, as in run 11.
        bool catchUp = false;
        if (realtime || tv.Paced())
        {
            tick += tv.Paced() ? tvFrame : frameTime;
            // after waiting for the DS, carry on from now instead of racing to catch up
            auto now = std::chrono::steady_clock::now();
            if (now - tick > std::chrono::milliseconds(250)) tick = now;
            if (!tv.Paced())
                std::this_thread::sleep_until(tick);
            else if (tick > now)
                tick = now;   // the TV's pace: never ahead of it
            else if (heavyMs > tvFrameMs * 0.85 || now - tick > tvFrame * 4)
                tick = now;   // a heavy stretch: no catching up
            else
                catchUp = now - tick > tvFrame && !caughtUp;   // and never two frames in a row
            caughtUp = catchUp;
        }
        auto frameStart = std::chrono::steady_clock::now();
        while (si < steps.size() && steps[si].frame <= f)
        {
            const InputStep& s = steps[si++];
            in.script = {(u16)(~s.mask & 0xFFF), s.touch, s.tx, s.ty};
        }
        bool gotReport = link && link->Poll(pad);
        if (link && std::exchange(reportWaiting, false))
            gotReport = true;
        if (gotReport)
        {
            if (std::exchange(firstReport, false))
            {
                shownBefore = pad.applied & ~MIRROR_INPUT_NEED_FULL;
                linkSeen = dsSeen = pad;
            }
            noteShown();
            // One full frame at a time: it can take seconds to get through, and the DS
            // keeps asking until it has arrived. Give it time to say so after one has gone
            // out, or each would bring another (run 7: minutes of nothing but full frames).
            if ((pad.applied & MIRROR_INPUT_NEED_FULL) && f - lastFullReq > 60 && !rec.fullPending &&
                f - rec.fullSentAt > 90)
            {
                lastFullReq = f;
                rec.RequestFull();
                fprintf(stderr, "link: the DS asked for a full frame at frame %u\n", f);
            }
            // the DS's pad (libnds bit set = held)
            in.ds = {(u16)(pad.keys & 0xFFF), (pad.keys & (1 << 12)) != 0, pad.touchX, pad.touchY};
            reports++;
            lastReportAt = f;
            if (pad.busyMax > busyMax) busyMax = pad.busyMax;
            if (deaf)
            {
                deaf = false;
                fprintf(stderr, "link: reports from the DS again at frame %u\n", f);
            }
        }
        else if (link && f - lastReportAt > 120 && f - lastFullReq > 300 && !rec.fullPending)
        {
            // Without reports the DS can't ask for a full frame after damage or a stall,
            // and would wait for one forever. Send one now and then until it talks again.
            if (!deaf)
                fprintf(stderr, "link: no reports from the DS since frame %u; sending a full frame every 5 s\n", lastReportAt);
            deaf = true;
            rec.dsShownValid = false;
            // and let go of its buttons, or the game keeps them held
            in.ds = {};
            pad.keys = 0;
            lastFullReq = f;
            rec.RequestFull();
        }
        if (link && in.ds.held) padFrames++;
        if (padsOn)
        {
            sionPads.Poll(in.pads.held, in.pads.touch, in.pads.x, in.pads.y);
            if (sionPads.MenuAsked()) fprintf(stderr, "menu: Back held at frame %u (the menu isn't there yet)\n", f);
        }
        in.Apply(*nds);
        if (pico) pico->Frame();
        if (gProf.on && f == gProf.from) memset(gProf.ms, 0, sizeof gProf.ms);
        if (gProf.on && f == gProf.to)
        {
            u32 n = gProf.to - gProf.from;
            fprintf(stderr, "prof frames %u-%u, ms a frame: game+tap %.2f (2D snapshot %.2f, lines %.2f, bundle %.2f, left-out 3D %.2f, capture sync %.2f), TV+sound %.2f\n",
                    gProf.from, gProf.to, gProf.ms[Prof::Frame] / n, gProf.ms[Prof::Snap] / n, gProf.ms[Prof::Lines] / n,
                    gProf.ms[Prof::Emit] / n, gProf.ms[Prof::Skipped] / n, gProf.ms[Prof::Sync] / n, gProf.ms[Prof::Tv] / n);
            fprintf(stderr, "prof bundle: 3D filter %.2f, 3D packing %.2f (of it delta %.2f)\n", gProf.ms[Prof::EmitFilter] / n,
                    gProf.ms[Prof::EmitPack] / n, gProf.ms[Prof::EmitDelta] / n);
            gProf.on = false;
        }
        {
            PROF(Frame);
            nds->RunFrame();
        }
        if (const char* st = getenv("MIRROR_STALLTEST"))
        {
            // cloud tests: a slow frame (N:MS, MS ms every N frames), as a course loading is
            unsigned every = 0, ms = 0;
            if (sscanf(st, "%u:%u", &every, &ms) == 2 && every && f % every == every - 1)
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        }
        void *top = nullptr, *bot = nullptr;
        nds->GPU.GetFramebuffers(&top, &bot);
        double frameMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - frameStart).count();
        longestMs = std::max(longestMs, frameMs);
        heavyMs = heavyMs * 0.9 + std::min(frameMs, tvFrameMs) * 0.1;
        workMs += frameMs;
        Prof::Timer tvTimer(gProf, Prof::Tv);
        if (soundSpec)
        {
            // the sound first: showing the picture can wait for the TV
            int n = nds->SPU.ReadOutput(soundBuf.data(), 4096);
            if (n > 0) sound.Push(soundBuf.data(), n);
            // how fast the game runs against the DS, over the last quarter second or so
            auto done = std::chrono::steady_clock::now();
            double gap = std::chrono::duration<double>(done - lastFrameDone).count();
            lastFrameDone = done;
            if (gap < 0.25) frameGap = frameGap * 0.9 + gap * 0.1;
            nds->SPU.SetOutputSkew(sound.Skew(stretch ? 16715757e-9 / frameGap : 1));
        }
        if (catchUp) tvSkipped++;
        else if (tvSpec && padsOn && sionPads.CursorShown())
        {
            // the pads' stylus, on the TV only (the DS has a stylus of its own)
            memcpy(cursorScreen.data(), bot, 256 * 192 * 4);
            DrawCursor(cursorScreen.data(), sionPads.CursorX(), sionPads.CursorY(), in.pads.touch);
            tv.Show((const u32*)top, cursorScreen.data());
        }
        else if (tvSpec) tv.Show((const u32*)top, (const u32*)bot);
        if (fo)
        {
            fwrite(top, 4, 256 * 192, fo);
            fwrite(bot, 4, 256 * 192, fo);
        }
        if (const char* rd = getenv("MIRROR_REGDUMP"))
        {
            // display state at the end of frame N: MIRROR_REGDUMP=N
            if (f == (u32)atoi(rd))
            {
                GPU& g = nds->GPU;
                fprintf(stderr, "f%u VRAMCNT", f);
                for (int b = 0; b < 9; b++) fprintf(stderr, " %02x", g.VRAMCNT[b]);
                fprintf(stderr, " DISPCNT A %08x B %08x POWCNT %04x\n", g.GPU2D_A.DispCnt, g.GPU2D_B.DispCnt, nds->PowerControl9);
                for (int b = 0; b < 9; b++)
                {
                    u32 sum = 0;
                    for (u32 i = 0; i < BankSize[b]; i++) sum = sum * 31 + g.VRAM[b][i];
                    fprintf(stderr, " bank%d %08x", b, sum);
                }
                fprintf(stderr, "\n");
            }
        }
        if (getenv("MIRROR_3DDEBUG") && f % 50 == 25)
        {
            auto& g = nds->GPU.GPU3D;
            fprintf(stderr, "f%u polys %u attr %08x mode %u proj %d %d %d %d pos %d %d %d %d\n", f, g.RenderNumPolygons,
                    g.PolygonAttr, g.MatrixMode, g.ProjMatrix[0], g.ProjMatrix[5], g.ProjMatrix[10], g.ProjMatrix[14],
                    g.PosMatrix[0], g.PosMatrix[5], g.PosMatrix[12], g.PosMatrix[14]);
        }
        if (fidx)
        {
            // which bundle the replay client has applied by the end of this frame
            if (statsAt < 0)
                for (u32 i = 0; i + 16 <= nds->MainRAMMask + 1; i += 4)
                    if (*(u32*)&nds->MainRAM[i] == 0x5354524D) { statsAt = i; break; }
            int applied = statsAt < 0 ? 0 : *(u32*)&nds->MainRAM[statsAt + 4];
            fprintf(fidx, "%u %d\n", f, applied);
        }
        if (shots && (f % every) == 0)
        {
            char name[512];
            snprintf(name, sizeof name, "%s/f%05u.ppm", shots, f);
            WritePPM(name, (u32*)top, (u32*)bot);
        }
    }
    if (fo) fclose(fo);
    if (fidx) fclose(fidx);
    waiter.Stop();
    rec.StopLink();
    if (link)
    {
        fprintf(stderr, "link: sent %u bundles, %u frames left out while the link was busy, %u pad reports from the DS, %u frames with buttons held\n",
                rec.frame, rec.linkSkips, reports, padFrames);
        if (pad.magic == MIRROR_INPUT_MAGIC)
            fprintf(stderr, "link: the DS's last report: %u shown%s, %u damaged bundles, %u with no memory, %u full refreshes, %u stalls, %u silent reads\n",
                    pad.applied & ~MIRROR_INPUT_NEED_FULL, pad.applied & MIRROR_INPUT_NEED_FULL ? " (waiting for a full frame)" : "",
                    pad.damaged, pad.noMemory, pad.fulls, pad.stalls, pad.silent);
    }
    if (pico)
        fprintf(stderr, "dspico sim: %s\n", pico->Configured() ? "configured" : "never configured");
    if (rec.out)
    {
        fclose(rec.out);
        if (rec.stats) fclose(rec.stats);
        fprintf(stderr, "recorded %u bundles, %.1f KB/frame raw, %.1f KB/frame zlib\n", rec.frame,
                rec.totalRaw / 1024.0 / rec.frame, rec.totalZ / 1024.0 / rec.frame);
        if (half)
            fprintf(stderr, "skipped frames' 3D commands: %u kept as state, %u dropped, %u unknown\n", rec.filt.kept, rec.filt.dropped, rec.filt.badCmds);
    }
    if (rec.filt.rebuilds || rec.filt.rebuildMisses)
        fprintf(stderr, "left-out frames' 3D state: %u carries rebuilt (%llu writes saved), %u sent as they were\n",
                rec.filt.rebuilds, (unsigned long long)rec.filt.rebuildSaved, rec.filt.rebuildMisses);
    if (rec.geoChecks)
        fprintf(stderr, "geocheck: %u checks, %u with a difference\n", rec.geoChecks, rec.geoMismatches);
    if (const char* dump = getenv("MIRROR_DUMPRAM"))
    {
        FILE* d = fopen(dump, "wb");
        if (d) { fwrite(nds->MainRAM, 1, nds->MainRAMMask + 1, d); fclose(d); }
    }
    // replay client counters (ds/source/main.c mirrorStats)
    for (u32 i = 0; i + 16 <= nds->MainRAMMask + 1; i += 4)
    {
        u32* w = (u32*)&nds->MainRAM[i];
        if (w[0] == 0x5354524D && w[3] <= 4 && w[1] <= frames)
        {
            fprintf(stderr, "client: %u frames applied, %u late, state %u\n", w[1], w[2], w[3]);
            break;
        }
    }
    if (savePath)
    {
        saves.Close();
        gSaveKeeper = nullptr;
    }
    if (soundSpec)
    {
        sound.Close();
        fprintf(stderr, "sound: ran dry %u times in all, the card %u times\n", sound.Underruns(), sound.Xruns());
    }
    tv.Close();
    gMirrorTap = nullptr;
    gCardHook = nullptr;
    return 0;
}
