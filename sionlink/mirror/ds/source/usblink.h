// USB device on the DSpico's USB port, driven from the ARM9 through the
// DSpico card commands (dspico-firmware docs/commands.md). A vendor-class
// device with one bulk OUT endpoint (Sion -> DS stream) and one bulk IN
// endpoint (DS -> Sion reports). Polled: call usbPoll() often.
#ifndef USBLINK_H
#define USBLINK_H

#include <nds.h>

#define SL_USB_VID 0x1209   // pid.codes
#define SL_USB_PID 0x0001   // pid.codes test PID, until we register one

void usbStart(void);
void usbPoll(void);
bool usbConfigured(void);

// bytes received from the host that haven't been read yet
u32 usbAvailable(void);
// copies up to n received bytes to dst, returns how many
u32 usbRead(void* dst, u32 n);

// sends up to 64 bytes to the host if the previous report has gone; returns false if busy
bool usbSendReport(const void* data, u32 len);

#endif
