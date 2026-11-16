# Dreams port — status

Last updated: 2026-11-15. Game under test: CUSA08010 v2.52 (user moving to v2.64 later).

## Milestones (from `notes/bbport-reuse-plan.md`, adjusted)

| # | Milestone | State |
|---|---|---|
| 1 | Toolchain: bbport builds unmodified on Fedora, `--vulkan-only` passes | **done** |
| 2 | Offline prep for Dreams: SELF → flat image, link libc/Fios2/JobManager prx | **done** |
| 3 | Runtime generalisation: NID-only resolvers, Dreams NID→name table, missing libc-init hooks | **done for boot** (more real implementations needed, see STUBS.md) |
| 4 | CPU-only boot: module inits return, game entry runs, reaches GPU/VideoOut init | **done** — `main()` runs, ~40 guest threads start, `GameUpdate` main loop thread created |
| 5 | Null GPU backend | **skipped for now**: bbport's libbbgpu already works as a real GPU backend for Dreams up to the first unsupported shader op (see below) |
| 6 | "vcore": GPU library from latest upstream shadPS4 video_core + shader_recompiler + gnmdriver + videoout behind bbport's runtime↔GPU seam | **next** |
| 7 | Apply Dreams fork patches (ref/shadps4 branch `dreams`); extend `CUSA04301` gates to our title ID; shader-hash census | todo (needed for DS_ORDERED_COUNT) |
| 8 | First frame of main menu / homespace; then audio, pad (motion), saves (tutorial-skip save), intro video | todo |
| 9 | Optional: bbport memory model / performance work | todo |

## What works right now

`./run-dreams.sh` (GPU on, bbport's libbbgpu, `BB_GAME_PROFILE=none`):
1. Loader maps 187,712,088 bytes, applies 35,240 relocations.
2. libc.prx, libSceFios2.prx and libSceJobManager.prx initialisers return 0.
3. Game entry → `_init_env` → `main()`. Dreams prints its engine banner ("UNICORN", built
   2022-11-21, perforce 208537, FINAL build).
4. Memory systems: GeneralPurpose, Graphics, Imp, Audio and Social pools are allocated
   (~1.3 GB of direct memory).
5. GPU: Vulkan presenter on RX 7800 XT (RADV, Mesa 26.2.6), GPU clock calibrated, end-of-pipe
   labels written by the GPU.
6. Video out opened; Hmd/VR, Ime keyboard, AppContent, Ajm (AAC via fdk-aac when `./build-deps.sh` has built `out/libdreams-codecs.so`, else silent), PlayGo (all 6
   chunks local), all offline tutorial videos found, SaveData/Camera/Move/VideoPlayer/GameUpdate
   threads created.
7. First compute dispatch: the shader recompiler hits **`DS_ORDERED_COUNT`** (unknown opcode) →
   assertion → `STOP: GPU library assertion failed`. Log: `docs/dreams/logs-gpu-run1.txt`.

`./run-dreams.sh --cpu-only` gets the same far on the CPU side and then idles (no GPU fences ever
signal), which the watchdog reports after `DREAMS_TIMEOUT` seconds.

## Current blocker

`DS_ORDERED_COUNT` (GDS ordered append/consume, used by Dreams' compute passes). bbport's shader
recompiler (an older shadPS4 copy) doesn't implement it. The Dreams fork implements it
("ordered-count", "doc-prefix", "ordered-guards" builds). That's why we move to milestone 6/7: build
the GPU library from `ref/shadps4` branch `dreams`. A short-term alternative is to port only the
fork's DS_ORDERED_COUNT translation into bbport's `gpu/shader/` (105/110 files still match
upstream layout); this isn't the chosen direction (decision D2) but could be a quick experiment.

## Temporary behaviours to remove later

| What | Where | Why it's there | Proper fix |
|---|---|---|---|
| Unimplemented imports return 0 (logged once as `STUB:`) | `src/probe.c` `unresolved()` | shadPS4 parity; unblocks boot | Implement each entry in `STUBS.md` that matters; `DREAMS_STRICT_IMPORTS=1` shows the old stop |
| Ajm AAC needs the optional codec library; MP3 is always silent | `src/runtime_ajm.c`, `src/runtime_codecs.c` | AAC is decoded by fdk-aac from `out/libdreams-codecs.so` (`./build-deps.sh`, dlopen at first use). Without the library AAC is silent (logged: "SILENT decoder"). Dreams creates no MP3 instances so far | Have `build-dreams.sh` run `./build-deps.sh` (incremental, ~6 s clean) before linking; MP3 via minimp3 if Dreams ever needs it. Licence note: fdk-aac (Fraunhofer FDK licence, which the FSF considers GPL-incompatible) and artswap (GPL-3.0-or-later) stay in this separate, locally built library; shadPS4 bundles the same sources |
| RAW AAC = one access unit per Ajm run | `src/runtime_ajm.c` `aac_frame` | fdk's RAW transport can't split concatenated AUs; shadPS4 has the same limit | Only if Dreams submits several raw AUs in one job (check `BB_AUDIO_TRACE=1` for AAC results `0xc0000000`) |
| Audio3d mixes to stereo without spatialisation | `src/runtime_audio3d.c` | port of shadPS4's software Audio3d (object position/spread ignored, objects with no GAIN are silent) | shadPS4's OpenAL variant (`audio3d_openal.cpp`) if positional audio matters |
| Ime keyboard: open succeeds, no key events | `src/runtime_dreams_services.c` | USB keyboard text entry isn't wired | SDL keyboard -> OrbisImeEvent (editor work) |
| Audit no-ops (`ok`) | `src/runtime_dreams_services.c` | 0 is right offline (profiler hooks, streaming/sharing, sanitizers, Neo mode off) | Revisit if a feature needs one (e.g. Neo mode for PS4 Pro resolution) |
| `BB_SKIP_GAME_CHECK=1` | `run-dreams.sh` | bbport's check pins Bloodborne's image hash | Title-based Dreams check (decision D7) |
| GPU library = bbport's libbbgpu | `build-dreams.sh` | unblocked CPU-side work | vcore (milestone 6) |

## Numbers to track

| Metric | Value | Source |
|---|---|---|
| Distinct imports served by the return-0 fallback before the GPU stop | 67 (GPU run, before the CPU audit) | `logs-gpu-run1.txt` |
| Same, `--cpu-only` (2026-11-15 audit) | 54 -> **17** (all 17 GnmDriver/VideoOut) | `STUBS.md`, `work/cpu-audit-base.log` -> `work/cpu-audit-2.log` |
| Non-GPU imports nothing resolves at load (`BB_LIST_UNRESOLVED=1`, `--cpu-only`) | 258 -> **154** (Np online, VR, camera, dialogs, Http...) | `tools/unresolved_report.py` on the logs above |
| Unresolved imports at load (`BB_LIST_UNRESOLVED=1`, `--cpu-only`) | 402 (incl. 120 GnmDriver/VideoOut that the GPU library provides when not `--cpu-only`) | `work/cpu-only-run4.log` |
| eboot imports | 812 across 63 libraries | `notes/dreams-imports.md` |

## Known risks

- Dreams fork fixes are gated on serial `CUSA04301` and ~135 shader hashes from v2.64 content.
  On v2.52 they may silently not apply. Moving to v2.64 (user's plan) mostly removes this.
- Async compute + GDS ordered-count correctness is still being worked on in the Dreams fork itself.
- 40-bit pointer packing: all host allocations visible to the guest must stay below 1 TiB.
- Stubs returning 0 can hide bugs: when something odd happens, check `STUBS.md` first, or run with
  `DREAMS_STRICT_IMPORTS=1`.
- The CPU audit (2026-11-15) could only exercise boot up to the point `--cpu-only` reaches (the main
  loop waits on GPU fences). Audio3d, AudioOut mastering, the error dialog etc. are first called
  later; they are unit-tested (`tests/run-dreams-tests.sh`) but not yet seen in a real run.
- With the vcore build, the 18 `libSceAvPlayer` imports are unresolved (bbport's libbbgpu used to
  provide them): the intro/tutorial videos need them (GPU-library side).
