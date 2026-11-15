# Era-style partitions (Windows PE based)

`New-CrimsonEraImage.ps1` builds the Game OS and System OS from Windows PE
instead of a full Windows install, the way EraOS is a pared-down Windows that
boots read-only:

| | Full-install partitions | Era-style partitions |
|---|---|---|
| Base | Windows 11 VHDX, ~20-25 GB | WinPE WIM, a few hundred MB compressed |
| Boots from | writable differencing disk | read-only ISO into a RAM disk (X:) |
| After reboot | keeps changes | always clean |
| Shell | Explorer | none: `crimson-agent` is the first program (winpeshl.ini) |
| Saves/settings | on C: | separate 2 GB data disk |
| Game | installed on C: | its own VHDX, attached at launch like a game package |

On top of WinPE it adds only `WinPE-WMI`, the files in `era-files.txt` (D3D,
audio, Hyper-V socket, XInput, VC++ runtime, Media Foundation for the game OS),
their service registrations, and the host GPU driver for GPU partitioning. It
then strips setup, recovery, PowerShell, spare fonts and locales. The script
prints each image's size in RAM and as a WIM.

Run (elevated, Windows ADK + WinPE add-on installed, donor ISO of the same build):

    .\New-CrimsonEraImage.ps1 -DonorIso D:\iso\Win11_24H2.iso -CreateVms
    ..\..\build\crimson-host.exe --hv
    Start-VM Crimson-Era-System, Crimson-Era-Game

A game disk is a VHDX with `Crimson\launch-game.cmd` at its root.

## Tested (QEMU + KVM, Windows 11 24H2 donor, `linux/build_era.py --minimal`)

| Partition | WIM | ISO | Boot to agent connected |
|---|---|---|---|
| System OS | 279 MB | 283 MB | 31 s |
| Game OS   | 313 MB | 317 MB | 32 s |

Both stream 1280x720 frames to the host. Stock Windows PE from the same ISO
is 539 MB. Fixed on the way: keep `WinSxS\Manifests` (removing it bugchecks
with CRITICAL_PROCESS_DIED), merge the svchost groups instead of replacing
them (replacing kills networking), keep `net1ix64` (QEMU's e1000e).

## Still open

- **GPU partitioning inside WinPE**: needs Hyper-V; not testable in QEMU.
- **Frame rate**: about 2 updates/s in emulation with GDI capture, far below
  the requested 10-15. Needs a faster capture path (DXGI) and checking on Hyper-V.
- **No DWM in WinPE**: flip-model D3D games may capture black.
- **Audio**: services are in place but no game has been tried.
- The PowerShell builder (`New-CrimsonEraImage.ps1`) has not been run on Windows yet.
