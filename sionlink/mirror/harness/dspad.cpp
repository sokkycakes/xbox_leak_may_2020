// dspad: the DS as a pad for the Sion while no DS game is mirrored. It holds the DSpico (the
// live link's USB device), tells the DS app it is a pad (MREC_PAD bundles: when it takes the
// DS, then every second) and turns the DS's reports into an Xbox 360 pad on uinput, laid out
// as the kernel's xpad driver reports one, so xbcompat (the dashboard, Frolick, games) and
// SDL take it like a real one. While a mirror runs it lets go: mirrorrun asks it to over a
// unix socket before it takes the DSpico (hostlink.cpp, HoldDsPadOff), and it takes the DS
// back once that connection closes, however mirrorrun ends.
//
//   dspad [--link usb|sim:PATH] [--sock PATH] [--dry 1]
//
// The DS's buttons as the pad's: A, B, X, Y as labelled, L and R the triggers, Start, Select
// as Back, the d-pad. Select held 1.5 s on its own also holds the Guide button (Select keeps
// working as Back meanwhile, as it does for a DS game's menu). The pad is there only while
// the DS app reports (a DS switched off takes it away after 3 s), so it never takes up a
// port with no DS. --dry 1 logs the pad instead of making it (cloud tests: no uinput).
// A log file it writes to (stderr, opened for appending) is emptied past 256 KB: the
// Sion's /var/log is in RAM, and a DS left on in another program keeps it talking.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <linux/uinput.h>
#include <mutex>
#include <poll.h>
#include <signal.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "hostlink.h"
#include "lzpack.h"

static bool dry;
static std::atomic<bool> quit {false};

static void TrimLog()
{
    struct stat st;
    if (fstat(2, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > (256 << 10) && ftruncate(2, 0)) {}
}

static void Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void Log(const char* fmt, ...)
{
    TrimLog();
    time_t t = time(nullptr);
    char when[32];
    strftime(when, sizeof when, "%F %T", localtime(&t));
    fprintf(stderr, "%s dspad: ", when);
    va_list a;
    va_start(a, fmt);
    vfprintf(stderr, fmt, a);
    va_end(a);
    fputc('\n', stderr);
}

// ---------------------------------------------------------------- the pad (uinput)

class Pad
{
    int fd = -1;
    bool made = false;
    uint32_t keysWas = 0;
    int hatX = 0, hatY = 0, ltWas = 0, rtWas = 0;
    bool guide = false;
    int64_t selectSince = 0, failedAt = -1;
    void Abs(int code, int min, int max)
    {
        uinput_abs_setup a {};
        a.code = code;
        a.absinfo.minimum = min;
        a.absinfo.maximum = max;
        ioctl(fd, UI_ABS_SETUP, &a);
    }
    void Emit(int type, int code, int value)
    {
        if (dry)
        {
            if (type != EV_SYN) Log("pad: %s %d = %d", type == EV_KEY ? "button" : "axis", code, value);
            return;
        }
        input_event e {};
        e.type = type;
        e.code = code;
        e.value = value;
        if (write(fd, &e, sizeof e) < 0) {}
    }
public:
    bool Made() const { return made; }
    void Make()
    {
        if (made) return;
        if (dry) { made = true; Log("pad: made (dry run)"); return; }
        // a pad that couldn't be made is tried again every 10 s, not at every report
        int64_t now = HostLink::NowMs();
        if (failedAt >= 0 && now - failedAt < 10000) return;
        failedAt = now;
        fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
        if (fd < 0) { Log("no /dev/uinput (%s): no pad", strerror(errno)); return; }
        ioctl(fd, UI_SET_EVBIT, EV_KEY);
        ioctl(fd, UI_SET_EVBIT, EV_ABS);
        static const int buttons[] = { BTN_A, BTN_B, BTN_X, BTN_Y, BTN_TL, BTN_TR, BTN_SELECT, BTN_START, BTN_MODE, BTN_THUMBL, BTN_THUMBR };
        for (int b : buttons) ioctl(fd, UI_SET_KEYBIT, b);
        static const int axes[] = { ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_Z, ABS_RZ, ABS_HAT0X, ABS_HAT0Y };
        for (int a : axes) ioctl(fd, UI_SET_ABSBIT, a);
        Abs(ABS_X, -32768, 32767);
        Abs(ABS_Y, -32768, 32767);
        Abs(ABS_RX, -32768, 32767);
        Abs(ABS_RY, -32768, 32767);
        Abs(ABS_Z, 0, 255);
        Abs(ABS_RZ, 0, 255);
        Abs(ABS_HAT0X, -1, 1);
        Abs(ABS_HAT0Y, -1, 1);
        uinput_setup s {};
        s.id.bustype = BUS_USB;
        s.id.vendor = 0x045e;
        s.id.product = 0x028e;
        s.id.version = 0x0114;
        snprintf(s.name, sizeof s.name, "Sion Link DS pad");
        if (ioctl(fd, UI_DEV_SETUP, &s) || ioctl(fd, UI_DEV_CREATE))
        {
            Log("uinput setup failed (%s): no pad", strerror(errno));
            close(fd);
            fd = -1;
            return;
        }
        made = true;
        failedAt = -1;
        Log("pad: made");
    }
    void Unmake()
    {
        if (!made) return;
        Release();
        if (fd >= 0)
        {
            ioctl(fd, UI_DEV_DESTROY);
            close(fd);
            fd = -1;
        }
        made = false;
        Log("pad: gone");
    }
    // the DS's keys (libnds keysHeld: A 0, B 1, Select 2, Start 3, Right 4, Left 5, Up 6,
    // Down 7, R 8, L 9, X 10, Y 11)
    void Set(uint32_t keys, int64_t nowMs)
    {
        if (!made) return;
        static const struct { int bit, code; } map[] = {
            {0, BTN_A}, {1, BTN_B}, {10, BTN_X}, {11, BTN_Y}, {2, BTN_SELECT}, {3, BTN_START},
        };
        bool any = false;
        for (auto& m : map)
            if (((keys ^ keysWas) >> m.bit) & 1)
            {
                Emit(EV_KEY, m.code, (keys >> m.bit) & 1);
                any = true;
            }
        int hx = (keys & 0x10 ? 1 : 0) - (keys & 0x20 ? 1 : 0);
        int hy = (keys & 0x80 ? 1 : 0) - (keys & 0x40 ? 1 : 0);
        if (hx != hatX) { Emit(EV_ABS, ABS_HAT0X, hatX = hx); any = true; }
        if (hy != hatY) { Emit(EV_ABS, ABS_HAT0Y, hatY = hy); any = true; }
        int lt = keys & 0x200 ? 255 : 0, rt = keys & 0x100 ? 255 : 0;
        if (lt != ltWas) { Emit(EV_ABS, ABS_Z, ltWas = lt); any = true; }
        if (rt != rtWas) { Emit(EV_ABS, ABS_RZ, rtWas = rt); any = true; }
        // Select alone for 1.5 s: the Guide (L+R+Select is the DS app's own status page)
        bool selectAlone = (keys & 0x304) == 0x004;
        if (!selectAlone) selectSince = 0;
        else if (!selectSince) selectSince = nowMs;
        bool g = selectAlone && nowMs - selectSince >= 1500;
        if (g != guide) { Emit(EV_KEY, BTN_MODE, guide = g); any = true; }
        keysWas = keys;
        if (any) Emit(EV_SYN, SYN_REPORT, 0);
    }
    void Release()
    {
        Set(0, 0);
        selectSince = 0;
    }
};

// ---------------------------------------------------------------- the DS app's pad screen

static std::vector<uint8_t> PadBundle(bool on)
{
    std::vector<uint8_t> b;
    auto put = [&](const void* p, size_t n) { b.insert(b.end(), (const uint8_t*)p, (const uint8_t*)p + n); };
    mirror_hdr h {MIRROR_MAGIC, 0, 0, MIRROR_FLAG_PAD};
    put(&h, sizeof h);
    std::vector<uint8_t> payload;
    mirror_pad p {};
    p.on = on;
    payload.insert(payload.end(), (uint8_t*)&p, (uint8_t*)&p + sizeof p);
    for (const char* s : {"Controller for the Sion", "Hold Select for the Guide"})
        payload.insert(payload.end(), s, s + strlen(s) + 1);
    payload.resize((payload.size() + 3) & ~size_t(3), 0);
    mirror_rec r {MREC_PAD, 0, (uint32_t)payload.size()};
    put(&r, sizeof r);
    put(payload.data(), payload.size());
    mirror_rec e {MREC_END, 0, 0};
    put(&e, sizeof e);
    ((mirror_hdr*)b.data())->size = (uint32_t)b.size();
    b = LzPackBundle(b);
    // whole 512-byte blocks, as mirrorrun sends them (the DS reads in 512-byte transfers)
    b.resize((b.size() + 511) & ~size_t(511), 0);
    return b;
}

// ---------------------------------------------------------------- letting go for a mirror

static std::mutex lock;
static std::condition_variable changed;
static int holders;               // mirrors that asked us to let go and are still running
static bool released = true;      // we don't have the DSpico
static std::atomic<bool> letGo {false};

static void Control(const std::string& path)
{
    int l = socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un a {};
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, path.c_str(), sizeof a.sun_path - 1);
    unlink(path.c_str());
    if (bind(l, (sockaddr*)&a, sizeof a) || listen(l, 4))
    {
        Log("can't listen on %s (%s): a mirror can't ask for the DS", path.c_str(), strerror(errno));
        return;
    }
    std::vector<int> held;
    while (!quit)
    {
        std::vector<pollfd> p {{l, POLLIN, 0}};
        for (int fd : held) p.push_back({fd, POLLIN, 0});
        if (poll(p.data(), p.size(), 500) <= 0) continue;
        if (p[0].revents & POLLIN)
        {
            int c = accept(l, nullptr, nullptr);
            if (c < 0) continue;
            char line[32] = {};
            pollfd cp {c, POLLIN, 0};
            if (poll(&cp, 1, 1000) <= 0 || read(c, line, sizeof line - 1) <= 0 || strncmp(line, "release", 7))
            {
                close(c);
                continue;
            }
            std::unique_lock<std::mutex> g(lock);
            holders++;
            letGo = true;
            gMirrorQuit = true;   // a send or a wait for the DS stops at once
            changed.notify_all();
            changed.wait_for(g, std::chrono::seconds(3), [] { return released; });
            Log("a mirror takes the DS");
            if (write(c, "released\n", 9) < 0) {}
            held.push_back(c);
        }
        for (size_t i = 1; i < p.size(); i++)
        {
            if (!(p[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            char tmp[64];
            if (read(p[i].fd, tmp, sizeof tmp) > 0) continue;
            close(p[i].fd);
            held.erase(std::find(held.begin(), held.end(), p[i].fd));
            std::lock_guard<std::mutex> g(lock);
            if (--holders == 0)
            {
                Log("the mirror is done: taking the DS back");
                letGo = false;
                changed.notify_all();
            }
            break;   // p no longer matches held
        }
    }
}

// ---------------------------------------------------------------- holding the DS

static void Serve(const std::string& spec, Pad& pad)
{
    {
        // from here until we say so, a mirror that asks waits for us to let go
        std::lock_guard<std::mutex> g(lock);
        if (letGo) return;
        released = false;
    }
    std::unique_ptr<HostLink> link = OpenHostLink(spec, &letGo);
    if (!link)
    {
        std::lock_guard<std::mutex> g(lock);
        released = true;
        changed.notify_all();
        return;
    }
    Log("the DS is the Sion's pad");
    // the pad screen now and every second (a mirror that comes has the DS show its own
    // screen meanwhile); sent on a thread of its own, as a DS that takes nothing blocks it
    std::atomic<bool> dead {false}, stopSending {false};
    std::thread sender([&] {
        std::vector<uint8_t> on = PadBundle(true);
        auto next = std::chrono::steady_clock::now();
        while (!stopSending && !dead)
        {
            if (!link->Send(on.data(), on.size()))
            {
                if (!gMirrorQuit) dead = true;
                break;
            }
            next += std::chrono::seconds(1);
            while (!stopSending && std::chrono::steady_clock::now() < next)
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });
    mirror_input in {};
    int64_t lastReport = 0, lastTrim = 0;
    while (!quit && !letGo && !dead)
    {
        int64_t now = HostLink::NowMs();
        if (link->Poll(in))
        {
            lastReport = now;
            pad.Make();
            pad.Set(in.keys, now);
        }
        else
            pad.Set(in.keys, now);   // the Guide's hold time runs on
        if (pad.Made() && now - lastReport > 3000)
        {
            Log("no reports from the DS for 3 s");
            pad.Unmake();
            in.keys = 0;
        }
        if (now / 1000 != lastTrim) { lastTrim = now / 1000; TrimLog(); }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    pad.Unmake();
    stopSending = true;
    if (letGo && !dead)
    {
        // tell the DS a mirror is coming (what it shows until the mirror's first frame);
        // gMirrorQuit stops the sender's wait, so let this one try for a moment on its own
        sender.join();
        gMirrorQuit = false;
        std::atomic<bool> said {false};
        std::thread bye([&] {
            std::vector<uint8_t> off = PadBundle(false);
            link->Send(off.data(), off.size());
            said = true;
        });
        auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
        while (!said && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        gMirrorQuit = true;
        bye.join();
    }
    else
        sender.join();
    if (dead) Log("the DS went away");
    link.reset();
    std::lock_guard<std::mutex> g(lock);
    released = true;
    changed.notify_all();
}

int main(int argc, char** argv)
{
    std::string spec = "usb", sock = "/run/sionlink-pad.sock";
    for (int i = 1; i + 1 < argc; i += 2)
    {
        std::string o = argv[i];
        if (o == "--link") spec = argv[i + 1];
        else if (o == "--sock") sock = argv[i + 1];
        else if (o == "--dry") dry = atoi(argv[i + 1]) != 0;
        else
        {
            fprintf(stderr, "usage: %s [--link usb|sim:PATH] [--sock PATH] [--dry 1]\n", argv[0]);
            return 1;
        }
    }
    gDsPadSelf = true;
    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa {};
    sa.sa_handler = [](int) { quit = true; gMirrorQuit = true; };
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
    std::thread control(Control, sock);
    control.detach();
    Pad pad;
    Log("waiting for the DS (%s)", spec.c_str());
    while (!quit)
    {
        {
            // while a mirror has the DS, wait for it to be done
            std::unique_lock<std::mutex> g(lock);
            changed.wait(g, [] { return !letGo || quit; });
            if (quit) break;
            gMirrorQuit = false;
        }
        Serve(spec, pad);
        if (!letGo && !quit) std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    pad.Unmake();
    unlink(sock.c_str());
    return 0;
}
