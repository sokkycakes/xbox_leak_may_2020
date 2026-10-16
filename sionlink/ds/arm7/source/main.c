// ARM7 side of Sion Link: the stock libnds services plus our DSWifi build.

#include <dswifi7.h>
#include <nds.h>

#include "boot7.h"

static volatile bool exit_loop = false;

static void PowerButton(void)
{
    exit_loop = true;
}

static void VBlank(void)
{
    inputGetAndSend();
    Wifi_Update();
}

int main(int argc, char *argv[])
{
    enableSound();
    readUserSettings();
    ledBlink(LED_ALWAYS_ON);
    touchInit();

    irqInit();
    fifoInit();

    installWifiFIFO();
    installSoundFIFO();
    installSystemFIFO();
    Boot7Install();

    setPowerButtonCB(PowerButton);
    initClockIRQTimer(LIBNDS_DEFAULT_TIMER_RTC);

    irqSet(IRQ_VBLANK, VBlank);
    irqEnable(IRQ_VBLANK);

    while (!exit_loop)
    {
        swiWaitForVBlank();
        Boot7Poll();
    }

    return 0;
}
