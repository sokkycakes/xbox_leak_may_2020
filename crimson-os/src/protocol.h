/* Crimson partition protocol.
 *
 * One stream per partition, agent -> host. Carried over TCP (process mode,
 * or VMs on an internal switch) or AF_HYPERV sockets (VMBus, the same channel
 * family the Xbox One host uses to talk to its Exclusive and Shared partitions).
 */
#ifndef CRIMSON_PROTOCOL_H
#define CRIMSON_PROTOCOL_H
#include <stdint.h>

#define CR_MAGIC        0x534D5243u /* "CRMS" */
#define CR_VERSION      1
#define CR_TCP_PORT     7700

/* Hyper-V socket service id. Registered on the host by
 * hyperv/New-CrimsonPartitions.ps1 under GuestCommunicationServices. */
#define CR_HV_SERVICE_STR "c71b0e5a-3f2d-4e8b-9a61-7c0de0c41a57"

enum cr_type {
    CR_HELLO  = 1, /* agent -> host  cr_hello                      */
    CR_FRAME  = 2, /* agent -> host  cr_frame + rows*width*4 BGRA  */
    CR_INPUT  = 3, /* host  -> agent cr_input                      */
    CR_LAYOUT = 4, /* host  -> agent cr_layout                     */
    CR_PING   = 5
};

enum cr_role { CR_ROLE_GAME = 1, CR_ROLE_SYSTEM = 2 };

/* Where a partition's output sits. Names follow the Xbox One
 * Windows.Xbox.ApplicationModel ApplicationLocation values found in the
 * SystemOS dump (Full / Fill / Snap / StartView / XboxUI). */
enum cr_location {
    CR_LOC_HIDDEN     = 0, /* not shown (System OS with nothing open)    */
    CR_LOC_FULL       = 1, /* whole screen                               */
    CR_LOC_FILL       = 2, /* game beside a snapped app                  */
    CR_LOC_SNAP       = 3, /* side strip                                 */
    CR_LOC_OVERLAY    = 4, /* System OS keyed over the game (Guide)      */
    CR_LOC_BACKGROUND = 5  /* game constrained while Home is up          */
};

#pragma pack(push, 1)
typedef struct { uint32_t magic; uint16_t type; uint16_t version; uint32_t len; } cr_hdr;
typedef struct { uint32_t role; uint32_t width, height; char name[32]; } cr_hello;
typedef struct { uint32_t width, height, y0, rows, seq; } cr_frame;
typedef struct {
    uint16_t buttons;          /* XINPUT_GAMEPAD_* bits (Guide never forwarded) */
    uint8_t  lt, rt;
    int16_t  lx, ly, rx, ry;
    uint32_t seq;
} cr_input;
typedef struct { uint32_t location, focused, width, height; } cr_layout;
#pragma pack(pop)

/* Pixels of this colour in a System OS frame are transparent when it is
 * composited as an overlay. Give the System OS desktop a solid magenta
 * wallpaper and everything the shell does not draw shows the game. */
#define CR_KEY_R 255
#define CR_KEY_G 0
#define CR_KEY_B 255

#endif
