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
//     --dspico PATH    run a DS client with a simulated DSpico whose USB host is the
//                      --serve run listening on PATH
//     --usbrate N      simulated USB bytes per frame (default 12000, ~700 KB/s)
//     --realtime 1     run at the DS's 59.83 frames a second instead of flat out
//     --jit 1          run the game with melonDS's JIT (faster)
//     --threaded 1     draw 3D on a second thread (faster on a multi-core host, same picture)
//     --gamecode XXXX  replace the header's game code, for prototypes whose code melonDS's
//                      cart list gives to another game (wrong cart/save type)
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

#include "NDS.h"
#include "NDSCart.h"
#include "GPU.h"
#include "Platform.h"
#include "MirrorTap.h"
#include "Args.h"
#include "SPI.h"
#include "../common/mirror_format.h"
#include "hostlink.h"
#include "dspico_sim.h"
#include "lzpack.h"

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

    static void Direct(std::vector<IOW>& out, u8 c, const std::vector<u32>& p)
    {
        u16 off = 0x400 + c * 4;
        if (p.empty()) out.push_back({off, 4, 0});
        for (u32 v : p) out.push_back({off, 4, v});
    }

    void Done(std::vector<IOW>& out, u8 c, const std::vector<u32>& p, bool sent)
    {
        if (sent || GXKeepWhenSkipped(c)) { Direct(out, c, p); if (!sent) kept++; }
        else dropped++;
    }

    // finish zero-parameter commands at the head of the packet
    void Advance(std::vector<IOW>& out, bool sent)
    {
        while (ci < ncmds && GXParamCount(cmds[ci]) == 0)
        {
            if (!packetRaw && GXValid(cmds[ci])) Done(out, cmds[ci], {}, sent);
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
                    if (!packetRaw) Done(out, c, params, sent);
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
                if (!dRaw && GXValid(c)) Done(out, c, dparams, sent);
                dActive = false;
            }
            return;
        }
        // 3D registers outside the command FIFO (DISP3DCNT, tables, POWCNT) always go
        out.push_back(w);
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
    std::thread sender;
    std::mutex sendLock;
    std::condition_variable sendWake;
    std::vector<std::vector<u8>> sendQueue;
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
            for (;;)
            {
                std::vector<u8> b;
                {
                    std::unique_lock<std::mutex> g(sendLock);
                    sendWake.wait(g, [this] { return sendStop || !sendQueue.empty(); });
                    if (sendQueue.empty()) return;
                    b.swap(sendQueue.front());
                    sendQueue.erase(sendQueue.begin());
                }
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
        full = true;
        prevGX.clear();
        fullRequests++;
    }
    bool half = false;  // 30 fps: every other frame, each shown twice
    bool skipThis = false, holdNext = false, capPrev = false, capThis = false;
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

    std::vector<u8> buf, part2D;
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
            skipThis = holdNext;
            holdNext = false;
            if (link && !skipThis && frame > 0 && !capThis && !capPrev && (LinkBehind() || DSBehind()))
            {
                skipThis = true;
                linkSkips++;
            }
            else if (link && !skipThis && frame > 0)
            {
                WaitForLink();
                WaitForDS();
            }
            if (!skipThis)
            {
                Snapshot2D();
                haveFrame = true;
            }
        }
        else if (vcount < 192 && haveFrame)
            TrackLine(vcount);
        else if (vcount == 192)
        {
            if (haveFrame && (out || link))
                Emit();
            else if (skipThis)
            {
                // not sent: keep the state its 3D commands leave behind for the next bundle
                for (const IOW& w : ready) filt.Feed(w, false, carry);
                ready.clear();
            }
            haveFrame = false;
            emuFrame++;
            ready.insert(ready.end(), pending.begin(), pending.end());
            pending.clear();
        }
        else if (vcount == 193)
            SyncCapture();
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

    struct Sizes { u32 gx, regs, pal, oam, vram, vramBlocks, lines; } sz {};

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
    void Emit()
    {
        buf.clear();
        // 30 fps: hold this frame for two unless the game is capturing (3D on both
        // screens, one per frame), where every frame has to go
        holdNext = half && !capThis && !capPrev;
        u32 flags = (fullThis ? MIRROR_FLAG_FULL : 0) | (holdNext ? MIRROR_FLAG_HOLD : 0);
        if (fullThis) prevGX.clear();   // its 3D commands go literally
        fullThis = false;
        mirror_hdr h {MIRROR_MAGIC, emuFrame, 0, flags};
        Put(buf, h);
        if (half || link)
        {
            std::vector<IOW> gx;
            gx.swap(carry);
            for (const IOW& w : ready) filt.Feed(w, true, gx);
            ready.swap(gx);
        }

        if (!ready.empty())
        {
            std::vector<u8> gx;
            u32 runs = 0;
            Put(gx, runs);
            for (size_t i = 0; i < ready.size();)
            {
                size_t j = i;
                while (j < ready.size() && j - i < 0x3FFF && ready[j].off == ready[i].off && ready[j].size == ready[i].size)
                    j++;
                u16 code = ready[i].size == 4 ? 0 : ready[i].size == 2 ? 1 : 2;
                mirror_run r {ready[i].off, (u16)((j - i) | (code << 14))};
                Put(gx, r);
                for (size_t k = i; k < j; k++) Put(gx, ready[k].val);
                runs++;
                i = j;
            }
            memcpy(&gx[0], &runs, 4);
            ready.clear();

            std::vector<u8> delta;
            if (gxDelta && !prevGX.empty() && (delta = DeltaGX(gx), delta.size() < gx.size()))
            {
                size_t at = BeginRec(buf, MREC_GXDELTA), s0 = buf.size();
                PutBytes(buf, delta.data(), delta.size());
                EndRec(buf, at);
                sz.gx = buf.size() - s0;
            }
            else
            {
                size_t at = BeginRec(buf, MREC_GX), s0 = buf.size();
                PutBytes(buf, gx.data(), gx.size());
                EndRec(buf, at);
                sz.gx = buf.size() - s0;
            }
            prevGX.swap(gx);
        }
        else
            prevGX.clear();

        PutBytes(buf, part2D.data(), part2D.size());

        if (!lines.empty())
        {
            size_t at = BeginRec(buf, MREC_LINES), s0 = buf.size();
            u32 n = lines.size();
            Put(buf, n);
            PutBytes(buf, lines.data(), n * sizeof(mirror_line));
            EndRec(buf, at);
            sz.lines = buf.size() - s0;
            // the client's registers now hold the end-of-frame values
            memcpy(sent, lineCur, sizeof(sent));
        }

        size_t at = BeginRec(buf, MREC_END);
        EndRec(buf, at);
        ((mirror_hdr*)buf.data())->size = buf.size();
        if (out) fwrite(buf.data(), 1, buf.size(), out);
        if (link)
        {
            std::lock_guard<std::mutex> g(sendLock);
            if (((mirror_hdr*)buf.data())->flags & MIRROR_FLAG_FULL) fullPending++;
            sendQueue.push_back(buf);   // packed and padded by the sender
            sendBusy++;
            sendWake.notify_one();
        }
        if (link)
        {
            unshown.push_back(emuFrame);
            if (unshown.size() > 600) unshown.pop_front();   // a DS that doesn't say what it shows
        }

        uLongf zlen = compressBound(buf.size());
        std::vector<u8> z(zlen);
        compress2(z.data(), &zlen, buf.data(), buf.size(), 1);
        totalRaw += buf.size();
        totalZ += zlen;
        if (stats)
            fprintf(stats, "%u,%zu,%lu,%u,%u,%u,%u,%u,%u,%u,%d\n", emuFrame, buf.size(), (unsigned long)zlen,
                    sz.gx, sz.regs, sz.pal, sz.oam, sz.vram, sz.vramBlocks, sz.lines, capThis ? 1 : 0);
        frame++;
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

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: %s ROM FRAMES [--input F] [--record F] [--frames F] [--shots DIR] [--every N] [--stats F] [--boottype N] [--lang N] [--gamecode XXXX] [--half 1] [--gxdelta 1] [--serve LINK] [--reconnect 1] [--dspico PATH] [--usbrate N] [--realtime 1] [--jit 1] [--threaded 1]\n", argv[0]);
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
    auto cart = NDSCart::ParseROM(std::move(rom), romlen);
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
    std::unique_ptr<HostLink> link;
    if (serve)
    {
        link = OpenHostLink(serve);
        if (!link) return 1;
        rec.StartLink(link.get());
    }
    if (record || serve) gMirrorTap = &rec;

    std::unique_ptr<DSpicoSim> pico;
    if (dspico)
    {
        pico = std::make_unique<DSpicoSim>(nds.get(), dspico, usbRate);
        gCardHook = pico.get();
    }
    u32 reports = 0, padFrames = 0, lastFullReq = 0, lastReportAt = 0, reportsShown = 0;
    u64 sentShown = 0;
    u32 shownBefore = 0;
    mirror_input dsSeen {};
    bool deaf = false;   // no reports for a while: the DS can't ask for a full frame
    mirror_input pad {};
    mirror_input linkSeen {};   // the DS's link counters at the last speed line
    u32 busyMax = 0;            // longest the DS went without looking at USB, since then
    u32 behindMin = ~0u, behindMax = 0;   // how far the DS's screen was behind the game, since then
    bool reportWaiting = false;   // pollReports took one while the game waited for the DS
    auto noteShown = [&] {
        if (!pad.shownFrame) return;
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
    const auto frameTime = std::chrono::nanoseconds(16715757);   // 560190 cycles at 33.51 MHz
    auto speedFrom = std::chrono::steady_clock::now();
    for (u32 f = 0; f < frames; f++)
    {
        if (link && rec.linkLost)
        {
            if (!reconnect)
                break;
            // switched off, or its DSpico stopped answering and then it was switched off:
            // keep the game going and wait for it to come back
            rec.StopLink();
            link.reset();
            nds->SetKeyMask(0xFFF);
            nds->ReleaseScreen();
            pad = {};
            linkSeen = {};
            shownBefore = 0;   // a new DS counts from 0
            fprintf(stderr, "link: waiting for the DS to come back (switch it on and start Sion Link)\n");
            link = OpenHostLink(serve);
            if (!link) return 1;
            rec.RestartLink(link.get());
            fprintf(stderr, "link: the DS is back at frame %u\n", f);
            lastReportAt = lastFullReq = f;
            deaf = false;
        }
        if (link && f && f % 600 == 0)
        {
            // how fast the game runs here, and whether the link keeps up
            auto now = std::chrono::steady_clock::now();
            double s = std::chrono::duration<double>(now - speedFrom).count();
            u64 sent = rec.sentBytes;
            u32 shown = pad.applied & ~MIRROR_INPUT_NEED_FULL;
            char behind[48] = "";
            if (behindMax)
                snprintf(behind, sizeof behind, " (%u to %u frames behind)", behindMin, behindMax);
            fprintf(stderr, "frame %u: %.1f fps, %u bundles sent (%llu KB/s), %u frames left out, DS showed %u%s, %u reports, pad %03x%s\n",
                    f, 600 / s, rec.frame, (unsigned long long)((sent - sentShown) / 1024 / s), rec.linkSkips,
                    shown - shownBefore, behind, reports - reportsShown, pad.keys & 0xFFF,
                    pad.applied & MIRROR_INPUT_NEED_FULL ? ", DS waiting for a full frame" : "");
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
                        "events read %u on IRQ %u timed, %u silent, %u recovered, %u restarted\n",
                        blocks, waitMs, busyMax / 32.768, (u16)(pad.irqReads - linkSeen.irqReads),
                        (u16)(pad.timedReads - linkSeen.timedReads), pad.silent - linkSeen.silent,
                        (u16)(pad.recovered - linkSeen.recovered), (u16)(pad.restarted - linkSeen.restarted));
                linkSeen = pad;
                busyMax = 0;
            }
            shownBefore = shown;
            reportsShown = reports;
            sentShown = sent;
            speedFrom = now;
        }
        if (realtime)
        {
            tick += frameTime;
            // after waiting for the DS, carry on from now instead of racing to catch up
            auto now = std::chrono::steady_clock::now();
            if (now - tick > std::chrono::milliseconds(250)) tick = now;
            std::this_thread::sleep_until(tick);
        }
        while (si < steps.size() && steps[si].frame <= f)
        {
            const InputStep& s = steps[si++];
            nds->SetKeyMask(s.mask);
            if (s.touch) nds->TouchScreen(s.tx, s.ty);
            else nds->ReleaseScreen();
        }
        bool gotReport = link && link->Poll(pad);
        if (link && std::exchange(reportWaiting, false))
            gotReport = true;
        if (gotReport)
        {
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
            // the DS's pad: libnds bit set = held, melonDS bit clear = pressed
            nds->SetKeyMask(~pad.keys & 0xFFF);
            if (pad.keys & (1 << 12)) nds->TouchScreen(pad.touchX, pad.touchY);
            else nds->ReleaseScreen();
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
            nds->SetKeyMask(0xFFF);
            nds->ReleaseScreen();
            pad.keys = 0;
            lastFullReq = f;
            rec.RequestFull();
        }
        if (link && (pad.keys & 0xFFF)) padFrames++;
        if (pico) pico->Frame();
        nds->RunFrame();
        void *top = nullptr, *bot = nullptr;
        nds->GPU.GetFramebuffers(&top, &bot);
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
    gMirrorTap = nullptr;
    gCardHook = nullptr;
    return 0;
}
