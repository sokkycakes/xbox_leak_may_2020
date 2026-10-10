/*
 * XInput: the XDK's input device API (xapilib's XID class driver + XPP device
 * bookkeeping) on SDL2 game controllers.
 *
 * On the console the "XInput" functions sit on a USB stack (usbd + the xidex
 * class driver, private/ntos/dd/usb) that xbcompat does not run, so every
 * public entry point of that layer is replaced here and nothing below it is
 * ever reached.  What the title sees is modelled on the sources:
 *
 *  - Device presence lives in the title's own 12-byte XPP_DEVICE_TYPE tables
 *    (XDEVICE_TYPE_GAMEPAD_TABLE and friends), which xapilib treats as
 *    { CurrentConnected, ChangeConnected, PreviousConnected } bitmasks
 *    (private/ntos/xapi/k32/xpp.c).  The same bookkeeping is kept in those
 *    guest tables, so XGetDevices / XGetDeviceChanges / XPeekDevices are the
 *    xpp.c algorithm bit for bit.  The gamepad table is not referenced from
 *    code, so it is found by walking the XID type-description table
 *    (?XID_BeginTypeDescriptionTable@@ .. ?XID_EndTypeDescriptionTable@@,
 *    private/ntos/dd/usb/xidex/typeinfo.cpp) whose entries carry the type
 *    (1 gamepad, 2 keyboard, 3 IR remote) and the table address.
 *  - XInputOpen hands out a handle per port (at most one per port, at most
 *    bRemainingHandles per type), XInputGetState copies the 18-byte
 *    XINPUT_GAMEPAD report and the packet number, and a removed device keeps
 *    its handle alive, returning the last report with
 *    ERROR_DEVICE_NOT_CONNECTED, until XInputClose (xidinp.cpp, xid.cpp).
 *  - XInputSetState is asynchronous like the driver: it returns
 *    ERROR_IO_PENDING with dwStatus == ERROR_IO_PENDING, and the kernel's DPC
 *    thread completes the feedback a couple of milliseconds later (dwStatus =
 *    ERROR_SUCCESS, hEvent signalled), or XInputClose completes it with
 *    ERROR_CANCELLED.
 *  - Memory units and the other device types are never present.
 *
 * SDL2 mapping (SDL_GameController -> XINPUT_GAMEPAD): A/B/X/Y and the
 * shoulders (right = Black, left = White) are 255 when pressed, triggers are
 * axis >> 7, the sticks are passed through with Y negated (up = +32767, the
 * sign ATG code and xemu use), rumble goes to SDL_GameControllerRumble.
 * Original Xbox pads get a mapping of their own (map_original_pad) that puts
 * Black and White on those shoulders.
 * Controllers take the lowest free port in the order SDL lists them.
 *
 * Keyboard fallback: when no game controller is attached (and a window
 * exists, since SDL keyboard state needs the video subsystem) a virtual pad
 * is reported on port 0 and driven by the keyboard:
 *
 *      left stick     W A S D            right stick    I J K L
 *      D-pad          arrow keys         A              Space or Enter
 *      B / X / Y      B / X / Y keys     Back / Start   Escape / Backspace
 *      White / Black  1 / 2              left / right thumb click  3 / 4
 *      left trigger   Q                  right trigger  E
 *
 * Plugging a controller in replaces the virtual pad (the title sees a removal
 * and an insertion).  XBCOMPAT_NO_KBD_PAD=1 disables the fallback so headless
 * runs have a deterministic device mask.  XBCOMPAT_VIRTUAL_PAD attaches an
 * SDL virtual game controller instead, so a title can be exercised with a
 * "real" controller without hardware: "1" leaves every input at rest, and a
 * list such as "a,start,lt=255,ly=32767" holds buttons down (a b x y black
 * white start back lthumb rthumb up down left right) and sets axes (lt rt
 * 0..255; lx ly rx ry -32768..32767, up positive) for the whole run.
 *
 * XBCOMPAT_INPUT_SCRIPT presses keyboard-pad buttons at given frames, for
 * headless runs that need to get through menus: "FRAME:BUTTON[/HOLD] ..."
 * (separated by spaces or commas), BUTTON being one of the names above and
 * HOLD the frames it stays down (default 6).  "1500:down 1530:a/10" moves
 * down a menu at frame 1500 and presses A at frame 1530.  A "p2" in front of
 * the button ("2000:p2start") presses it on a second scripted pad, which is
 * plugged into port 1 when the script names one (split-screen menus).
 */
#define _GNU_SOURCE
#include <SDL.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../xbcompat.h"
#include "hle.h"
#include "../cpu.h"

/* ---- XDK definitions (public/xdk/inc/xbox.h, winerror.h) ------------- */

#define ERROR_SUCCESS               0
#define ERROR_ACCESS_DENIED         5
#define ERROR_OUTOFMEMORY           14
#define ERROR_INVALID_DRIVE         15
#define ERROR_SHARING_VIOLATION     32
#define ERROR_NOT_SUPPORTED         50
#define ERROR_ALREADY_ASSIGNED      85
#define ERROR_INVALID_PARAMETER     87
#define ERROR_IO_PENDING            997
#define ERROR_DEVICE_NOT_CONNECTED  1167
#define ERROR_CANCELLED             1223

#define XGetPortCount()             4
#define XDEVICE_NO_SLOT             0
#define XDEVICE_BOTTOM_SLOT         1

/* The internal view of the 12-byte XPP_DEVICE_TYPE (xpp.c:36-41). */
typedef struct { ULONG Current, Change, Previous; } XPP_DEVICE_TYPE;
typedef struct { XPP_DEVICE_TYPE *DeviceType; ULONG dwPreallocCount; } XDEVICE_PREALLOC_TYPE;

/* xbox.h wraps these in #include <pshpack1.h>. */
typedef struct __attribute__((packed)) {
    USHORT wButtons;
    UCHAR  bAnalogButtons[8];
    SHORT  sThumbLX, sThumbLY, sThumbRX, sThumbRY;
} XINPUT_GAMEPAD;
typedef struct __attribute__((packed)) { ULONG dwPacketNumber; XINPUT_GAMEPAD Gamepad; } XINPUT_STATE;
typedef struct __attribute__((packed)) { USHORT wLeftMotorSpeed, wRightMotorSpeed; } XINPUT_RUMBLE;
typedef struct __attribute__((packed)) {
    ULONG  dwStatus;
    HANDLE hEvent;
    UCHAR  Reserved[58];        /* the driver keeps its URB here; we leave it alone */
    XINPUT_RUMBLE Rumble;
} XINPUT_FEEDBACK;
typedef struct __attribute__((packed)) {
    UCHAR  SubType;
    USHORT Reserved;
    XINPUT_GAMEPAD In;
    XINPUT_RUMBLE  Out;
} XINPUT_CAPABILITIES;
typedef struct __attribute__((packed)) {
    UCHAR flags;                /* bit 0 fAutoPoll, bit 1 fInterruptOut */
    UCHAR bInputInterval, bOutputInterval, ReservedMBZ2;
} XINPUT_POLLING_PARAMETERS;

_Static_assert(sizeof(XINPUT_GAMEPAD) == 18, "XINPUT_GAMEPAD");
_Static_assert(sizeof(XINPUT_STATE) == 22, "XINPUT_STATE");
_Static_assert(sizeof(XINPUT_FEEDBACK) == 70, "XINPUT_FEEDBACK");
_Static_assert(sizeof(XINPUT_CAPABILITIES) == 25, "XINPUT_CAPABILITIES");
_Static_assert(sizeof(XINPUT_POLLING_PARAMETERS) == 4, "XINPUT_POLLING_PARAMETERS");

#define XINPUT_GAMEPAD_DPAD_UP      0x0001
#define XINPUT_GAMEPAD_DPAD_DOWN    0x0002
#define XINPUT_GAMEPAD_DPAD_LEFT    0x0004
#define XINPUT_GAMEPAD_DPAD_RIGHT   0x0008
#define XINPUT_GAMEPAD_START        0x0010
#define XINPUT_GAMEPAD_BACK         0x0020
#define XINPUT_GAMEPAD_LEFT_THUMB   0x0040
#define XINPUT_GAMEPAD_RIGHT_THUMB  0x0080

#define XINPUT_GAMEPAD_A            0
#define XINPUT_GAMEPAD_B            1
#define XINPUT_GAMEPAD_X            2
#define XINPUT_GAMEPAD_Y            3
#define XINPUT_GAMEPAD_BLACK        4
#define XINPUT_GAMEPAD_WHITE        5
#define XINPUT_GAMEPAD_LEFT_TRIGGER 6
#define XINPUT_GAMEPAD_RIGHT_TRIGGER 7

#define XINPUT_DEVSUBTYPE_GC_GAMEPAD     0x01   /* the original "Duke" */
#define XINPUT_DEVSUBTYPE_GC_GAMEPAD_ALT 0x02   /* the Controller S */

#define XINPUT_POLL_AUTO            0x01

/* XID_TYPE_INFORMATION (xidex/xid.h:108-120) as seen in the guest image. */
#define XID_TI_TYPE                 0   /* UCHAR ucType: 1 gamepad, 2 keyboard, 3 IR remote */
#define XID_TI_HANDLES              1   /* BYTE bRemainingHandles */
#define XID_TI_XPPTYPE              4   /* PXPP_DEVICE_TYPE XppType */
#define XID_TYPE_GAMEPAD            1
#define XID_TYPE_KEYBOARD           2
#define XID_TYPE_IR_REMOTE          3

/* Memory unit drive letters (private/ntos/xapi/k32/basedll.h:134-152). */
#define MU_FIRST_DRIVE              'F'
#define MU_LAST_DRIVE               'M'
#define MU_SLOTS                    2

/* ---- kernel services used (src/kernel/ob.c, ke.c) --------------------- */

extern ULONG ExEventObjectType[];
NTSTATUS NTAPI ObReferenceObjectByHandle(HANDLE Handle, void *ObjectType, PVOID *Object);
LONG NTAPI KeSetEvent(KEVENT *Event, LONG Increment, BOOLEAN Wait);
void NTAPI KeInitializeTimerEx(KTIMER *Timer, ULONG Type);
void NTAPI KeInitializeDpc(KDPC *Dpc, PVOID DeferredRoutine, PVOID DeferredContext);
BOOLEAN NTAPI KeSetTimerEx(KTIMER *Timer, ULONG DueLow, LONG DueHigh, LONG Period, KDPC *Dpc);
BOOLEAN NTAPI KeCancelTimer(KTIMER *Timer);
BOOLEAN NTAPI KeRemoveQueueDpc(KDPC *Dpc);

/* ---- host state ------------------------------------------------------- */

#define XI_PORTS        4
#define XI_HANDLE_MAGIC 0x504E4958u             /* 'XINP' */
#define XI_SYNC_US      4000                    /* hotplug poll rate limit */
#define XI_ENUM_MS      500                     /* USB enumeration after XInitDevices */
#define XI_FEEDBACK_US  2000                    /* output report "transfer time" */
#define XI_RUMBLE_MS    30000                   /* the motors run until the title changes them; SDL caps at 30 s, re-armed on every XInputSetState */
#define XI_MAX_PENDING  16

typedef struct xi_handle {                     /* pool_alloc'ed: the HANDLE the title gets */
    ULONG magic;
    ULONG port;
    ULONG generation;                           /* of the device it was opened on */
    XINPUT_POLLING_PARAMETERS pp;
    ULONG packet;                               /* dwPacketNumber */
    uint64_t sampled_us;                        /* when `last` was taken */
    XINPUT_GAMEPAD last;                        /* latest report; kept after removal */
} xi_handle;

typedef struct {
    SDL_GameController *gc;                     /* NULL: empty or the keyboard pad */
    SDL_JoystickID instance;
    bool connected;
    bool kbd;                                   /* virtual keyboard pad */
    UCHAR subtype;
    ULONG generation;                           /* bumped on every insertion */
    xi_handle *open;
} xi_port;

typedef struct {                                /* an XInputSetState in flight */
    bool in_use;
    XINPUT_FEEDBACK *fb;
    xi_handle *handle;
    KTIMER timer;
    KDPC dpc;
} xi_pending;

static struct {
    pthread_mutex_t lock;
    bool inited, bound, sdl_ready, kbd_disabled, adopted, log_input;
    unsigned remaining;                         /* gamepad handles left (bRemainingHandles) */
    unsigned default_handles;                   /* from the guest GamepadTypeInfo, else 4 */
    XPP_DEVICE_TYPE *gamepad, *keyboard, *ir, *mu;   /* guest tables, NULL = unknown */
    ULONG *init_flag;                           /* guest _XPP_XInitDevicesHasBeenCalled */
    void (NTAPI *set_last_error)(ULONG);   /* guest _SetLastError@4 */
    xi_port port[XI_PORTS];
    uint64_t last_sync_us;
    uint64_t enum_done_us;                      /* devices show up from here on */
    xi_pending pending[XI_MAX_PENDING];
    int virtual_index;                          /* XBCOMPAT_VIRTUAL_PAD joystick, or -1 */
} xi = { .lock = PTHREAD_MUTEX_INITIALIZER, .default_handles = 4, .virtual_index = -1 };

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + ts.tv_nsec / 1000;
}

static bool guest_pointer(ULONG va)
{
    return va >= 0x10000 && va < CONTIG_BASE && !(va & 3);
}

/* ---- guest globals ---------------------------------------------------- */

/*
 * Find the title's device-type tables.  typeinfo.cpp puts a pointer to each
 * XID_TYPE_INFORMATION between XID_BeginTypeDescriptionTable and
 * XID_EndTypeDescriptionTable; the XppType member is the table the title
 * passes to XGetDevices.  The data symbols come from the HLE map and are never
 * patched.
 */
static void bind_globals(void)
{
    if (xi.bound) return;
    xi.bound = true;
    xi.init_flag = (ULONG *)hle_lookup("_XPP_XInitDevicesHasBeenCalled");
    xi.set_last_error = (void *)hle_lookup("_SetLastError@4");

    ULONG begin = hle_lookup_prefix("?XID_BeginTypeDescriptionTable@@");
    ULONG end = hle_lookup_prefix("?XID_EndTypeDescriptionTable@@");
    if (begin && end && end > begin && end - begin <= 64) {
        for (ULONG *p = (ULONG *)begin + 1; (ULONG)p < end; p++) {
            ULONG ti = *p;
            if (!guest_pointer(ti)) continue;
            ULONG xpp = *(ULONG *)(ti + XID_TI_XPPTYPE);
            if (!guest_pointer(xpp)) continue;
            switch (*(UCHAR *)(ti + XID_TI_TYPE)) {
            case XID_TYPE_GAMEPAD:
                xi.gamepad = (XPP_DEVICE_TYPE *)xpp;
                xi.default_handles = *(UCHAR *)(ti + XID_TI_HANDLES);
                break;
            case XID_TYPE_KEYBOARD: xi.keyboard = (XPP_DEVICE_TYPE *)xpp; break;
            case XID_TYPE_IR_REMOTE: xi.ir = (XPP_DEVICE_TYPE *)xpp; break;
            }
        }
    }
    if (!xi.gamepad) xi.gamepad = (XPP_DEVICE_TYPE *)hle_lookup("_XDEVICE_TYPE_GAMEPAD_TABLE");
    if (!xi.gamepad) {
        /* typeinfo.obj's .XPP$Data: GAMEPAD_TABLE at +0, GamepadTypeInfo at
           +0x38, DEBUG_KEYBOARD_TABLE at +0x54 (same in retail and debug). */
        ULONG ti = hle_lookup_prefix("?GamepadTypeInfo@@");
        ULONG kbd = hle_lookup("_XDEVICE_TYPE_DEBUG_KEYBOARD_TABLE");
        if (ti && guest_pointer(*(ULONG *)(ti + XID_TI_XPPTYPE)))
            xi.gamepad = (XPP_DEVICE_TYPE *)*(ULONG *)(ti + XID_TI_XPPTYPE);
        else if (kbd)
            xi.gamepad = (XPP_DEVICE_TYPE *)(kbd - 0x54);
    }
    if (!xi.keyboard) xi.keyboard = (XPP_DEVICE_TYPE *)hle_lookup("_XDEVICE_TYPE_DEBUG_KEYBOARD_TABLE");
    if (!xi.mu) xi.mu = (XPP_DEVICE_TYPE *)hle_lookup("_XDEVICE_TYPE_MEMORY_UNIT_TABLE");
    if (xi.default_handles == 0 || xi.default_handles > XGetPortCount()) xi.default_handles = XGetPortCount();

    if (xi.gamepad)
        xlog("XInput: gamepad table at %p, keyboard %p, IR remote %p, MU %p, init flag %p",
             (void *)xi.gamepad, (void *)xi.keyboard, (void *)xi.ir, (void *)xi.mu, (void *)xi.init_flag);
    else
        xlog("XInput: XDEVICE_TYPE_GAMEPAD_TABLE not found in the map; "
             "the first table passed to XGetDevices will be taken for it");
}

/* ---- device bookkeeping (xpp.c) --------------------------------------- */

static void report_insertion_removal(ULONG port, bool inserted)
{
    if (!xi.gamepad) return;                    /* published when the table is adopted */
    ULONG bit = 1u << port;
    xi.gamepad->Change |= bit;
    if (inserted) xi.gamepad->Current |= bit;
    else xi.gamepad->Current &= ~bit;
}

static void adopt_gamepad_table(XPP_DEVICE_TYPE *t)
{
    xi.gamepad = t;
    xi.adopted = true;
    xlog("XInput: taking table %p for XDEVICE_TYPE_GAMEPAD_TABLE (not in the HLE map)", (void *)t);
    for (unsigned i = 0; i < XI_PORTS; i++)
        if (xi.port[i].connected) report_insertion_removal(i, true);
}

static const char *table_name(const XPP_DEVICE_TYPE *t)
{
    if (t == xi.gamepad) return "gamepad";
    if (t == xi.keyboard) return "debug keyboard";
    if (t == xi.ir) return "IR remote";
    if (t == xi.mu) return "memory unit";
    return "unknown";
}

/* ---- SDL --------------------------------------------------------------- */

static void insert_port(unsigned i, SDL_GameController *gc, SDL_JoystickID instance, bool kbd)
{
    xi_port *p = &xi.port[i];
    p->gc = gc;
    p->instance = instance;
    p->connected = true;
    p->kbd = kbd;
    p->generation++;
    const char *name = kbd ? "keyboard" : SDL_GameControllerName(gc);
    /* Only the Gamepad sample looks at the subtype (to pick the mesh). */
    p->subtype = (name && strstr(name, "Duke")) ? XINPUT_DEVSUBTYPE_GC_GAMEPAD : XINPUT_DEVSUBTYPE_GC_GAMEPAD_ALT;
    xlog("XInput: port %u: %s inserted", i, name ? name : "controller");
    report_insertion_removal(i, true);
}

static void remove_port(unsigned i)
{
    xi_port *p = &xi.port[i];
    if (!p->connected) return;
    xlog("XInput: port %u: %s removed", i, p->kbd ? "keyboard pad" : "controller");
    if (p->gc) {
        SDL_GameControllerRumble(p->gc, 0, 0, 0);
        SDL_GameControllerClose(p->gc);
    }
    p->gc = NULL;
    p->instance = -1;
    p->connected = false;
    p->kbd = false;
    /* The handle outlives the device (XInputGetState keeps answering
       ERROR_DEVICE_NOT_CONNECTED until XInputClose), but it is no longer
       attached to the port: a device inserted here later gets a fresh node
       that can be opened at once (xid.cpp XID_fRemoveDevice/XID_fOpenDevice). */
    p->open = NULL;
    report_insertion_removal(i, false);
}

static int port_of_instance(SDL_JoystickID id)
{
    for (unsigned i = 0; i < XI_PORTS; i++)
        if (xi.port[i].connected && xi.port[i].gc && xi.port[i].instance == id) return (int)i;
    return -1;
}

static bool any_controller(void)
{
    for (unsigned i = 0; i < XI_PORTS; i++)
        if (xi.port[i].connected && xi.port[i].gc) return true;
    return false;
}

/*
 * Mirror SDL's controller list into the four ports: removals first, then new
 * controllers into the lowest free port, then the keyboard fallback.  Called
 * (rate limited) from every API entry so hotplug works without the event
 * pump, which belongs to D3DDevice_Swap.
 */
/*
 * Original Xbox controllers on Linux.  The kernel's xpad driver gives an
 * XTYPE_XBOX pad (Duke, Controller S and the third-party pads in its table,
 * through a breakaway-cable USB adapter) the keys A B C X Y Z Select Start
 * ThumbL ThumbR, with Black on BTN_C and White on BTN_Z, a d-pad hat and the
 * triggers on ABS_Z/ABS_RZ.  SDL's database only knows a few versions of
 * 045e:0202/0285/0289, and its automatic evdev mapping has no place for
 * BTN_C/BTN_Z, so every other original pad came up without Black and White.
 * This adds the mapping for each device in xpad's XTYPE_XBOX table, and for
 * "Generic X-Box pad" devices with the original pad's 10 buttons, 6 axes and
 * one hat.  Button and axis numbers follow SDL's evdev order (key code, then
 * ABS code), which shifts on the dance pads xpad gives d-pad and trigger
 * buttons.  The face buttons stay digital: xpad reports their pressure byte
 * as a key, so the title sees 0 or 255.
 */
enum { OG_DPAD_BUTTONS = 1, OG_TRIGGER_BUTTONS = 2, OG_NO_STICKS = 4 };

/* The SDL mapping for an XTYPE_XBOX pad with xpad's mapping flags. */
static void original_pad_mapping(const char *guid, int flags, char *m, size_t size)
{
    int b = 6, a = 0, n;
    n = snprintf(m, size, "%s,Xbox Controller (original),a:b0,b:b1,rightshoulder:b2,"
                 "x:b3,y:b4,leftshoulder:b5,", guid);
    if (flags & OG_TRIGGER_BUTTONS) {
        n += snprintf(m + n, size - n, "lefttrigger:b%d,righttrigger:b%d,", b, b + 1);
        b += 2;
    }
    n += snprintf(m + n, size - n, "back:b%d,start:b%d,leftstick:b%d,rightstick:b%d,",
                  b, b + 1, b + 2, b + 3);
    b += 4;
    if (flags & OG_DPAD_BUTTONS)
        n += snprintf(m + n, size - n, "dpup:b%d,dpdown:b%d,dpleft:b%d,dpright:b%d,",
                      b, b + 1, b + 2, b + 3);
    else
        n += snprintf(m + n, size - n, "dpup:h0.1,dpdown:h0.4,dpleft:h0.8,dpright:h0.2,");
    /* ABS_X, ABS_Y, ABS_Z (left trigger), ABS_RX, ABS_RY, ABS_RZ (right trigger) */
    if (!(flags & OG_NO_STICKS)) n += snprintf(m + n, size - n, "leftx:a%d,lefty:a%d,", a, a + 1), a += 2;
    if (!(flags & OG_TRIGGER_BUTTONS)) n += snprintf(m + n, size - n, "lefttrigger:a%d,", a++);
    if (!(flags & OG_NO_STICKS)) n += snprintf(m + n, size - n, "rightx:a%d,righty:a%d,", a, a + 1), a += 2;
    if (!(flags & OG_TRIGGER_BUTTONS)) snprintf(m + n, size - n, "righttrigger:a%d,", a++);
}

#ifdef __linux__

static const struct { Uint16 vendor, product; Uint8 flags; } og_pads[] = {
    { 0x044f, 0x0f00, 0 }, { 0x044f, 0x0f03, 0 }, { 0x044f, 0x0f07, 0 }, { 0x044f, 0x0f10, 0 },
    { 0x045e, 0x0202, 0 }, { 0x045e, 0x0285, 0 }, { 0x045e, 0x0287, 0 }, { 0x045e, 0x0288, 0 },
    { 0x045e, 0x0289, 0 }, { 0x046d, 0xca84, 0 }, { 0x046d, 0xca88, 0 }, { 0x046d, 0xca8a, 0 },
    { 0x05fd, 0x1007, 0 }, { 0x05fd, 0x107a, 0 }, { 0x05fe, 0x3030, 0 }, { 0x05fe, 0x3031, 0 },
    { 0x062a, 0x0020, 0 }, { 0x062a, 0x0033, 0 }, { 0x06a3, 0x0200, 0 }, { 0x06a3, 0x0201, 0 },
    { 0x0738, 0x4506, 0 }, { 0x0738, 0x4516, 0 }, { 0x0738, 0x4520, 0 }, { 0x0738, 0x4522, 0 },
    { 0x0738, 0x4526, 0 }, { 0x0738, 0x4530, 0 }, { 0x0738, 0x4536, 0 }, { 0x0738, 0x4540, 1 },
    { 0x0738, 0x4556, 0 }, { 0x0738, 0x4586, 0 }, { 0x0738, 0x4588, 0 }, { 0x0738, 0x45ff, 1 },
    { 0x0738, 0x4743, 1 }, { 0x0738, 0x6040, 1 }, { 0x0c12, 0x0005, 0 }, { 0x0c12, 0x8801, 0 },
    { 0x0c12, 0x8802, 0 }, { 0x0c12, 0x8809, 7 }, { 0x0c12, 0x880a, 0 }, { 0x0c12, 0x8810, 0 },
    { 0x0c12, 0x9902, 0 }, { 0x0d2f, 0x0002, 1 }, { 0x0e4c, 0x1097, 0 }, { 0x0e4c, 0x1103, 2 },
    { 0x0e4c, 0x2390, 0 }, { 0x0e4c, 0x3510, 0 }, { 0x0e6f, 0x0003, 0 }, { 0x0e6f, 0x0005, 0 },
    { 0x0e6f, 0x0006, 0 }, { 0x0e6f, 0x0008, 0 }, { 0x0e8f, 0x0201, 0 }, { 0x0e8f, 0x3008, 0 },
    { 0x0f30, 0x010b, 0 }, { 0x0f30, 0x0202, 0 }, { 0x0f30, 0x8888, 0 }, { 0x102c, 0xff0c, 0 },
    { 0x12ab, 0x8809, 1 }, { 0x1430, 0x8888, 1 }, { 0x3767, 0x0101, 0 }, { 0xffff, 0xffff, 0 }
};

static void map_original_pad(int j)
{
    static SDL_JoystickGUID done[16];
    static unsigned ndone;
    SDL_JoystickGUID guid = SDL_JoystickGetDeviceGUID(j);
    guid.data[2] = guid.data[3] = 0;            /* mappings never carry the name CRC */
    for (unsigned i = 0; i < ndone; i++)
        if (!memcmp(&done[i], &guid, sizeof(guid))) return;
    if (guid.data[0] != 0x03 || guid.data[1] != 0) return;      /* USB only */

    Uint16 vendor = SDL_JoystickGetDeviceVendor(j), product = SDL_JoystickGetDeviceProduct(j);
    int flags = -1;
    for (unsigned i = 0; i < sizeof(og_pads) / sizeof(og_pads[0]) && flags < 0; i++)
        if (og_pads[i].vendor == vendor && og_pads[i].product == product) flags = og_pads[i].flags;
    if (flags < 0) {
        const char *name = SDL_JoystickNameForIndex(j);
        if (!name || !strstr(name, "Generic X-Box pad")) return;
        SDL_Joystick *js = SDL_JoystickOpen(j);   /* a generic 360 pad has 11 buttons */
        if (!js) return;
        bool og = SDL_JoystickNumButtons(js) == 10 && SDL_JoystickNumAxes(js) == 6 &&
                  SDL_JoystickNumHats(js) == 1;
        SDL_JoystickClose(js);
        if (!og) {
            if (ndone < sizeof(done) / sizeof(done[0])) done[ndone++] = guid;
            return;
        }
        flags = 0;
    }

    char g[33], m[512];
    SDL_JoystickGetGUIDString(guid, g, sizeof(g));
    original_pad_mapping(g, flags, m, sizeof(m));
    if (SDL_GameControllerAddMapping(m) < 0)
        xlog("XInput: original Xbox pad %04x:%04x: SDL_GameControllerAddMapping: %s",
             vendor, product, SDL_GetError());
    else
        xlog("XInput: original Xbox pad %04x:%04x mapped (Black, White on the shoulders)", vendor, product);
    if (ndone < sizeof(done) / sizeof(done[0])) done[ndone++] = guid;
}
#else
static void map_original_pad(int j) { (void)j; }
#endif

/* XBCOMPAT_INPUT_SCRIPT (see the top of the file), or NULL. */
static const char *input_script(void)
{
    static const char *script;
    static int checked;
    if (!checked) { script = getenv("XBCOMPAT_INPUT_SCRIPT"); checked = 1; }
    return script;
}

static void sync_devices(bool force)
{
    uint64_t t = now_us();
    if (!xi.inited || !xi.sdl_ready) return;
    if (!force && t - xi.last_sync_us < XI_SYNC_US) return;
    xi.last_sync_us = t;
    if (t < xi.enum_done_us) return;            /* the USB stack is still enumerating */

    SDL_GameControllerUpdate();
    for (unsigned i = 0; i < XI_PORTS; i++)
        if (xi.port[i].gc && !SDL_GameControllerGetAttached(xi.port[i].gc)) remove_port(i);

    int n = SDL_NumJoysticks();
    for (int j = 0; j < n; j++) {
        SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(j);
        if (id < 0 || port_of_instance(id) >= 0) continue;
        map_original_pad(j);
        if (!SDL_IsGameController(j)) continue;
        if (xi.port[0].kbd) remove_port(0);     /* a real controller takes port 0 */
        int free = -1;
        for (unsigned i = 0; i < XI_PORTS && free < 0; i++)
            if (!xi.port[i].connected) free = (int)i;
        if (free < 0) break;                    /* more than four: ignored */
        SDL_GameController *gc = SDL_GameControllerOpen(j);
        if (!gc) {
            xlog("XInput: SDL_GameControllerOpen(%d): %s", j, SDL_GetError());
            continue;
        }
        insert_port((unsigned)free, gc, id, false);
    }

    bool want_kbd = !xi.kbd_disabled && !any_controller() && SDL_WasInit(SDL_INIT_VIDEO);
    if (want_kbd && !xi.port[0].connected) insert_port(0, NULL, -1, true);
    else if (!want_kbd && xi.port[0].kbd) remove_port(0);
    if (want_kbd && input_script() && strstr(input_script(), ":p2") && !xi.port[1].connected)
        insert_port(1, NULL, -1, true);
}

/*
 * SDL scales a full-range axis bound to a trigger onto 0..32767, which the
 * report turns into 0..255 with >> 7: the top of each 256-wide bucket of the
 * raw range lands exactly on the wanted byte.
 */
static Sint16 virtual_trigger_raw(int byte)
{
    if (byte < 0) byte = 0;
    if (byte > 255) byte = 255;
    return (Sint16)((byte << 8) + 255 - 32768);
}

/*
 * XBCOMPAT_VIRTUAL_PAD's value: "1" is a pad at rest; otherwise a list of
 * buttons to hold and axes to set, in the title's units (see the header).
 */
static void apply_virtual_spec(SDL_Joystick *js, const char *spec)
{
    static const struct { const char *name; SDL_GameControllerButton button; } buttons[] = {
        { "a", SDL_CONTROLLER_BUTTON_A }, { "b", SDL_CONTROLLER_BUTTON_B },
        { "x", SDL_CONTROLLER_BUTTON_X }, { "y", SDL_CONTROLLER_BUTTON_Y },
        { "black", SDL_CONTROLLER_BUTTON_RIGHTSHOULDER }, { "white", SDL_CONTROLLER_BUTTON_LEFTSHOULDER },
        { "start", SDL_CONTROLLER_BUTTON_START }, { "back", SDL_CONTROLLER_BUTTON_BACK },
        { "lthumb", SDL_CONTROLLER_BUTTON_LEFTSTICK }, { "rthumb", SDL_CONTROLLER_BUTTON_RIGHTSTICK },
        { "up", SDL_CONTROLLER_BUTTON_DPAD_UP }, { "down", SDL_CONTROLLER_BUTTON_DPAD_DOWN },
        { "left", SDL_CONTROLLER_BUTTON_DPAD_LEFT }, { "right", SDL_CONTROLLER_BUTTON_DPAD_RIGHT },
    };
    static const struct { const char *name; SDL_GameControllerAxis axis; bool trigger, up_positive; } axes[] = {
        { "lt", SDL_CONTROLLER_AXIS_TRIGGERLEFT, true, false }, { "rt", SDL_CONTROLLER_AXIS_TRIGGERRIGHT, true, false },
        { "lx", SDL_CONTROLLER_AXIS_LEFTX, false, false }, { "ly", SDL_CONTROLLER_AXIS_LEFTY, false, true },
        { "rx", SDL_CONTROLLER_AXIS_RIGHTX, false, false }, { "ry", SDL_CONTROLLER_AXIS_RIGHTY, false, true },
    };
    char buf[256], *save = NULL;
    snprintf(buf, sizeof(buf), "%s", spec ? spec : "");
    for (char *tok = strtok_r(buf, ", ", &save); tok; tok = strtok_r(NULL, ", ", &save)) {
        char *eq = strchr(tok, '=');
        if (eq) *eq++ = 0;
        bool found = false;
        for (unsigned i = 0; i < sizeof(buttons) / sizeof(buttons[0]) && !found; i++) {
            if (strcmp(tok, buttons[i].name)) continue;
            SDL_JoystickSetVirtualButton(js, buttons[i].button, eq ? atoi(eq) != 0 : 1);
            found = true;
        }
        for (unsigned i = 0; i < sizeof(axes) / sizeof(axes[0]) && !found; i++) {
            if (strcmp(tok, axes[i].name) || !eq) continue;
            int v = atoi(eq);
            if (v < -32768) v = -32768;
            if (v > 32767) v = 32767;
            Sint16 raw = axes[i].trigger ? virtual_trigger_raw(v)
                       : (Sint16)(axes[i].up_positive ? -1 - v : v);   /* stick_y() undoes this */
            SDL_JoystickSetVirtualAxis(js, axes[i].axis, raw);
            found = true;
        }
        if (!found && strcmp(tok, "1")) xlog("XInput: XBCOMPAT_VIRTUAL_PAD: \"%s\" not understood", tok);
    }
}

/*
 * An SDL virtual joystick laid out like a game controller (buttons and axes
 * in SDL_CONTROLLER_BUTTON_* / SDL_CONTROLLER_AXIS_* order).  SDL maps it by
 * itself in recent versions; otherwise the mapping is added here.  Returns
 * the device index or -1.
 */
static int attach_virtual_pad(const char *spec)
{
    int index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
                                          SDL_CONTROLLER_AXIS_MAX, SDL_CONTROLLER_BUTTON_MAX, 0);
    if (index < 0) {
        xlog("XInput: SDL_JoystickAttachVirtual: %s", SDL_GetError());
        return -1;
    }
    if (!SDL_IsGameController(index)) {
        char guid[40], mapping[400];
        SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(index), guid, sizeof(guid));
        snprintf(mapping, sizeof(mapping),
                 "%s,xbcompat virtual pad,a:b0,b:b1,x:b2,y:b3,back:b4,guide:b5,start:b6,leftstick:b7,"
                 "rightstick:b8,leftshoulder:b9,rightshoulder:b10,dpup:b11,dpdown:b12,dpleft:b13,"
                 "dpright:b14,leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5",
                 guid);
        if (SDL_GameControllerAddMapping(mapping) < 0)
            xlog("XInput: SDL_GameControllerAddMapping: %s", SDL_GetError());
    }
    /* SDL scales a full-range axis bound to a trigger to 0..32767, so the
       virtual trigger axes rest at -32768, not 0. */
    SDL_Joystick *js = SDL_JoystickOpen(index);
    if (js) {
        SDL_JoystickSetVirtualAxis(js, SDL_CONTROLLER_AXIS_TRIGGERLEFT, SDL_JOYSTICK_AXIS_MIN);
        SDL_JoystickSetVirtualAxis(js, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, SDL_JOYSTICK_AXIS_MIN);
        if (spec) apply_virtual_spec(js, spec);
        SDL_JoystickClose(js);
    }
    return index;
}

static void init_sdl(void)
{
    if (xi.sdl_ready) return;
    /* The video subsystem is d3d8.c's (SDL_Init at CreateDevice); SDL
       refcounts subsystems, so the order does not matter. */
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        xlog("XInput: SDL_InitSubSystem(GAMECONTROLLER): %s; no controllers", SDL_GetError());
        return;
    }
    if (SDL_InitSubSystem(SDL_INIT_HAPTIC) != 0)
        xlog("XInput: SDL_InitSubSystem(HAPTIC): %s", SDL_GetError());
    install_fault_handlers();
    xi.sdl_ready = true;
    const char *e = getenv("XBCOMPAT_NO_KBD_PAD");
    xi.kbd_disabled = e && *e && *e != '0';
    e = getenv("XBCOMPAT_LOG_INPUT");               /* log every change in a report */
    xi.log_input = e && *e && *e != '0';
    e = getenv("XBCOMPAT_VIRTUAL_PAD");
    if (e && *e && *e != '0') xi.virtual_index = attach_virtual_pad(e);
    xlog("XInput: SDL game controllers ready, %d joystick(s) present, keyboard pad %s",
         SDL_NumJoysticks(), xi.kbd_disabled ? "disabled" : "enabled");
}

/* ---- reports ----------------------------------------------------------- */

static UCHAR trigger_byte(Sint16 axis)
{
    return axis <= 0 ? 0 : (UCHAR)(axis >> 7);
}

static SHORT stick_y(Sint16 axis)
{
    return (SHORT)(-1 - axis);                  /* SDL: down positive; Xbox: up positive */
}

static void read_keyboard(XINPUT_GAMEPAD *g)
{
    int n = 0;
    const Uint8 *k = SDL_GetKeyboardState(&n);
    if (!k) return;
#define K(sc) ((int)(sc) < n && k[sc])
    if (K(SDL_SCANCODE_SPACE) || K(SDL_SCANCODE_RETURN)) g->bAnalogButtons[XINPUT_GAMEPAD_A] = 255;
    if (K(SDL_SCANCODE_B)) g->bAnalogButtons[XINPUT_GAMEPAD_B] = 255;
    if (K(SDL_SCANCODE_X)) g->bAnalogButtons[XINPUT_GAMEPAD_X] = 255;
    if (K(SDL_SCANCODE_Y)) g->bAnalogButtons[XINPUT_GAMEPAD_Y] = 255;
    if (K(SDL_SCANCODE_1)) g->bAnalogButtons[XINPUT_GAMEPAD_WHITE] = 255;
    if (K(SDL_SCANCODE_2)) g->bAnalogButtons[XINPUT_GAMEPAD_BLACK] = 255;
    if (K(SDL_SCANCODE_Q)) g->bAnalogButtons[XINPUT_GAMEPAD_LEFT_TRIGGER] = 255;
    if (K(SDL_SCANCODE_E)) g->bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER] = 255;
    if (K(SDL_SCANCODE_UP)) g->wButtons |= XINPUT_GAMEPAD_DPAD_UP;
    if (K(SDL_SCANCODE_DOWN)) g->wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
    if (K(SDL_SCANCODE_LEFT)) g->wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
    if (K(SDL_SCANCODE_RIGHT)) g->wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
    if (K(SDL_SCANCODE_BACKSPACE)) g->wButtons |= XINPUT_GAMEPAD_START;
    if (K(SDL_SCANCODE_ESCAPE)) g->wButtons |= XINPUT_GAMEPAD_BACK;
    if (K(SDL_SCANCODE_3)) g->wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
    if (K(SDL_SCANCODE_4)) g->wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;
    if (K(SDL_SCANCODE_W)) g->sThumbLY = 32767;
    if (K(SDL_SCANCODE_S)) g->sThumbLY = -32768;
    if (K(SDL_SCANCODE_A)) g->sThumbLX = -32768;
    if (K(SDL_SCANCODE_D)) g->sThumbLX = 32767;
    if (K(SDL_SCANCODE_I)) g->sThumbRY = 32767;
    if (K(SDL_SCANCODE_K)) g->sThumbRY = -32768;
    if (K(SDL_SCANCODE_J)) g->sThumbRX = -32768;
    if (K(SDL_SCANCODE_L)) g->sThumbRX = 32767;
#undef K
}

static void read_controller(SDL_GameController *gc, XINPUT_GAMEPAD *g)
{
#define BTN(b) SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_##b)
#define AXIS(a) SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_##a)
    g->bAnalogButtons[XINPUT_GAMEPAD_A] = BTN(A) ? 255 : 0;
    g->bAnalogButtons[XINPUT_GAMEPAD_B] = BTN(B) ? 255 : 0;
    g->bAnalogButtons[XINPUT_GAMEPAD_X] = BTN(X) ? 255 : 0;
    g->bAnalogButtons[XINPUT_GAMEPAD_Y] = BTN(Y) ? 255 : 0;
    g->bAnalogButtons[XINPUT_GAMEPAD_BLACK] = BTN(RIGHTSHOULDER) ? 255 : 0;
    g->bAnalogButtons[XINPUT_GAMEPAD_WHITE] = BTN(LEFTSHOULDER) ? 255 : 0;
    g->bAnalogButtons[XINPUT_GAMEPAD_LEFT_TRIGGER] = trigger_byte(AXIS(TRIGGERLEFT));
    g->bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER] = trigger_byte(AXIS(TRIGGERRIGHT));
    g->wButtons = (BTN(DPAD_UP) ? XINPUT_GAMEPAD_DPAD_UP : 0) |
                  (BTN(DPAD_DOWN) ? XINPUT_GAMEPAD_DPAD_DOWN : 0) |
                  (BTN(DPAD_LEFT) ? XINPUT_GAMEPAD_DPAD_LEFT : 0) |
                  (BTN(DPAD_RIGHT) ? XINPUT_GAMEPAD_DPAD_RIGHT : 0) |
                  (BTN(START) ? XINPUT_GAMEPAD_START : 0) |
                  (BTN(BACK) ? XINPUT_GAMEPAD_BACK : 0) |
                  (BTN(LEFTSTICK) ? XINPUT_GAMEPAD_LEFT_THUMB : 0) |
                  (BTN(RIGHTSTICK) ? XINPUT_GAMEPAD_RIGHT_THUMB : 0);
    g->sThumbLX = AXIS(LEFTX);
    g->sThumbLY = stick_y(AXIS(LEFTY));
    g->sThumbRX = AXIS(RIGHTX);
    g->sThumbRY = stick_y(AXIS(RIGHTY));
#undef BTN
#undef AXIS
}

/* XBCOMPAT_INPUT_SCRIPT (see the top of the file). */
static void read_script(XINPUT_GAMEPAD *g, unsigned port)
{
    const char *script = input_script();
    if (!script) return;
    ULONG now = d3d_frame_count();
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", script);
    for (char *save, *tok = strtok_r(buf, " ,", &save); tok; tok = strtok_r(NULL, " ,", &save)) {
        char buf2[16] = "", *name = buf2;
        unsigned frame = 0, hold = 6;
        if (sscanf(tok, "%u:%15[a-z0-9]/%u", &frame, buf2, &hold) < 2) continue;
        unsigned to = 0;
        if (!strncmp(name, "p2", 2)) { to = 1; name += 2; }
        if (to != port || now < frame || now >= frame + hold) continue;
        static const char *analog[] = { "a", "b", "x", "y", "black", "white", "lt", "rt" };
        static const int analog_idx[] = { XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y,
                                          XINPUT_GAMEPAD_BLACK, XINPUT_GAMEPAD_WHITE,
                                          XINPUT_GAMEPAD_LEFT_TRIGGER, XINPUT_GAMEPAD_RIGHT_TRIGGER };
        static const char *digital[] = { "up", "down", "left", "right", "start", "back", "lthumb", "rthumb" };
        static const USHORT digital_bit[] = { XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN,
                                              XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_RIGHT,
                                              XINPUT_GAMEPAD_START, XINPUT_GAMEPAD_BACK,
                                              XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB };
        for (int i = 0; i < 8; i++) {
            if (!strcmp(name, analog[i])) g->bAnalogButtons[analog_idx[i]] = 255;
            if (!strcmp(name, digital[i])) g->wButtons |= digital_bit[i];
        }
    }
}

static void read_port(const xi_port *p, XINPUT_GAMEPAD *g)
{
    memset(g, 0, sizeof(*g));
    if (p->kbd) {
        if (p == &xi.port[0]) read_keyboard(g);
        read_script(g, (unsigned)(p - xi.port));
    }
    else if (p->gc) {
        read_controller(p->gc, g);
        /* The keyboard keeps working beside a controller in port 0. */
        if (p == &xi.port[0] && !xi.kbd_disabled) read_keyboard(g);
    }
}

/*
 * The hardware delivers a new report every bInputInterval ms (8 by default)
 * and bumps the packet number for each one (xid.cpp XID_NewInterruptData),
 * whether or not the data changed.  A title polling faster than that sees
 * the same report, and one polling slower sees the packet number advance by
 * the number of transfers that happened in between; both come from the
 * interval gate here.  An XInputPoll (force) is a single transfer.
 */
static void sample(xi_handle *h, const xi_port *p, bool force)
{
    uint64_t t = now_us();
    uint64_t interval = (h->pp.bInputInterval ? h->pp.bInputInterval : 1) * 1000ull;
    uint64_t elapsed = t - h->sampled_us;
    if (!force && elapsed < interval) return;
    XINPUT_GAMEPAD before = h->last;
    read_port(p, &h->last);
    if (xi.log_input && memcmp(&before, &h->last, sizeof(before)))
        xlog("XInput: port %u: buttons %#06x, A %u B %u X %u Y %u, left stick %d,%d",
             h->port, h->last.wButtons, h->last.bAnalogButtons[XINPUT_GAMEPAD_A],
             h->last.bAnalogButtons[XINPUT_GAMEPAD_B], h->last.bAnalogButtons[XINPUT_GAMEPAD_X],
             h->last.bAnalogButtons[XINPUT_GAMEPAD_Y], h->last.sThumbLX, h->last.sThumbLY);
    if (force) {
        h->sampled_us = t;
        h->packet++;
    } else {
        uint64_t n = elapsed / interval;
        h->packet += (ULONG)n;
        h->sampled_us += n * interval;
    }
}

/*
 * L + R + Back + Start on any pad (Q + E + Escape + Backspace on the
 * keyboard pad) goes back to the dashboard, the in-game reset modded Xboxes
 * had (kernel/reset.c). It fires when the last of the four goes down, so a
 * combo still held from the title before doesn't reset this one.
 */
void xinput_check_reset_combo(void)
{
    static bool was_held = true;
    if (!reset_enabled() || !xi.inited || !xi.sdl_ready) return;
    bool held = false;
    pthread_mutex_lock(&xi.lock);
    for (unsigned i = 0; i < XI_PORTS && !held; i++) {
        const xi_port *p = &xi.port[i];
        if (!p->connected) continue;
        XINPUT_GAMEPAD g;
        read_port(p, &g);
        held = (g.wButtons & (XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START)) ==
                   (XINPUT_GAMEPAD_BACK | XINPUT_GAMEPAD_START) &&
               g.bAnalogButtons[XINPUT_GAMEPAD_LEFT_TRIGGER] >= 128 &&
               g.bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER] >= 128;
    }
    pthread_mutex_unlock(&xi.lock);
    if (held && !was_held) reset_request("L + R + Back + Start");
    was_held = held;
}

/* ---- handles ------------------------------------------------------------ */

static xi_handle *check_handle(HANDLE h, const char *api)
{
    xi_handle *d = h;
    if (!d || d->magic != XI_HANDLE_MAGIC || d->port >= XI_PORTS) {
        xlog("XInput: %s called with invalid handle %p", api, h);
        return NULL;
    }
    return d;
}

/* Is the device this handle was opened on still there? */
static bool handle_connected(const xi_handle *d)
{
    const xi_port *p = &xi.port[d->port];
    return p->connected && p->generation == d->generation;
}

static void set_last_error(ULONG code)
{
#ifdef XBC_TRANSLATED
    if (xi.set_last_error) CPU_CALL(xi.set_last_error, CONV_STD, code);
#else
    if (xi.set_last_error) xi.set_last_error(code);
#endif
}

/* ---- feedback completion (the DPC thread plays the USB controller) ------ */

static void complete_pending(xi_pending *pe, ULONG status)
{
    XINPUT_FEEDBACK *fb = pe->fb;
    HANDLE hEvent = fb->hEvent;
    pe->in_use = false;
    pe->fb = NULL;
    pe->handle = NULL;
    fb->dwStatus = status;
    if (!hEvent) return;
    /* Dropped the lock before the dispatcher (see feedback_dpc). */
    pthread_mutex_unlock(&xi.lock);
    PVOID ev = NULL;
    if (NT_SUCCESS(ObReferenceObjectByHandle(hEvent, ExEventObjectType, &ev)) && ev)
        KeSetEvent(ev, 0, 0);
    else
        xlog("XInput: feedback hEvent %p is not an event handle; not signalled", hEvent);
    pthread_mutex_lock(&xi.lock);
}

static void NTAPI feedback_dpc(KDPC *Dpc, PVOID Context, PVOID Arg1, PVOID Arg2)
{
    (void)Dpc; (void)Arg1; (void)Arg2;
    xi_pending *pe = Context;
    pthread_mutex_lock(&xi.lock);
    if (pe->in_use) complete_pending(pe, ERROR_SUCCESS);
    pthread_mutex_unlock(&xi.lock);
}

static void cancel_pending_for(xi_handle *h)
{
    for (unsigned i = 0; i < XI_MAX_PENDING; i++) {
        xi_pending *pe = &xi.pending[i];
        if (!pe->in_use || pe->handle != h) continue;
        KeCancelTimer(&pe->timer);
        KeRemoveQueueDpc(&pe->dpc);
        complete_pending(pe, ERROR_CANCELLED);
    }
}

/* ---- the API ------------------------------------------------------------- */

static void NTAPI XInitDevices(ULONG dwPreallocTypeCount, XDEVICE_PREALLOC_TYPE *PreallocTypes)
{
    pthread_mutex_lock(&xi.lock);
    if (xi.inited) {
        xlog("XInput: XInitDevices called more than once (the debug library would RIP)");
        pthread_mutex_unlock(&xi.lock);
        return;
    }
    bind_globals();
    init_sdl();
    for (unsigned i = 0; i < XI_PORTS; i++) xi.port[i].instance = -1;

    /* With a list, a type gets the handles it asks for and 0 if left out
       (xid.cpp XID_Init + usbinit.cpp GetMaxDeviceTypeCount). */
    xi.remaining = xi.default_handles;
    if (PreallocTypes && !xi.gamepad) {
        xlog("XInput: XInitDevices: prealloc list given but the gamepad table is unknown; "
             "allowing %u gamepad handles", xi.remaining);
    } else if (PreallocTypes) {
        xi.remaining = 0;
        for (ULONG i = 0; i < dwPreallocTypeCount; i++) {
            XPP_DEVICE_TYPE *t = PreallocTypes[i].DeviceType;
            ULONG n = PreallocTypes[i].dwPreallocCount;
            xlog("XInput: XInitDevices: %s table %p, %u handles", table_name(t), (void *)t, n);
            if (t == xi.gamepad) {
                if (n > XGetPortCount()) {
                    xlog("XInput: more than %u gamepad handles requested (the driver would RIP)", XGetPortCount());
                    n = XGetPortCount();
                }
                xi.remaining = n;
            }
        }
    }
    if (xi.init_flag) *xi.init_flag = 1;
    xi.inited = true;
    xlog("XInput: XInitDevices: %u gamepad handle(s) available", xi.remaining);
    /* XInitDevices only starts the USB host (USBD_Init); devices plugged in
       at boot are enumerated afterwards and reach the title as insertions,
       so an XGetDevices right after it sees none.  Titles that set a player
       up only on an insertion (Orbz) otherwise ask for the pad to be
       reconnected.  XBCOMPAT_ENUM_MS changes the delay (0: none). */
    const char *e = getenv("XBCOMPAT_ENUM_MS");
    xi.enum_done_us = now_us() + 1000ull * (e && *e ? (unsigned)atoi(e) : XI_ENUM_MS);
    sync_devices(true);
    pthread_mutex_unlock(&xi.lock);
}

static void check_inited(const char *api)
{
    if (!xi.inited) xlog("XInput: %s: XInitDevices must be called first (the debug library would RIP)", api);
}

static XPP_DEVICE_TYPE *device_table(XPP_DEVICE_TYPE *t, const char *api)
{
    if (!t) {
        xlog("XInput: %s called with a NULL device type", api);
        return NULL;
    }
    if (!xi.gamepad && !xi.adopted) adopt_gamepad_table(t);
    return t;
}

static ULONG NTAPI XGetDevices(XPP_DEVICE_TYPE *DeviceType)
{
    pthread_mutex_lock(&xi.lock);
    check_inited("XGetDevices");
    XPP_DEVICE_TYPE *t = device_table(DeviceType, "XGetDevices");
    ULONG ret = 0;
    if (t) {
        sync_devices(false);
        ret = t->Current;
        t->Change = 0;
        t->Previous = t->Current;
        TRACE_ALL("XInput: XGetDevices(%s) = %#x", table_name(t), ret);
    }
    pthread_mutex_unlock(&xi.lock);
    return ret;
}

static ULONG NTAPI XGetDeviceChanges(XPP_DEVICE_TYPE *DeviceType, ULONG *pdwInsertions, ULONG *pdwRemovals)
{
    pthread_mutex_lock(&xi.lock);
    check_inited("XGetDeviceChanges");
    XPP_DEVICE_TYPE *t = device_table(DeviceType, "XGetDeviceChanges");
    ULONG ins = 0, rem = 0;
    if (t) {
        sync_devices(false);
        if (t->Change) {
            ins = t->Current & ~t->Previous;
            rem = t->Previous & ~t->Current;
            ULONG reinserted = t->Change & t->Current & t->Previous;
            ins |= reinserted;
            rem |= reinserted;
            t->Change = 0;
            t->Previous = t->Current;
            xlog("XInput: XGetDeviceChanges(%s): inserted %#x, removed %#x", table_name(t), ins, rem);
        }
    }
    if (pdwInsertions) *pdwInsertions = ins;
    if (pdwRemovals) *pdwRemovals = rem;
    pthread_mutex_unlock(&xi.lock);
    return (ins | rem) ? 1 : 0;
}

/* Internal API: what is connected, what the title last saw, what was
   removed and reinserted since (xpp.c:92-153). */
static ULONG NTAPI XPeekDevices(XPP_DEVICE_TYPE *DeviceType, ULONG *pLastGotten, ULONG *pStale)
{
    pthread_mutex_lock(&xi.lock);
    check_inited("XPeekDevices");
    XPP_DEVICE_TYPE *t = device_table(DeviceType, "XPeekDevices");
    ULONG ret = 0;
    if (t) {
        sync_devices(false);
        ret = t->Current;
        if (pLastGotten) *pLastGotten = t->Previous;
        if (pStale) *pStale = t->Current & t->Previous & t->Change;
    } else {
        if (pLastGotten) *pLastGotten = 0;
        if (pStale) *pStale = 0;
    }
    pthread_mutex_unlock(&xi.lock);
    return ret;
}

static HANDLE NTAPI XInputOpen(XPP_DEVICE_TYPE *DeviceType, ULONG dwPort, ULONG dwSlot,
                               XINPUT_POLLING_PARAMETERS *pPollingParameters)
{
    static const XINPUT_POLLING_PARAMETERS gamepad_defaults = { XINPUT_POLL_AUTO, 8, 0, 0 };
    pthread_mutex_lock(&xi.lock);
    check_inited("XInputOpen");
    sync_devices(false);
    ULONG err = ERROR_SUCCESS;
    xi_handle *d = NULL;

    if (!device_table(DeviceType, "XInputOpen")) {
        err = ERROR_INVALID_PARAMETER;
    } else if (DeviceType != xi.gamepad) {
        if (DeviceType == xi.keyboard || DeviceType == xi.ir) {
            err = ERROR_DEVICE_NOT_CONNECTED;   /* valid types; nothing is ever plugged in */
        } else {
            xlog("XInput: XInputOpen: %p is not an XInput device type (the driver would RIP)", (void *)DeviceType);
            err = ERROR_INVALID_PARAMETER;
        }
    } else if (dwPort >= XGetPortCount() || dwSlot != XDEVICE_NO_SLOT) {
        /* The debug driver RIPs; the retail one looks for a node on that
           port (port + 16 for a bottom slot) and finds none. */
        xlog("XInput: XInputOpen: bad port %u / slot %u (the debug driver would RIP)", dwPort, dwSlot);
        err = ERROR_DEVICE_NOT_CONNECTED;
    } else if (!xi.port[dwPort].connected) {
        err = ERROR_DEVICE_NOT_CONNECTED;
    } else if (xi.port[dwPort].open) {
        err = ERROR_SHARING_VIOLATION;
    } else if (xi.remaining == 0) {
        err = ERROR_OUTOFMEMORY;
    } else if (!(d = pool_alloc(sizeof(*d)))) {
        err = ERROR_OUTOFMEMORY;
    } else {
        xi_port *p = &xi.port[dwPort];
        xi.remaining--;
        memset(d, 0, sizeof(*d));
        d->magic = XI_HANDLE_MAGIC;
        d->port = dwPort;
        d->generation = p->generation;
        d->pp = pPollingParameters ? *pPollingParameters : gamepad_defaults;
        p->open = d;
        /* The driver reads one report synchronously at open; the packet
           number only starts counting with the interrupt transfers. */
        read_port(p, &d->last);
        d->sampled_us = now_us();
        xlog("XInput: XInputOpen port %u: handle %p (%s, autopoll %s, %u ms)", dwPort, (void *)d,
             p->kbd ? "keyboard" : SDL_GameControllerName(p->gc),
             (d->pp.flags & XINPUT_POLL_AUTO) ? "on" : "off", d->pp.bInputInterval);
    }
    if (!d) {
        xlog("XInput: XInputOpen(%s, port %u, slot %u) failed: error %u", table_name(DeviceType), dwPort, dwSlot, err);
        set_last_error(err);
    }
    pthread_mutex_unlock(&xi.lock);
    return d;
}

static void NTAPI XInputClose(HANDLE hDevice)
{
    pthread_mutex_lock(&xi.lock);
    xi_handle *d = check_handle(hDevice, "XInputClose");
    if (d) {
        xi_port *p = &xi.port[d->port];
        cancel_pending_for(d);
        if (p->open == d) {
            p->open = NULL;
            if (p->gc) SDL_GameControllerRumble(p->gc, 0, 0, 0);
        }
        xi.remaining++;
        TRACE_ALL("XInput: XInputClose %p (port %u)", hDevice, d->port);
        d->magic = 0;
        pool_free(d);
    }
    pthread_mutex_unlock(&xi.lock);
}

static ULONG NTAPI XInputGetCapabilities(HANDLE hDevice, XINPUT_CAPABILITIES *pCapabilities)
{
    pthread_mutex_lock(&xi.lock);
    sync_devices(false);
    ULONG err = ERROR_SUCCESS;
    xi_handle *d = check_handle(hDevice, "XInputGetCapabilities");
    if (!d) {
        err = ERROR_INVALID_PARAMETER;
    } else if (!handle_connected(d)) {
        err = ERROR_DEVICE_NOT_CONNECTED;      /* the buffer is left alone (xidinp.cpp) */
    } else if (pCapabilities) {
        /* The In/Out capability reports are hardware replies ("every bit
           of every field is supported"); xemu answers all ones too. */
        memset(pCapabilities, 0, sizeof(*pCapabilities));
        pCapabilities->SubType = xi.port[d->port].subtype;
        memset(&pCapabilities->In, 0xFF, sizeof(pCapabilities->In));
        memset(&pCapabilities->Out, 0xFF, sizeof(pCapabilities->Out));
    }
    /* Cleared on every path so nobody divines anything from it (bug 2578). */
    if (pCapabilities) pCapabilities->Reserved = 0;
    pthread_mutex_unlock(&xi.lock);
    return err;
}

static ULONG NTAPI XInputGetState(HANDLE hDevice, XINPUT_STATE *pState)
{
    pthread_mutex_lock(&xi.lock);
    sync_devices(false);
    ULONG err = ERROR_SUCCESS;
    xi_handle *d = check_handle(hDevice, "XInputGetState");
    if (!d) {
        if (pState) memset(pState, 0, sizeof(*pState));
        err = ERROR_INVALID_PARAMETER;
    } else {
        if (!handle_connected(d)) err = ERROR_DEVICE_NOT_CONNECTED;   /* the state is still copied */
        else if (d->pp.flags & XINPUT_POLL_AUTO) sample(d, &xi.port[d->port], false);
        if (pState) {
            pState->dwPacketNumber = d->packet;
            memcpy(&pState->Gamepad, &d->last, sizeof(XINPUT_GAMEPAD));
        }
    }
    pthread_mutex_unlock(&xi.lock);
    return err;
}

static ULONG NTAPI XInputPoll(HANDLE hDevice)
{
    pthread_mutex_lock(&xi.lock);
    sync_devices(false);
    ULONG err = ERROR_SUCCESS;
    xi_handle *d = check_handle(hDevice, "XInputPoll");
    if (!d) err = ERROR_INVALID_PARAMETER;
    else if (!handle_connected(d)) err = ERROR_DEVICE_NOT_CONNECTED;
    else if (!(d->pp.flags & XINPUT_POLL_AUTO)) sample(d, &xi.port[d->port], true);
    pthread_mutex_unlock(&xi.lock);
    return err;
}

static ULONG NTAPI XInputSetState(HANDLE hDevice, XINPUT_FEEDBACK *pFeedback)
{
    pthread_mutex_lock(&xi.lock);
    sync_devices(false);
    ULONG err;
    xi_handle *d = check_handle(hDevice, "XInputSetState");
    if (!d || !pFeedback) {
        err = ERROR_INVALID_PARAMETER;
    } else if (!handle_connected(d)) {
        err = pFeedback->dwStatus = ERROR_DEVICE_NOT_CONNECTED;
    } else {
        xi_port *p = &xi.port[d->port];
        USHORT l = pFeedback->Rumble.wLeftMotorSpeed, r = pFeedback->Rumble.wRightMotorSpeed;
        if (p->gc) SDL_GameControllerRumble(p->gc, l, r, (l || r) ? XI_RUMBLE_MS : 0);
        TRACE_ALL("XInput: XInputSetState port %u: rumble %u/%u", d->port, l, r);

        /* The driver references the completion event when the report is
           submitted and drops a handle that is not an event (xid.cpp
           XID_fSendDeviceReport); the 6-byte output report it builds in the
           hidden part of the header starts with the report id and size. */
        if (pFeedback->hEvent) {
            PVOID ev = NULL;
            if (!NT_SUCCESS(ObReferenceObjectByHandle(pFeedback->hEvent, ExEventObjectType, &ev)) || !ev) {
                xlog("XInput: XInputSetState: hEvent %p is not an event handle; it will not be signalled",
                     pFeedback->hEvent);
                pFeedback->hEvent = NULL;
            }
        }
        pFeedback->Reserved[56] = 0;            /* bReportId */
        pFeedback->Reserved[57] = 2 + sizeof(XINPUT_RUMBLE);   /* bSize */

        /* One transfer per feedback structure: resubmitting one that is in
           flight just updates the motors. */
        xi_pending *pe = NULL, *free = NULL;
        for (unsigned i = 0; i < XI_MAX_PENDING; i++) {
            if (xi.pending[i].in_use && xi.pending[i].fb == pFeedback) pe = &xi.pending[i];
            else if (!xi.pending[i].in_use && !free) free = &xi.pending[i];
        }
        if (!pe && free) {
            pe = free;
            pe->in_use = true;
            pe->fb = pFeedback;
            pe->handle = d;
            KeInitializeTimerEx(&pe->timer, 0);
            KeInitializeDpc(&pe->dpc, feedback_dpc, pe);
            KeSetTimerEx(&pe->timer, (ULONG)-(XI_FEEDBACK_US * 10), -1, 0, &pe->dpc);
        }
        if (pe) {
            err = pFeedback->dwStatus = ERROR_IO_PENDING;
        } else {
            /* No slot left: complete it on the spot. */
            pFeedback->dwStatus = ERROR_SUCCESS;
            xi_pending tmp = { .in_use = true, .fb = pFeedback, .handle = d };
            complete_pending(&tmp, ERROR_SUCCESS);
            err = ERROR_IO_PENDING;
        }
    }
    pthread_mutex_unlock(&xi.lock);
    return err;
}

/* ---- memory units: never present ---------------------------------------- */

static bool mu_args_ok(const char *api, ULONG dwPort, ULONG dwSlot)
{
    if (dwPort < XGetPortCount() && dwSlot <= XDEVICE_BOTTOM_SLOT) return true;
    xlog("XInput: %s: bad port %u / slot %u (the debug library would RIP)", api, dwPort, dwSlot);
    return false;
}

static ULONG NTAPI XMountMUA(ULONG dwPort, ULONG dwSlot, CHAR *pchDrive)
{
    check_inited("XMountMU");
    if (pchDrive) *pchDrive = 0;
    mu_args_ok("XMountMU", dwPort, dwSlot);
    TRACE_ALL("XInput: XMountMU(%u, %u): no memory unit", dwPort, dwSlot);
    return ERROR_DEVICE_NOT_CONNECTED;
}

static ULONG NTAPI XMountMURootA(ULONG dwPort, ULONG dwSlot, CHAR *pchDrive)
{
    if (pchDrive) *pchDrive = 0;
    mu_args_ok("XMountMURoot", dwPort, dwSlot);
    TRACE_ALL("XInput: XMountMURoot(%u, %u): no memory unit", dwPort, dwSlot);
    return ERROR_DEVICE_NOT_CONNECTED;
}

static ULONG NTAPI XUnmountMU(ULONG dwPort, ULONG dwSlot)
{
    mu_args_ok("XUnmountMU", dwPort, dwSlot);
    TRACE_ALL("XInput: XUnmountMU(%u, %u): not mounted", dwPort, dwSlot);
    return ERROR_INVALID_DRIVE;
}

static ULONG NTAPI XMUPortFromDriveLetterA(ULONG chDrive)
{
    int d = (CHAR)chDrive;
    if (d < MU_FIRST_DRIVE || d > MU_LAST_DRIVE) return (ULONG)-1;
    return (ULONG)((d - MU_FIRST_DRIVE) / MU_SLOTS);
}

static ULONG NTAPI XMUSlotFromDriveLetterA(ULONG chDrive)
{
    int d = (CHAR)chDrive;
    if (d < MU_FIRST_DRIVE || d > MU_LAST_DRIVE) return (ULONG)-1;
    return (ULONG)((d - MU_FIRST_DRIVE) % MU_SLOTS);
}

static ULONG NTAPI XMUNameFromDriveLetter(ULONG chDrive, WCHAR *lpName, ULONG cchName)
{
    (void)lpName; (void)cchName;
    TRACE_ALL("XInput: XMUNameFromDriveLetter(%c): not mounted", (CHAR)chDrive);
    return ERROR_INVALID_DRIVE;
}

static ULONG NTAPI XReadMUMetaData(ULONG dwPort, ULONG dwSlot, PVOID lpBuffer, ULONG dwByteOffset,
                                   ULONG dwNumberOfBytesToRead)
{
    (void)lpBuffer; (void)dwByteOffset; (void)dwNumberOfBytesToRead;
    check_inited("XReadMUMetaData");
    mu_args_ok("XReadMUMetaData", dwPort, dwSlot);
    return ERROR_DEVICE_NOT_CONNECTED;
}

/* The retail XInitDevices is a bare jmp to USBD_Init (too short for a
   signature), so the USB core's entry is replaced with the same function. */
#define F(name, impl) { name, (void *)impl }
const struct hle_func xinput_funcs[] = {
    F("_XInitDevices@8", XInitDevices),
    F("_USBD_Init@8", XInitDevices),
    F("_XGetDevices@4", XGetDevices),
    F("_XGetDeviceChanges@12", XGetDeviceChanges),
    F("_XPeekDevices@12", XPeekDevices),
    F("_XInputOpen@16", XInputOpen),
    F("_XInputClose@4", XInputClose),
    F("_XInputGetCapabilities@8", XInputGetCapabilities),
    F("_XInputGetState@8", XInputGetState),
    F("_XInputSetState@8", XInputSetState),
    F("_XInputPoll@4", XInputPoll),
    F("_XMountMUA@12", XMountMUA),
    F("_XMountMURootA@12", XMountMURootA),
    F("_XUnmountMU@8", XUnmountMU),
    F("_XMUPortFromDriveLetterA@4", XMUPortFromDriveLetterA),
    F("_XMUSlotFromDriveLetterA@4", XMUSlotFromDriveLetterA),
    F("_XMUNameFromDriveLetter@12", XMUNameFromDriveLetter),
    F("_XReadMUMetaData@20", XReadMUMetaData),
    { NULL, NULL },
};
#undef F
