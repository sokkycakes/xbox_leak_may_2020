// USB device on the DSpico's USB port (see usblink.h). The DSpico firmware runs
// the RP2040's USB controller; we answer its events: enumeration on endpoint 0,
// then bulk data. Commands follow dspico-firmware docs/commands.md; the event
// encoding follows its usbEventQueue.h and the dspico-usb-examples driver.

#include <string.h>
#include "usblink.h"

// ---- card bus

// KEY2 flags added to every command: none in unscrambled game mode, or the
// launcher's flags if it left the scrambler running (see usbStart)
static u32 secFlags;
usb_diag usbDiag;

static void diagLog(char kind, u32 x, u32 a, u32 b)
{
    u32 i = usbDiag.logCount++ % 8;
    usbDiag.log[i].kind = kind;
    usbDiag.log[i].x = x;
    usbDiag.log[i].a = a;
    usbDiag.log[i].b = b;
}

static void cardStart(u64 cmd, u32 flags)
{
    while (REG_ROMCTRL & CARD_BUSY)
        ;
    REG_AUXSPICNTH = CARD_CR1_ENABLE;
    for (int i = 0; i < 8; i++)
        REG_CARD_COMMAND[i] = cmd >> (56 - 8 * i);
    // 6.7 MHz, no KEY2 scrambling (the DSpico is in unscrambled game mode)
    REG_ROMCTRL = CARD_ACTIVATE | CARD_nRESET | secFlags | flags;
}

static void cardCmd(u64 cmd)
{
    cardStart(cmd, CARD_BLK_SIZE(0));
    while (REG_ROMCTRL & CARD_BUSY)
        ;
}

// The documented latency (docs/commands.md: at least 4 cycles reading, 8 writing).
// A longer gap 1 (0x180) garbled the data on a real DSpico: run 5 drew nothing.
#define READ_GAP  CARD_DELAY2(4)
#define WRITE_GAP CARD_DELAY2(8)

static u32 cardRead4(u64 cmd)
{
    u32 v = 0;
    cardStart(cmd, CARD_BLK_SIZE(7) | READ_GAP);
    while (REG_ROMCTRL & CARD_BUSY)
        if (REG_ROMCTRL & CARD_DATA_READY)
            v = REG_CARD_DATA_RD;
    return v;
}

static void cardRead512(u64 cmd, u32* dst)
{
    u32 i = 0;
    cardStart(cmd, CARD_BLK_SIZE(1) | READ_GAP);
    while (REG_ROMCTRL & CARD_BUSY)
    {
        if (REG_ROMCTRL & CARD_DATA_READY)
        {
            u32 v = REG_CARD_DATA_RD;
            if (i < 128)
                dst[i++] = v;
        }
    }
}

static void cardWrite512(u64 cmd, const u32* src)
{
    u32 i = 0;
    cardStart(cmd, CARD_WR | CARD_BLK_SIZE(1) | WRITE_GAP);
    while (REG_ROMCTRL & CARD_BUSY)
        if (REG_ROMCTRL & CARD_DATA_READY)
            REG_CARD_DATA_RD = i < 128 ? src[i++] : 0;
}

// ---- DSpico commands

#define USB_CMD(sub, args)          (0xE800000000000000ull | ((u64)(sub) << 48) | (args))
#define USB_INIT                    USB_CMD(1, 0)
#define USB_BEGIN_SET_ADDRESS       USB_CMD(2, 0)
#define USB_CONNECT                 USB_CMD(4, 0)
#define USB_EP_STALL(ep)            USB_CMD(9, (u64)(ep) << 40)
#define USB_EP_CLEAR_STALL(ep)      USB_CMD(10, (u64)(ep) << 40)
#define USB_FINISH_SET_ADDRESS(a)   USB_CMD(12, (u64)(a) << 40)
#define USB_EP_OPEN(ep, size, type) USB_CMD(13, ((u64)(ep) << 40) | ((u64)((type) & 3) << 32) | ((size) & 0x7FF))
#define USB_CLEAR_EVENT_QUEUE       USB_CMD(14, 0)
#define USB_BEGIN_TRANSFER(ep, buf, len) USB_CMD(16, ((u64)(ep) << 40) | ((u64)(buf) << 32) | (len))
#define USB_INTERRUPT_ENABLE        USB_CMD(17, 0)
#define USB_WRITE_DATA(buf, ep, start, len) \
    (0xE900000000000000ull | ((u64)(buf) << 48) | ((u64)(ep) << 40) | ((u64)(start) << 32) | (len))
#define USB_READ_DATA(buf, ep)      (0xEA00000000000000ull | ((u64)(buf) << 32) | ((u64)(ep) << 40))
#define USB_GET_EVENT               0xEB00000000000000ull

#define EV_LAST       (1u << 31)
#define EV_BUS_RESET  1

#define EP_OUT 0x01
#define EP_IN  0x81
#define PACKET 64

// ---- descriptors

static const u8 devDesc[18] = {
    18, 1, 0x00, 0x02, 0xFF, 0x00, 0x00, 64,
    SL_USB_VID & 0xFF, SL_USB_VID >> 8, SL_USB_PID & 0xFF, SL_USB_PID >> 8,
    0x00, 0x01, 1, 2, 3, 1,
};

static const u8 cfgDesc[32] = {
    9, 2, 32, 0, 1, 1, 0, 0x80, 50,
    9, 4, 0, 0, 2, 0xFF, 0x00, 0x00, 0,
    7, 5, EP_OUT, 2, PACKET, 0, 0,
    7, 5, EP_IN, 3, PACKET, 0, 1,   // interrupt, every frame
};

static const char* const strings[] = {NULL, "Sion", "Sion Link DS", "1"};

// ---- state

static u32 xfer[128] __attribute__((aligned(4)));  // one 512-byte card transfer

#define RING 65536
static u8 ring[RING];
static volatile u32 ringHead, ringTail;  // write / read positions

static bool configured;
static u8 pendingAddr;
static enum { CTRL_IDLE, CTRL_DATA_IN, CTRL_STATUS_OUT, CTRL_STATUS_IN } ctrl;

static bool outArmed;   // an OUT transfer is waiting for data
static u8 outBuf;       // the DSpico buffer it lands in
static bool inBusy;
static u8 inBuf;
static volatile u32 ticks;   // VBlanks (usbTick)
static u16 lastDrain;        // usbClock() of the last event read
static u16 lastPoll;         // usbClock() of the last usbPoll
static u16 armedAt;          // usbClock() when the OUT transfer was armed
static u32 lastHeard;        // tick of the last event read the DSpico answered
static u32 deadRun;          // event reads in a row that came back all ones
static u32 outSince;         // tick the OUT transfer was armed or last finished
static u32 inSince;          // tick the last report was started
static u32 lastBlock[2][128];  // what we last read from each of the DSpico's OUT buffers

void usbTick(void)
{
    ticks++;
}

// free-running 16-bit clock in 1/32768 s (timer 3, started by usbStart)
static inline u16 usbClock(void)
{
    return TIMER_DATA(3);
}

bool usbSilent(void)
{
    return deadRun >= 500 && ticks - lastHeard > 120;
}

static u32 ringFree(void)
{
    return RING - 1 - (ringHead - ringTail);
}

u32 usbAvailable(void)
{
    return ringHead - ringTail;
}

u32 usbRead(void* dst, u32 n)
{
    u32 avail = usbAvailable();
    if (n > avail)
        n = avail;
    u32 at = ringTail % RING, first = RING - at < n ? RING - at : n;
    memcpy(dst, &ring[at], first);
    memcpy((u8*)dst + first, ring, n - first);
    ringTail += n;
    return n;
}

bool usbConfigured(void)
{
    return configured;
}

// ---- endpoint 0

static void ep0Send(const void* data, u32 len, u32 maxLen)
{
    if (len > maxLen)
        len = maxLen;
    if (len == 0)
    {
        cardCmd(USB_WRITE_DATA(0, 0x80, 1, 0));
        diagLog('Z', 0x80, 0, 0);
        ctrl = CTRL_STATUS_IN;
        return;
    }
    memset(xfer, 0, sizeof xfer);
    memcpy(xfer, data, len);
    cardWrite512(USB_WRITE_DATA(0, 0x80, 1, len), xfer);
    ctrl = CTRL_DATA_IN;
}

static void ep0Status(void)
{
    ep0Send(NULL, 0, 0);
}

static void ep0Stall(void)
{
    cardCmd(USB_EP_STALL(0x80));
    cardCmd(USB_EP_STALL(0x00));
    ctrl = CTRL_IDLE;
}

static void armOut(void)
{
    if (outArmed || !configured || ringFree() < 2 * 512)
        return;
    cardCmd(USB_BEGIN_TRANSFER(EP_OUT, outBuf, 512));
    outArmed = true;
    outSince = ticks;
    armedAt = usbClock();
}

// The block in DSpico OUT buffer b, `bytes` of it into the ring
static void takeBlock(u8 b, u32 bytes)
{
    cardRead512(USB_READ_DATA(b, EP_OUT), xfer);
    memcpy(lastBlock[b], xfer, sizeof xfer);
    if (bytes > 512)
        bytes = 512;
    u32 at = ringHead % RING, first = RING - at < bytes ? RING - at : bytes;
    memcpy(&ring[at], xfer, first);
    memcpy(ring, (const u8*)xfer + first, bytes - first);
    ringHead += bytes;
}

// The Sion has had data waiting a while and we took none, although our OUT transfer
// has been armed for more than 0.1 s: the DSpico missed a card command.
// Either the command that armed the transfer (its buffer still holds what we read
// from it last time: arm it again), or the event read that would have told us it
// finished (new data in the buffer: take its 512 bytes, all the Sion sends are whole
// blocks, and go on). Re-arming a transfer that is really still waiting crashes the
// DSpico's firmware ("already available"), so this only runs when the Sion says it
// has been sending all along: an armed transfer takes its data within a millisecond.
static void nudgeOut(void)
{
    if (!outArmed)
    {
        armOut();
        return;
    }
    if (ticks - outSince < 6)
        return;  // it moved lately: we were only slow to take the data
    cardRead512(USB_READ_DATA(outBuf, EP_OUT), xfer);
    if (memcmp(xfer, lastBlock[outBuf], sizeof xfer) == 0)
    {
        usbDiag.rearmed++;
        diagLog('A', EP_OUT, outBuf, 0);
        outArmed = false;
        armOut();
        return;
    }
    usbDiag.recovered++;
    diagLog('N', EP_OUT, outBuf, 0);
    u8 done = outBuf;
    outArmed = false;
    outBuf ^= 1;
    armOut();
    takeBlock(done, 512);
}

// The Sion has been reading our reports all along and has had none for a while,
// although our last one started 0.2 s ago: the DSpico missed the command that started
// it, and it would stay busy forever (the Sion would keep the pad as it was)
static void nudgeIn(void)
{
    if (!inBusy || ticks - inSince < 12)
        return;
    usbDiag.inRearmed++;
    diagLog('I', EP_IN, inBuf, 0);
    inBusy = false;
}

static void sendString(u32 index, u32 maxLen)
{
    u8 d[64];
    if (index == 0)
    {
        d[0] = 4; d[1] = 3; d[2] = 0x09; d[3] = 0x04;  // English (US)
    }
    else if (index < sizeof strings / sizeof strings[0])
    {
        const char* s = strings[index];
        u32 n = strlen(s);
        d[0] = 2 + 2 * n;
        d[1] = 3;
        for (u32 i = 0; i < n; i++)
        {
            d[2 + 2 * i] = s[i];
            d[3 + 2 * i] = 0;
        }
    }
    else
    {
        ep0Stall();
        return;
    }
    ep0Send(d, d[0], maxLen);
}

static void setup(u32 w0, u32 w1)
{
    u32 length = (w0 >> 16) & 0x1FFF;
    u32 index = w0 & 0xFFFF;
    u32 value = w1 & 0xFFFF;
    u32 request = (w1 >> 16) & 0xFF;
    u32 type = (w1 >> 24) & 0x7F;  // without the direction bit
    diagLog('S', request, value, length);

    if ((type & 0x60) == 0x40 && request == SL_REQ_NUDGE)
    {
        usbDiag.nudges++;
        if (configured)
        {
            if (value & SL_NUDGE_IN)
                nudgeIn();
            if (value != SL_NUDGE_IN)  // 0 from older Sion builds means OUT
                nudgeOut();
        }
        ep0Status();
        return;
    }
    if (type & 0x60)
    {
        ep0Stall();  // no class requests, no other vendor requests
        return;
    }
    switch (request)
    {
    case 0x00:  // GET_STATUS
    {
        static const u8 zero[2];
        ep0Send(zero, 2, length);
        break;
    }
    case 0x01:  // CLEAR_FEATURE
    case 0x03:  // SET_FEATURE
        if ((type & 0x1F) == 2 && value == 0)  // endpoint halt
            cardCmd(request == 0x01 ? USB_EP_CLEAR_STALL(index & 0xFF) : USB_EP_STALL(index & 0xFF));
        ep0Status();
        break;
    case 0x05:  // SET_ADDRESS: the DSpico sends the status stage itself
        pendingAddr = value & 0x7F;
        cardCmd(USB_BEGIN_SET_ADDRESS);
        ctrl = CTRL_STATUS_IN;
        break;
    case 0x06:  // GET_DESCRIPTOR
        switch (value >> 8)
        {
        case 1: ep0Send(devDesc, sizeof devDesc, length); break;
        case 2: ep0Send(cfgDesc, sizeof cfgDesc, length); break;
        case 3: sendString(value & 0xFF, length); break;
        default: ep0Stall(); break;
        }
        break;
    case 0x08:  // GET_CONFIGURATION
    {
        u8 c = configured;
        ep0Send(&c, 1, length);
        break;
    }
    case 0x09:  // SET_CONFIGURATION
        if (value == 1)
        {
            cardCmd(USB_EP_OPEN(EP_OUT, PACKET, 2));
            // interrupt, not bulk: the DSpico's firmware queues an event every 1 ms
            // while a bulk IN transfer waits (RP2040 errata E15), flooding its queue
            cardCmd(USB_EP_OPEN(EP_IN, PACKET, 3));
            configured = true;
            usbDiag.configured++;
            outArmed = inBusy = false;
            ep0Status();
            armOut();
        }
        else
        {
            configured = false;
            ep0Status();
        }
        break;
    case 0x0A:  // GET_INTERFACE
    {
        static const u8 zero;
        ep0Send(&zero, 1, length);
        break;
    }
    case 0x0B:  // SET_INTERFACE
        ep0Status();
        break;
    default:
        ep0Stall();
        break;
    }
}

static void xferComplete(u32 ep, u32 bytes)
{
    if (ep != EP_OUT && ep != EP_IN)
        diagLog('C', ep, bytes, 0);
    if (ep == 0x80)
    {
        if (ctrl == CTRL_DATA_IN)
        {
            // data sent; take the host's zero-length status packet
            cardCmd(USB_BEGIN_TRANSFER(0x00, 0, 0));
            ctrl = CTRL_STATUS_OUT;
        }
        else if (ctrl == CTRL_STATUS_IN)
        {
            if (pendingAddr)
            {
                cardCmd(USB_FINISH_SET_ADDRESS(pendingAddr));
                pendingAddr = 0;
            }
            ctrl = CTRL_IDLE;
        }
    }
    else if (ep == 0x00)
        ctrl = CTRL_IDLE;
    else if (ep == EP_OUT)
    {
        outSince = ticks;
        usbDiag.outDone++;
        usbDiag.armTicks += (u16)(usbClock() - armedAt);
        usbDiag.outBytes += bytes;
        u8 done = outBuf;
        outArmed = false;
        outBuf ^= 1;
        armOut();  // the next transfer fills the other buffer while we read this one
        if (bytes)
            takeBlock(done, bytes);
    }
    else if (ep == EP_IN)
    {
        usbDiag.inDone++;
        inBusy = false;
    }
}

// true if there were any
static bool drainEvents(void)
{
    bool any = false;
    for (;;)
    {
        u32 e = cardRead4(USB_GET_EVENT);
        if (e == 0xFFFFFFFF)
        {
            // nothing drove the bus: the DSpico answered too late, or has stopped
            usbDiag.deadReads++;
            deadRun++;
            break;
        }
        lastHeard = ticks;
        deadRun = 0;
        bool last = e & EV_LAST;
        e &= ~EV_LAST;
        if (e == 0)
            break;
        any = true;
        usbDiag.events++;
        usbDiag.lastEvent = e;
        if ((e >> 30) == 1)
        {
            // setup packet: a second word follows
            u32 e2 = cardRead4(USB_GET_EVENT);
            last = e2 & EV_LAST;
            usbDiag.setups++;
            usbDiag.lastSetup = e2 & ~EV_LAST;
            setup(e, e2 & ~EV_LAST);
        }
        else if ((e >> 28) == 3)
            xferComplete(e & 0xFF, (e >> 8) & 0x1FFF);
        else if (e == EV_BUS_RESET)
        {
            usbDiag.resets++;
            diagLog('R', 0, 0, 0);
            configured = false;
            outArmed = inBusy = false;
            pendingAddr = 0;
            ctrl = CTRL_IDLE;
        }
        // SOF, suspend, resume, unplugged: nothing to do
        if (last)
            break;
    }
    return any;
}

#define CARD_READ_ID 0xB800000000000000ull
#define KEY2_FLAGS (CARD_SEC_CMD | CARD_SEC_DAT | CARD_SEC_EN)

static bool goodId(u32 id)
{
    return id != 0 && id != 0xFFFFFFFF;
}

void usbStart(void)
{
    sysSetCardOwner(BUS_OWNER_ARM9);
    TIMER_CR(3) = 0;
    TIMER_DATA(3) = 0;
    TIMER_CR(3) = TIMER_ENABLE | TIMER_DIV_1024;
    // the DSpico answers its USB commands only in unscrambled game mode, which
    // its launcher leaves it in; read the card ID to see the bus is talking.
    // Only if that fails, try with KEY2: in unscrambled mode a scrambled command
    // reads as one the DSpico doesn't know, and it stops answering for good.
    usbDiag.idPlain = cardRead4(CARD_READ_ID);
    if (!goodId(usbDiag.idPlain))
    {
        secFlags = KEY2_FLAGS;
        usbDiag.idKey2 = cardRead4(CARD_READ_ID);
        if (!goodId(usbDiag.idKey2))
            secFlags = 0;
    }
    usbDiag.secFlags = secFlags;
    cardCmd(USB_CLEAR_EVENT_QUEUE);
    cardCmd(USB_INIT);
    cardCmd(USB_INTERRUPT_ENABLE);
    cardCmd(USB_CONNECT);
}

// Every card command is a chance for a glitch to reach the DSpico as a command it
// doesn't know, and then it ignores the card bus until the DS is switched off (run 8
// died that way after 6 minutes, having read events 15,000 times a second). So read
// events when the DSpico raises the card IRQ line (it does while it has some), and
// otherwise now and then, in case that edge was missed: every 4 ms, or every 1 ms
// while those reads find events. Run 10 read 1 ms apart, ~950 times a second against
// ~200 on the IRQ line, and hung after 14 minutes.
#define POLL_IDLE 131   // 4 ms in usbClock() units
#define POLL_BUSY 33    // 1 ms
static u16 pollGap = POLL_BUSY;

void usbPoll(void)
{
    u16 now = usbClock();
    u16 gap = now - lastPoll;
    if (gap > usbDiag.busyMax)
        usbDiag.busyMax = gap;
    lastPoll = now;
    if (REG_IF & IRQ_CARD_LINE)
    {
        REG_IF = IRQ_CARD_LINE;   // before reading, so events from now on raise it again
        usbDiag.irqs++;
        lastDrain = now;
        drainEvents();
    }
    else if ((u16)(now - lastDrain) >= pollGap)
    {
        usbDiag.timedPolls++;
        lastDrain = now;
        if (drainEvents())
        {
            usbDiag.timedHits++;
            pollGap = POLL_BUSY;
        }
        else
            pollGap = POLL_IDLE;
    }
    armOut();
    usbDiag.outArmed = outArmed;
    usbDiag.inBusy = inBusy;
}

bool usbSendReport(const void* data, u32 len)
{
    if (!configured || inBusy)
        return false;
    if (len > PACKET)
        len = PACKET;
    memset(xfer, 0, sizeof xfer);
    memcpy(xfer, data, len);
    cardWrite512(USB_WRITE_DATA(inBuf, EP_IN, 1, len), xfer);
    inBuf ^= 1;
    inBusy = true;
    inSince = ticks;
    usbDiag.reports++;
    return true;
}
