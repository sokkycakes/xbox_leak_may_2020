#!/usr/bin/env python3
"""The EDID the Sion's display connector is given for 15 kHz NTSC output.

A VGA-to-composite transcoder needs the signal of an NTSC TV: 480 lines
interlaced, 15.734 kHz lines, 59.94 fields a second. BT.601 draws that as
720 pixels on an 858-pixel line at 13.5 MHz, but i915 won't send HDMI
below 25 MHz (intel_hdmi.c, MODE_CLOCK_LOW), so the line is drawn four
times as finely instead: 2880 pixels on a 3432-pixel line at 54 MHz, the
same line length and so the same sync. On VGA, which has no pixels, the
picture is the same; everything drawn 720 (or 640) wide is stretched over
2880. i915 does interlace on HDMI and DP up to display version 11 (the
Sion's Kaby Lake is 9).

The adapter between the Chromebox and the transcoder has no EDID for this
(or none at all), so this one is put on the connector through DRM's
edid_override (see sion-video):

  block 0  one detailed timing, 2880x480i at 59.94, negative syncs, and
           the monitor name and range limits
  block 1  CEA-861: 720x480p (VIC 2, the HDMI-to-AV box's mode, for
           going back), stereo LPCM and an HDMI vendor block, so i915
           keeps sending sound over HDMI (HDMI-to-VGA adapters with a
           3.5 mm jack take it from there)

The front porch is 80 pixels, not CEA's 76 (VIC 10 is the same mode), so
DRM doesn't take the mode for VIC 10: CEA modes get limited-range RGB
(16-235) over HDMI, which a VGA DAC turns into grey blacks.

  mkedid.py OUT     write the 256-byte EDID
"""
import struct
import sys

CLOCK_KHZ = 54000
H_ACTIVE, H_FP, H_SYNC, H_TOTAL = 2880, 80, 248, 3432
# per field: 240 lines, 262 in all (525 a frame with the half line)
V_ACTIVE, V_FP, V_SYNC, V_TOTAL = 240, 4, 3, 262


def mfg_id(s):
    a, b, c = (ord(ch) - 64 for ch in s)
    return struct.pack(">H", a << 10 | b << 5 | c)


def dtd(clock_khz, ha, hfp, hs, ht, va, vfp, vs, vt, interlaced, w_mm, h_mm):
    hb, vb = ht - ha, vt - va
    flags = 0x18  # digital separate sync, both syncs negative
    if interlaced:
        flags |= 0x80
    return bytes([
        (clock_khz // 10) & 0xFF, (clock_khz // 10) >> 8,
        ha & 0xFF, hb & 0xFF, (ha >> 8) << 4 | hb >> 8,
        va & 0xFF, vb & 0xFF, (va >> 8) << 4 | vb >> 8,
        hfp & 0xFF, hs & 0xFF, (vfp & 0xF) << 4 | (vs & 0xF),
        (hfp >> 8) << 6 | (hs >> 8) << 4 | (vfp >> 4) << 2 | vs >> 4,
        w_mm & 0xFF, h_mm & 0xFF, (w_mm >> 8) << 4 | h_mm >> 8,
        0, 0, flags])


def text(tag, s):
    b = s.encode()[:13]
    if len(b) < 13:
        b += b"\n" + b" " * (12 - len(b))
    return bytes([0, 0, 0, tag, 0]) + b


def checksum(block):
    return block + bytes([(-sum(block)) & 0xFF])


def base_block():
    b = b"\x00\xff\xff\xff\xff\xff\xff\x00"
    b += mfg_id("CRM") + struct.pack("<HI", 0x0480, 0)
    b += bytes([43, 2026 - 1990, 1, 3])          # week, year, EDID 1.3
    b += bytes([0x80, 40, 30, 120, 0x0A])         # digital; 40x30 cm (4:3); gamma 2.2; RGB, DTD 1 preferred
    b += bytes([0xEE, 0x91, 0xA3, 0x54, 0x4C, 0x99, 0x26, 0x0F, 0x50, 0x54])  # sRGB
    b += bytes([0, 0, 0])                          # no established timings
    b += b"\x01\x01" * 8                           # no standard timings
    b += dtd(CLOCK_KHZ, H_ACTIVE, H_FP, H_SYNC, H_TOTAL, V_ACTIVE, V_FP, V_SYNC, V_TOTAL, True, 400, 300)
    b += text(0xFC, "CRIMSON 15K")
    # range limits: 50-61 Hz, 15-32 kHz, 60 MHz, no secondary timing formula
    b += bytes([0, 0, 0, 0xFD, 0, 50, 61, 15, 32, 6, 0, 0x0A]) + b" " * 6
    b += bytes([0, 0, 0, 0x10, 0]) + bytes(13)    # unused
    b += bytes([1])                                # one extension
    assert len(b) == 127
    return checksum(b)


def cea_block():
    blocks = b""
    blocks += bytes([2 << 5 | 1, 2])                        # video: VIC 2, 720x480p 59.94 4:3
    blocks += bytes([1 << 5 | 3, 0x09, 0x07, 0x07])         # audio: LPCM 2 ch, 32/44.1/48 kHz, 16/20/24 bit
    blocks += bytes([4 << 5 | 3, 0x01, 0, 0])               # speakers: front left/right
    blocks += bytes([3 << 5 | 5, 0x03, 0x0C, 0x00, 0x10, 0x00])  # HDMI (OUI 00-0C-03), address 1.0.0.0
    b = bytes([0x02, 0x03, 4 + len(blocks), 0x40]) + blocks  # basic audio, no native DTDs
    b += bytes(127 - len(b))
    return checksum(b)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    edid = base_block() + cea_block()
    assert len(edid) == 256
    with open(sys.argv[1], "wb") as f:
        f.write(edid)


if __name__ == "__main__":
    main()
