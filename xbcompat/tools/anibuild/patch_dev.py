#!/usr/bin/env python3
# patch_dev.py IN OUT: port ani2's DEV.C from the Aug 2001 DirectSound mix-bin
# API (a DWORD mask plus a volume array) to the one in public/xdk/inc, which
# takes DSMIXBINS (mix bin index/volume pairs). Nothing else changes.
import re, sys
s = open(sys.argv[1], encoding="latin-1").read()
shim = r'''#include "sosdsp.h"

/* xbcompat anibuild: Aug 2001 mix-bin masks on the newer DSMIXBINS API. */
#define ANI_MB_FRONT_LEFT   0x00000001
#define ANI_MB_FRONT_RIGHT  0x00000002
#define ANI_MB_FXSEND_0     0x00000800
static DSMIXBINVOLUMEPAIR g_aniPairs[32];
static DSMIXBINS g_aniBins;
static LPCDSMIXBINS AniMixBins(DWORD mask, const long *vol)
{
    DWORD i, n = 0;
    for (i = 0; i < 32; i++)
        if (mask & (1u << i)) {
            g_aniPairs[n].dwMixBin = i;
            g_aniPairs[n].lVolume = vol ? vol[n] : 0;
            n++;
        }
    g_aniBins.dwMixBinCount = n;
    g_aniBins.lpMixBinVolumePairs = g_aniPairs;
    return &g_aniBins;
}
'''
s = s.replace('#include "sosdsp.h"\n', shim, 1)
s = re.sub(r'\bDSMIXBIN_(FRONT_LEFT|FRONT_RIGHT|FXSEND_0)\b', r'ANI_MB_\1', s)
s = re.sub(r'dsbdesc\.dwMixBinMask\s*=\s*dwMixBinMask;', 'dsbdesc.lpMixBins = AniMixBins(dwMixBinMask, lVolumes);', s)
s = s.replace('IDirectSoundBuffer_SetMixBinVolumes(m_pDSBuffer[i], dwMixBinMask, lVolumes);',
              'IDirectSoundBuffer_SetMixBinVolumes(m_pDSBuffer[i], AniMixBins(dwMixBinMask, lVolumes));')
open(sys.argv[2], "w", encoding="latin-1").write(s)
