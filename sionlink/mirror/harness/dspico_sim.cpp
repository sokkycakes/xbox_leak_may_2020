// See dspico_sim.h. Event words and buffer layout follow dspico-firmware
// (usbEventQueue.h, ntrCardRomGameUsb.cpp).

#include "dspico_sim.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>

using namespace melonDS;

enum : u32
{
    EV_BUS_RESET = 1,
    EV_XFER_COMPLETE = 0x30000000,
    EV_SETUP = 0x40000000,
    EV_LAST = 1u << 31,
    SILENT = 0xFFFFFFFF,   // fault test: a read that comes back all ones, losing its event
};

static u32 Slot(u32 ep) { return (ep & 0x7F) * 2 + (ep >> 7); }

// fault tests: DSPICO_<name>=N hits the Nth time, and every Nth after it with
// DSPICO_LOSE_EVERY=1 (a real DSpico now and then misses a card command)
static bool Fault(const char* name, unsigned count)
{
    const char* v = getenv(name);
    unsigned n = v ? atoi(v) : 0;
    static int every = getenv("DSPICO_LOSE_EVERY") ? 1 : 0;
    return n && (count == n || (every && count % n == 0));
}

DSpicoSim::DSpicoSim(NDS* n, const std::string& path, unsigned bytesPerFrame)
    : nds(n), budgetPerFrame(bytesPerFrame)
{
    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un a {};
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, path.c_str(), sizeof a.sun_path - 1);
    for (int tries = 0; connect(sock, (sockaddr*)&a, sizeof a) != 0; tries++)
    {
        if (tries > 200) { perror("dspico sim: connect"); close(sock); sock = -1; return; }
        usleep(50000);
    }
    fcntl(sock, F_SETFL, fcntl(sock, F_GETFL) | O_NONBLOCK);
    // a real USB host only sends when the DSpico has a buffer free: keep the
    // socket's own buffering small so the Sion side feels the same backpressure
    int small = 4096;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &small, sizeof small);
}

DSpicoSim::~DSpicoSim()
{
    if (sock >= 0) close(sock);
}

// ---------------------------------------------------------------- device side

void DSpicoSim::Event(u32 e)
{
    if (events.empty())
        nds->SetIRQ(0, IRQ_CartIREQMC);  // the DSpico raises the card IRQ line
    events.push_back(e);
}

void DSpicoSim::Event64(u32 e0, u32 e1)
{
    if (events.empty())
        nds->SetIRQ(0, IRQ_CartIREQMC);
    events.push_back(e0);
    events.push_back(e1);
}

bool DSpicoSim::Start(const u8* c, u32 len)
{
    if (c[0] < 0xE8 || c[0] > 0xEB)
        return false;
    if (crashed)
    {
        cmd[0] = 0;   // nothing drives the bus: reads come back all ones
        return true;
    }
    memcpy(cmd, c, 8);
    cmd0 = (c[0] << 24) | (c[1] << 16) | (c[2] << 8) | c[3];
    cmd1 = (c[4] << 24) | (c[5] << 16) | (c[6] << 8) | c[7];
    pos = 0;
    switch (c[0])
    {
    case 0xE8:
        UsbCommand((cmd0 >> 16) & 0xFF);
        break;
    case 0xEB:
        if (events.empty())
            eventWord = 0;
        else
        {
            eventWord = events.front();
            events.pop_front();
            if (eventWord == SILENT)
                break;
            if (events.empty())
                eventWord |= EV_LAST;
        }
        break;
    }
    return true;
}

u32 DSpicoSim::Receive()
{
    u32 v = 0xFFFFFFFF;
    if (cmd[0] == 0xEA)
    {
        u32 ep = (cmd0 >> 8) & 0xFF, idx = cmd0 & 1;
        if (pos < 512) memcpy(&v, &bufs[Slot(ep)][idx * 512 + pos], 4);
        pos += 4;
    }
    else if (cmd[0] == 0xEB)
        v = eventWord;
    return v;
}

void DSpicoSim::Transmit(u32 val)
{
    if (cmd[0] != 0xE9) return;
    u32 ep = (cmd0 >> 8) & 0xFF, idx = (cmd0 >> 16) & 1;
    if (pos < 512) memcpy(&bufs[Slot(ep)][idx * 512 + pos], &val, 4);
    pos += 4;
}

void DSpicoSim::Finish()
{
    if (crashed)
        return;
    if (cmd[0] == 0xE9 && (cmd0 & 1))
    {
        u32 ep = (cmd0 >> 8) & 0xFF, idx = (cmd0 >> 16) & 1;
        StartXfer(ep, idx, cmd1 > 512 ? 512 : cmd1);
    }
    PumpOut();
}

void DSpicoSim::UsbCommand(u32 sub)
{
    u32 ep = (cmd0 >> 8) & 0xFF;
    switch (sub)
    {
    case 1: usbInit = true; break;
    case 2: DeviceSent(0x80, nullptr, 0); break;  // BEGIN_SET_ADDRESS: zero-length status IN
    case 4:
        if (!connected && usbInit)
        {
            connected = true;
            // DSPICO_ATTACH=N: the cable goes in N frames after the DS connects (default 3)
            attachDelay = getenv("DSPICO_ATTACH") ? atoi(getenv("DSPICO_ATTACH")) : 3;
        }
        break;
    case 5: connected = false; configured = false; break;
    case 9:  // EP_STALL
        if ((ep & 0x7F) == 0 && step >= 0 && step < ENUM_DONE)
            ControlDone(false);
        break;
    case 12: address = ep; break;  // FINISH_SET_ADDRESS
    case 14: events.clear(); break;
    case 16:  // BEGIN_TRANSFER
        StartXfer(ep, cmd0 & 1, cmd1 > 512 ? 512 : cmd1);
        break;
    default: break;  // open/close endpoints, SOF, interrupts: nothing to model
    }
}

void DSpicoSim::StartXfer(u32 ep, u32 idx, u32 len)
{
    if (ep == 0x80)
    {
        DeviceSent(ep, &bufs[Slot(ep)][idx * 512], len);
        return;
    }
    if (ep & 0x80)
    {
        if (configured && Fault("DSPICO_LOSEIN", ++inStarts))
        {
            fprintf(stderr, "dspico sim: missed the command that starts report %u\n", inStarts);
            return;
        }
        if (in.armed)
            return Crash(ep);
        in.armed = true;
        in.ep = ep;
        in.idx = idx;
        in.len = len;
        return;
    }
    Out& o = outs[ep & 0xF];
    if (ep == outEp && configured && Fault("DSPICO_LOSEARM", ++arms))
    {
        fprintf(stderr, "dspico sim: missed the command that arms OUT transfer %u\n", arms);
        return;
    }
    if (o.armed && ep != 0)
        return Crash(ep);
    o.armed = true;
    o.idx = idx;
    o.len = len;
    o.got = 0;
    if (ep == 0 && step >= 0 && step < ENUM_DONE && stage == 1)
    {
        // status stage of a control read: the host sends a zero-length packet
        o.armed = false;
        Event(EV_XFER_COMPLETE | 0x00);
        ControlDone(true);
    }
}

// Starting a transfer on an endpoint that still waits panics the real firmware
// (tinyusb rp2040_usb.c); it stops answering the card bus
void DSpicoSim::Crash(u32 ep)
{
    fprintf(stderr, "dspico sim: PANIC ep %02X was already available (the DSpico has crashed)\n", ep);
    crashed = true;
    events.clear();
}

// ---------------------------------------------------------------- host side

void DSpicoSim::DeviceSent(u32 ep, const u8* data, u32 len)
{
    if (ep == 0x80)
    {
        Event(EV_XFER_COMPLETE | 0x80 | (len << 8));
        if (nudging && len == 0)
        {
            nudging = false;
            nudges++;
            fprintf(stderr, "dspico sim: nudged the DS (%s) at frame %u\n", nudgeWhy == 2 ? "no reports for 24 frames" : "no OUT progress for 9 frames", frameNo);
            return;
        }
        if (step < 0 || step >= ENUM_DONE) return;
        if (len > 0)
        {
            ctrlData.assign(data, data + len);
            stage = 1;   // now wait for the device to take the status packet
        }
        else
            ControlDone(true);  // zero-length status of a request without data
        return;
    }
    // fault test: DSPICO_DEAF=1 never delivers the DS's reports (the Sion stopped reading them)
    static int deaf = getenv("DSPICO_DEAF") ? 1 : 0;
    if (ep == inEp && deaf)
        return;
    if (ep == inEp)
        lastInFrame = frameNo;
    if (ep == inEp && sock >= 0)
    {
        // bulk IN: hand it to the Sion side
        u32 n = 0;
        while (n < len)
        {
            ssize_t w = send(sock, data + n, len - n, MSG_NOSIGNAL);
            if (w > 0) n += w;
            else if (errno != EAGAIN) break;
        }
    }
    Event(EV_XFER_COMPLETE | ep | (len << 8));
}

void DSpicoSim::SendSetup(u8 type, u8 req, u16 value, u16 index, u16 length)
{
    outs[0].armed = false;   // a setup packet ends whatever endpoint 0 was doing
    stage = 0;
    ctrlData.clear();
    Event64(EV_SETUP | ((length & 0x1FFF) << 16) | ((type >> 7) << 29) | index,
            value | (req << 16) | ((type & 0x7F) << 24));
}

void DSpicoSim::NextSetup()
{
    switch (step)
    {
    case ENUM_GET_DEV64:   SendSetup(0x80, 6, 0x0100, 0, 64); break;
    case ENUM_SET_ADDRESS: SendSetup(0x00, 5, 9, 0, 0); break;
    case ENUM_GET_DEV:     SendSetup(0x80, 6, 0x0100, 0, 18); break;
    case ENUM_GET_CFG9:    SendSetup(0x80, 6, 0x0200, 0, 9); break;
    case ENUM_GET_STR0:    SendSetup(0x80, 6, 0x0300, 0, 255); break;
    case ENUM_GET_STR2:    SendSetup(0x80, 6, 0x0302, 0x0409, 255); break;
    case ENUM_SET_CONFIG:  SendSetup(0x00, 9, 1, 0, 0); break;
    }
}

void DSpicoSim::ControlDone(bool ok)
{
    const std::vector<u8>& d = ctrlData;
    if (!ok)
    {
        fprintf(stderr, "dspico sim: device stalled enumeration step %d\n", step);
        step = ENUM_DONE;
        return;
    }
    switch (step)
    {
    case ENUM_GET_DEV:
        if (d.size() < 18 || d[0] != 18 || d[1] != 1)
        {
            fprintf(stderr, "dspico sim: bad device descriptor\n");
            step = ENUM_DONE;
            return;
        }
        fprintf(stderr, "dspico sim: device %04x:%04x class %02x, address %u\n",
                d[8] | (d[9] << 8), d[10] | (d[11] << 8), d[4], address);
        break;
    case ENUM_GET_CFG9:
        if (d.size() >= 4)
        {
            // ask for the whole configuration next
            step++;
            SendSetup(0x80, 6, 0x0200, 0, d[2] | (d[3] << 8));
            return;
        }
        break;
    case ENUM_GET_CFG:
        for (size_t i = 0; i + 1 < d.size() && d[i]; i += d[i])
        {
            if (d[i + 1] == 4 && i + 5 < d.size() && d[i + 5] != 0xFF)
                fprintf(stderr, "dspico sim: interface class %02x, expected vendor (ff)\n", d[i + 5]);
            if (d[i + 1] == 5 && i + 3 < d.size() && (d[i + 3] & 3) >= 2)   // bulk or interrupt
                (d[i + 2] & 0x80 ? inEp : outEp) = d[i + 2];
        }
        if (!inEp || !outEp)
        {
            fprintf(stderr, "dspico sim: no data endpoints\n");
            step = ENUM_DONE;
            return;
        }
        break;
    case ENUM_GET_STR2:
        product.clear();
        for (size_t i = 2; i + 1 < d.size(); i += 2) product += (char)d[i];
        break;
    case ENUM_SET_CONFIG:
        configured = true;
        lastInFrame = lastInNudge = frameNo;
        fprintf(stderr, "dspico sim: \"%s\" configured, OUT %02x IN %02x\n", product.c_str(), outEp, inEp);
        break;
    }
    step++;
    if (step < ENUM_DONE)
        NextSetup();
}

void DSpicoSim::ReadSocket()
{
    if (sock < 0 || fromHost.size() >= 1024) return;
    u8 tmp[1024];
    ssize_t r = read(sock, tmp, sizeof tmp);
    if (r <= 0) return;
    // fault test: DSPICO_DROP=N loses 100 bytes of the stream at byte N
    static long long dropAt = getenv("DSPICO_DROP") ? atoll(getenv("DSPICO_DROP")) : -1;
    static long long seen = 0;
    long long from = seen;
    seen += r;
    if (dropAt >= from && dropAt < seen)
    {
        size_t k = dropAt - from, n = std::min<size_t>(100, r - k);
        memmove(tmp + k, tmp + k + n, r - k - n);
        r -= n;
        fprintf(stderr, "dspico sim: dropped %zu bytes of the stream at byte %lld\n", n, dropAt);
    }
    // fault test: DSPICO_FLIP=N damages the byte at N and then one every 100 KB, 10 in all
    static long long flipAt = getenv("DSPICO_FLIP") ? atoll(getenv("DSPICO_FLIP")) : -1;
    static int flips = 0;
    while (flipAt >= from && flipAt < seen && flips < 10)
    {
        tmp[flipAt - from] ^= 0x5A;
        fprintf(stderr, "dspico sim: damaged the stream at byte %lld\n", flipAt);
        flipAt += 100000;
        flips++;
    }
    fromHost.insert(fromHost.end(), tmp, tmp + r);
}

void DSpicoSim::PumpOut()
{
    if (!configured) return;
    Out& o = outs[outEp & 0xF];
    if (!o.armed) return;
    ReadSocket();
    u32 n = std::min<u32>({o.len - o.got, (u32)fromHost.size(), budget});
    if (n)
    {
        memcpy(&bufs[Slot(outEp)][o.idx * 512 + o.got], fromHost.data(), n);
        fromHost.erase(fromHost.begin(), fromHost.begin() + n);
        o.got += n;
        budget -= n;
    }
    // a transfer ends when full, or with a short packet when the host's write has ended
    if (o.got == o.len || (o.got > 0 && fromHost.empty() && budget > 0))
    {
        o.armed = false;
        completes++;
        progressed = true;
        // DSPICO_HANG=N: after the Nth OUT transfer the DSpico stops answering the card
        // bus for good, the way a real one does after a command it doesn't know (run 8)
        if (Fault("DSPICO_HANG", completes))
        {
            fprintf(stderr, "dspico sim: stopped answering after OUT transfer %u\n", completes);
            crashed = true;
            return;
        }
        // DSPICO_LOSE=N: the DS's read of the Nth completion event comes back all ones
        // (the firmware answered too late) and the event is gone
        if (Fault("DSPICO_LOSE", completes))
        {
            fprintf(stderr, "dspico sim: lost the completion of OUT transfer %u\n", completes);
            Event(SILENT);
            return;
        }
        // DSPICO_LOSEDONE=N: the event never shows up at all
        if (Fault("DSPICO_LOSEDONE", completes))
        {
            fprintf(stderr, "dspico sim: lost the completion of OUT transfer %u without a trace\n", completes);
            return;
        }
        Event(EV_XFER_COMPLETE | outEp | (o.got << 8));
    }
}

void DSpicoSim::Nudge(u16 why)
{
    nudging = true;
    nudgeWhy = why;
    SendSetup(0x41, 1, why, 0, 0);
}

void DSpicoSim::Frame()
{
    static int trace = getenv("DSPICO_TRACE") ? 1 : 0;
    static unsigned completes0 = 0;
    if (trace && configured)
        fprintf(stderr, "f%u out %u xfers %u waiting %zu armed %d\n", frameNo, budgetPerFrame - budget, completes - completes0, fromHost.size(), (int)outs[outEp & 0xF].armed);
    completes0 = completes;
    frameNo++;
    budget = budgetPerFrame;
    if (crashed)
        return;
    if (attachDelay && --attachDelay == 0)
    {
        step = ENUM_GET_DEV64;
        Event(EV_BUS_RESET);
        NextSetup();
    }
    // the host polls the report endpoint every millisecond
    if (in.armed)
    {
        in.armed = false;
        DeviceSent(in.ep, &bufs[Slot(in.ep)][in.idx * 512], in.len);
    }
    PumpOut();
    // The Sion side nudges the DS (vendor request 1) as mirrorrun does over real USB:
    // when it has had data waiting 0.15 s without the DS taking any, and when it has
    // had no report for 0.4 s
    ReadSocket();
    if (configured && step >= ENUM_DONE && !nudging)
    {
        if (progressed || fromHost.empty())
            stuckFrames = 0;
        else if (++stuckFrames >= 9)
        {
            stuckFrames = 0;
            Nudge(1);
        }
        if (!nudging && frameNo - lastInFrame >= 24 && frameNo - lastInNudge >= 24)
        {
            lastInNudge = frameNo;
            Nudge(2);
        }
    }
    progressed = false;
}
