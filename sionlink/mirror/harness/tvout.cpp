// The game on the Sion's TV (see tvout.h). The display code follows xbox-av
// (xbcompat/tools/sion/av/xbox-av.c), which keeps the picture between titles: same
// connector and CRTC search, same dumb buffers, and the TV's current mode as it is,
// so taking the screen over doesn't make the HDMI-to-AV box resync.

#include "tvout.h"

#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

static int Xioctl(int fd, unsigned long req, void* arg)
{
    int r;
    do r = ioctl(fd, req, arg);
    while (r < 0 && (errno == EINTR || errno == EAGAIN));
    return r;
}

static inline uint32_t Lerp(uint32_t a, uint32_t b, uint32_t f)   // f: 0..256 of b
{
    uint32_t rb = (((a & 0xFF00FF) * (256 - f) + (b & 0xFF00FF) * f) >> 8) & 0xFF00FF;
    uint32_t g = (((a & 0x00FF00) * (256 - f) + (b & 0x00FF00) * f) >> 8) & 0x00FF00;
    return rb | g;
}

// The screens as big as they fit inside the TV's title-safe area (an SD picture on a CRT
// loses its edges), at the DS's shape: SD modes fill a 4:3 TV, so their pixels aren't
// square. Stacked, each screen gets half the height; side by side, half the width; with
// one big, the other is half its size beside it, on the side and at the edge where it
// would be on the DS (the bottom screen low on the right, the top one high on the left).
void TVOut::Layout()
{
    bool sd = h <= 576;
    // The 15 kHz mode for a VGA transcoder (2880x480i, sion-video) cuts each line into four
    // times BT.601's 720 pixels: lay the picture out 720 wide and widen each row as it goes
    // out, which costs a copy instead of four times the scaling.
    rep = sd && w > 720 && w % 720 == 0 ? w / 720 : 1;
    const uint32_t w = this->w / rep;
    narrow.assign(rep > 1 ? (size_t)w * h : 0, 0);
    double aspect = sd ? 4.0 / 3.0 : (double)w / h;
    par = aspect / ((double)w / h);
    double aw = w * (sd ? 0.9 : 1.0), ah = h * (sd ? 0.9 : 1.0);
    double r = 4.0 / 3.0 / par;   // a screen's width in pixels for each line of its height
    // the whole arrangement's size, in screen heights (the big one's)
    double fw = r, fh = 2;
    if (SingleScreen(layout)) fh = 1;
    else if (layout == TVLayout::SideBySide) fw = 2 * r, fh = 1;
    else if (layout == TVLayout::BigTop || layout == TVLayout::BigBottom) fw = 1.5 * r, fh = 1;
    uint32_t bh = (uint32_t)std::min(aw / fw, ah / fh) & ~1u;
    uint32_t bw = (uint32_t)lround(bh * r) & ~1u;
    if (bw * fw / r > w)
    {
        bw = (uint32_t)(w / (fw / r)) & ~1u;
        bh = (uint32_t)lround(bw / r) & ~1u;
    }
    Place& t = place[0];
    Place& b = place[1];
    switch (layout)
    {
    case TVLayout::TopOnly:
    case TVLayout::BottomOnly:
    {
        // the other one isn't drawn at all
        Place& one = layout == TVLayout::TopOnly ? t : b;
        Place& none = layout == TVLayout::TopOnly ? b : t;
        one.w = bw; one.h = bh;
        one.x = (w - bw) / 2;
        one.y = (h - bh) / 2;
        none.x = none.y = none.w = none.h = 0;
        break;
    }
    case TVLayout::Stacked:
        t.w = b.w = bw; t.h = b.h = bh;
        t.x = b.x = (w - bw) / 2;
        t.y = (h - 2 * bh) / 2;
        b.y = t.y + bh;
        break;
    case TVLayout::SideBySide:
        t.w = b.w = bw; t.h = b.h = bh;
        t.x = (w - 2 * bw) / 2;
        b.x = t.x + bw;
        t.y = b.y = (h - bh) / 2;
        break;
    case TVLayout::BigTop:
        t.w = bw; t.h = bh;
        b.w = bw / 2 & ~1u; b.h = bh / 2 & ~1u;
        t.x = (w - t.w - b.w) / 2;
        b.x = t.x + t.w;
        t.y = (h - bh) / 2;
        b.y = t.y + t.h - b.h;
        break;
    case TVLayout::BigBottom:
        b.w = bw; b.h = bh;
        t.w = bw / 2 & ~1u; t.h = bh / 2 & ~1u;
        t.x = (w - t.w - b.w) / 2;
        b.x = t.x + t.w;
        t.y = b.y = (h - bh) / 2;
        break;
    default:
        break;
    }
    if (swap && !SingleScreen(layout))
    {
        std::swap(t.x, b.x); std::swap(t.y, b.y);
        std::swap(t.w, b.w); std::swap(t.h, b.h);
    }
    // bilinear: per output column/row the source pixel (<< 9) and how much of the next one
    auto table = [](std::vector<uint32_t>& tab, uint32_t out, uint32_t in) {
        tab.resize(out);
        for (uint32_t i = 0; i < out; i++)
        {
            double p = (i + 0.5) * in / out - 0.5;
            if (p < 0) p = 0;
            uint32_t k = (uint32_t)p;
            uint32_t f = (uint32_t)lround((p - k) * 256);
            if (k >= in - 1) { k = in - 2; f = 256; }
            tab[i] = k << 9 | f;
        }
    };
    for (Place& p : place)
    {
        table(p.xs, p.w, 256);
        table(p.ys, p.h, 192);
    }
    rowCache.resize(192 * std::max(t.w, b.w));
    fprintf(stderr, "tv: %ux%u%s, top screen %ux%u at %u,%u, bottom %ux%u at %u,%u\n", this->w, h,
            rep > 1 ? " (each pixel sent as several)" : "", t.w, t.h, t.x, t.y, b.w, b.h, b.x, b.y);
}

void TVOut::SetLayout(TVLayout l, bool s)
{
    if (SingleScreen(l)) s = false;
    if (l == layout && s == swap) return;
    layout = l;
    swap = s;
    Layout();
    clearFrames = 2;   // both buffers
}

void TVOut::SetOverlay(Overlay o)
{
    if (overlay && !o) clearFrames = 2;
    overlay = std::move(o);
}

void TVOut::Compose(uint32_t* dst, uint32_t pitch, const uint32_t* top, const uint32_t* bottom)
{
    if (rep == 1)
    {
        Draw(dst, pitch, w, top, bottom);
        return;
    }
    // laid out 720 wide, then each pixel sent rep times: all of it when the picture outside
    // the screens changes, else the screens' rows
    uint32_t nw = w / rep;
    bool whole = overlay || clearFrames > 0;
    Draw(narrow.data(), nw * 4, nw, top, bottom);
    for (uint32_t y = 0; y < h; y++)
    {
        bool in = false;
        for (const Place& p : place) in |= p.w && y >= p.y && y < p.y + p.h;
        if (!whole && !in) continue;
        const uint32_t* a = &narrow[(size_t)y * nw];
        uint32_t* o = dst + (size_t)y * (pitch / 4);
        for (uint32_t x = 0; x < nw; x++, o += rep)
            for (uint32_t k = 0; k < rep; k++) o[k] = a[x];
    }
}

void TVOut::Draw(uint32_t* dst, uint32_t pitch, uint32_t w, const uint32_t* top, const uint32_t* bottom)
{
    if (overlay || clearFrames > 0)
    {
        // what was outside the screens (the menu, the layout before) goes
        for (uint32_t y = 0; y < h; y++) memset((uint8_t*)dst + (size_t)y * pitch, 0, w * 4);
        if (clearFrames > 0) clearFrames--;
    }
    const uint32_t* screens[2] = {top, bottom};
    for (int s = 0; s < 2; s++)
    {
        const uint32_t* src = screens[s];
        const Place& p = place[s];
        if (!p.w) continue;   // not shown (one screen alone)
        for (uint32_t y = 0; y < 192; y++)
        {
            const uint32_t* in = src + y * 256;
            uint32_t* out = &rowCache[y * p.w];
            for (uint32_t x = 0; x < p.w; x++)
            {
                uint32_t k = p.xs[x] >> 9, f = p.xs[x] & 511;
                out[x] = Lerp(in[k], in[k + 1], f);
            }
        }
        for (uint32_t y = 0; y < p.h; y++)
        {
            uint32_t k = p.ys[y] >> 9, f = p.ys[y] & 511;
            const uint32_t* a = &rowCache[k * p.w];
            const uint32_t* b = a + p.w;
            uint32_t* out = dst + (size_t)(p.y + y) * (pitch / 4) + p.x;
            for (uint32_t x = 0; x < p.w; x++) out[x] = Lerp(a[x], b[x], f);
        }
    }
    if (overlay) overlay(dst, pitch, w, h, par);
}

bool TVOut::Open(const std::string& spec)
{
    if (spec.rfind("dump:", 0) == 0)
    {
        dump = fopen(spec.c_str() + 5, "wb");
        if (!dump) { perror(spec.c_str() + 5); return false; }
        // MIRROR_TVSIZE=WxH: another TV mode (2880x480 for the 15 kHz one)
        if (const char* e = getenv("MIRROR_TVSIZE")) sscanf(e, "%ux%u", &w, &h);
        dumpBuf.assign((size_t)w * h, 0);
        if (const char* e = getenv("MIRROR_TVDUMPEVERY")) dumpEvery = std::max(1, atoi(e));
        if (const char* e = getenv("MIRROR_TVDUMPFROM")) dumpFrom = std::max(0, atoi(e));
        Layout();
        if (getenv("MIRROR_TVPACE"))
        {
            vblankPeriod = 1.0 / 60;
            paced = true;
        }
        return true;
    }
    if (spec != "drm")
    {
        fprintf(stderr, "tv: unknown output %s (drm or dump:FILE)\n", spec.c_str());
        return false;
    }

    // xbox-av holds the display between titles: tell it a title is starting, and it lets go
    const char* sock = getenv("XBOX_AV_SOCKET");
    if (!sock || !*sock) sock = "/var/run/xbox-av.sock";
    av = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un sa {};
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", sock);
    char k = 0;
    timeval tv {5, 0};
    if (av >= 0 && connect(av, (sockaddr*)&sa, sizeof sa) == 0 &&
        setsockopt(av, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) == 0 &&
        send(av, "T", 1, MSG_NOSIGNAL) == 1 && recv(av, &k, 1, 0) == 1 && k == 'k')
        fprintf(stderr, "tv: xbox-av let go of the display\n");
    else
    {
        fprintf(stderr, "tv: no xbox-av at %s; taking the display as it is\n", sock);
        if (av >= 0) close(av);
        av = -1;
    }

    for (int i = 0; i < 8 && drm < 0; i++)
    {
        char path[32];
        snprintf(path, sizeof path, "/dev/dri/card%d", i);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        drm_mode_card_res res {};
        if (Xioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) == 0 && res.count_connectors && res.count_crtcs)
            drm = fd;
        else
            close(fd);
    }
    if (drm < 0) { fprintf(stderr, "tv: no display device\n"); Close(); return false; }
    // the display is ours once xbox-av has let go of it (or whoever had it has gone)
    bool master = false;
    for (int t = 0; t < 300 && !master; t++)
    {
        master = ioctl(drm, DRM_IOCTL_SET_MASTER, 0) == 0;
        if (!master) usleep(10000);
    }
    if (!master) { fprintf(stderr, "tv: the display is someone else's\n"); Close(); return false; }

    // the connected connector, its CRTC, and the mode that CRTC is in
    drm_mode_card_res res {};
    Xioctl(drm, DRM_IOCTL_MODE_GETRESOURCES, &res);
    std::vector<uint32_t> conns(res.count_connectors + 1), crtcs(res.count_crtcs + 1), encs(res.count_encoders + 1);
    res.connector_id_ptr = (uintptr_t)conns.data();
    res.crtc_id_ptr = (uintptr_t)crtcs.data();
    res.encoder_id_ptr = (uintptr_t)encs.data();
    res.fb_id_ptr = 0;
    res.count_fbs = 0;
    if (Xioctl(drm, DRM_IOCTL_MODE_GETRESOURCES, &res)) { fprintf(stderr, "tv: GETRESOURCES: %s\n", strerror(errno)); Close(); return false; }
    drm_mode_modeinfo mode {};
    bool haveMode = false;
    for (uint32_t c = 0; c < res.count_connectors && !haveMode; c++)
    {
        drm_mode_get_connector gc {};
        gc.connector_id = conns[c];
        if (Xioctl(drm, DRM_IOCTL_MODE_GETCONNECTOR, &gc) || gc.connection != 1 || !gc.count_modes) continue;
        std::vector<drm_mode_modeinfo> modes(gc.count_modes + 1);
        std::vector<uint32_t> cencs(gc.count_encoders + 1);
        uint32_t nmodes = gc.count_modes, nenc = gc.count_encoders;
        gc.modes_ptr = (uintptr_t)modes.data();
        gc.encoders_ptr = (uintptr_t)cencs.data();
        gc.count_props = 0;
        gc.props_ptr = gc.prop_values_ptr = 0;
        if (Xioctl(drm, DRM_IOCTL_MODE_GETCONNECTOR, &gc)) continue;
        if (gc.count_modes < nmodes) nmodes = gc.count_modes;
        if (gc.count_encoders < nenc) nenc = gc.count_encoders;
        uint32_t crtc = 0;
        if (gc.encoder_id)
        {
            drm_mode_get_encoder ge {};
            ge.encoder_id = gc.encoder_id;
            if (!Xioctl(drm, DRM_IOCTL_MODE_GETENCODER, &ge)) crtc = ge.crtc_id;
        }
        for (uint32_t e = 0; !crtc && e < nenc; e++)
        {
            drm_mode_get_encoder ge {};
            ge.encoder_id = cencs[e];
            if (Xioctl(drm, DRM_IOCTL_MODE_GETENCODER, &ge)) continue;
            for (uint32_t i = 0; i < res.count_crtcs; i++)
                if (ge.possible_crtcs & (1u << i)) { crtc = crtcs[i]; break; }
        }
        if (!crtc) continue;
        drm_mode_crtc gcr {};
        gcr.crtc_id = crtc;
        if (!Xioctl(drm, DRM_IOCTL_MODE_GETCRTC, &gcr) && gcr.mode_valid)
            mode = gcr.mode;   // what's on the TV now: keep it
        else
        {
            // nothing on screen: KMS_MODE (as bootani and xbox-av), else the preferred mode
            int ww = 0, wh = 0;
            const char* want = getenv("KMS_MODE");
            int pick = -1;
            // progressive first; interlaced when that's all there is (2880x480i, 15 kHz)
            if (want && sscanf(want, "%dx%d", &ww, &wh) == 2)
                for (int il = 0; il < 2 && pick < 0; il++)
                    for (uint32_t m = 0; m < nmodes && pick < 0; m++)
                        if (modes[m].hdisplay == ww && modes[m].vdisplay == wh &&
                            !!(modes[m].flags & DRM_MODE_FLAG_INTERLACE) == il) pick = m;
            for (uint32_t m = 0; m < nmodes && pick < 0; m++)
                if (modes[m].type & DRM_MODE_TYPE_PREFERRED) pick = m;
            mode = modes[pick < 0 ? 0 : pick];
        }
        connId = conns[c];
        crtcId = crtc;
        haveMode = true;
    }
    if (!haveMode) { fprintf(stderr, "tv: no connected display\n"); Close(); return false; }
    modeInfo.assign((uint8_t*)&mode, (uint8_t*)&mode + sizeof mode);
    w = mode.hdisplay;
    h = mode.vdisplay;
    fprintf(stderr, "tv: connector %u on CRTC %u, %ux%u %u Hz\n", connId, crtcId, w, h, mode.vrefresh);

    for (Buf& b : bufs)
    {
        drm_mode_create_dumb cd {};
        cd.width = w;
        cd.height = h;
        cd.bpp = 32;
        if (Xioctl(drm, DRM_IOCTL_MODE_CREATE_DUMB, &cd)) { fprintf(stderr, "tv: CREATE_DUMB: %s\n", strerror(errno)); Close(); return false; }
        b.handle = cd.handle;
        b.pitch = cd.pitch;
        b.size = cd.size;
        drm_mode_fb_cmd fc {};
        fc.width = w;
        fc.height = h;
        fc.pitch = cd.pitch;
        fc.bpp = 32;
        fc.depth = 24;
        fc.handle = cd.handle;
        drm_mode_map_dumb md {};
        md.handle = cd.handle;
        if (Xioctl(drm, DRM_IOCTL_MODE_ADDFB, &fc) || Xioctl(drm, DRM_IOCTL_MODE_MAP_DUMB, &md))
        {
            fprintf(stderr, "tv: framebuffer: %s\n", strerror(errno));
            Close();
            return false;
        }
        b.fb = fc.fb_id;
        void* map = mmap(nullptr, cd.size, PROT_READ | PROT_WRITE, MAP_SHARED, drm, md.offset);
        if (map == MAP_FAILED) { fprintf(stderr, "tv: mmap: %s\n", strerror(errno)); Close(); return false; }
        b.map = (uint8_t*)map;
        memset(b.map, 0, b.size);
    }
    Layout();
    paced = true;
    return true;
}

bool TVOut::WaitFlip(int ms)
{
    while (flipPending)
    {
        pollfd p {drm, POLLIN, 0};
        if (poll(&p, 1, ms) <= 0) return false;
        alignas(8) char ev[1024];
        ssize_t n = read(drm, ev, sizeof ev);
        for (ssize_t i = 0; i + (ssize_t)sizeof(drm_event) <= n;)
        {
            const drm_event* e = (const drm_event*)(ev + i);
            if (e->type == DRM_EVENT_FLIP_COMPLETE) flipPending = false;
            if (!e->length) break;
            i += e->length;
        }
    }
    return true;
}

void TVOut::Show(const uint32_t* top, const uint32_t* bottom)
{
    if (dump)
    {
        if (vblankPeriod)
        {
            // as a page flip: wait for the frame before to be on screen, and this one goes
            // up at the next vblank
            using namespace std::chrono;
            auto secs = [] { return duration<double>(steady_clock::now().time_since_epoch()).count(); };
            double now = secs();
            if (flipAt > now)
            {
                std::this_thread::sleep_for(duration<double>(flipAt - now));
                now = flipAt;
            }
            flipAt = (floor(now / vblankPeriod + 1e-6) + 1) * vblankPeriod;
        }
        if (frames++ >= dumpFrom && (frames - 1 - dumpFrom) % dumpEvery == 0)
        {
            Compose(dumpBuf.data(), w * 4, top, bottom);
            fwrite(dumpBuf.data(), 4, dumpBuf.size(), dump);
        }
        return;
    }
    if (drm < 0) return;
    // the frame before has to be on screen before its buffer can take this one
    if (flipPending && !WaitFlip(100))
    {
        // no word from the display: carry on at the game's own pace
        if (++flipTimeouts == 3 && paced)
        {
            fprintf(stderr, "tv: the display stopped answering page flips; the game keeps its own time\n");
            paced = false;
        }
        flipPending = false;
    }
    int back = front == 0 ? 1 : 0;
    Buf& b = bufs[back];
    Compose((uint32_t*)b.map, b.pitch, top, bottom);
    if (!crtcSet)
    {
        // the first picture: the mode the TV is already in, so it doesn't resync
        drm_mode_crtc sc {};
        sc.crtc_id = crtcId;
        sc.fb_id = b.fb;
        sc.set_connectors_ptr = (uintptr_t)&connId;
        sc.count_connectors = 1;
        memcpy(&sc.mode, modeInfo.data(), sizeof sc.mode);
        sc.mode_valid = 1;
        if (Xioctl(drm, DRM_IOCTL_MODE_SETCRTC, &sc))
        {
            fprintf(stderr, "tv: SETCRTC: %s\n", strerror(errno));
            return;
        }
        crtcSet = true;
        front = back;
        return;
    }
    drm_mode_crtc_page_flip pf {};
    pf.crtc_id = crtcId;
    pf.fb_id = b.fb;
    pf.flags = DRM_MODE_PAGE_FLIP_EVENT;
    pf.user_data = 1;
    if (ioctl(drm, DRM_IOCTL_MODE_PAGE_FLIP, &pf) == 0)
    {
        flipPending = true;
        front = back;
        flipTimeouts = 0;
    }
    else
    {
        static int errs;
        if (errs++ < 5) fprintf(stderr, "tv: PAGE_FLIP: %s\n", strerror(errno));
    }
}

double TVOut::Hz() const
{
    if (modeInfo.size() == sizeof(drm_mode_modeinfo))
    {
        const drm_mode_modeinfo* m = (const drm_mode_modeinfo*)modeInfo.data();
        if (m->htotal && m->vtotal && m->clock)
            return m->clock * 1000.0 / ((double)m->htotal * m->vtotal);
    }
    return 60;
}

void TVOut::Close()
{
    if (dump) { fclose(dump); dump = nullptr; }
    std::vector<uint32_t> last;
    if (drm >= 0)
    {
        if (flipPending) WaitFlip(100);
        if (crtcSet && front >= 0 && bufs[front].map)
        {
            last.resize((size_t)w * h);
            for (uint32_t y = 0; y < h; y++)
                memcpy(&last[(size_t)y * w], bufs[front].map + (size_t)y * bufs[front].pitch, w * 4);
        }
        // let go of the display, but keep our buffers until xbox-av has put its own up:
        // the TV keeps showing ours meanwhile (freeing them first would switch it off)
        ioctl(drm, DRM_IOCTL_DROP_MASTER, 0);
    }
    if (av >= 0)
    {
        // the last picture goes to xbox-av, which keeps it up until the dashboard is back
        uint32_t wh[2] = {last.empty() ? 0 : w, last.empty() ? 0 : h};
        char k = 0;
        bool ok = send(av, "S", 1, MSG_NOSIGNAL) == 1 && send(av, wh, sizeof wh, MSG_NOSIGNAL) == sizeof wh;
        size_t n = last.size() * 4, at = 0;
        while (ok && at < n)
        {
            ssize_t r = send(av, (const char*)last.data() + at, n - at, MSG_NOSIGNAL);
            if (r <= 0) ok = false;
            else at += r;
        }
        if (ok && recv(av, &k, 1, 0) == 1 && k == 'k')
            fprintf(stderr, "tv: handed the display back to xbox-av\n");
        else
            fprintf(stderr, "tv: xbox-av didn't take the display back (it shows black)\n");
        close(av);
        av = -1;
    }
    if (drm >= 0)
    {
        for (Buf& b : bufs)
        {
            if (b.map) munmap(b.map, b.size);
            if (b.fb) Xioctl(drm, DRM_IOCTL_MODE_RMFB, &b.fb);
            if (b.handle)
            {
                drm_mode_destroy_dumb dd {};
                dd.handle = b.handle;
                Xioctl(drm, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
            }
            b = {};
        }
        close(drm);
        drm = -1;
    }
    paced = false;
}
