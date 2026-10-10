# Pi 3 native renderer: remaining MSAA bottleneck

Measured 2026-10-10 UTC on the 109-draw dashboard main menu. This follows
commit 7ce8302 on codex/native-arm-renderer. Same Pi, native client, patched
Box86, 640x480 backbuffer and 720x480 composite output as the earlier test.

## Findings

The native renderer alone did not make this a CPU-only problem. At 4x MSAA,
the VC4 render queue takes roughly the entire 33.33 ms budget for 30 FPS.
CPU work, GPU binning, presentation and scheduling leave additional gaps.
The render engine was busy about 82% of the corrected-hash capture; that does
not mean the remaining 18% is freely recoverable CPU time, since it can also
be waiting for binning and dependencies.

A real CPU/cache defect was found and fixed: conv_find masked its hash down
to ten bits without first mixing the pointer's high bits. Buffers separated
by aligned address strides collide in the same eight-slot probe window.
The menu evicted 30 entries and uploaded 437,040 bytes each frame. Mixing
all address bits eliminated steady-state evictions and uploads in this scene.
No buffer validation or invalidation rule was relaxed.

## Same-library hash comparison

Diagnostic build; only the experimental hash toggle differed. Both retain
4x MSAA, the original fullscreen filter and swap interval 1. Initial startup
intervals excluded; five-second averages. The old hash had seven measured
intervals, the corrected hash eleven. Counters add a little measurement cost.

| Measurement | Old hash | Corrected hash |
|---|---:|---:|
| FPS | 24.43 | 25.60 |
| Frame wall time, ms | 40.96 | 39.10 |
| Draw wall time, ms | 15.30 | 11.26 |
| Presentation wall time, ms | 17.11 | 19.37 |
| Other wall time, ms | 8.50 | 8.49 |
| Cache hits/frame | 410 | 440 |
| Cache misses/evictions/frame | 30 | 0 |
| Cache uploads/frame | 30 | 0 |
| Cache bytes uploaded/frame | 437040 | 0 |

The saved draw time partly reappears as a longer swap wait: the CPU reaches
presentation earlier while the GPU is still busy. Wall-clock GL calls do not
measure GPU execution. Likewise, 'other' is a residual bucket, not a precise
measurement of bridge cost.

## Hardware GPU trace

Five-second isolated tracefs captures of vc4_submit_cl, vc4_bcl_end_irq and
vc4_rcl_end_irq. Durations pair command-list execution start and completion
interrupt by seqno. Initial/last incomplete jobs are excluded. Job roles are
inferred from command-list order and the filter-off / MSAA-off comparisons.

| Render-queue stage | 4x MSAA, corrected hash | MSAA off, corrected hash |
|---|---:|---:|
| Main scene | 26.07 ms | 20.64 ms |
| MSAA resolve | 2.13 ms | absent |
| Fullscreen filter/output | 4.00 ms | 3.95 ms |
| Total render work/frame | 32.20 ms | 24.59 ms |

The separate binning engine averaged 16.26 ms per scene at 4x and 13.43 ms
without MSAA. It overlaps rendering, so it must **not** be added to the table
as if it were another serial pass. The render queue's remaining roughly
6.9 ms per 39.1 ms frame consists of gaps between jobs; this experiment does
not assign all of those gaps to a single cause.

MSAA increases measured render work by roughly 7.6 ms per frame (including
resolve), not merely a few additional CPU wrapper calls. Mesa documents VC4's
32x32 MSAA tiles versus 64x64 non-MSAA tiles and the associated binning work:
https://docs.mesa3d.org/drivers/vc4.html

## Other isolated experiments

- Original hash, filter disabled, MSAA retained: 25.58 FPS versus about 24.4.
  The final GPU pass fell from about 4.0 to 1.98 ms. It still needs an output
  blit, so disabling the filter does not save its full 4 ms. This changes the
  image and was only a diagnostic; the filter is enabled in the final run.
- Corrected hash, MSAA off: 30.0 FPS. Main scene render work falls to 20.64 ms.
- Corrected hash, 4x MSAA, swap interval 0: 26.89 FPS. This may permit tearing;
  it does not reach 30 and is not the final setting. VC4_DEBUG=perf reported no
  explanatory performance warning during this run.
- Final build, short performance-governor test: two fully measured intervals
  at 26.4 and 26.5 FPS versus 25.6 with ondemand. CPU draw time fell from about
  10.4 to 8.5 ms, but swap wait grew. The original ondemand governor was restored.
  Temperature remained around 77–78 C; V3D was observed at 300 MHz. These spot
  observations do not constitute a long thermal soak or throttling analysis.

## Next optimization target

The largest target is the main scene's 26 ms of GPU rendering and its binning
work. Per-draw VC4 performance counters / an EGL apitrace can locate expensive
translated shaders and overdraw without removing visible content. The
nine-sample fullscreen filter is another concrete target: test an equivalent
filter implementation with fewer texture fetches, checking output pixels at
the actual 640-to-720 scaling ratio. Finally, examine the swap/GBM submission
sequence to reduce gaps while retaining vsync. More CPU optimizations alone
will increasingly turn into GPU wait unless that pipeline also improves.

## Final state and validation

The final library enables the corrected hash by default and makes detailed
timing/counters opt-in. It preserves 4x MSAA, the original filter, swap interval
1 and ondemand. It is running from /tmp/xbcompat-native-test.if8zCJ/fixed;
installed binaries and boot settings are untouched. Reboot returns to the
installed runtime. The isolated tracing instance was removed and tracefs
returned to its original read-only mount state.

The i386 entry ABI test (60,000 calls), renderer service/callback integration,
and llvmpipe triangle/presentation test passed. The Pi's final menu screenshot
was visually inspected and normal animation continued at 25.6 FPS. This is
not proof of correctness for every title or of long-session stability.
