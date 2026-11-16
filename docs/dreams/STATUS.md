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
6. Video out opened; Hmd/VR, Ime keyboard, AppContent, Ajm (AAC = silent), PlayGo (all 6
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
| Ajm MP3/AAC instances are silent | `src/runtime_ajm.c` | Dreams creates AAC decoders at audio init | Port shadPS4 `ajm_aac.cpp` (fdk-aac; Fedora `fdk-aac-free-devel`) |
| `BB_SKIP_GAME_CHECK=1` | `run-dreams.sh` | bbport's check pins Bloodborne's image hash | Title-based Dreams check (decision D7) |
| GPU library = bbport's libbbgpu | `build-dreams.sh` | unblocked CPU-side work | vcore (milestone 6) |

## Numbers to track

| Metric | Value | Source |
|---|---|---|
| Distinct imports served by the return-0 fallback before the GPU stop | 67 | `STUBS.md` |
| Unresolved imports at load (`BB_LIST_UNRESOLVED=1`, `--cpu-only`) | 402 (incl. 120 GnmDriver/VideoOut that the GPU library provides when not `--cpu-only`) | `work/cpu-only-run4.log` |
| eboot imports | 812 across 63 libraries | `notes/dreams-imports.md` |

## Known risks

- Dreams fork fixes are gated on serial `CUSA04301` and ~135 shader hashes from v2.64 content.
  On v2.52 they may silently not apply. Moving to v2.64 (user's plan) mostly removes this.
- Async compute + GDS ordered-count correctness is still being worked on in the Dreams fork itself.
- 40-bit pointer packing: all host allocations visible to the guest must stay below 1 TiB.
- Stubs returning 0 can hide bugs: when something odd happens, check `STUBS.md` first, or run with
  `DREAMS_STRICT_IMPORTS=1`.
