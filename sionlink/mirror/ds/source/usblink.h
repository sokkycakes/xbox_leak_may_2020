// USB device on the DSpico's USB port, driven from the ARM9 through the
// DSpico card commands (dspico-firmware docs/commands.md). A vendor-class
// device with one bulk OUT endpoint (Sion -> DS stream) and one interrupt IN
// endpoint (DS -> Sion reports). Polled: call usbPoll() often, usbTick() at VBlank.
#ifndef USBLINK_H
#define USBLINK_H

#include <nds.h>

#define SL_USB_VID 0x1209   // pid.codes
#define SL_USB_PID 0x0001   // pid.codes test PID, until we register one
// vendor request (host to device, no data): the Sion has had data waiting a while
#define SL_REQ_NUDGE 1

void usbStart(void);

// what the link has seen so far, shown on the DS while it waits for the Sion
typedef struct {
    u32 idPlain, idKey2;    // card ID read without and with KEY2 scrambling
    u32 secFlags;           // scrambling flags in use
    u32 irqs, events, resets, setups, configured;
    u32 lastEvent, lastSetup;
    u32 deadReads;          // event reads that came back all ones (DSpico not answering)
    u32 nudges, recovered;  // Sion's nudges, lost OUT completions recovered
    u32 outDone, outBytes, inDone, reports;  // transfers completed, reports started
    bool outArmed, inBusy;
    // recent link history: S setup (x = request, a = value, b = length),
    // C transfer complete (x = endpoint, a = bytes), R bus reset, Z status sent,
    // N lost OUT completion recovered (a = buffer)
    struct { char kind; u8 x; u16 a, b; } log[8];
    u32 logCount;
} usb_diag;
extern usb_diag usbDiag;
void usbPoll(void);
void usbTick(void);
bool usbConfigured(void);

// bytes received from the host that haven't been read yet
u32 usbAvailable(void);
// copies up to n received bytes to dst, returns how many
u32 usbRead(void* dst, u32 n);

// sends up to 64 bytes to the host if the previous report has gone; returns false if busy
bool usbSendReport(const void* data, u32 len);

#endif
