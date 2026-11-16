#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Pull the AMD firmware pieces coreboot needs out of a stock BC-250 BIOS.

The BC-250's PSP (AMD security processor) trains the GDDR6 and starts the
x86 cores using blobs that only exist in ASRock's BIOS. This script copies
them out verbatim so amdfwtool can repack them around a coreboot image:

  - every $PSP directory entry (bootloader, SMU, ABL0-4, UMC, keys, ...)
  - the APCB (memory/board config) from the BIOS directory
  - the "IMC" and "xHCI" blobs the EFS points at
  - the RBN GPU VBIOS and AMD GOP driver from the UEFI volume (graphics)

Usage: extract_stock_blobs.py STOCK.ROM OUTDIR
"""
import hashlib
import lzma
import os
import re
import struct
import sys

ROM_SIZE = 16 * 1024 * 1024
EFS_MAGIC = 0x55AA55AA

# PSP entry type -> fw.cfg file name (see fw.cfg in this folder).
PSP_NAMES = {
    0x01: "PspBootLoader.sbin",
    0x02: "PspSecureOs.sbin",
    0x08: "SmuFirmware.csbin",
    0x09: "SecureDebugPublicKey.bin",
    0x12: "SmuFirmware2.csbin",
    0x13: "SecureDebugUnlock.sbin",
    0x20: "HwIpConfig.sbin",
    0x24: "SecGasket.sbin",
    0x28: "PspDrivers.sbin",
    0x30: "AgesaBootloader0.csbin",
    0x31: "AgesaBootloader1.csbin",
    0x32: "AgesaBootloader2.csbin",
    0x33: "AgesaBootloader3.csbin",
    0x34: "AgesaBootloader4.csbin",
    0x44: "UsbPhy.sbin",
    0x45: "TosSecurityPolicy.sbin",
    0x4F: "UmcFirmware.csbin",
    0x50: "KeyDbBootloader.bin",
    0x51: "KeyDbTos.bin",
}

VBIOS_GUID = bytes.fromhex("e07f32a0da1f5b4e905db510c45a61d0")  # a0327fe0-1fda-4e5b-905d-b510c45a61d0


def flash_off(addr):
    """Map a 32-bit MMIO address (0xFFxxxxxx) or flash offset to a file offset."""
    return addr & (ROM_SIZE - 1)


def find_efs(rom):
    for off in (0x20000, 0x820000, 0xC20000, 0xE20000, 0xF20000, 0xFA0000):
        if struct.unpack_from("<I", rom, off)[0] == EFS_MAGIC:
            return off
    sys.exit("no EFS (0x55AA55AA) found at any known location")


def write(outdir, name, data, manifest):
    with open(os.path.join(outdir, name), "wb") as f:
        f.write(data)
    manifest.append((name, len(data), hashlib.sha256(data).hexdigest()))


LZMA_SECTION_GUID = bytes.fromhex("98584eee1439594 29d6edc7bd79403cf".replace(" ", ""))  # EE4E5898-3914-4259-9D6E-DC7BD79403CF


def lzma_sections(blob, depth=0):
    """Yield the decompressed payload of every LZMA GUID-defined section,
    recursing into nested ones (AMI nests the DXE FV and some files)."""
    start = 0
    while depth < 4:
        g = blob.find(LZMA_SECTION_GUID, start)
        if g < 0:
            return
        start = g + 1
        hdr = g - 4  # EFI_GUID_DEFINED_SECTION: 4-byte common header, then GUID
        data_offset = struct.unpack_from("<H", blob, g + 16)[0]
        try:
            dec = lzma.LZMADecompressor(lzma.FORMAT_ALONE).decompress(
                blob[hdr + data_offset:hdr + data_offset + 0x400000],
                max_length=0x2000000)
        except lzma.LZMAError:
            continue
        yield dec
        yield from lzma_sections(dec, depth + 1)


def extract_vbioses(rom):
    """The VBIOS FFS file holds one LZMA section wrapping several free-form
    sections: the legacy VBIOS, the AMD GOP driver and two unrelated RAID
    option ROMs. Return {file_name: bytes} for the graphics ones."""
    for dec in lzma_sections(rom):
        # The GUID also shows up in dependency lists; the FFS header is the
        # hit that is followed directly by an LZMA GUID-defined section.
        f = -1
        while True:
            f = dec.find(VBIOS_GUID, f + 1)
            if f < 0 or dec[f + 28:f + 44] == LZMA_SECTION_GUID:
                break
        if f < 0:
            continue
        payload = next(lzma_sections(dec[f + 24:f + 24 + 0x40000]), None)
        if payload is None:
            continue
        out = {}
        off = 0
        while off + 20 <= len(payload):
            size = int.from_bytes(payload[off:off + 3], "little")
            stype = payload[off + 3]
            if size < 4:
                break
            data = payload[off + 20:off + size]  # type 0x18: 4-byte header + GUID
            if stype == 0x18 and data[:2] == b"\x55\xaa" and b"ATOMBIOS" in data[:0x400]:
                m = re.search(rb"113-[A-Z0-9-]+", data[:0x400])
                part = m.group().decode() if m else f"unknown-{len(out)}"
                out[f"vbios-{part}.bin"] = data[:data[2] * 512]
            elif stype == 0x18 and data[:2] == b"MZ" and "AMD GOP".encode("utf-16-le") in data:
                out["AmdGopDriver.efi"] = data  # UEFI GOP, for an edk2 payload
            off = (off + size + 3) & ~3
        return out
    return {}


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    rom = open(sys.argv[1], "rb").read()
    outdir = sys.argv[2]
    if len(rom) != ROM_SIZE:
        sys.exit(f"expected a 16 MiB image, got {len(rom):#x} bytes")
    os.makedirs(outdir, exist_ok=True)
    manifest = []

    efs = find_efs(rom)
    efs_words = struct.unpack_from("<10I", rom, efs)
    imc, xhci, psp_dir, bhd = efs_words[1], efs_words[3], efs_words[5], efs_words[7]
    print(f"EFS at {efs:#x}: PSP dir {psp_dir:#x}, BIOS dir {bhd:#x}")

    off = flash_off(psp_dir)
    magic, _, count, _ = struct.unpack_from("<4sIII", rom, off)
    if magic != b"$PSP":
        sys.exit(f"no $PSP directory at {off:#x}")
    for n in range(count):
        etype, size, addr = struct.unpack_from("<IIQ", rom, off + 16 + n * 16)
        etype &= 0xFF
        name = PSP_NAMES.get(etype)
        if name is None:
            sys.exit(f"unexpected PSP entry type {etype:#x}; update PSP_NAMES and fw.cfg")
        src = flash_off(addr)
        write(outdir, name, rom[src:src + size], manifest)

    off = flash_off(bhd)
    magic, _, count, _ = struct.unpack_from("<4sIII", rom, off)
    if magic != b"$BHD":
        sys.exit(f"no $BHD directory at {off:#x}")
    for n in range(count):
        etype, size, src, dst = struct.unpack_from("<IIQQ", rom, off + 16 + n * 24)
        if etype & 0xFF == 0x60:
            s = flash_off(src)
            write(outdir, "APCB.bin", rom[s:s + size], manifest)
        elif etype & 0xFF == 0x62:
            print(f"stock reset image: {size:#x} bytes -> DRAM {dst:#x}")

    for name, addr in (("Imc.bin", imc), ("Xhci.bin", xhci)):
        if addr in (0, 0xFFFFFFFF):
            continue
        s = flash_off(addr)
        blob = rom[s:s + 0x10000].rstrip(b"\xff")
        write(outdir, name, blob, manifest)

    vbioses = extract_vbioses(rom)
    if not vbioses:
        print("warning: VBIOS not found (only needed once graphics init exists)")
    for name, blob in vbioses.items():
        write(outdir, name, blob, manifest)

    with open(os.path.join(outdir, "MANIFEST.txt"), "w") as f:
        f.write(f"# extracted from {os.path.basename(sys.argv[1])} "
                f"sha256 {hashlib.sha256(rom).hexdigest()}\n")
        for name, size, digest in manifest:
            f.write(f"{name:28} {size:#9x} {digest}\n")
    for name, size, _ in manifest:
        print(f"  {name:28} {size:#x}")


if __name__ == "__main__":
    main()
