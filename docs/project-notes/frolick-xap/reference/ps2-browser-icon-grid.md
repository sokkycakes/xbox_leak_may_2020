# PS2 1.00 memory-card browser perspective grid (summary)

Copied from sokky's emulauncher/tools/ps2-bios-extract/analysis/BROWSER_ICON_GRID.md (2026-10-22) for the Frolick All games screen.

- Program: MBROWS (BIOS 0100JC20000117), linked at 0x007A0000. Draw function 0x7b3c20, orientation 0x7b05cc, placement 0x7b4190, slot layout 0x7b4348.
- Slots form a flat grid: column = index % stride, row = index / stride, and both axes step by 24.
- Row scale from back to front is 0.65, 0.68, 0.71, 0.74 (row 3 and later use 0.74). Then scale += (4 - page) * 0.121, where page is the word at object +0x148.
- Screen position: horizontal x/25; vertical y/18 - y/80. A negative x adds x/64, and a negative y adds y/64.
- Each model is pitched -0.64 rad (-36.7 deg) and yawed +pi/2 or -pi/2 depending on which side of an x threshold the icon is on, so the two halves turn inward. A -0.1963 rad (-11.25 deg) constant is loaded on the same path.
- The placement transform adds another -0.444 rad (-25.4 deg) to one vec3 component.
- Decompiles on the PC in the same folder: browser_core_funcs.txt, browser_grid_decomp.txt, icon_grid_deep.txt, icon_grid_r5900.txt.
