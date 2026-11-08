# trigger6: Linux driver for the j5create JUA365 (MCT Trigger 6)

A DRM/KMS driver for USB 3.0 display adapters built on the Magic Control
Technology **Trigger 6** chip (T6-688L / T6-688SL, USB `0711:5600` and
`0711:5601`): the j5create JUA365 / JCA365 dual HDMI adapter, and very likely
the JUA214, JUA254, JUA311, JCD543 and StarTech USB32HD2 / USB32HDES rev 2.
There was no Linux driver for this chip before.

Each HDMI port is its own connector and CRTC on a new `/dev/dri/cardN`, with
EDID, hotplug (interrupt driven), the adapter's own mode table, and custom
progressive modes from about 25 to 300 MHz (port 1) / 200 MHz (port 2).

## How it works

Built from Forest Crossman's public reverse-engineering notes and USB
captures (https://github.com/cyrozap/mct-usb-display-adapter-re, 0BSD),
decoded further here from the JUA365 capture:

| Thing | Vendor request / data |
|---|---|
| Mode set | `0x40 0x12`, wValue = port, 32 bytes: clock, refresh, h/v timings, PLL, sync polarity |
| PLL | VCO = 40 MHz × (N + frac/1000), pixel clock = VCO / {32, 8, 16, 4}[index]; the chip picks the largest divider with VCO < 1600 MHz. Checked against all 36 entries of the chip's own table. |
| Port on / off | `0x40 0x03`, wValue = port, wIndex = 1 / 0 |
| Gamma | `0x40 0x1c` begin / commit, `0x40 0x1d` × 3 ramps |
| EDID | `0xc0 0x80`, wValue = byte offset, wIndex = port |
| Connected? | `0xc0 0x87`, wValue = port |
| Frames | bulk OUT: 32-byte "select" packet + up to 100 KB of data, repeated. Frame header (48 bytes) lands in video RAM just below the picture. |
| Frame done / hotplug | interrupt IN, 64 bytes: type 4, byte 19 bit 2 = frame done, bits 0/1 = port 0/1 changed |

The Windows driver mostly sends JPEG files, but the captures also contain
raw **NV12** frames (encoding 6), which decode to the right picture. This
driver sends raw NV12 (full-range BT.601), so there's no JPEG encoder in
the kernel. A 24-bit RGB format (9) also shows up in a capture; it can be
tried with `rgb888=1` but is unverified.

Tested on a real JUA365 (firmware image 24010201) on the Sion. That adapter
sends nothing on its interrupt endpoint, so after three missed frame-done
reports the driver stops waiting for them (`ack_timeout_ms`, `slots`).

`tools/t6-mirror.c` shows the main screen on the adapter (720x480p by
default). It reads the main card's scanout buffer through EGL dma-buf import,
or on the CPU for linear and Intel X/Y tiled buffers. `S99t6mirror` runs it
whenever an adapter is plugged in.

HDMI audio is an ALSA card ("Trigger6", S16_LE stereo 48 kHz). From the
capture: request 0x24 (16 bytes, first byte 1/0) turns the audio stream on
and off; PCM goes to session 3 as {3, len, 0, len, 0, 0, 0, 0} + raw data,
50 ms up front and then 10 ms every 10 ms, with no feedback from the adapter.

Unknown / not done: interlaced modes (no flag found yet), the hardware cursor,
partial-frame updates.

## Build

    make KDIR=/path/to/linux-6.18.7     # configured + built (needs Module.symvers)

## Test without the hardware

`test/` has an emulated JUA365 for raw-gadget + dummy_hcd that answers with
the real adapter's replies from the capture, checks every frame packet and
writes the frames to a disk image (`test/nv12png.py` turns them into PNGs).
`test/qemu-init.sh` is the initramfs `/init` used to run it in QEMU: probe,
EDID, fbdev at boot, hotplug of port 2 over the interrupt endpoint, a custom
2880x480p mode, the preferred 1080p mode, unplug. The 1080p mode request the
driver sends is byte-for-byte what the Windows driver sends.

## Sion

Kit: `bootani/sion/xbox/push-trigger6` in the project files
(`trigger6.ko` built against vanilla 6.18.7 + the Sion fragments,
`t6-pattern`, `sion-install.sh`). `TEST=1 sh sion-install.sh` installs,
loads, and draws colour bars on each connected port.
