# Running the Xbox dashboard on Sion

Short answer: run the dashboard XBE under xbcompat on the Sion Buildroot image. Nothing about the dashboard needs a native kernel port. It's an ordinary XDK title (`TARGETTYPE=PROGRAM`, `UMTYPE=xboxapp`) that links D3D8, DSound and xapilib like any game, plus a handful of privileged kernel calls that xbcompat can fake in user space.

Paths below are under `xbox trunk/xbox/private/ui/` unless noted.

## What the dashboard is in the leak

- **Engine**: `xapp/` (74 .cpp files, target `xboxdash`). It's a small VRML-style scene engine with a script interpreter (`Lexer/Parser/Compiler/Runner.cpp`) that loads UI scenes from XIP archives.
- **Content**: `dash/`. The XIPs are **already built and checked in** (`default.xip`, `mainmenu5.xip`, `settings3.xip`, `memory2.xip`, `music2.xip`, ...), along with the `.xap` scene scripts, the fonts (`Xbox.xtf`, `XBox Book.xtf`) and the translation tables. The dashboard loads them from `y:\` (`main.cpp:717`, `main.cpp:829`).
- **No prebuilt `xboxdash.xbe`** exists anywhere in the tree. The only XBEs are the recovery ones in `ntos/recovery/`. It has to be built.
- **Libraries** (`xapp/sources`): `d3d8`, `d3dx8`, `xgraphics` and `dsound` from `public/xdk/lib-aug01`, plus `xapilibp` and `dashrecovery` from `private/lib/aug01`, the Ravisent DVD lib and the WMA encoder lib (both binary-only).
- **Problem**: `public/xdk/lib-aug01` only contains `xgraphics.lib`. The aug01 `d3d8`, `d3dx8` and `dsound` libs are missing. `public/xdk/lib` has the newer trunk versions, and the D3D8/DSound source is in `private/windows/directx/`.

## What the dashboard needs from xbcompat

| Area | Dashboard usage | xbcompat today | Work |
|---|---|---|---|
| D3D8 | Fixed function, vertex shaders (`effect*.vsh`, `aniso.vsh`, `reflect.vsh`, `XAppCreateVertexShader` in `falloff.cpp`), texture stage state, render-to-texture (`DynTexture`, `surfx`, `fft` for the visualizer) | ATG samples render; vsh/psh translators wired in (`bd60918e`) | Signatures for whichever d3d8.lib the dash links; fill the gaps it traps on |
| DirectSound | `DSound.cpp`, `Audio.cpp`, `AudioPump.cpp` (menu sounds, ambient loop, music player) | `dsound.c` is an empty table | Implement buffers/streams on SDL2 audio. Same work any game needs |
| Input | `XInputOpen/GetState/Close`, `XGetDevices/XGetDeviceChanges` (`Joystick.cpp`) | `xinput.c` is an empty table | SDL2 GameController to XInput gamepad. Same work any game needs |
| Settings | `XQueryValue` x28, `XSetValue` x16 (language, timezone, clock, video, parental, audio) | `ExQueryNonVolatileSetting` returns a few defaults; `ExSaveNonVolatileSetting` is ignored | Back both with a fake EEPROM/config file on disk |
| Launching games | `XLaunchNewImage("d:\\default.xbe")` (`disc.cpp:227`), `HalReturnToFirmware` x6 | `HalReturnToFirmware` logs and exits | Add a launcher loop: write LaunchData, exec xbcompat on the next XBE, return to the dash when a title exits |
| Storage | `c:\tdata`, `c:\udata` saves, `XMountMURoot`, `XapiFormatFATVolume`, `IoCreateSymbolicLink`, free-space queries | Host-directory drives, symlinks traced | Map C:/E:/Y: to host dirs; stub MU and format |
| Hardware | `HalReadSMCTrayState`, `HalEnableSecureTrayEject`, `AvSendTVEncoderOption`, `D3DDevice_SetFlickerFilter`, `XGetAVPack`, `ExReadWriteRefurbInfo` | Some stubs exist | Return "no disc / tray closed / HDMI pack" constants |
| XIP integrity | `xip.cpp` checks every 64 KB block against SHA digests in an `XIPS` section | n/a | Works as-is if the XIPs and xipsums come from the same build; otherwise skip the check |
| DVD / CD / WMA | Ravisent DVD player, CD audio (`ntiosvc`, `xcdplay`), WMA ripping | n/a | **Leave out.** `xapp/sources` already treats these as optional components. Sion has no optical drive |

## Blockers, in order

1. **Building `xboxdash.xbe`.** It's the biggest unknown. The leak ships the toolchain (`public/mstools/vc60` and `vc70` cl/link, `public/idw/build.exe` and `imagebld.exe`), but it's Windows-only. Options: run that toolchain under Wine, or use clang-cl + lld-link with the leak's headers and xbcompat's `pe2xbe.py`. You also have to replace the missing aug01 d3d8/dsound libs, either by linking the trunk `public/xdk/lib` versions (with header fixups in `xapp/`) or by rebuilding them from source. Drop `_DVDPLAYER`, `_CDPLAYER` and `wma8.cpp` for the first build.
2. **DirectSound and XInput in xbcompat.** Without input the dashboard can't be used. Without sound it's silent but runs. Both are already on xbcompat's todo list, so the dashboard just becomes their first real consumer.
3. **Launcher semantics.** The dashboard is only useful if picking a game launches it and quitting returns to the dashboard. That means a small supervisor around xbcompat plus `LaunchDataPage` support.
4. **32-bit userspace on Sion.** xbcompat is a 32-bit process (XBE mapped at 0x10000) and needs i386 SDL2 and Mesa (iris). The current Buildroot image is x86_64-only. The simplest route is a second defconfig built for i686 (Kaby Lake runs it fine, with a PAE kernel) that ships xbcompat, SDL2 (KMSDRM video, ALSA audio) and Mesa iris, and starts `xbcompat y:/xboxdash.xbe` from init instead of bootani. Bootani can still play first, then hand off, which gives the real boot sequence.

## Not needed

- The native kernel port (see `native-kernel-on-sion.md`). The dashboard's GPU and audio go through D3D8 and DSound like any title, so it hits the same NV2A wall. xbcompat already sidesteps that.
- An emulator.

## Alternative considered

`xapp/xapp.dsp` has old Win32/DX8 PC configurations (`_WINDOWS`), so a native Linux port of the dashboard engine is possible in principle. But that code path has clearly bitrotted against the `_XBOX` one. It would need a D3D8-on-GL layer anyway, and it wouldn't help run games. Running the real XBE under xbcompat gets the dashboard and the game launcher out of the same work.
