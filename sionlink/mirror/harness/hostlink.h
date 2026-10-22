// Sion side of the live link: the bundle stream goes out to the DS, the DS's
// pad reports (mirror_input) come back.
//   sim:PATH   unix socket that dspico_sim (the cloud stand-in for a DSpico) connects to
//   usb        the DSpico on the Sion's USB port (libusb, VID 1209 PID 0001)
#ifndef HOSTLINK_H
#define HOSTLINK_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include "../common/mirror_format.h"

class HostLink
{
public:
    virtual ~HostLink() {}
    // blocks until the bytes are handed over; false once the link is gone
    virtual bool Send(const uint8_t* data, size_t len) = 0;
    // newest pad report since the last call, if any
    virtual bool Poll(mirror_input& in) = 0;
};

std::unique_ptr<HostLink> OpenHostLink(const std::string& spec);

#endif
