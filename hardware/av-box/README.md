# AV box: HDMI in, console-style AV out

FPGA design for a small box that turns a PC's 15 kHz HDMI output (the Sion's
2880x480i, VIC 10) into composite, S-video and RGB/YPbPr on a Mega Drive 2
style 9-pin mini-DIN plus a 4-pin S-video jack. Design doc:
https://claude.ai/artifact/Xdtx1teAB1gWyq4qremGeo

## RTL (`rtl/`)

- `tmds_lane.v`: bit alignment by bitslip over a 20-bit window (finds runs of
  control tokens), 8b/10b decode.
- `hdmi_rx.v`: three lanes, lane deskew, video preamble and guard band
  handling, data islands (hsync/vsync read from TERC4 on lane 0). No audio yet.
- `ntsc_enc.v`: genlocked NTSC encoder at the 54 MHz pixel clock. Line =
  3432 clocks = exactly 227.5 subcarrier cycles. Equalizing and serrated
  vertical pulses, burst, 7.5 IRE setup, composite + Y/C + RGB/YPbPr + csync.
- `av_top.v`: top level.

The Gowin I/O deserializers (IDES10) and the TMDS clock PLL are not in here
yet; the testbench feeds 10-bit words with a different bit offset per lane,
which is what IDES10 hands over.

## Simulation (`sim/`)

`sim/run.sh picture.png` builds an HDMI stream from a picture (negative syncs,
limited range, pixel x4, preambles, a data island across every hsync edge,
bit offsets 3/7/0 per lane), runs the RTL under Verilator, checks the RGB
outputs bit for bit, and decodes the composite and S-video waveforms with a
software TV (`decode_ntsc.py`) that only sees the analog samples.

Results (2026-11-08, Crimson dashboard frame):

| Check | Result |
| --- | --- |
| Lane lock | all lanes aligned and deskewed by clock 65535 (1.2 ms) |
| RGB output vs source | bit exact, 480 lines x 2880 |
| Composite decoded vs source | 34.4 dB PSNR |
| S-video decoded vs source | 36.4 dB PSNR |
| 75% colour bars via composite | every bar within 3% of its value |

Yosys `synth_gowin` (no place and route yet): about 3700 LUT4 plus muxes and
1088 ALU cells, before moving the multipliers to DSP blocks and the sine
table to block RAM. The GW1NR-9 has 8640 LUT4.
