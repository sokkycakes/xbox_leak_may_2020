# xbt: an Xbox-specific x86 to AArch64 translator

xbt runs original Xbox executables (XBE) on 64-bit ARM Linux. It translates
the Xbox's Pentium III code (integer, x87, MMX, SSE) into AArch64 code and
replaces the Xbox kernel with a high-level emulation (HLE) of its exports.

General-purpose translators such as box86 have to handle any Linux x86
program. That means emulating operating-system calls, translating guest
addresses, computing CPU flags eagerly, and keeping memory ordering strict
for threads running on several cores. An Xbox title always runs in the same
environment:

- one flat 64 MB physical address space, mirrored at a few fixed addresses
- a single CPU core
- the Microsoft compiler's calling conventions
- no operating system underneath, only a kernel that the title links against
  by ordinal

xbt is built around those facts. It works for any title, because it relies
only on the platform, never on a particular game.

## Host requirements

- **A 64-bit (AArch64) Linux kernel.** xbt identity-maps the guest's 4 GB
  address space into the low 4 GB of the host process, which needs a 64-bit
  process. The userland may be 32-bit: `make static` builds a self-contained
  binary that runs on a 64-bit kernel with an armhf userland (check with
  `uname -m`, which prints `aarch64`). A 32-bit kernel (`armv7l`) is not
  supported.
- **ARMv8.0 with Advanced SIMD (NEON) is enough.** No newer extensions are
  used. A Raspberry Pi 3 (Cortex-A53) meets this, as does anything newer.
- **4K, 16K or 64K host pages.** Write protection for self-modifying code
  covers the whole host page.
- **Not yet run on real hardware.** All development and measurement so far
  has been done under qemu-aarch64 (see Results).

## Building and running

```sh
make                       # cross-compiles with aarch64-linux-gnu-gcc on x86-64
build/xbt game/default.xbe # on an AArch64 host
qemu-aarch64 -L /usr/aarch64-linux-gnu build/xbt game/default.xbe   # elsewhere
make static                # build/xbt-static: no shared libraries needed
```

To time xbt against box86 on the device itself, copy `build/xbt-static` (as
`xbt`), `bench/build/b_*` and `bench/pi-run.sh` into one directory there and
run `sh pi-run.sh`. Stop anything else that loads the CPU first.

The game's directory becomes `D:`. The hard-disk partitions (`C:`, `E:`,
the `Z:` cache and X/Y/F) are directories under `<game dir>/xbt_hdd/`.

| option | effect |
|---|---|
| `--stats` | print translation, flag and SMC statistics on exit |
| `--strict-abi` | do not assume flags are dead across `call`/`ret` |
| `--x86-nan` | give SSE invalid operations x86's negative default NaN (see Deviations) |
| `--no-link`, `--no-fusion`, `--no-flag-elim` | turn off block linking, NZCV fusion or flag elimination (for measurement) |
| `--max-insns N` | guest instructions per block (default 200) |
| `--hle-trace`, `--mmio-log` | log kernel calls / device register accesses |
| `--hle-strict` | stop at the first unimplemented kernel call |
| `--run-seconds S`, `--sample` | stop after S seconds; print where the guest spent its time |
| `--devkit` | 128 MB of RAM instead of 64 MB |
| `--trace`, `--dump DIR` | log translated blocks / write their host code to DIR |
| `--elf prog` | run a static i386 Linux ELF (`write`/`exit` only), used by the benchmarks |
| `--test in out` | run a differential test batch (used by `tests/difftest.py`) |

## Design

### Memory: guest address == host address

`mem_init` reserves host addresses `0x10000`–`0x1_0001_0000` and maps one
memfd holding the Xbox RAM three times, exactly where the Xbox has it:

- at `0x10000` (the identity view, where the XBE loads)
- at `0x80000000` (the kernel's contiguous-memory mirror)
- at `0xF0000000` (the write-combined mirror)

Everything else in the 4 GB stays `PROT_NONE`. A guest pointer therefore
*is* a host pointer:

- `mov eax, [esi+8]` becomes a single `ldr w19, [x25, #8]`.
- Absolute addresses use `adrp` plus an offset.
- There is no translation table or bounds check.

Device registers (NV2A, APU, ...) are left unmapped. Accesses to them fault,
and the fault handler emulates the instruction against a device model.

### Registers

- Guest GPRs live in `w19`–`w26` and the x87 TOP in `w27`; the CPU context
  pointer is in `x28`. All of these are callee-saved, so helper calls do not
  disturb them.
- Within a block, the x87 stack is cached in `d8`–`d15`:
  - TOP changes are tracked statically.
  - `fxch` is a register rename that emits no code.
  - Values are written back only at block exits and before helpers that read
    x87 state.
- XMM registers are cached in `v16`–`v23`.
- Within a block, `x12` holds the page of the last absolute address and `x13`
  the last `base + index*scale`, so repeated `[ebx+esi*4+disp]` accesses cost
  one instruction each.

### Flags only when they are read

Each block gets a backward liveness pass over the six status flags. The pass
looks into the successor blocks (up to 96 instructions) to find flags that
they overwrite before reading. It also assumes, as the MSVC ABI allows, that
flags are dead across `call` and `ret`; `--strict-abi` turns that off. Each
flag producer then does the least work it can:

- **Eliminated:** no reader exists, so no flag code is emitted.
- **Via host NZCV only:** the reader is in the same block, so the producer
  uses the flag-setting AArch64 form (`subs`, `adds`, `ands`, `fcmp`, ...) and
  the reader uses the matching host condition, including the compound
  conditions that x86 `ja`/`jbe`/`jp` need after `fcomi`. Byte and word
  operations are shifted into the top bits so that the host N/Z/C/V match.
- **Stored:** only when a later block, or something like `pushf`, reads the
  flags. The producer then saves a lazy record (operation, result, operands),
  which is evaluated on demand.

On the recovery dashboard (below), 61% of flag producers are eliminated and
35% go through host NZCV only; 3% store a record. 97% of flag consumers read
host NZCV directly.

### Block linking

- **Direct branches** are patched into plain `b` instructions to the target
  block. If the target is already translated, this happens at translation
  time.
- **Indirect jumps, calls and returns** look up a flat 4-byte-per-entry table
  that covers 128 MB of guest code, inline with no hashing.
- **Backward branches** check a single pending flag, which is how the
  scheduler preempts a thread.

### No memory-ordering cost

The Xbox has one core, so xbt emits no barriers or atomics. The `lock` prefix
is ignored, and `xchg` with memory is a plain load and store.

### Self-modifying code

Pages holding translated code are write-protected. A write to one does the
following:

- It is emulated through a separate unprotected alias of RAM.
- Only the blocks overlapping the written bytes are invalidated: their entry
  is patched to jump back into the dispatcher.
- Pages that are written over and over become "volatile": their blocks check
  their own guest bytes on entry instead of being write-protected.

No title code is ever flushed wholesale.

### Kernel (HLE)

`tools/gen_kexports.py` builds the export table (ordinal, name, argument
count, calling convention) from the kernel's export definition and headers
in this repository. 181 of the 367 exports are implemented, covering:

- memory (physical, virtual, contiguous, pool)
- objects and handles
- threads with a preemptive scheduler on one host thread
- events, semaphores, mutants, timers and DPCs
- critical sections
- file I/O on mapped device paths
- SEH and exception dispatch
- the TLS layout that XAPI expects
- the system time
- the RTL string, memory and arithmetic helpers that titles import

An unimplemented call returns `STATUS_SUCCESS` and is logged once.

## Testing

**Differential tests** (`tests/difftest.py`):

- `gen.py` writes random instruction sequences in 27 groups: ALU, shifts,
  bit ops, BCD, string ops, loops, calls, flag crossings between blocks, SMC,
  x87, x87 state save/restore, SSE and MMX.
- Each test runs natively on the build machine's x86 CPU (`runner32.c`) and
  through xbt.
- The two results are compared: registers, defined flags, XMM registers and
  all of memory the test can touch.

```sh
cd tests
python3 difftest.py --seed 1 --count 4000 --length 8
python3 difftest.py --seed 2 --count 3000 --kinds x87,x87misc,sse,mmx
python3 difftest.py --seed 3 --count 3000 --strict                # --strict-abi
python3 difftest.py --seed 4 --count 3000 --xbt-args "--max-insns 1"
python3 difftest.py --seed 5 --count 3000 --xbt-args "--no-link --no-fusion --no-flag-elim"
```

Every configuration above passes all of its tests. About 40,000 tests were
run after the last code generation change.

A failing test that contains an SSE operation able to produce a default NaN
is re-run with `--x86-nan`. It counts as passing only if it then matches
exactly, so a failure is only excused when the default-NaN sign is the sole
difference.

**Encoder check:** `tests/run_a64_test.sh` assembles every instruction
form the encoder produces and compares the result with GNU objdump
(222 forms, no mismatches).

### Deviations from a Pentium III

These are deliberate. They trade exactness that games do not rely on for
speed:

- **x87 precision:** x87 arithmetic is done in double precision (53-bit
  mantissa), not 80-bit, and the precision-control field is ignored.
  - 80-bit loads and stores convert, keeping NaN payloads.
  - The constant ROM values (`fldpi`, `fldlg2`, ...) are therefore rounded
    to double.
  - Transcendentals use the host libm.
- **Default NaN sign:** an invalid operation on non-NaN inputs (0/0,
  ∞−∞, √−1) gives x86's negative "real indefinite" NaN but AArch64's
  positive default NaN. box86 has the same deviation by default
  (`FASTNAN`).
  - `--x86-nan` emulates the sign for SSE at 7–9 extra instructions per
    arithmetic op.
  - x87 NaN signs are not emulated.
- **Two-NaN operations:** when both inputs are NaN, AArch64 prefers a
  signalling NaN in the second operand, while x86 returns the first operand.
- **MXCSR:** it is stored and reloaded, but its rounding mode, FTZ/DAZ and
  exception flags are not applied. SSE uses the x87 control word's rounding
  mode, which xbt mirrors into FPCR.
- **`rcpps`/`rsqrtps`** are more precise than the Pentium III's 12-bit
  estimates.
- **x87 exceptions:** the tag word and exception flags are not tracked
  (`fnstsw` reports the condition codes and TOP).
- **Flags across `call`/`ret`** are assumed dead (MSVC ABI), unless
  `--strict-abi` is given.

## Results

### A real title

The Xbox recovery disc's `default.xbe` (from this repository) boots and runs
the following:

1. CRT and XAPI initialization
2. thread creation
3. file I/O on `D:`
4. Direct3D device creation
5. push-buffer submission to a stubbed NV2A
6. the dashboard's x87 software-blit loops, which keep running until stopped

No images are displayed, because there is no GPU emulation (see Limitations).
40 seconds of this run gives the following figures:

- 3,846 blocks translated
- 5,892 flag producers: 3,620 eliminated, 2,080 handled through NZCV only,
  192 stored
- 2,245 flag consumers: 2,182 read NZCV directly
- 256 writes to code pages, emulated, which invalidated 82 blocks and caused
  0 full cache flushes

### Instruction counts against box86

`bench/` holds three freestanding i386 programs, built with clang
`-O2 -march=pentium3`:

- **integer:** CRC-32, quicksort, string hashing, 64-bit arithmetic and a
  small bytecode interpreter
- **x87:** 4×4 matrix products, vertex transform and normalize, and an
  n-body integration, compiled for x87
- **SSE:** the same program compiled with `-mfpmath=sse`

`bench/compare.py` runs each program three ways under qemu's `libinsn`
plugin, which counts every instruction the emulated CPU executes:

- natively under qemu-i386
- through xbt under qemu-aarch64
- through box86 (v0.3.9, dynarec, `RPI3` build) under qemu-arm

The counts include each translator's own decoding, translation and
dispatching. Start-up cost is removed by subtracting an empty program.

The table shows host instructions executed per x86 instruction (lower is
better):

| | integer | x87 | SSE |
|---|---|---|---|
| **xbt** | **2.33** | **1.65** | **1.90** |
| box86 | 4.19 | 2.11 | 2.58 |
| xbt, no flag elimination | 11.30 | 2.64 | 3.08 |
| xbt, no NZCV fusion | 3.43 | 1.94 | 2.05 |
| xbt, no block linking | 20.36 | 3.81 | 4.48 |
| xbt, none of the three | 30.02 | 5.01 | 5.75 |

Start-up costs 0.35 M instructions for xbt and 2.4 M for box86.

Ways to read the table:

- **These are instruction counts, not times.** xbt executes AArch64 code and
  box86 executes ARMv7 code, so the two counts are similar but not equal
  measures of work. On a real Pi 3 they would mostly show the difference in
  generated code; cache and branch-predictor effects were not measured.
- **The ablation rows** show where the gains come from on integer code.
  Turning off flag elimination alone costs 4.9×. Turning off linking alone
  costs 8.7×, because every block exit then goes back through the C
  dispatcher.
- **The floating-point gains** come mainly from the x87/XMM register caches.
  Before the caches, the same build measured 6.59 (x87) and 5.03 (SSE)
  instructions per x86 instruction.

## Limitations

- **No GPU, audio or input emulation.** NV2A, APU and USB register accesses
  are trapped and answered by small stubs. Push buffers are consumed but not
  rendered. Running games visibly needs an NV2A implementation, which is a
  project of its own.
- **Partial kernel:** about half of the kernel exports are implemented
  (above), enough for the dashboard and the CRT/XAPI/D3D start-up path.
- **No hardware interrupts.** Interrupts are not delivered to the guest, and
  DPCs and timers are driven by the scheduler.
- **Untested on real hardware:** xbt has only run under qemu so far.

## Source layout

| file | contents |
|---|---|
| `src/a64.h` | AArch64 instruction encoders |
| `src/x86dec.[ch]` | x86-32 decoder |
| `src/translate.c` | block translator: analysis, flag liveness, fusion, linking, address reuse |
| `src/jit_int.c` | integer instructions |
| `src/jit_x87.c` | x87 on the d8–d15 register cache |
| `src/jit_sse.c` | SSE on the v16–v23 cache, and MMX |
| `src/flags.c`, `src/helpers.c`, `src/fpu.c` | lazy flag evaluation, rare instructions, x87 state helpers |
| `src/runtime.c` | entry/exit stubs, dispatcher, block lookup, SMC handling, fault handler |
| `src/memory.c` | identity-mapped Xbox address space |
| `src/kernel.[ch]`, `src/kexports.h` | HLE kernel and its generated export table |
| `src/xbe.c` | XBE loader, device stubs, port I/O |
| `src/elfrun.c` | `--elf` runner for the benchmarks |
| `tests/` | differential tester, instruction generator, native runner, encoder check |
| `bench/` | benchmarks and the comparison script |
| `tools/gen_kexports.py` | export-table generator |
