// USB device on the DSpico's USB port, driven from the ARM9 through the
// DSpico card commands (dspico-firmware docs/commands.md). A vendor-class
// device with one bulk OUT endpoint (Sion -> DS stream) and one interrupt IN
// endpoint (DS -> Sion reports). Polled: call usbPoll() often, usbTick() at VBlank.
#ifndef USBLINK_H
#define USBLINK_H

#include <nds.h>

#define SL_USB_VID 0x1209   // pid.codes
#define SL_USB_PID 0x0001   // pid.codes test PID, until we register one
// vendor request (host to device, no data, wValue says why): the Sion has had data
// waiting a while (OUT), or has been reading reports and had none for a while (IN)
#define SL_REQ_NUDGE 1
#define SL_NUDGE_OUT 1
#define SL_NUDGE_IN  2

void usbStart(void);

// what the link has seen so far, shown on the DS while it waits for the Sion
typedef struct {
    u32 idPlain, idKey2;    // card ID read without and with KEY2 scrambling
    u32 secFlags;           // scrambling flags in use
    u32 irqs, timedPolls;   // event reads because the DSpico raised its IRQ line / because time went by
    u32 timedHits;          // timed event reads that found events
    u32 events, resets, setups, configured;
    u32 lastEvent, lastSetup;
    u32 deadReads;          // event reads that came back all ones (DSpico not answering)
    u32 nudges;             // the Sion's nudges
    u32 recovered, rearmed; // OUT blocks whose completion was lost, OUT transfers armed again
    u32 inRearmed;          // reports started again
    u32 outDone, outBytes, inDone, reports;  // transfers completed, reports started
    u32 armTicks;           // time OUT transfers spent armed, 1/32768 s
    u16 busyMax;            // longest gap between usbPoll calls, 1/32768 s (the caller resets it)
    bool outArmed, inBusy;
    // recent link history: S setup (x = request, a = value, b = length),
    // C transfer complete (x = endpoint, a = bytes), R bus reset, Z status sent,
    // N lost OUT completion recovered, A OUT armed again, I report started again (a = buffer)
    struct { char kind; u8 x; u16 a, b; } log[8];
    u32 logCount;
} usb_diag;
extern usb_diag usbDiag;
void usbPoll(void);
void usbTick(void);
bool usbConfigured(void);
// the DSpico has answered no event read for 2 s: it has stopped listening to the
// card bus, and only switching the DS off and on brings it back
bool usbSilent(void);

// bytes received from the host that haven't been read yet
u32 usbAvailable(void);
// copies up to n received bytes to dst, returns how many
u32 usbRead(void* dst, u32 n);

// sends up to 64 bytes to the host if the previous report has gone; returns false if busy
bool usbSendReport(const void* data, u32 len);

#endif
