// FFCC GBA client on the 3DS: what the client's global.h maps the GBA
// hardware to when built with PORT_CTR (setup.py patches global.h to include
// this). A 3DS program can't have memory at the GBA's hardware addresses, so
// the I/O registers, palettes, VRAM and OAM are plain arrays (setup.py turns
// the client's literal 0x05/0x06/0x07 addresses into GBA_*_ADDR()), drawn
// each frame by a software GBA picture processor (gbappu.c). DMA and the
// BIOS calls are done by the CPU, and the JOY port is a software device fed
// over Sion Link (ffproto.h), as on the DS (ds/source/ds_port.h).

#ifndef CTR_PORT_H
#define CTR_PORT_H

#include "gba_types.h"

extern u8 gPortIo[0x400];
extern u16 gPortPltt[0x200];
extern u8 gPortVram[0x18000];
extern u16 gPortOam[0x200];
extern vu16 gPortIE, gPortIME, gPortDummy16;
extern void *gPortDummyPtr;
extern u8 gPortMbHeader[0x100], gPortCartHeader[0x100];
extern u8 gPortDownload[0xC000];

vu32 *PortJoyRecv(void);
vu32 *PortJoyTrans(void);
vu16 *PortJoyStat(void);
vu16 *PortJoyCnt(void);
void PortDma(u32 src, u32 dst, u32 control);

#define GBA_IO16(o)   (*(vu16 *)(gPortIo + (o)))

#define REG_DISPCNT   GBA_IO16(0x00)
#define REG_DISPSTAT  gPortDummy16
#define REG_BG0CNT    GBA_IO16(0x08)
#define REG_BG1CNT    GBA_IO16(0x0A)
#define REG_BG2CNT    GBA_IO16(0x0C)
#define REG_BG3CNT    GBA_IO16(0x0E)
#define REG_WINOUT    GBA_IO16(0x4A)
#define REG_BLDCNT    GBA_IO16(0x50)
#define REG_BLDALPHA  GBA_IO16(0x52)
#define REG_KEYINPUT  GBA_IO16(0x130)
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

#define GBA_PLTT_ADDR(o) ((u32)gPortPltt + (o))
#define GBA_VRAM_ADDR(o) ((u32)gPortVram + (o))
#define GBA_OAM_ADDR(o)  ((u32)gPortOam + (o))

#define BG_SCROLL_REGS ((vu16 *)(gPortIo + 0x10))
#define OBJ_VRAM      GBA_VRAM_ADDR(0x10000)
#define MB_HEADER     gPortMbHeader
#define CART_HEADER   gPortCartHeader
#define PORT_DOWNLOAD ((u32)gPortDownload) // the GameCube's download area (xfer.h)

#define DmaSet(dmaNo, src, dst, control) PortDma((u32)(src), (u32)(dst), (u32)(control))

#endif
