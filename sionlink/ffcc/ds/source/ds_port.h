// FFCC GBA client on the DS: what the client's global.h maps the GBA
// hardware to when built with PORT_DS (setup.py patches global.h to include
// this). The DS main engine has the GBA's display registers, palettes and OAM
// at the same addresses; the rest is emulated in port.c:
//   - DISPCNT, IE, IME and the BG scroll registers are shadows applied each
//     frame (the picture is centered on the 256x192 screen);
//   - DMA and the BIOS calls are done by the CPU;
//   - the JOY port is a software device fed over Sion Link (ffproto.h).

#ifndef DS_PORT_H
#define DS_PORT_H

#include "gba_types.h"

extern vu16 gPortDispcnt, gPortIE, gPortIME, gPortDummy16;
extern void *gPortDummyPtr;
extern vu16 gPortScroll[8];
extern u8 gPortMbHeader[0x100], gPortCartHeader[0x100];
extern u8 gPortDownload[0xC000];

vu32 *PortJoyRecv(void);
vu32 *PortJoyTrans(void);
vu16 *PortJoyStat(void);
vu16 *PortJoyCnt(void);
void PortDma(u32 src, u32 dst, u32 control);

#define REG_DISPCNT   gPortDispcnt
#define REG_DISPSTAT  gPortDummy16
#define REG_BG0CNT    (*(vu16 *)0x04000008)
#define REG_BG1CNT    (*(vu16 *)0x0400000A)
#define REG_BG2CNT    (*(vu16 *)0x0400000C)
#define REG_BG3CNT    (*(vu16 *)0x0400000E)
#define REG_WINOUT    (*(vu16 *)0x0400004A)
#define REG_BLDCNT    (*(vu16 *)0x04000050)
#define REG_BLDALPHA  (*(vu16 *)0x04000052)
#define REG_KEYINPUT  (*(vu16 *)0x04000130)
#define REG_RCNT      gPortDummy16
#define REG_JOYCNT    (*PortJoyCnt())
#define REG_JOY_RECV  (*PortJoyRecv())
#define REG_JOY_TRANS (*PortJoyTrans())
#define REG_JOYSTAT   (*PortJoyStat())
#define REG_IE        gPortIE
#define REG_IF        gPortDummy16
#define REG_WAITCNT   gPortDummy16
#define REG_IME       gPortIME

#define INTR_CHECK    gPortDummy16
#define INTR_VECTOR   gPortDummyPtr

#define BG_SCROLL_REGS gPortScroll
#define OBJ_VRAM      0x06400000
#define MB_HEADER     gPortMbHeader
#define CART_HEADER   gPortCartHeader
#define PORT_DOWNLOAD ((u32)gPortDownload) // the GameCube's download area (xfer.h)

#define DmaSet(dmaNo, src, dst, control) PortDma((u32)(src), (u32)(dst), (u32)(control))

#endif
