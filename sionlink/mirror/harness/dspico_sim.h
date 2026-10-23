// Cloud stand-in for a DSpico with a USB cable to the Sion: answers the DSpico
// USB card commands (dspico-firmware docs/commands.md) the way its firmware
// does, and plays the USB host itself: it enumerates the device the DS client
// presents, then carries bulk data between the client's endpoints and a socket
// (the Sion side, `mirrorrun --serve`). USB bandwidth is capped per frame like
// full-speed USB.
#ifndef DSPICO_SIM_H
#define DSPICO_SIM_H

#include <deque>
#include <map>
#include <string>
#include <vector>
#include "NDS.h"
#include "MirrorTap.h"

class DSpicoSim : public melonDS::CardHook
{
public:
    DSpicoSim(melonDS::NDS* nds, const std::string& socketPath, unsigned bytesPerFrame);
    ~DSpicoSim();

    bool Start(const melonDS::u8* cmd, melonDS::u32 len) override;
    melonDS::u32 Receive() override;
    void Transmit(melonDS::u32 val) override;
    void Finish() override;

    void Frame();   // once per emulated frame: refill the bandwidth budget, move data
    bool Configured() const { return configured; }

private:
    using u8 = melonDS::u8;
    using u16 = melonDS::u16;
    using u32 = melonDS::u32;

    melonDS::NDS* nds;
    int sock = -1;
    std::string path;           // the Sion side's socket: connected to again when it goes (a new Sion process)
    void Connect(int tries);
    unsigned budgetPerFrame, budget = 0;

    // device side (what the DSpico firmware keeps)
    u8 bufs[32][1024] {};
    std::deque<u32> events;
    bool usbInit = false, connected = false;
    u8 address = 0;
    struct Out { bool armed = false; u8 idx = 0; u32 len = 0, got = 0; } outs[16];

    // the transfer in progress over the card bus
    u8 cmd[8] {};
    u32 cmd0 = 0, cmd1 = 0, pos = 0;
    u32 eventWord = 0;

    // host side
    enum Step { ENUM_GET_DEV64, ENUM_SET_ADDRESS, ENUM_GET_DEV, ENUM_GET_CFG9, ENUM_GET_CFG,
                ENUM_GET_STR0, ENUM_GET_STR2, ENUM_SET_CONFIG, ENUM_DONE };
    int step = -1;          // -1: not attached yet
    int stage = 0;          // 0 setup sent, 1 data received / waiting for status
    int attachDelay = 0;
    std::vector<u8> ctrlData;
    bool configured = false;
    u32 outEp = 0, inEp = 0;
    std::vector<u8> fromHost;   // bytes read from the socket, not yet delivered
    std::string product;
    unsigned completes = 0, arms = 0, inStarts = 0;
    bool progressed = false, nudging = false;
    unsigned stuckFrames = 0, nudges = 0;
    u16 nudgeWhy = 0;
    unsigned frameNo = 0, lastInFrame = 0, lastInNudge = 0;
    // the report endpoint: a report waits here until the host polls (next frame)
    struct In { bool armed = false; u8 idx = 0; u32 ep = 0, len = 0; } in;
    bool crashed = false;   // the firmware hit a panic: it answers nothing any more
    // DSPICO_PROF=1: where the DS's CPU is at the end of frames in which it sent no card
    // command at all (it went a whole frame without looking at USB)
    unsigned cmdsThisFrame = 0;
    std::map<u32, unsigned> busyPCs;

    void Event(u32 e);
    void Event64(u32 e0, u32 e1);
    void UsbCommand(u32 sub);
    void StartXfer(u32 ep, u32 idx, u32 len);
    void DeviceSent(u32 ep, const u8* data, u32 len);
    void NextSetup();
    void SendSetup(u8 type, u8 req, u16 value, u16 index, u16 length);
    void ControlDone(bool ok);
    void PumpOut();
    void ReadSocket();
    void Nudge(u16 why);
    void Crash(u32 ep);
};

#endif
