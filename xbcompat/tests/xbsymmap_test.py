#!/usr/bin/env python3
"""xbsymmap.py's naming of XDK 5xxx and LTCG entry points, from canned
XbSymbolDatabase output and a fake host binary (no scanner, no XBE).

Run: python3 tests/xbsymmap_test.py
"""
from pathlib import Path
import io
import sys
import tempfile
import types
import unittest
from contextlib import redirect_stderr
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import xbsymmap

HOSTS = [
    "_D3D_MakeRequestedSpace@8", "_D3D_CreateTexture@36", "@D3D_BlockOnResource@4",
    "_D3D_BlockOnResource@4", "_D3DDevice_EnableOverlay@4", "_D3DDevice_EnableOverlay_LTCG@0",
    "_D3DDevice_SetSoftDisplayFilter@4", "_D3DDevice_SetSoftDisplayFilter_LTCG@0",
    "_D3DDevice_SetVertexShaderConstantNotInline_LTCG@12",
    "_IDirectSoundBuffer_SetMixBinVolumes@8", "_IDirectSoundBuffer_SetMixBinVolumes_8@8",
    "?Use3DVoiceData@CDirectSoundBuffer@DirectSound@@QAGJPAU_DS3DVOICEDATA@@@Z",
]

SCAN = """\
D3D8__FUN__stdcall__D3D_MakeRequestedSpace_8(psh MinimumSpace, psh RequestedSpace) = 0x00011000
D3D8LTCG__FUN__unknown__D3D_CreateTexture_28__LTCG_dl8_edi9(psh Width, psh Height, psh Depth, psh Levels, \
psh Usage, psh Format, psh isCube, dl isVolume, edi ppTexture) = 0x00011010
D3D8LTCG__FUN__fastcall__D3D_BlockOnResource_0__LTCG_ecx1(ecx pResource) = 0x00011020
D3D8LTCG__FUN__stdcall__D3DDevice_EnableOverlay_0__LTCG() = 0x00011030
D3D8LTCG__FUN__unknown__D3DDevice_SetSoftDisplayFilter_0__LTCG_unk1(unk Enable) = 0x00011040
D3D8LTCG__FUN__unknown__D3DDevice_SetVertexShaderConstantNotInline_0__LTCG_ebx1_edx2_eax3(ebx Register, \
edx pConstantData, eax ConstantCount) = 0x00011050
DSOUND__FUN__stdcall__IDirectSoundBuffer_SetMixBinVolumes_8(psh pThis, psh pMixBins) = 0x00011060
DSOUND__FUN__stdcall__CDirectSoundBuffer_Use3DVoiceData(psh this, psh pUnknown2) = 0x00011070
"""


class XbSymMapTest(unittest.TestCase):
    def run_map(self):
        with tempfile.TemporaryDirectory(prefix="xbsymmap-") as d:
            binary = Path(d) / "host"
            binary.write_bytes(b"\0" + b"\0".join(h.encode() for h in HOSTS) + b"\0")
            out = Path(d) / "map"
            fake = types.SimpleNamespace(run=lambda *a, **k: types.SimpleNamespace(stdout=SCAN))
            with mock.patch.object(xbsymmap, "subprocess", fake), \
                 mock.patch.object(xbsymmap, "section_range", lambda *a: None), \
                 mock.patch.object(xbsymmap, "extra_sigs", lambda *a: {}), \
                 mock.patch.object(sys, "argv", ["xbsymmap", "cli", "xbe", str(binary), "-o", str(out)]), \
                 redirect_stderr(io.StringIO()):
                xbsymmap.main()
            m = {}
            for line in out.read_text().splitlines():
                f = line.split()
                m.setdefault(int(f[1], 16), []).append([f[0]] + f[2:])
            return m

    def test_names(self):
        m = self.run_map()
        # STACK() suffix: X_8 is the host's _X@8.
        self.assertEqual(m[0x11000], [["_D3D_MakeRequestedSpace@8"]])
        # A byte argument in dl arrives as edx.
        self.assertEqual(m[0x11010], [["_D3D_CreateTexture@36", "regs=s,s,s,s,s,s,s,edx,edi"]])
        # LTCG fastcall and stdcall entry points by their API name.
        self.assertEqual(m[0x11020], [["@D3D_BlockOnResource@4"]])
        self.assertEqual(m[0x11030], [["_D3DDevice_EnableOverlay_LTCG@0"]])
        self.assertEqual(m[0x11040], [["_D3DDevice_SetSoftDisplayFilter_LTCG@0"]])
        self.assertEqual(m[0x11050], [["_D3DDevice_SetVertexShaderConstantNotInline_LTCG@12",
                                      "regs=ebx,edx,eax"]])
        # A host name with the suffix itself wins over the stripped one.
        self.assertEqual(m[0x11060], [["_IDirectSoundBuffer_SetMixBinVolumes_8@8"]])
        self.assertEqual(m[0x11070][0][0][:16], "?Use3DVoiceData@")


if __name__ == "__main__":
    unittest.main()
