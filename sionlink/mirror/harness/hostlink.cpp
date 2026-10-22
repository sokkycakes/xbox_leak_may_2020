#include "hostlink.h"
#include <cstdio>
#include <cstring>
#include <vector>
#include <mutex>
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
    bool closing = false, inIdle = false;
    static void InDone(libusb_transfer* t)
    {
        UsbLink* self = (UsbLink*)t->user_data;
        if (t->status == LIBUSB_TRANSFER_COMPLETED)
        {
            std::lock_guard<std::mutex> g(self->accLock);
            self->acc.insert(self->acc.end(), t->buffer, t->buffer + t->actual_length);
        }
        bool again = !self->closing && (t->status == LIBUSB_TRANSFER_COMPLETED || t->status == LIBUSB_TRANSFER_TIMED_OUT);
        if (!again || libusb_submit_transfer(t))
            self->inIdle = true;
    }
public:
    UsbLink()
    {
        if (libusb_init(&ctx)) return;
        fprintf(stderr, "link: waiting for the DSpico (1209:0001)\n");
        while (!(h = libusb_open_device_with_vid_pid(ctx, 0x1209, 0x0001)))
            usleep(250000);
        libusb_set_auto_detach_kernel_driver(h, 1);
        if (libusb_claim_interface(h, 0)) { fprintf(stderr, "link: can't claim the DSpico\n"); libusb_close(h); h = nullptr; return; }
        inXfer = libusb_alloc_transfer(0);
        libusb_fill_bulk_transfer(inXfer, h, 0x81, inBuf, sizeof inBuf, InDone, this, 0);
        libusb_submit_transfer(inXfer);
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
        if (h) { libusb_release_interface(h, 0); libusb_close(h); }
        if (ctx) libusb_exit(ctx);
    }
    bool Ok() const { return h != nullptr; }
    static void OutDone(libusb_transfer* t) { *(int*)t->user_data = 1; }
    bool Send(const uint8_t* d, size_t n) override
    {
        // one transfer per bundle; the zero-length packet ends the DS's 512-byte
        // read when the bundle fills its last packet exactly
        int done = 0;
        libusb_transfer* t = libusb_alloc_transfer(0);
        libusb_fill_bulk_transfer(t, h, 0x01, (uint8_t*)d, (int)n, OutDone, &done, 0);
        t->flags = LIBUSB_TRANSFER_ADD_ZERO_PACKET;
        if (libusb_submit_transfer(t)) { libusb_free_transfer(t); return false; }
        int waited = 0;
        while (!done)
        {
            timeval tv {1, 0};
            libusb_handle_events_timeout_completed(ctx, &tv, &done);
            if (!done && ++waited == 5)
                fprintf(stderr, "link: the DS has taken no data for 5 s\n");
        }
        bool ok = t->status == LIBUSB_TRANSFER_COMPLETED;
        static const char* const names[] = {"completed", "error", "timed out", "cancelled", "stalled", "device gone", "overflow"};
        if (!ok) fprintf(stderr, "link: transfer %s\n", t->status <= 6 ? names[t->status] : "failed");
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
