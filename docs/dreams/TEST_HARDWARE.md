# Test hardware: the reference machine and the real requirements

Normative docs are written for any Linux x86-64 host. This file is the only place that names
the machine the project has been measured on. Other docs say "the reference machine (this file)"
when a number depends on it.

Working directory: `$REPO` (the OpenDream project root; examples run from `$REPO/port` where
stated).

## The current reference machine (not a requirement)

Recorded 2026-11-15/16. Historical logs (`WORKLOG.md`, `HANDOFF.md`) may name it too; they are
not edited to stay current.

| Part | Value |
|---|---|
| Distribution | Fedora 44, x86-64 |
| CPU | AMD Ryzen 7 5700X (16 hardware threads) |
| RAM | 31 GB |
| GPU | AMD Radeon RX 7800 XT (RADV, Navi 32), Mesa 26.2.6 |
| Driver | Mesa RADV (Vulkan 1.3/1.4) |
| Desktop | Wayland (Hyprland compositor) |
| Display for runs | headless weston 1920x1080 in agent runs; the desktop's own output for user runs |
| Filesystem | ext4/btrfs on the project disk; `/tmp` is a 16 GB tmpfs |

Every FPS, frame-time, thread-CPU and build-time number in `FPS.md`, `PERF.md` and `STATUS.md`
was taken here unless the text says otherwise. Numbers from this machine are evidence, not a
target other hosts must reproduce exactly.

## What the port actually requires

Required:

- Linux x86-64 (the guest executable is x86-64 and runs natively).
- A Vulkan 1.3-capable GPU driver with 64-bit shaderInt64 (Mesa RADV and NVIDIA proprietary
  drivers are known to work; shadPS4's own requirements apply).
- Enough VRAM for Dreams' graphics memory (several GB; the reference machine reports ~47% GPU
  busy in the menu at 120 fps).
- 16 GB RAM or more recommended (the runtime maps ~1.3 GB of guest pools plus a 176 MiB image
  and host-side caches; the agent sandbox caps itself at 20 GB).
- Build tools: gcc/g++ (C11/C++23), CMake, Ninja, a Clang recent enough for shadPS4's C++23
  code, Vulkan loader and headers, SDL3, FFmpeg development packages, glslang, SPIRV-Tools.

Convenience, not required:

- A specific number of CPU cores. Builds and agent runs scale with cores (`VCORE_JOBS`,
  default 15; the vcore build takes ~6 min on the reference machine's 16 threads).
- A specific desktop compositor. User runs need a Wayland or X11 session; agent runs need
  `weston` and `bubblewrap` and do not touch the desktop at all.
- Mesa RADV specifically. NVIDIA works for the game; some driver-limit details in
  `GPU_HANG.md` are stated for RADV and are clamped per device at runtime.
- A specific distribution. `BUILD.md` gives the generic steps plus a Fedora example where
  package names differ.
- lavapipe (Mesa's CPU Vulkan device). Only used by optional GPU unit tests
  (`tools/fault_buffer_test.cpp`, `tools/loop_guard_test.cpp`); those tests skip themselves
  when it is missing.

## Reporting results from a different machine

State the host with the result; do not silently compare against the reference numbers:

1. Name the CPU, GPU, driver and driver version, RAM, and distribution (one line is enough).
2. Say whether the run was an agent run (`tools/agent-run.sh`, headless) or a user run, and
   which presentation mode and FPS mode was used.
3. Report what was measured (render FPS, gameplay updates/s, frame-time p50/p99, thread CPU,
   build time) and mark that it is from a different host when it is.
4. Do not add the new machine's model names to normative docs; add it to this file as an
   additional row/section if it becomes a second reference.
5. Absolute statements ("needs 16 cores", "only RADV works") are not accepted from a single
   host. Distinguish a driver requirement from one machine's observation.
