/* Xbox D3D8 types that XFONT needs, without the Xbox SDK.  The D3DFORMAT
   values are the Xbox ones (not PC D3D8): XFONT's format tables index by
   them.  Copied from the XDK's d3d8types.h. */
#pragma once

#ifndef D3DCOLOR_DEFINED
typedef DWORD D3DCOLOR;
#define D3DCOLOR_DEFINED
#endif

#define D3DCOLOR_ARGB(a,r,g,b) \
    ((D3DCOLOR)((((a)&0xff)<<24)|(((r)&0xff)<<16)|(((g)&0xff)<<8)|((b)&0xff)))
#define D3DCOLOR_XRGB(r,g,b) D3DCOLOR_ARGB(0xff,r,g,b)

#ifndef D3DRECT_DEFINED
typedef struct _D3DRECT {
    LONG x1;
    LONG y1;
    LONG x2;
    LONG y2;
} D3DRECT;
#define D3DRECT_DEFINED
#endif

typedef enum _D3DFORMAT
{
    D3DFMT_UNKNOWN              = 0xFFFFFFFF,

    /* Swizzled formats */

    D3DFMT_A8R8G8B8             = 0x00000006,
    D3DFMT_X8R8G8B8             = 0x00000007,
    D3DFMT_R5G6B5               = 0x00000005,
    D3DFMT_R6G5B5               = 0x00000027,
    D3DFMT_X1R5G5B5             = 0x00000003,
    D3DFMT_A1R5G5B5             = 0x00000002,
    D3DFMT_A4R4G4B4             = 0x00000004,
    D3DFMT_A8                   = 0x00000019,
    D3DFMT_A8B8G8R8             = 0x0000003A,   
    D3DFMT_B8G8R8A8             = 0x0000003B,   
    D3DFMT_R4G4B4A4             = 0x00000039,   
    D3DFMT_R5G5B5A1             = 0x00000038,   
    D3DFMT_R8G8B8A8             = 0x0000003C,   
    D3DFMT_R8B8                 = 0x00000029,   
    D3DFMT_G8B8                 = 0x00000028,   

    D3DFMT_P8                   = 0x0000000B,

    D3DFMT_L8                   = 0x00000000,
    D3DFMT_A8L8                 = 0x0000001A,
    D3DFMT_AL8                  = 0x00000001,   
    D3DFMT_L16                  = 0x00000032,   

    D3DFMT_V8U8                 = 0x00000028,
    D3DFMT_L6V5U5               = 0x00000027,
    D3DFMT_X8L8V8U8             = 0x00000007,
    D3DFMT_Q8W8V8U8             = 0x0000003A,
    D3DFMT_V16U16               = 0x00000033,

    D3DFMT_D16_LOCKABLE         = 0x0000002C,
    D3DFMT_D16                  = 0x0000002C,
    D3DFMT_D24S8                = 0x0000002A,
    D3DFMT_F16                  = 0x0000002D,   
    D3DFMT_F24S8                = 0x0000002B,

    /* YUV formats */

    D3DFMT_YUY2                 = 0x00000024,
    D3DFMT_UYVY                 = 0x00000025,

    /* Compressed formats */

    D3DFMT_DXT1                 = 0x0000000C,
    D3DFMT_DXT2                 = 0x0000000E,
    D3DFMT_DXT3                 = 0x0000000E,
    D3DFMT_DXT4                 = 0x0000000F,
    D3DFMT_DXT5                 = 0x0000000F,

    /* Linear formats */

    D3DFMT_LIN_A1R5G5B5         = 0x00000010,   
    D3DFMT_LIN_A4R4G4B4         = 0x0000001D,   
    D3DFMT_LIN_A8               = 0x0000001F,   
    D3DFMT_LIN_A8B8G8R8         = 0x0000003F,   
    D3DFMT_LIN_A8R8G8B8         = 0x00000012,   
    D3DFMT_LIN_B8G8R8A8         = 0x00000040,   
    D3DFMT_LIN_G8B8             = 0x00000017,   
    D3DFMT_LIN_R4G4B4A4         = 0x0000003E,   
    D3DFMT_LIN_R5G5B5A1         = 0x0000003D,   
    D3DFMT_LIN_R5G6B5           = 0x00000011,   
    D3DFMT_LIN_R6G5B5           = 0x00000037,   
    D3DFMT_LIN_R8B8             = 0x00000016,   
    D3DFMT_LIN_R8G8B8A8         = 0x00000041,   
    D3DFMT_LIN_X1R5G5B5         = 0x0000001C,   
    D3DFMT_LIN_X8R8G8B8         = 0x0000001E,   

    D3DFMT_LIN_A8L8             = 0x00000020,   
    D3DFMT_LIN_AL8              = 0x0000001B,   
    D3DFMT_LIN_L16              = 0x00000035,   
    D3DFMT_LIN_L8               = 0x00000013,   

    D3DFMT_LIN_V16U16           = 0x00000036,
    D3DFMT_LIN_V8U8             = 0x00000017,
    D3DFMT_LIN_L6V5U5           = 0x00000037,
    D3DFMT_LIN_X8L8V8U8         = 0x0000001E,
    D3DFMT_LIN_Q8W8V8U8         = 0x00000012,

    D3DFMT_LIN_D24S8            = 0x0000002E,   
    D3DFMT_LIN_F24S8            = 0x0000002F,   
    D3DFMT_LIN_D16              = 0x00000030,   
    D3DFMT_LIN_F16              = 0x00000031,   

    D3DFMT_VERTEXDATA           = 100,
    D3DFMT_INDEX16              = 101,

    D3DFMT_FORCE_DWORD          =0x7fffffff
} D3DFORMAT;
