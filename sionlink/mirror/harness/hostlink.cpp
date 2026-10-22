#include "hostlink.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <mutex>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#ifdef HAVE_LIBUSB
#include <libusb-1.0/libusb.h>
#endif

// Reports arrive as whole 16-byte mirror_input structs; keep the newest valid one.
static bool TakeReports(std::vector<uint8_t>& acc, mirror_input& in)
{
    bool got = false;
    size_t i = 0;
    while (acc.size() - i >= sizeof(mirror_input))
    {
        mirror_input r;
        memcpy(&r, &acc[i], sizeof r);
        if (r.magic != MIRROR_INPUT_MAGIC) { i++; continue; }   // resync
        in = r;
        got = true;
        i += sizeof r;
    }
    acc.erase(acc.begin(), acc.begin() + i);
    return got;
}

class SocketLink : public HostLink
{
    int fd = -1;
    std::vector<uint8_t> acc;
public:
    explicit SocketLink(const std::string& path)
    {
        int l = socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un a {};
        a.sun_family = AF_UNIX;
        strncpy(a.sun_path, path.c_str(), sizeof a.sun_path - 1);
        unlink(path.c_str());
        if (bind(l, (sockaddr*)&a, sizeof a) || listen(l, 1)) { perror("link: bind"); close(l); return; }
        fprintf(stderr, "link: waiting for the DS on %s\n", path.c_str());
        fd = accept(l, nullptr, nullptr);
        close(l);
        unlink(path.c_str());
        int small = 4096;   // see dspico_sim.cpp: little buffering, like USB
        if (fd >= 0) setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &small, sizeof small);
    }
    ~SocketLink() { if (fd >= 0) close(fd); }
    bool Ok() const { return fd >= 0; }
    bool Send(const uint8_t* d, size_t n) override
    {
        while (n)
        {
            // keep draining reports while the DS side is slow to take data
            pollfd p {fd, POLLOUT | POLLIN, 0};
            poll(&p, 1, 100);
            if (p.revents & POLLIN) ReadSome();
            if (!(p.revents & POLLOUT)) { if (p.revents & (POLLHUP | POLLERR)) return false; continue; }
            ssize_t w = send(fd, d, n, MSG_DONTWAIT | MSG_NOSIGNAL);
            if (w > 0) { d += w; n -= w; }
            else if (errno != EAGAIN) return false;
        }
        return true;
    }
    void ReadSome()
    {
        uint8_t tmp[4096];
        ssize_t r = recv(fd, tmp, sizeof tmp, MSG_DONTWAIT);
        if (r > 0) acc.insert(acc.end(), tmp, tmp + r);
    }
    bool Poll(mirror_input& in) override
    {
        ReadSome();
        return TakeReports(acc, in);
    }
};

#ifdef HAVE_LIBUSB
class UsbLink : public HostLink
{
    libusb_context* ctx = nullptr;
    libusb_device_handle* h = nullptr;
    libusb_transfer* inXfer = nullptr;
    uint8_t inBuf[64];
    std::vector<uint8_t> acc;
    std::mutex accLock;          // InDone runs on whichever thread handles events
    std::atomic<bool> closing {false}, inIdle {false};
    std::atomic<int> inStatus {0};
    int inErrors = 0;
    // the DS's nudge request (ds/source/usblink.h: SL_REQ_NUDGE, wValue SL_NUDGE_OUT or _IN)
    enum { REQ_NUDGE = 1, NUDGE_OUT = 1, NUDGE_IN = 2 };
    libusb_transfer* nudgeXfer = nullptr;
    uint8_t nudgeBuf[LIBUSB_CONTROL_SETUP_SIZE];
    std::atomic<bool> nudgeBusy {false};
    std::atomic<int> nudges {0};
    std::atomic<int64_t> lastReportMs {0}, inSubmittedMs {0};
    int64_t lastInNudgeMs = 0;
    static int64_t NowMs()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    static const char* StatusName(int s)
    {
        static const char* const names[] = {"completed", "error", "timed out", "cancelled", "stalled", "device gone", "overflow"};
        return s >= 0 && s <= 6 ? names[s] : "failed";
    }
    static void InDone(libusb_transfer* t)
    {
        UsbLink* self = (UsbLink*)t->user_data;
        if (t->status == LIBUSB_TRANSFER_COMPLETED)
        {
            std::lock_guard<std::mutex> g(self->accLock);
            self->acc.insert(self->acc.end(), t->buffer, t->buffer + t->actual_length);
            if (t->actual_length) self->lastReportMs = NowMs();
        }
        bool again = !self->closing && (t->status == LIBUSB_TRANSFER_COMPLETED || t->status == LIBUSB_TRANSFER_TIMED_OUT);
        if (!again || libusb_submit_transfer(t))
        {
            // Poll restarts it (a stalled endpoint needs a clear-halt, which can't run in here)
            self->inStatus = t->status;
            self->inIdle = true;
        }
    }
    // A report transfer that ended in an error used to stop the DS's reports for good,
    // and with them its requests for a full frame. Start it again.
    void RestartIn()
    {
        if (!inIdle || closing) return;
        if (inStatus == LIBUSB_TRANSFER_NO_DEVICE) return;
        if (++inErrors <= 20)
            fprintf(stderr, "link: reading the DS's reports %s, starting again\n", StatusName(inStatus));
        if (inStatus == LIBUSB_TRANSFER_STALL) libusb_clear_halt(h, 0x81);
        inIdle = false;
        inSubmittedMs = NowMs();
        if (libusb_submit_transfer(inXfer))
            inIdle = true;
    }
    static void NudgeDone(libusb_transfer* t)
    {
        UsbLink* self = (UsbLink*)t->user_data;
        if (t->status != LIBUSB_TRANSFER_COMPLETED && self->nudges <= 10)
            fprintf(stderr, "link: the DS didn't answer a nudge (%s)\n", StatusName(t->status));
        self->nudgeBusy = false;
    }
    // Tell the DS something has been stuck a while. It checks for itself and starts the
    // transfer again if the DSpico missed a command (nudgeOut / nudgeIn in usblink.c).
    void Nudge(uint16_t why)
    {
        if (closing || nudgeBusy.exchange(true)) return;
        libusb_fill_control_setup(nudgeBuf, LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE | LIBUSB_ENDPOINT_OUT,
                                  REQ_NUDGE, why, 0, 0);
        libusb_fill_control_transfer(nudgeXfer, h, nudgeBuf, NudgeDone, this, 1000);
        if (libusb_submit_transfer(nudgeXfer)) { nudgeBusy = false; return; }
        int n = ++nudges;
        if (n <= 10 || n % 100 == 0)
            fprintf(stderr, "link: nudged the DS (%s; %d so far)\n", why == NUDGE_IN ? "no reports" : "it takes no data", n);
    }
public:
    UsbLink()
    {
        if (libusb_init(&ctx)) return;
        fprintf(stderr, "link: waiting for the DSpico (1209:0001)\n");
        while (!(h = libusb_open_device_with_vid_pid(ctx, 0x1209, 0x0001)))
            usleep(250000);
        libusb_set_auto_detach_kernel_driver(h, 1);
        // The DS's first SET_CONFIGURATION after its client starts times out (Linux logs
        // "can't set config #1, error -110") and leaves the DSpico unconfigured; asking
        // again works. Do that here instead of needing a replug.
        for (int tries = 0; tries < 5; tries++)
        {
            int cfg = -1;
            if (libusb_get_configuration(h, &cfg) == 0 && cfg == 1) break;
            fprintf(stderr, "link: the DSpico isn't set up (configuration %d), setting it up\n", cfg);
            int r = libusb_set_configuration(h, 1);
            if (r == 0) { fprintf(stderr, "link: DSpico set up\n"); break; }
            fprintf(stderr, "link: setting up the DSpico failed (%s)\n", libusb_error_name(r));
            usleep(500000);
        }
        if (libusb_claim_interface(h, 0)) { fprintf(stderr, "link: can't claim the DSpico\n"); libusb_close(h); h = nullptr; return; }
        nudgeXfer = libusb_alloc_transfer(0);
        inXfer = libusb_alloc_transfer(0);
        libusb_fill_interrupt_transfer(inXfer, h, 0x81, inBuf, sizeof inBuf, InDone, this, 0);
        inSubmittedMs = lastReportMs = NowMs();
        if (libusb_submit_transfer(inXfer))
        {
            inStatus = LIBUSB_TRANSFER_ERROR;
            inIdle = true;   // Poll tries again
        }
        fprintf(stderr, "link: DSpico connected\n");
    }
    ~UsbLink()
    {
        closing = true;
        if (inXfer)
        {
            // the IN transfer may still be in flight: wait for its callback before freeing it
            libusb_cancel_transfer(inXfer);
            for (int i = 0; i < 50 && !inIdle; i++)
            {
                timeval tv {0, 20000};
                libusb_handle_events_timeout(ctx, &tv);
            }
            if (inIdle) libusb_free_transfer(inXfer);
        }
        if (nudgeXfer)
        {
            if (nudgeBusy) libusb_cancel_transfer(nudgeXfer);
            for (int i = 0; i < 50 && nudgeBusy; i++)
            {
                timeval tv {0, 20000};
                libusb_handle_events_timeout(ctx, &tv);
            }
            if (!nudgeBusy) libusb_free_transfer(nudgeXfer);
        }
        if (h) { libusb_release_interface(h, 0); libusb_close(h); }
        if (ctx) libusb_exit(ctx);
    }
    bool Ok() const { return h != nullptr; }
    static void OutDone(libusb_transfer* t) { *(int*)t->user_data = 1; }
    bool Send(const uint8_t* d, size_t n) override
    {
        // in pieces of 4 KB (8 DS reads; the recorder pads bundles to whole reads), so
        // waiting long on one means the DS stopped taking data, not that a bundle is big
        while (n)
        {
            size_t k = std::min<size_t>(n, 4096);
            if (!SendPiece(d, k)) return false;
            d += k;
            n -= k;
        }
        return true;
    }
    bool SendPiece(const uint8_t* d, size_t n)
    {
        int done = 0;
        libusb_transfer* t = libusb_alloc_transfer(0);
        libusb_fill_bulk_transfer(t, h, 0x01, (uint8_t*)d, (int)n, OutDone, &done, 0);
        if (n % 512 && n % 64 == 0) t->flags = LIBUSB_TRANSFER_ADD_ZERO_PACKET;
        if (libusb_submit_transfer(t)) { libusb_free_transfer(t); return false; }
        int64_t start = NowMs(), lastNudge = start;
        bool told = false;
        while (!done)
        {
            timeval tv {0, 50000};
            libusb_handle_events_timeout_completed(ctx, &tv, &done);
            if (done) break;
            // 0.15 s without the DS taking 4 KB: it may have missed the DSpico
            // command that arms its transfer, or the event that ends one
            int64_t now = NowMs();
            if (now - lastNudge >= 150)
            {
                lastNudge = now;
                Nudge(NUDGE_OUT);
            }
            if (!told && now - start >= 5000)
            {
                told = true;
                fprintf(stderr, "link: the DS has taken no data for 5 s\n");
            }
        }
        if (told) fprintf(stderr, "link: the DS takes data again\n");
        bool ok = t->status == LIBUSB_TRANSFER_COMPLETED;
        if (!ok) fprintf(stderr, "link: transfer %s\n", StatusName(t->status));
        libusb_free_transfer(t);
        return ok;
    }
    bool Poll(mirror_input& in) override
    {
        // the sender thread handles events while it sends; when it is idle, do it here
        if (libusb_try_lock_events(ctx) == 0)
        {
            timeval tv {0, 0};
            if (libusb_event_handling_ok(ctx)) libusb_handle_events_locked(ctx, &tv);
            libusb_unlock_events(ctx);
        }
        RestartIn();
        // The DS sends a report at least every 8 frames. None for 0.4 s while we've been
        // reading all along: the DSpico may have missed the command that started one,
        // and the DS would wait for it forever while we keep its old buttons held.
        int64_t now = NowMs();
        if (!inIdle && now - inSubmittedMs > 250 && now - lastReportMs > 400 && now - lastInNudgeMs > 400)
        {
            lastInNudgeMs = now;
            Nudge(NUDGE_IN);
        }
        std::lock_guard<std::mutex> g(accLock);
        return TakeReports(acc, in);
    }
};
#endif

std::unique_ptr<HostLink> OpenHostLink(const std::string& spec)
{
    if (spec.rfind("sim:", 0) == 0)
    {
        auto l = std::make_unique<SocketLink>(spec.substr(4));
        if (l->Ok()) return l;
    }
#ifdef HAVE_LIBUSB
    else if (spec == "usb")
    {
        auto l = std::make_unique<UsbLink>();
        if (l->Ok()) return l;
    }
#endif
    else
        fprintf(stderr, "link: unknown link %s (sim:PATH or usb)\n", spec.c_str());
    return nullptr;
}
