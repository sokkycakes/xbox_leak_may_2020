/*
 * xinput_test: the XInput HLE (src/hle/xinput.c) against an SDL virtual game
 * controller, without a title, a window or real hardware.
 *
 * The test includes xinput.c directly so it can call the static entry points
 * and stub what they need from the rest of xbcompat (logging, the pool, the
 * HLE map and the kernel's events/timers).  The guest side is faked in host
 * memory: an XID type-description table pointing at XPP_DEVICE_TYPE tables,
 * the XInitDevices flag and a stdcall SetLastError.  Timers do not fire by
 * themselves; run_timers() plays the DPC thread.
 *
 * Build (32-bit, like xbcompat itself; needs libsdl2-dev:i386):
 *   gcc -m32 -no-pie -fno-pie -std=gnu11 -Wall -Isrc \
 *       $(PKG_CONFIG_PATH=/usr/lib/i386-linux-gnu/pkgconfig pkg-config --cflags sdl2) \
 *       -o xinput_test tests/xinput_test.c \
 *       $(PKG_CONFIG_PATH=/usr/lib/i386-linux-gnu/pkgconfig pkg-config --libs sdl2) -lpthread
 * Run:  SDL_VIDEODRIVER=dummy ./xinput_test
 */
#include <stdarg.h>
#include <stddef.h>
#include <unistd.h>

#include "../src/hle/xinput.c"

/* ---- stubs for the rest of xbcompat ------------------------------------ */

int g_trace = 0;
FILE *g_log;

void xlog(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("  log: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void install_fault_handlers(void) {}
ULONG d3d_frame_count(void) { return 0; }
static bool reset_on = true;
static int resets;
bool reset_enabled(void) { return reset_on; }
void reset_request(const char *why) { (void)why; resets++; }

void fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(2);
}

void *pool_alloc(size_t size) { return calloc(1, size); }
void pool_free(void *p) { free(p); }

/* The fake guest image: typeinfo.cpp's tables. */
static XPP_DEVICE_TYPE fake_gamepad, fake_keyboard, fake_ir, fake_mu;
static UCHAR fake_gamepad_ti[28], fake_keyboard_ti[28], fake_ir_ti[28];
static ULONG fake_xid_table[5];        /* Begin, 3 entries, End */
static ULONG fake_init_flag;
static ULONG last_error;

static void NTAPI fake_set_last_error(ULONG code) { last_error = code; }

/* Only SetLastError crosses into the fake guest in this HLE test. */
#ifdef XBC_TRANSLATED
uint64_t cpu_call(uint32_t fn, int conv, int n, const uint32_t *args)
{
    if (fn != (uint32_t)(uintptr_t)fake_set_last_error || conv != CONV_STD || n != 1)
        fatal("unexpected guest callback in XInput test");
    fake_set_last_error(args[0]);
    return 0;
}
#endif

ULONG hle_lookup(const char *name)
{
    if (!strcmp(name, "_XPP_XInitDevicesHasBeenCalled")) return (ULONG)&fake_init_flag;
    if (!strcmp(name, "_SetLastError@4")) return (ULONG)fake_set_last_error;
    if (!strcmp(name, "_XDEVICE_TYPE_MEMORY_UNIT_TABLE")) return (ULONG)&fake_mu;
    return 0;
}

ULONG hle_lookup_prefix(const char *prefix)
{
    if (!strcmp(prefix, "?XID_BeginTypeDescriptionTable@@")) return (ULONG)&fake_xid_table[0];
    if (!strcmp(prefix, "?XID_EndTypeDescriptionTable@@")) return (ULONG)&fake_xid_table[4];
    return 0;
}

static void make_type_info(UCHAR *ti, UCHAR type, UCHAR handles, XPP_DEVICE_TYPE *table)
{
    memset(ti, 0, 28);
    ti[XID_TI_TYPE] = type;
    ti[XID_TI_HANDLES] = handles;
    memcpy(ti + XID_TI_XPPTYPE, &table, 4);
}

/* Kernel: one fake event, timers fired by hand. */
ULONG ExEventObjectType[8];
#define FAKE_EVENT ((HANDLE)0x4444)
static KEVENT fake_event;
static int signals;
static KDPC *armed[XI_MAX_PENDING];
static int narmed;

NTSTATUS NTAPI ObReferenceObjectByHandle(HANDLE Handle, void *ObjectType, PVOID *Object)
{
    if (Handle != FAKE_EVENT || ObjectType != ExEventObjectType) return STATUS_INVALID_HANDLE;
    *Object = &fake_event;
    return STATUS_SUCCESS;
}

LONG NTAPI KeSetEvent(KEVENT *Event, LONG Increment, BOOLEAN Wait)
{
    if (Event == &fake_event) signals++;
    return 0;
}

void NTAPI KeInitializeTimerEx(KTIMER *Timer, ULONG Type) { memset(Timer, 0, sizeof(*Timer)); }
void NTAPI KeInitializeDpc(KDPC *Dpc, PVOID DeferredRoutine, PVOID DeferredContext)
{
    memset(Dpc, 0, sizeof(*Dpc));
    Dpc->DeferredRoutine = DeferredRoutine;
    Dpc->DeferredContext = DeferredContext;
}

BOOLEAN NTAPI KeSetTimerEx(KTIMER *Timer, ULONG DueLow, LONG DueHigh, LONG Period, KDPC *Dpc)
{
    Timer->Dpc = Dpc;
    armed[narmed++] = Dpc;
    return 0;
}

BOOLEAN NTAPI KeCancelTimer(KTIMER *Timer)
{
    for (int i = 0; i < narmed; i++)
        if (armed[i] == Timer->Dpc) { armed[i] = armed[--narmed]; return 1; }
    return 0;
}

BOOLEAN NTAPI KeRemoveQueueDpc(KDPC *Dpc) { return 0; }

static void run_timers(void)
{
    while (narmed) {
        KDPC *d = armed[--narmed];
        ((void (NTAPI *)(KDPC *, PVOID, PVOID, PVOID))d->DeferredRoutine)(d, d->DeferredContext, NULL, NULL);
    }
}

/* ---- helpers ------------------------------------------------------------ */

static int failures, checks;
#define CHECK(cond) do { checks++; if (!(cond)) { failures++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void reset_hle(void)
{
    memset(&xi, 0, sizeof(xi));
    pthread_mutex_init(&xi.lock, NULL);
    xi.default_handles = 4;
    xi.virtual_index = -1;
    memset(&fake_gamepad, 0, sizeof(fake_gamepad));
    memset(&fake_mu, 0, sizeof(fake_mu));
    fake_init_flag = 0;
    last_error = 0;
}

static void force_sync(void)
{
    pthread_mutex_lock(&xi.lock);
    sync_devices(true);
    pthread_mutex_unlock(&xi.lock);
}

/* Push the virtual joystick's values into SDL and get past the 8 ms report
   gate by exactly one interval (so the packet number advances by one). */
static void settle(SDL_Joystick *js, xi_handle *h)
{
    (void)js;
    SDL_GameControllerUpdate();
    if (h) h->sampled_us = now_us() - 8000;
}

/* Exercise individual press/release edges through SDL and the public HLE
   report. Literal byte offsets are the guest wire layout, independent of the
   implementation's button constants: packet[4], digital[2], A B X Y Black White.
   Run with both automatic and explicit polling, as titles use both modes. */
static void check_face_buttons(SDL_Joystick *js, xi_handle *h, const int raw[6])
{
    XINPUT_POLLING_PARAMETERS saved = h->pp;
    for (int manual = 0; manual < 2; manual++) {
        h->pp.flags = manual ? 0 : XINPUT_POLL_AUTO;
        for (int button = 0; button < 6; button++) {
            for (int pressed = 1; pressed >= 0; pressed--) {
                CHECK(SDL_JoystickSetVirtualButton(js, raw[button], pressed) == 0);
                settle(js, h);
                if (manual) CHECK(XInputPoll(h) == ERROR_SUCCESS);
                struct { unsigned char before[4]; XINPUT_STATE state; unsigned char after[4]; } out;
                memset(&out, 0xA5, sizeof(out));
                CHECK(XInputGetState(h, &out.state) == ERROR_SUCCESS);
                const unsigned char *wire = (const unsigned char *)&out.state;
                for (int i = 0; i < 6; i++)
                    CHECK(wire[6 + i] == (pressed && i == button ? 255 : 0));
                CHECK(wire[4] == 0 && wire[5] == 0);
                for (int i = 0; i < 4; i++)
                    CHECK(out.before[i] == 0xA5 && out.after[i] == 0xA5);
                if (manual) {
                    ULONG packet = out.state.dwPacketNumber;
                    CHECK(XInputGetState(h, &out.state) == ERROR_SUCCESS);
                    CHECK(out.state.dwPacketNumber == packet);
                }
            }
        }
    }
    h->pp = saved;
}

/* ---- the tests ---------------------------------------------------------- */

static void test_layouts(void)
{
    CHECK(offsetof(XINPUT_STATE, Gamepad) == 4);
    CHECK(offsetof(XINPUT_GAMEPAD, sThumbLX) == 10);
    CHECK(offsetof(XINPUT_FEEDBACK, Rumble) == 66);
    CHECK(offsetof(XINPUT_CAPABILITIES, In) == 3);
    CHECK(offsetof(XINPUT_CAPABILITIES, Out) == 21);
    CHECK(trigger_byte(32767) == 255 && trigger_byte(0) == 0 && trigger_byte(-5) == 0 && trigger_byte(128) == 1);
    CHECK(stick_y(-32768) == 32767 && stick_y(32767) == -32768 && stick_y(0) == -1);
}

static void test_mu(void)
{
    CHECK(XMUPortFromDriveLetterA('F') == 0 && XMUSlotFromDriveLetterA('F') == 0);
    CHECK(XMUPortFromDriveLetterA('G') == 0 && XMUSlotFromDriveLetterA('G') == 1);
    CHECK(XMUPortFromDriveLetterA('L') == 3 && XMUSlotFromDriveLetterA('L') == 0);
    CHECK(XMUPortFromDriveLetterA('M') == 3 && XMUSlotFromDriveLetterA('M') == 1);
    CHECK(XMUPortFromDriveLetterA('E') == (ULONG)-1 && XMUSlotFromDriveLetterA('N') == (ULONG)-1);
    CHECK(XUnmountMU(0, 0) == ERROR_INVALID_DRIVE);
    CHECK(XMUNameFromDriveLetter('F', NULL, 0) == ERROR_INVALID_DRIVE);
    CHAR drive = 'Z';
    CHECK(XMountMUA(0, 0, &drive) == ERROR_DEVICE_NOT_CONNECTED && drive == 0);
    drive = 'Z';
    CHECK(XMountMURootA(1, 1, &drive) == ERROR_DEVICE_NOT_CONNECTED && drive == 0);
    CHECK(XReadMUMetaData(0, 0, NULL, 0, 0) == ERROR_DEVICE_NOT_CONNECTED);
}

static void test_no_devices(void)
{
    reset_hle();
    XInitDevices(0, NULL);
    CHECK(xi.gamepad == &fake_gamepad && xi.keyboard == &fake_keyboard && xi.ir == &fake_ir && xi.mu == &fake_mu);
    CHECK(fake_init_flag == 1);
    CHECK(xi.remaining == 4);
    XInitDevices(0, NULL);                      /* logged, ignored */

    ULONG ins = 7, rem = 7;
    CHECK(XGetDevices(&fake_gamepad) == 0);
    CHECK(XGetDeviceChanges(&fake_gamepad, &ins, &rem) == 0 && ins == 0 && rem == 0);
    CHECK(XGetDevices(&fake_mu) == 0);
    CHECK(XGetDevices(&fake_keyboard) == 0);

    CHECK(XInputOpen(&fake_gamepad, 0, XDEVICE_NO_SLOT, NULL) == NULL && last_error == ERROR_DEVICE_NOT_CONNECTED);
    CHECK(XInputOpen(&fake_keyboard, 0, XDEVICE_NO_SLOT, NULL) == NULL && last_error == ERROR_DEVICE_NOT_CONNECTED);
    CHECK(XInputOpen(&fake_mu, 0, XDEVICE_NO_SLOT, NULL) == NULL && last_error == ERROR_INVALID_PARAMETER);
    /* The retail driver finds no node on a bad port/slot (the debug one RIPs). */
    CHECK(XInputOpen(&fake_gamepad, 4, XDEVICE_NO_SLOT, NULL) == NULL && last_error == ERROR_DEVICE_NOT_CONNECTED);
    CHECK(XInputOpen(&fake_gamepad, 0, XDEVICE_BOTTOM_SLOT, NULL) == NULL && last_error == ERROR_DEVICE_NOT_CONNECTED);

    XINPUT_STATE st;
    memset(&st, 0xAA, sizeof(st));
    CHECK(XInputGetState(NULL, &st) == ERROR_INVALID_PARAMETER && st.dwPacketNumber == 0 && st.Gamepad.wButtons == 0);
    XINPUT_CAPABILITIES caps;
    memset(&caps, 0xAA, sizeof(caps));
    /* On failure only Reserved is written (xidinp.cpp exit_input_get_caps). */
    CHECK(XInputGetCapabilities(NULL, &caps) == ERROR_INVALID_PARAMETER && caps.SubType == 0xAA && caps.Reserved == 0);
}

static void test_controller(void)
{
    reset_hle();
    XInitDevices(0, NULL);
    CHECK(XGetDevices(&fake_gamepad) == 0);

    int index = attach_virtual_pad(NULL);
    CHECK(index >= 0 && SDL_IsGameController(index));
    SDL_Joystick *js = SDL_JoystickOpen(index);
    CHECK(js != NULL);
    SDL_JoystickID id = SDL_JoystickInstanceID(js);

    /* Insertion. */
    ULONG ins = 0, rem = 0;
    force_sync();
    CHECK(fake_gamepad.Current == 1 && fake_gamepad.Change == 1 && fake_gamepad.Previous == 0);
    CHECK(XGetDeviceChanges(&fake_gamepad, &ins, &rem) == 1 && ins == 1 && rem == 0);
    CHECK(fake_gamepad.Change == 0 && fake_gamepad.Previous == 1);
    CHECK(XGetDeviceChanges(&fake_gamepad, &ins, &rem) == 0 && ins == 0 && rem == 0);
    CHECK(XGetDevices(&fake_gamepad) == 1);
    CHECK(xi.port[0].gc != NULL && xi.port[0].instance == id && !xi.port[0].kbd);

    /* Open, sharing, capabilities. */
    HANDLE h = XInputOpen(&fake_gamepad, 0, XDEVICE_NO_SLOT, NULL);
    CHECK(h != NULL);
    CHECK(xi.remaining == 3);
    CHECK(XInputOpen(&fake_gamepad, 0, XDEVICE_NO_SLOT, NULL) == NULL && last_error == ERROR_SHARING_VIOLATION);
    CHECK(XInputOpen(&fake_gamepad, 1, XDEVICE_NO_SLOT, NULL) == NULL && last_error == ERROR_DEVICE_NOT_CONNECTED);
    XINPUT_CAPABILITIES caps;
    memset(&caps, 0xAA, sizeof(caps));
    CHECK(XInputGetCapabilities(h, &caps) == ERROR_SUCCESS);
    CHECK(caps.SubType == XINPUT_DEVSUBTYPE_GC_GAMEPAD_ALT && caps.Reserved == 0);
    CHECK(caps.In.wButtons == 0xFFFF && caps.In.bAnalogButtons[7] == 0xFF && caps.In.sThumbRY == -1);
    CHECK(caps.Out.wLeftMotorSpeed == 0xFFFF && caps.Out.wRightMotorSpeed == 0xFFFF);

    static const int face_buttons[6] = {
        SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B,
        SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y,
        SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, SDL_CONTROLLER_BUTTON_LEFTSHOULDER
    };
    check_face_buttons(js, h, face_buttons);
    /* Start the following interval tests from a fresh open report. */
    XInputClose(h);
    h = XInputOpen(&fake_gamepad, 0, XDEVICE_NO_SLOT, NULL);
    CHECK(h != NULL);

    /* The report. */
    XINPUT_STATE st;
    memset(&st, 0xAA, sizeof(st));
    CHECK(XInputGetState(h, &st) == ERROR_SUCCESS);
    CHECK(st.dwPacketNumber == 0 && st.Gamepad.wButtons == 0 && st.Gamepad.sThumbLX == 0 && st.Gamepad.sThumbLY == -1);
    CHECK(st.Gamepad.bAnalogButtons[XINPUT_GAMEPAD_LEFT_TRIGGER] == 0 && st.Gamepad.bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER] == 0);

    SDL_JoystickSetVirtualButton(js, SDL_CONTROLLER_BUTTON_A, 1);
    SDL_JoystickSetVirtualButton(js, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 1);
    SDL_JoystickSetVirtualButton(js, SDL_CONTROLLER_BUTTON_DPAD_UP, 1);
    SDL_JoystickSetVirtualButton(js, SDL_CONTROLLER_BUTTON_START, 1);
    SDL_JoystickSetVirtualButton(js, SDL_CONTROLLER_BUTTON_LEFTSTICK, 1);
    SDL_JoystickSetVirtualAxis(js, SDL_CONTROLLER_AXIS_TRIGGERLEFT, 32767);
    SDL_JoystickSetVirtualAxis(js, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 16384);   /* SDL: -32768..32767 -> 0..32767 */
    SDL_JoystickSetVirtualAxis(js, SDL_CONTROLLER_AXIS_LEFTX, 1000);
    SDL_JoystickSetVirtualAxis(js, SDL_CONTROLLER_AXIS_LEFTY, -32768);      /* SDL up */
    SDL_JoystickSetVirtualAxis(js, SDL_CONTROLLER_AXIS_RIGHTX, -32768);
    SDL_JoystickSetVirtualAxis(js, SDL_CONTROLLER_AXIS_RIGHTY, 16384);      /* SDL down-ish */
    settle(js, h);
    CHECK(XInputGetState(h, &st) == ERROR_SUCCESS);
    CHECK(st.dwPacketNumber == 1);
    CHECK(st.Gamepad.bAnalogButtons[XINPUT_GAMEPAD_A] == 255);
    CHECK(st.Gamepad.bAnalogButtons[XINPUT_GAMEPAD_B] == 0);
    CHECK(st.Gamepad.bAnalogButtons[XINPUT_GAMEPAD_BLACK] == 255 && st.Gamepad.bAnalogButtons[XINPUT_GAMEPAD_WHITE] == 0);
    CHECK(st.Gamepad.bAnalogButtons[XINPUT_GAMEPAD_LEFT_TRIGGER] == 255);
    CHECK(st.Gamepad.bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER] == 191);   /* 24575 >> 7 */
    CHECK(st.Gamepad.wButtons == (XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_LEFT_THUMB));
    CHECK(st.Gamepad.sThumbLX == 1000 && st.Gamepad.sThumbLY == 32767);
    CHECK(st.Gamepad.sThumbRX == -32768 && st.Gamepad.sThumbRY == -16385);
    /* Faster than the report interval: the same packet again. */
    CHECK(XInputGetState(h, &st) == ERROR_SUCCESS && st.dwPacketNumber == 1);
    /* Slower: one packet per 8 ms transfer that happened in between. */
    ((xi_handle *)h)->sampled_us -= 25000;
    CHECK(XInputGetState(h, &st) == ERROR_SUCCESS && st.dwPacketNumber == 4);
    CHECK(XInputGetState(h, &st) == ERROR_SUCCESS && st.dwPacketNumber == 4);
    CHECK(XInputPoll(h) == ERROR_SUCCESS);

    /* Feedback: pending, then completed by the "DPC thread". */
    XINPUT_FEEDBACK fb;
    memset(&fb, 0, sizeof(fb));
    fb.hEvent = FAKE_EVENT;
    fb.Rumble.wLeftMotorSpeed = 65535;
    fb.Rumble.wRightMotorSpeed = 1;
    CHECK(XInputSetState(h, &fb) == ERROR_IO_PENDING && fb.dwStatus == ERROR_IO_PENDING);
    CHECK(XInputSetState(h, &fb) == ERROR_IO_PENDING && narmed == 1);   /* resubmit: same transfer */
    run_timers();
    CHECK(fb.dwStatus == ERROR_SUCCESS && signals == 1);
    fb.hEvent = NULL;
    CHECK(XInputSetState(h, &fb) == ERROR_IO_PENDING && fb.dwStatus == ERROR_IO_PENDING);
    run_timers();
    CHECK(fb.dwStatus == ERROR_SUCCESS && signals == 1);
    fb.hEvent = (HANDLE)0x1234;                 /* not an event: dropped at submit, completed, not signalled */
    memset(fb.Reserved, 0xAA, sizeof(fb.Reserved));
    CHECK(XInputSetState(h, &fb) == ERROR_IO_PENDING && fb.hEvent == NULL);
    CHECK(fb.Reserved[56] == 0 && fb.Reserved[57] == 6);   /* bReportId, bSize of the output report */
    run_timers();
    CHECK(fb.dwStatus == ERROR_SUCCESS && signals == 1);

    /* Close with a feedback in flight: cancelled and signalled. */
    fb.hEvent = FAKE_EVENT;
    CHECK(XInputSetState(h, &fb) == ERROR_IO_PENDING);
    XInputClose(h);
    CHECK(fb.dwStatus == ERROR_CANCELLED && signals == 2 && narmed == 0);
    CHECK(xi.remaining == 4 && xi.port[0].open == NULL);

    /* Removal with the handle still open: the last report is kept. */
    h = XInputOpen(&fake_gamepad, 0, XDEVICE_NO_SLOT, NULL);
    CHECK(h != NULL);
    SDL_JoystickSetVirtualButton(js, SDL_CONTROLLER_BUTTON_A, 0);
    settle(js, h);
    CHECK(XInputGetState(h, &st) == ERROR_SUCCESS && st.Gamepad.bAnalogButtons[XINPUT_GAMEPAD_A] == 0);
    ULONG packet = st.dwPacketNumber;
    SDL_JoystickClose(js);
    SDL_JoystickDetachVirtual(index);
    force_sync();
    CHECK(fake_gamepad.Current == 0 && fake_gamepad.Change == 1);
    CHECK(XGetDeviceChanges(&fake_gamepad, &ins, &rem) == 1 && ins == 0 && rem == 1);
    memset(&st, 0xAA, sizeof(st));
    CHECK(XInputGetState(h, &st) == ERROR_DEVICE_NOT_CONNECTED);
    CHECK(st.dwPacketNumber == packet && st.Gamepad.sThumbLY == 32767 && st.Gamepad.bAnalogButtons[XINPUT_GAMEPAD_A] == 0);
    caps.SubType = 0x55;
    CHECK(XInputGetCapabilities(h, &caps) == ERROR_DEVICE_NOT_CONNECTED && caps.SubType == 0x55);
    CHECK(XInputPoll(h) == ERROR_DEVICE_NOT_CONNECTED);
    fb.dwStatus = 0;
    CHECK(XInputSetState(h, &fb) == ERROR_DEVICE_NOT_CONNECTED && fb.dwStatus == ERROR_DEVICE_NOT_CONNECTED);
    XInputClose(h);
    CHECK(xi.remaining == 4);

    /* A device inserted into the port later is a new node: it can be opened
       while a stale handle to the old one is still open (xid.cpp). */
    index = attach_virtual_pad(NULL);
    force_sync();
    h = XInputOpen(&fake_gamepad, 0, XDEVICE_NO_SLOT, NULL);
    CHECK(h != NULL);
    SDL_JoystickDetachVirtual(index);
    force_sync();
    index = attach_virtual_pad(NULL);
    force_sync();
    HANDLE h2 = XInputOpen(&fake_gamepad, 0, XDEVICE_NO_SLOT, NULL);
    CHECK(h2 != NULL && h2 != h && xi.remaining == 2);
    CHECK(XInputGetState(h, &st) == ERROR_DEVICE_NOT_CONNECTED);
    CHECK(XInputGetState(h2, &st) == ERROR_SUCCESS);
    XInputClose(h);                             /* must not detach the new handle */
    CHECK(xi.port[0].open == (xi_handle *)h2 && xi.remaining == 3);
    CHECK(XInputGetState(h2, &st) == ERROR_SUCCESS);
    XInputClose(h2);
    CHECK(xi.remaining == 4 && xi.port[0].open == NULL);
    SDL_JoystickDetachVirtual(index);
    force_sync();
    CHECK(XGetDevices(&fake_gamepad) == 0);

    /* Remove and reinsert between two calls: both bits reported (xpp.c). */
    index = attach_virtual_pad(NULL);
    force_sync();
    CHECK(XGetDevices(&fake_gamepad) == 1);
    SDL_JoystickDetachVirtual(index);
    force_sync();
    index = attach_virtual_pad(NULL);
    force_sync();
    CHECK(fake_gamepad.Current == 1 && fake_gamepad.Previous == 1 && fake_gamepad.Change == 1);
    ULONG last = 0, stale = 0;
    CHECK(XPeekDevices(&fake_gamepad, &last, &stale) == 1 && last == 1 && stale == 1);
    CHECK(XGetDeviceChanges(&fake_gamepad, &ins, &rem) == 1 && ins == 1 && rem == 1);
    CHECK(XPeekDevices(&fake_gamepad, &last, &stale) == 1 && last == 1 && stale == 0);
    SDL_JoystickDetachVirtual(index);
    force_sync();
    CHECK(XGetDevices(&fake_gamepad) == 0);
}

/* L + R + Back + Start: one reset when the last button goes down, none
   for a combo already held, none while resets are off (the dashboard). */
static void test_reset_combo(void)
{
    reset_hle();
    XInitDevices(0, NULL);
    int index = attach_virtual_pad(NULL);
    SDL_Joystick *js = SDL_JoystickOpen(index);
    force_sync();
    resets = 0;
    SDL_JoystickSetVirtualButton(js, SDL_CONTROLLER_BUTTON_BACK, 1);
    SDL_JoystickSetVirtualButton(js, SDL_CONTROLLER_BUTTON_START, 1);
    SDL_JoystickSetVirtualAxis(js, SDL_CONTROLLER_AXIS_TRIGGERLEFT, 32767);
    settle(js, NULL);
    xinput_check_reset_combo();
    CHECK(resets == 0);                         /* three of four */
    SDL_JoystickSetVirtualAxis(js, SDL_CONTROLLER_AXIS_TRIGGERRIGHT, 32767);
    settle(js, NULL);
    xinput_check_reset_combo();
    CHECK(resets == 1);
    xinput_check_reset_combo();
    CHECK(resets == 1);                         /* still held */
    SDL_JoystickSetVirtualButton(js, SDL_CONTROLLER_BUTTON_START, 0);
    settle(js, NULL);
    xinput_check_reset_combo();
    SDL_JoystickSetVirtualButton(js, SDL_CONTROLLER_BUTTON_START, 1);
    settle(js, NULL);
    reset_on = false;
    xinput_check_reset_combo();
    CHECK(resets == 1);
    reset_on = true;
    SDL_JoystickClose(js);
    SDL_JoystickDetachVirtual(index);
}

static void test_prealloc(void)
{
    /* A list without the gamepad type leaves it no handles (XID_Init). */
    reset_hle();
    XDEVICE_PREALLOC_TYPE types[2] = { { &fake_mu, 2 }, { &fake_gamepad, 1 } };
    XInitDevices(1, types);
    CHECK(xi.remaining == 0);
    int index = attach_virtual_pad(NULL);
    force_sync();
    CHECK(XGetDevices(&fake_gamepad) == 1);
    CHECK(XInputOpen(&fake_gamepad, 0, XDEVICE_NO_SLOT, NULL) == NULL && last_error == ERROR_OUTOFMEMORY);
    SDL_JoystickDetachVirtual(index);
    force_sync();

    reset_hle();
    XInitDevices(2, types);
    CHECK(xi.remaining == 1);
}

static void test_virtual_spec(void)
{
    /* XBCOMPAT_VIRTUAL_PAD="...": values in the title's units. */
    reset_hle();
    XInitDevices(0, NULL);
    int index = attach_virtual_pad("a,start,white, lt=255,rt=128,lx=1000,ly=32767,rx=5,ry=-32768,bogus,b=0");
    force_sync();
    HANDLE h = XInputOpen(&fake_gamepad, 0, XDEVICE_NO_SLOT, NULL);
    CHECK(h != NULL);
    XINPUT_STATE st;
    settle(NULL, h);
    CHECK(XInputGetState(h, &st) == ERROR_SUCCESS);
    CHECK(st.Gamepad.bAnalogButtons[XINPUT_GAMEPAD_A] == 255 && st.Gamepad.bAnalogButtons[XINPUT_GAMEPAD_WHITE] == 255);
    CHECK(st.Gamepad.bAnalogButtons[XINPUT_GAMEPAD_B] == 0 && st.Gamepad.bAnalogButtons[XINPUT_GAMEPAD_BLACK] == 0);
    CHECK(st.Gamepad.wButtons == XINPUT_GAMEPAD_START);
    CHECK(st.Gamepad.bAnalogButtons[XINPUT_GAMEPAD_LEFT_TRIGGER] == 255);
    CHECK(st.Gamepad.bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER] == 128);
    CHECK(st.Gamepad.sThumbLX == 1000 && st.Gamepad.sThumbLY == 32767);
    CHECK(st.Gamepad.sThumbRX == 5 && st.Gamepad.sThumbRY == -32768);

    /* Every trigger byte is reachable exactly through SDL's trigger scaling. */
    SDL_Joystick *js = SDL_JoystickFromInstanceID(xi.port[0].instance);
    int wrong = 0;
    for (int v = 0; v <= 255 && js; v++) {
        SDL_JoystickSetVirtualAxis(js, SDL_CONTROLLER_AXIS_TRIGGERLEFT, virtual_trigger_raw(v));
        SDL_GameControllerUpdate();
        if (trigger_byte(SDL_GameControllerGetAxis(xi.port[0].gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT)) != v) wrong++;
    }
    CHECK(js != NULL && wrong == 0);
    XInputClose(h);
    SDL_JoystickDetachVirtual(index);
    force_sync();
}

/* An original Xbox pad laid out the way xpad reports it (10 buttons in key
   code order with Black on b2 and White on b5, X Y Z RX RY RZ, one hat),
   through the mapping xbcompat gives such pads. */
static void test_original_pad(void)
{
    static const struct { int flags, buttons, axes, hats; } layouts[] = {
        { 0, 10, 6, 1 },                                        /* Duke, Controller S */
        { OG_DPAD_BUTTONS, 14, 6, 0 },                          /* dance pads */
        { OG_DPAD_BUTTONS | OG_TRIGGER_BUTTONS | OG_NO_STICKS, 16, 0, 0 },
    };
    for (unsigned l = 0; l < sizeof(layouts) / sizeof(layouts[0]); l++) {
        reset_hle();
        XInitDevices(0, NULL);
        SDL_VirtualJoystickDesc desc;
        SDL_zero(desc);
        desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
        desc.type = SDL_JOYSTICK_TYPE_UNKNOWN;     /* no automatic virtual mapping */
        desc.naxes = layouts[l].axes;
        desc.nbuttons = layouts[l].buttons;
        desc.nhats = layouts[l].hats;
        desc.vendor_id = 0x045e;
        desc.product_id = 0x0287;
        desc.name = "Microsoft Xbox Controller S";
        int index = SDL_JoystickAttachVirtualEx(&desc);
        CHECK(index >= 0);
        SDL_JoystickGUID guid = SDL_JoystickGetDeviceGUID(index);
        guid.data[2] = guid.data[3] = 0;
        char g[33], m[512];
        SDL_JoystickGetGUIDString(guid, g, sizeof(g));
        original_pad_mapping(g, layouts[l].flags, m, sizeof(m));
        CHECK(SDL_GameControllerAddMapping(m) >= 0);
        CHECK(SDL_IsGameController(index));
        char *used = SDL_GameControllerMappingForDeviceIndex(index);
        CHECK(used && strstr(used, "Xbox Controller (original)"));
        SDL_free(used);
        force_sync();
        HANDLE h = XInputOpen(&fake_gamepad, 0, XDEVICE_NO_SLOT, NULL);
        CHECK(h != NULL);
        SDL_Joystick *js = SDL_JoystickFromInstanceID(xi.port[0].instance);
        CHECK(js != NULL);
        if (!js) continue;

        static const int raw_face_buttons[6] = { 0, 1, 3, 4, 2, 5 };
        check_face_buttons(js, h, raw_face_buttons);

        bool dpad_buttons = layouts[l].flags & OG_DPAD_BUTTONS;
        bool trig_buttons = layouts[l].flags & OG_TRIGGER_BUTTONS;
        int back = trig_buttons ? 8 : 6;
        SDL_JoystickSetVirtualButton(js, 2, 1);                 /* BTN_C: Black */
        SDL_JoystickSetVirtualButton(js, 5, 1);                 /* BTN_Z: White */
        SDL_JoystickSetVirtualButton(js, 0, 1);                 /* BTN_A */
        SDL_JoystickSetVirtualButton(js, back + 1, 1);          /* BTN_START */
        SDL_JoystickSetVirtualButton(js, back + 3, 1);          /* BTN_THUMBR */
        if (dpad_buttons) SDL_JoystickSetVirtualButton(js, back + 4, 1);   /* BTN_DPAD_UP */
        else SDL_JoystickSetVirtualHat(js, 0, SDL_HAT_UP);
        if (trig_buttons) SDL_JoystickSetVirtualButton(js, 6, 1);          /* BTN_TL2 */
        else {
            SDL_JoystickSetVirtualAxis(js, 2, 32767);           /* ABS_Z */
            SDL_JoystickSetVirtualAxis(js, 5, SDL_JOYSTICK_AXIS_MIN);   /* ABS_RZ at rest is 0 */
        }
        if (!(layouts[l].flags & OG_NO_STICKS)) SDL_JoystickSetVirtualAxis(js, 3, 20000);  /* ABS_RX */
        XINPUT_STATE st;
        settle(js, h);
        CHECK(XInputGetState(h, &st) == ERROR_SUCCESS);
        CHECK(st.Gamepad.bAnalogButtons[XINPUT_GAMEPAD_BLACK] == 255);
        CHECK(st.Gamepad.bAnalogButtons[XINPUT_GAMEPAD_WHITE] == 255);
        CHECK(st.Gamepad.bAnalogButtons[XINPUT_GAMEPAD_A] == 255);
        CHECK(st.Gamepad.bAnalogButtons[XINPUT_GAMEPAD_B] == 0 && st.Gamepad.bAnalogButtons[XINPUT_GAMEPAD_X] == 0);
        CHECK(st.Gamepad.bAnalogButtons[XINPUT_GAMEPAD_LEFT_TRIGGER] == 255);
        CHECK(st.Gamepad.bAnalogButtons[XINPUT_GAMEPAD_RIGHT_TRIGGER] == 0);
        CHECK(st.Gamepad.wButtons == (XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_RIGHT_THUMB | XINPUT_GAMEPAD_DPAD_UP));
        CHECK(st.Gamepad.sThumbRX == ((layouts[l].flags & OG_NO_STICKS) ? 0 : 20000));
        XInputClose(h);
        SDL_JoystickDetachVirtual(index);
        force_sync();
    }
}

/* A pad plugged in at boot is enumerated after XInitDevices: the title's
   first XGetDevices sees nothing, then the pad arrives as an insertion. */
static void test_boot_enumeration(void)
{
    reset_hle();
    setenv("XBCOMPAT_ENUM_MS", "60", 1);
    int index = attach_virtual_pad(NULL);
    XInitDevices(0, NULL);
    ULONG ins = 0, rem = 0;
    CHECK(XGetDevices(&fake_gamepad) == 0);
    CHECK(XGetDeviceChanges(&fake_gamepad, &ins, &rem) == 0);
    SDL_Delay(80);
    force_sync();
    CHECK(XGetDeviceChanges(&fake_gamepad, &ins, &rem) == 1 && ins == 1 && rem == 0);
    CHECK(XGetDevices(&fake_gamepad) == 1);
    setenv("XBCOMPAT_ENUM_MS", "0", 1);
    SDL_JoystickDetachVirtual(index);
    force_sync();
}

int main(void)
{
    make_type_info(fake_gamepad_ti, XID_TYPE_GAMEPAD, 4, &fake_gamepad);
    make_type_info(fake_keyboard_ti, XID_TYPE_KEYBOARD, 1, &fake_keyboard);
    make_type_info(fake_ir_ti, XID_TYPE_IR_REMOTE, 1, &fake_ir);
    fake_xid_table[1] = (ULONG)fake_gamepad_ti;
    fake_xid_table[2] = (ULONG)fake_ir_ti;
    fake_xid_table[3] = (ULONG)fake_keyboard_ti;
    g_log = stderr;
    setenv("XBCOMPAT_NO_KBD_PAD", "1", 1);
    setenv("XBCOMPAT_ENUM_MS", "0", 1);         /* devices at once, except in test_boot_enumeration */

    test_layouts();
    test_mu();
    test_no_devices();
    test_controller();
    test_reset_combo();
    test_prealloc();
    test_virtual_spec();
    test_original_pad();
    test_boot_enumeration();

    printf("xinput_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
