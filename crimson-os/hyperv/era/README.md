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
| Game OS, 64-bit only (`--no-wow64`) | 286 MB | | |
| Game OS with 32-bit support (default) | 487 MB | 491 MB | 50 s |

Both stream 1280x720 frames to the host. Stock Windows PE from the same ISO
is 539 MB. Fixed on the way: keep `WinSxS\Manifests` (removing it bugchecks
with CRITICAL_PROCESS_DIED), leave WinPE's svchost groups alone
(the donor's lack BFE, which kills networking), keep `net1ix64` (QEMU's e1000e).

32-bit programs (Game OS): WinPE has no WoW64 layer and only part of SysWOW64,
so the builder adds the wow64 DLLs, a SysWOW64 twin of every System32 file,
all x86/wow64 manifests, the 32-bit SxS `Winners` keys and the System32
`CatRoot` catalogs. Without the catalogs it resets about 30 s into boot (code
integrity rejects the 32-bit KnownDLLs); without the manifests and Winners,
every 32-bit exe fails with a side-by-side error. `samples/bitness-test` checks
a 32-bit and a 64-bit GUI app with common controls 6 and OpenGL: both pass
(OpenGL is Microsoft's software 1.1, "GDI Generic").

## Still open

- **GPU partitioning inside WinPE**: needs Hyper-V; not testable in QEMU.
- **Frame rate**: about 2 updates/s in emulation with GDI capture, far below
  the requested 10-15. Needs a faster capture path (DXGI) and checking on Hyper-V.
- **No DWM in WinPE**: flip-model D3D games may capture black.
- **Audio**: services are in place but no game has been tried.
- The PowerShell builder (`New-CrimsonEraImage.ps1`) has not been run on Windows yet.
