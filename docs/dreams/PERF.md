# Performance: smooth frame pacing and high fps

Working directory: `$REPO` (your OpenDream checkout). Measurements were taken on the reference
machine (`TEST_HARDWARE.md`); numbers from another host are not comparable without saying so.

User goal (2026-11-15): "smooth frame pacing, with high fps" on the reference machine's CPU + GPU.
Frame-rate switches and their measurements are in `FPS.md`. This file is about *where the time
goes* and what to fix, from measurements.

## Current measured baseline — 2026-11-16

Run `20261116-130730`, `DREAMS_FPS=120`, private save, Mailbox, Precise readbacks, guards enabled:
- Offline-notice/menu screen only, steady window approximately 35–65 s.
- Mean render rate 93.0 FPS. Measured gameplay cadence 30.0 updates/s.
- Frame p50 10.1–10.4 ms, p99 13.8–14.1 ms. GPU busy approximately 47.5%.
- GpuComm used 72.7% of one CPU core. Main used 20.5%.
- Evidence: `$REPO/work/perf-visual-baseline/report.json`. No gameplay/editor or PS4-parity claim.

Current stack profile `20261116-132301` used the same native build and six bounded `eu-stack` snapshots:
- GpuComm: three snapshots in buffer synchronization/access, two in texture upload/copy, one in a fault trampoline.
- Main: three snapshots in Precise fault readback waits, two in guest execution, one in an event-queue wait.
- No GpuComm snapshot caught queue submission/wait. This does not prove that submission cost is zero.
- Six point samples are qualitative, not a time distribution. Attachments also perturb pacing.
- Evidence: `$REPO/work/perf-hotpath-profile/report.md`. Do not reuse pre-fault-fix stacks as the current profile.

The allocator-free backing-hash prototype passed isolated digest/reentrancy tests, but remains unmerged.
Its microbenchmark gain is small, and current samples do not establish frame-time relevance.
No TLS hash state, bulk-span lifetime change, or runtime speedup is accepted.
Next: measure existing volume-refresh and readback diagnostics before changing behavior.
`SHADPS4_DREAMS_VOLUME_TRACE` covers only PRT/large 3D images, not every uploaded texture.
A refresh/upload stack alone does not prove the same image repeats. Match image identity and increasing counters.

The wait and readback-span probes use private directories with persistent `arm.request` files.
Their timing/logging overhead is nonzero. Counts-only diagnostic runs are not FPS comparisons.
`waits.tsv` has no timestamps/window delimiters. Readback address counts can undercount when slots drop.
Do not enable `SHADPS4_DREAMS_SUBMISSION_SPAN_DIR` in a baseline: it calls `EndRendering()` on every submission,
even before arming, and can wait at shutdown. This changes the submission path.

## How to measure (no access to the user's screen needed)

- **Per-thread CPU:** during `$REPO/tools/agent-run.sh`, find the game process by its exe path
  (`readlink /proc/<pid>/exe` ends in `port/out/dreams-probe`; the main thread renames itself `Main`,
  so `pgrep -x dreams-probe` misses it), read `/proc/<pid>/task/*/stat` utime+stime twice, 5 s apart.
- **Stacks with symbols:** `eu-stack -p <pid> -n 16` works on sandboxed runs (elfutils). `perf` can't attach
  across the bubblewrap user namespace ("Failure to open any events"), and gdb attaches but
  can't resolve symbols there. Take 8–15 samples and count the top frames.
- **Frame times:** MangoHud for user runs (`DREAMS_MANGOHUD=1 mangohud ./run-dreams.sh`; it does
  not load in the sandbox); the game's own frame counters for agent runs (FPS.md). The port
  disables MangoHud by default because its Vulkan layer can leave the window on an older frame
  while `DREAMS_CAPTURE` still shows the newest rendered frame. Measure with the overlay on only
  when the comparison is overlay-on vs overlay-on.

## Findings (2026-11-15, v2.52, main menu, 30 fps default)

| Thread | CPU | What it does | Status |
|---|---|---|---|
| `OSC` | 103.8% | Dreams' Open Sound Control receive loop spinning: every libSceNet call failed instantly offline | **fixed** (22ef1f4: offline sockets wait) → 0.4% |
| `shadPS4:GpuComm` | 61–97% | shadPS4's PM4 command processor. Samples: 5/8 in `vkQueueSubmit` (kernel CS ioctl), 3/8 in `BufferCache::EnsureResident` → `radv_alloc_memory` → libdrm `amdgpu_vamgr_find_va` (allocating GPU memory during steady-state play; the VA hole list gets long) | open |
| `Main` | ~11% | Stalls in `bbgpu_handle_fault` → `BufferCache::ReadMemory`, spin-waiting (sched_yield) for GPU→CPU readbacks: the fork's `readbacks_mode=2` (Precise) | open |

At ~90–100 fps (DREAMS_FPS=120) GpuComm is the ceiling (FPS.md).

## Plan (ordered by expected gain for pacing, then fps)

1. **Readback stalls (pacing):** Main thread waits on GPU page faults. Measure frame-time spikes vs
   readback count; make readbacks cheaper (avoid a full flush+submit+wait per fault; batch; reuse the
   fork's EXTENDED_READBACK_CACHE/PREFETCH paths), or limit Precise mode to the memory ranges Dreams
   really reads back. Don't switch to Relaxed globally: the fork found it breaks sculpts.
2. **Submit count (GpuComm):** count `vkQueueSubmit` per frame; coalesce submissions (readback
   flushes, fault-buffer processing, per-dispatch flushes) into fewer, larger submits.
3. **Allocation churn (GpuComm):** `EnsureResident` allocating device memory during play: pre-size
   or pool residency chunks so steady state allocates nothing; fewer, larger VkDeviceMemory blocks
   (also fixes the libdrm VA-list walk).
4. **Frame pacing at the end of the frame:** **done in part:** `DREAMS_PRESENT=mailbox|fifo|immediate`
   in run-dreams.sh, default **mailbox** (the fork used Immediate, which tears and shows frame-time
   wobble directly). Still to do: a precise frame limiter at the target rate (sleep-then-spin;
   bbport/Lantern `NATIVE_FRAME_RATE.md`) and checking pacing with MangoHud's frame-time graph.
5. **Parallelism:** Lantern's parallel draw pipeline (`ref/lantern/engine/docs/parallel_gpu.md`,
   `parallel-stageb.md`) to split GpuComm's work, for >100 fps.
Measure every step with the same method (per-thread CPU, frame-time p99, fps) before and after.

## Frame statistics in every run (2026-11-15)

vcore prints `vcore frames: <fps>, frame time p50 / p99 / max ms, stutters (>1.5x p50)` every 5 s
(`VCORE_FRAME_STATS=<s>`, 0 = off; measured per game flip in Presenter::PrepareFrame via the
screenshot hook in vcore/shim/kernel_glue.cpp). Baseline, sandbox, main menu, v2.52:

| Mode | fps | p50 | p99 | max | stutters |
|---|---|---|---|---|---|
| default (30) | 30.0 | 33.3 | 33.5–36.2 | 33.6–36.3 | 0 / 150 |
| DREAMS_60HZ=1 | 60.0 | 16.7 | 18.2–18.3 | 18.3–18.8 | 0 / 300 |

Menu pacing is already even; heavy scenes (sculpts, Dreamiverse browsing) still need measuring.

## Progressive slowdown: uninitialized fault bitmap (2026-11-16)

The earlier 5 → <1 FPS slowdown is not adequately explained by another GPU client or a bad save.
Two new sandbox runs reproduced it with separate copies of the player's save:
`run-20261116-100826` and `-101443`. Frame times increased throughout each run.
The latter run already contained the zero-copy texture lookup below, so that lookup did not resolve the slowdown.

Read-only stacks (`$REPO/work/smooth-profile-stacks.out`) showed:

```
FaultManager::ProcessFaultBuffer
  -> BufferCache::ObtainBuffer
  -> BufferCache::EnsureResident
  -> radv_alloc_memory
  -> amdgpu_vamgr_find_va
```

`FaultManager` allocated its device-local fault bitmap without initialization.
Vulkan does not guarantee zero contents. The parser interpreted stale bits as requested guest pages.
Its overflow path then kept those requests for later batches. This produced unnecessary sparse allocations and increasing CPU cost.

The constructor now records a zero fill and a transfer-write → shader-read/write barrier.
No readback policy, shader route, guest behavior, or fault-overflow recovery changed.
The patch is in the `ref/shadps4` working tree. Backup: `backups/shadps4-vcore-fault-bitmap-init.patch`.

Results (same reference machine, isolated headless runner, guards enabled):

| Test | Result |
|---|---|
| Before clear, `DREAMS_FPS=240`, two runs | ~5 FPS initially, below 2 FPS later, below 1 FPS in the first run |
| After clear, `-101934`, 90 s | Menu steady-state ~93.6–94.8 FPS, p50 ~10.1 ms, p99 ~13.7–14.2 ms |
| After clear, default PS4 timing, `-102341`, 75 s | 30.0 render FPS, 30.0 gameplay updates/s, mean render dt 0.03349 s |
| High-FPS repeat, `-102752`, t=35–65 s | 94.4 render FPS, 30.0 gameplay updates/s, mean render dt 0.01062 s |
| Copy-mode control, `-103044`, t=35–65 s | 90.5 render FPS, 30.0 gameplay updates/s, mean render dt 0.01093 s |
| 60 FPS, `-103421`, t=50–90 s | 60.0 render FPS, 30.0 gameplay updates/s, steady-state p50/p99 ~16.7 ms |

The fixed runs had no fault-overflow warning, loop-guard trip, indirect clamp report, or device loss.
These tests cover the menu/offline notice, not editor or physics parity (`ACCURACY.md`).
The 60 FPS run injected a one-second Cross press at t=35 s. It did not dismiss the notice.
Do not count that run as homespace or editor coverage.

Regression: `port/tools/fault_buffer_test.cpp` runs the production parser on Mesa's CPU Vulkan device (lavapipe; skip it where the driver is missing).
Dirty initial memory produces phantom faults. The clear removes them. Real boundary-page faults remain valid.
Overflowed requests drain without loss or duplicates. `tests/run-dreams-tests.sh` runs this test when the generated shader and lavapipe are available.
This is a unit test, not a software-GPU game run.

## Texture backing pages: avoid redundant copies (2026-11-16)

`HashBackingMemory` previously copied 16 KiB into thread-local storage at each page lookup.
The runtime now lends complete pages from its permanent shared backing view.
The GPU-protected guest view is never used for this fast path.
Split mappings and private direct allocations keep the original copy path.
Returned pointers are read-only and consumed immediately, never cached across remaps.

`VCORE_BACKING_COPY=1` restores the old copy path for A/B measurements.
This switch does not disable bitmap initialization or any GPU safety guard.
The runtime and adapter tests cover alias content, protected pages, VMA boundaries, holes,
flexible memory, private fallback, invalid inputs, and explicit copy mode.
One paired comparison measured **94.4 FPS with borrowed pages versus 90.5 FPS with copying** (~4.3%).
Both kept bitmap initialization and all guards enabled. Gameplay remained at 30.0 updates/s.
End-of-run captures show the same notice, text, imp, and animated purple backdrop.
The images are not pixel-identical because animation state differs. No new visible defect was found in this scene.
This is one menu comparison, not an editor benchmark or a hardware accuracy claim.
