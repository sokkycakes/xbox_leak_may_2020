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

## Open questions (first boot will answer them)

- **GPU partitioning inside WinPE** has not been tried. It needs `vpci.sys`
  and the vendor driver to load in PE. If it doesn't, the partitions fall back
  to software D3D (WARP), which works but is slow.
- **No DWM in WinPE.** Capturing the screen with GDI should work for the
  shell. A flip-model D3D game may capture black, in which case the agent
  needs a DXGI capture path or the game must present through GDI-compatible
  blt model.
- **Audio** in PE works in community builds (PhoenixPE, Win10XPE) with this
  same set of services; ours is untested.
- None of the PowerShell here has been run yet; it was written in a Linux
  cloud box without Windows.
