# Performance: smooth frame pacing and high fps

User goal (2026-11-15): "smooth frame pacing, with high fps" on a Ryzen 7 5700X + RX 7800 XT.
Frame-rate switches and their measurements are in `FPS.md`. This file is about *where the time
goes* and what to fix, from measurements.

## How to measure (no access to the user's screen needed)

- **Per-thread CPU:** during `tools/agent-run.sh`, find the game process by its exe path
  (`readlink /proc/<pid>/exe` ends in `port/out/dreams-probe`; the main thread renames itself `Main`,
  so `pgrep -x dreams-probe` misses it), read `/proc/<pid>/task/*/stat` utime+stime twice, 5 s apart.
- **Stacks with symbols:** `eu-stack -p <pid> -n 16` works on sandboxed runs. `perf` can't attach
  across the bubblewrap user namespace ("Failure to open any events"), and gdb attaches but
  can't resolve symbols there. Take 8–15 samples and count the top frames.
- **Frame times:** MangoHud for user runs (`mangohud ./run-dreams.sh`; doesn't load in the sandbox);
  the game's own frame counters for agent runs (FPS.md).

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
4. **Frame pacing at the end of the frame:** `present_mode=Immediate` (fork config) tears and lets
   frame times wobble. Offer FIFO/Mailbox plus a frame limiter at the target rate, with precise
   sleep-then-spin pacing (bbport/Lantern `NATIVE_FRAME_RATE.md`).
5. **Parallelism:** Lantern's parallel draw pipeline (`ref/lantern/engine/docs/parallel_gpu.md`,
   `parallel-stageb.md`) to split GpuComm's work, for >100 fps.
Measure every step with the same method (per-thread CPU, frame-time p99, fps) before and after.
