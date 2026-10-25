# MechAssault fix: Sion kit

The Sion's live xbcompat (gudfcx ff23b324 + Frolick's stream patch from
63e9f03b dist/stream, sha256 9f1b886d...) plus two fixes:

- MechAssault no longer crashes at its loading screen: XFileMediaObject
  gets DoWork (vtable slot 9), which MechAssault calls on the object from
  XFileCreateMediaObjectAsync (src/hle/dsound.c). The crash handler now
  logs a jump to unmapped code instead of faulting again (src/main.c).
- Present honours the presentation interval (D3DPRESENT_INTERVAL_TWO etc.):
  MechAssault asks for 30 fps and its Bink intros played at double speed
  (src/hle/d3d8.c).

Built the same way as the live binary (GL=opengl, strip -s, then patchelf
--set-interpreter /opt/xbcompat/lib/ld-linux.so.2). The same tree without
mechfix.patch rebuilds the live binary byte for byte apart from its build id.

Install (no game running): keep /opt/xbcompat/bin/xbcompat as
xbcompat.pre-mechfix-<date>, copy this one over it (chmod 755), restart the
dashboard. Undo: put the .pre-mechfix copy back.
The Guide is unchanged, so kazeta-guide needs no rebuild.
