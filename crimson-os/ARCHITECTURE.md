# Crimson architecture

## What the console does

From the earlier research in this project and Microsoft's public talks:

- **Hyper-V is the foundation.** The host OS is the root partition. The
  game runs in the *Exclusive* partition (6 of the 8 Jaguar cores at launch,
  fixed memory, most of the GPU). The dashboard, Guide, Snap apps, Kinect and
  party chat run in the *Shared* partition (the rest).
- **The GPU is partitioned in hardware** (per-partition GPU address spaces,
  IOMMU, separate queues with preemption, display planes). The host
  guarantees the system its slice and composites the two partitions.
- **The host owns the Xbox button.** Input goes to whichever partition has
  focus; the Xbox button never reaches the game.
- **Layout is a host decision.** The SystemOS dump still lists the
  locations `Full / Fill / Snap / StartView / XboxUI`, plus
  `TrySnapSelf`, `TryUnsnapToFullScreen`, `FillSnapBalance` (audio mix
  between the Fill game and the snapped app) and `EvictApplicationAtLocation`.

## Crimson on Windows

```
 ┌──────────────────────────── Host OS (Windows, Hyper-V root) ───────────────────────────┐
 │  crimson-host.exe                                                                       │
 │   ├─ XInputGetStateEx: physical pad incl. Xbox button   ──► focus router               │
 │   ├─ mode: Full | Guide overlay | Snap (Fill + Snap) | Home                             │
 │   ├─ compositor: game frame ─► dim ─► System OS frame (magenta = transparent) ─► screen │
 │   └─ listeners: AF_HYPERV service c71b0e5a-…  and  TCP :7700                           │
 └────────────▲──────────────────────────────────────────────▲─────────────────────────────┘
      frames  │  input + layout                     frames   │  input + layout
 ┌────────────┴───────────── Game OS ─────────┐  ┌───────────┴──────── System OS ─────────┐
 │ VM "Crimson-Game": 6 vCPU reserved 100%,   │  │ VM "Crimson-System": 2 vCPU, 3 GB,     │
 │ 8 GB static, GPU-P 80%                     │  │ GPU-P 20%, magenta desktop             │
 │ game  +  crimson-agent --role game         │  │ shell (Blades / Home / Guide)          │
 │                                            │  │  +  crimson-agent --role system        │
 └────────────────────────────────────────────┘  └────────────────────────────────────────┘
```

### Pieces

- **`crimson-host`** (host OS). Full screen Win32 window. Polls the pad
  through `XInputGetStateEx` (ordinal 100, the only call that reports the
  Xbox button), keeps the mode state machine, routes the pad to the focused
  partition and composites their frames with GDI.
- **`crimson-agent`** (one per partition). Captures the partition's screen,
  sends only the rows that changed, injects the pad it receives as keys
  (window messages in process mode, `SendInput` in VM mode), and reacts to
  layout changes: the System OS agent presses the shell's Guide key when the
  overlay opens or closes, and in process mode the agent resizes the game
  window to its Fill slot.
- **Protocol** (`src/protocol.h`): `HELLO`, `FRAME` (changed rows, BGRA),
  `INPUT` (XINPUT_GAMEPAD without the Xbox button), `LAYOUT`
  (location + focus + slot size). The same stream runs over TCP or Hyper-V
  sockets.
- **Overlay transparency.** The System OS desktop is solid magenta, and the
  host treats magenta as see-through. That stands in for the console's
  alpha-blended system plane with no driver work.

### Isolation: what is real and what is stand-in

| Concern            | Xbox One                         | Crimson VM mode                               | Crimson process mode |
|--------------------|----------------------------------|-----------------------------------------------|----------------------|
| CPU                | dedicated cores                  | Hyper-V reserve/weight (6 reserved vs 2)      | none                 |
| Memory             | fixed split                      | static VM memory                              | none                 |
| GPU                | hardware partitions, preemption  | GPU-P (`Add-VMGpuPartitionAdapter`), WDDM scheduling | shared             |
| Display            | hardware planes                  | frames streamed to the host, GDI composite    | same                 |
| Input              | host-owned                       | host-owned, guests only see injected input    | shell may read pad itself |
| Inter-partition    | VMBus                            | Hyper-V sockets on VMBus                      | TCP loopback         |

### Known limits of this first cut

- Frames are uncompressed rows over the socket. Hyper-V sockets carry this
  fine at 1080p30 for UI, but a 1080p60 game wants the guest's hardware
  encoder (GPU-P exposes it) or a shared-memory surface. Next step.
- Input reaches partitions as keys. A virtual pad in each guest (ViGEmBus)
  would give games a real controller.
- No audio path yet. Plan: each agent captures WASAPI loopback, the host mixes
  them with a Fill/Snap balance like `FillSnapBalance`.
- GDI compositing is simple but CPU-bound. Moving the host to D3D11 with
  DirectComposition would make it cheap.
- Snap in VM mode keeps the System OS at its own resolution and scales it into
  the strip; the console re-laid the app out at the strip size.
- Process mode: the Blades dashboard reads the controller itself, so it also
  sees the pad while the game has focus. VM mode doesn't have that problem.
