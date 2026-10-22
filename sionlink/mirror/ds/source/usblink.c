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

static u32 cardRead4(u64 cmd)
{
    u32 v = 0;
    cardStart(cmd, CARD_BLK_SIZE(7) | CARD_DELAY2(4));
    while (REG_ROMCTRL & CARD_BUSY)
        if (REG_ROMCTRL & CARD_DATA_READY)
            v = REG_CARD_DATA_RD;
    return v;
}

static void cardRead512(u64 cmd, u32* dst)
{
    u32 i = 0;
    cardStart(cmd, CARD_BLK_SIZE(1) | CARD_DELAY2(4));
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
    cardStart(cmd, CARD_WR | CARD_BLK_SIZE(1) | CARD_DELAY2(8));
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
    7, 5, EP_IN, 2, PACKET, 0, 0,
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
static u32 pollCount;

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
    u8* d = dst;
    for (u32 i = 0; i < n; i++)
        d[i] = ring[(ringTail + i) % RING];
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
    (void)index;

    if (type & 0x60)
    {
        ep0Stall();  // no class or vendor requests
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
            cardCmd(USB_EP_OPEN(EP_IN, PACKET, 2));
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
        u8 done = outBuf;
        outArmed = false;
        outBuf ^= 1;
        armOut();  // the next transfer fills the other buffer while we read this one
        if (bytes)
        {
            cardRead512(USB_READ_DATA(done, EP_OUT), xfer);
            const u8* s = (const u8*)xfer;
            for (u32 i = 0; i < bytes && i < 512; i++)
                ring[(ringHead + i) % RING] = s[i];
            ringHead += bytes;
        }
    }
    else if (ep == EP_IN)
        inBusy = false;
}

static void drainEvents(void)
{
    for (;;)
    {
        u32 e = cardRead4(USB_GET_EVENT);
        bool last = e & EV_LAST;
        e &= ~EV_LAST;
        if (e == 0)
            break;
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
            configured = false;
            outArmed = inBusy = false;
            pendingAddr = 0;
            ctrl = CTRL_IDLE;
        }
        // SOF, suspend, resume, unplugged: nothing to do
        if (last)
            break;
    }
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
    // the DSpico answers its USB commands only in unscrambled game mode, which
    // its launcher leaves it in; read the card ID to see the bus is talking
    usbDiag.idPlain = cardRead4(CARD_READ_ID);
    secFlags = KEY2_FLAGS;
    usbDiag.idKey2 = cardRead4(CARD_READ_ID);
    secFlags = 0;
    if (!goodId(usbDiag.idPlain) && goodId(usbDiag.idKey2))
        secFlags = KEY2_FLAGS;
    usbDiag.secFlags = secFlags;
    cardCmd(USB_CLEAR_EVENT_QUEUE);
    cardCmd(USB_INIT);
    cardCmd(USB_INTERRUPT_ENABLE);
    cardCmd(USB_CONNECT);
}

void usbPoll(void)
{
    // the DSpico raises the card IRQ line while it has events; check now and then anyway
    if ((REG_IF & IRQ_CARD_LINE) || (++pollCount & 63) == 0)
    {
        if (REG_IF & IRQ_CARD_LINE)
            usbDiag.irqs++;
        REG_IF = IRQ_CARD_LINE;
        drainEvents();
    }
    armOut();
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
    return true;
}
