# OpenDream — status

**OpenDream** — a native PC port of Dreams (CUSA08010) for the PS4.

Working directory: `$REPO` (your OpenDream checkout). Hardware is described generically; the
machine behind the measured numbers is the reference machine (`TEST_HARDWARE.md`).

Last updated: 2026-11-16. Game under test: CUSA08010 v2.52 (user moving to v2.64 later).

## Milestones (from `notes/bbport-reuse-plan.md`, adjusted)

| # | Milestone | State |
|---|---|---|
| 1 | Toolchain: bbport builds unmodified on Linux x86-64 (the reference machine runs Fedora, `TEST_HARDWARE.md`), `--vulkan-only` passes | **done** |
| 2 | Offline prep for Dreams: SELF → flat image, link libc/Fios2/JobManager prx | **done** |
| 3 | Runtime generalisation: NID-only resolvers, Dreams NID→name table, missing libc-init hooks | **done for boot** (more real implementations needed, see STUBS.md) |
| 4 | CPU-only boot: module inits return, game entry runs, reaches GPU/VideoOut init | **done** — `main()` runs, ~40 guest threads start, `GameUpdate` main loop thread created |
| 5 | Null GPU backend | **not needed**: bbport's libbbgpu was the interim backend; vcore (milestone 6) is the default now, and `DREAMS_GPU=bbport` keeps the legacy path |
| 6 | "vcore": GPU library from latest upstream shadPS4 video_core + shader_recompiler + gnmdriver + videoout behind bbport's runtime↔GPU seam | **done** (2026-11-15): `port/vcore` → `libvcore.so`, default in `build-dreams.sh`. See `VCORE.md` |
| 7 | Apply Dreams fork patches (ref/shadps4 branch `dreams`); extend `CUSA04301` gates to our title ID; shader-hash census | **mostly done**: vcore compiles branch `vcore` (= `dreams` + 2 native edits); `GameSerial()=CUSA08010` turns the fork's gates on; readbacks Precise + `SHADPS4_DREAMS_*` applied by `run-dreams.sh`. Fork shader routes now matched on any version by code hash / DOC signature (`VERSIONS.md`, 26 routes on v2.52, DOC fallbacks 22 -> 1). Open: 66 non-DOC fork hashes unmapped until a v2.64 `dd.shaders` is available; eboot byte patches not ported |
| 8 | First frame of main menu / homespace; then audio, pad (motion), saves (tutorial-skip save), intro video | **boot/menu/homespace done**; editor verification open |
| 9 | Optional: bbport memory model / performance work | **started**: opt-in 60 fps (`DREAMS_60HZ=1`) and 61-240 fps (`DREAMS_FPS=n`, ~90-100 fps measured in the menu) with correct motion speed, gameplay at 30 Hz; limit above ~90 is vcore's GPU command thread. `FPS.md` |

## What works right now

`./run-dreams.sh` (GPU on, **vcore**, `BB_GAME_PROFILE=none`). Steps 1-6 were first reached with
bbport's libbbgpu, step 7 is vcore:
1. Loader maps 187,712,088 bytes, applies 35,240 relocations.
2. libc.prx, libSceFios2.prx and libSceJobManager.prx initialisers return 0.
3. Game entry → `_init_env` → `main()`. Dreams prints its engine banner ("UNICORN", built
   2022-11-21, perforce 208537, FINAL build).
4. Memory systems: GeneralPurpose, Graphics, Imp, Audio and Social pools are allocated
   (~1.3 GB of direct memory).
5. GPU: Vulkan presenter on the reference machine's GPU (Mesa RADV there; `TEST_HARDWARE.md`),
   GPU clock calibrated, end-of-pipe labels written by the GPU.
6. Video out opened; Hmd/VR, Ime keyboard, AppContent, Ajm (AAC via fdk-aac when `./build-deps.sh` has built `out/libdreams-codecs.so`, else silent), PlayGo (all 6
   chunks local), all offline tutorial videos found, SaveData/Camera/Move/VideoPlayer/GameUpdate
   threads created.
7. With vcore (2026-11-15): `DS_ORDERED_COUNT` compute passes run (fork DOC paths), the intro
   video plays (AvPlayer), and the screen shows "Sony Interactive Entertainment presents" → the
   indreams.me card → the first-run **preferences screen ("✕ CONTINUE")**, animated, waiting for
   input, ~40-60 s after start. Same point as the reference emulator. No GPU assertion in 150 s.
   Logs `work/vcore-run3.log`, `work/vcore-run4.log`; screenshots via `VCORE_SCREENSHOT_EVERY`.
   (With `DREAMS_GPU=bbport` it still stops at `DS_ORDERED_COUNT`: `docs/dreams/logs-gpu-run1.txt`.)

`./run-dreams.sh --cpu-only` gets the same far on the CPU side and then idles (no GPU fences ever
signal), which the watchdog reports after `DREAMS_TIMEOUT` seconds.

## Frame rate and test boots (2026-11-15, branch `fps`)

`DREAMS_60HZ=1` gives 60 fps, and `DREAMS_FPS=120|240` gives ~90-100 fps in the main menu. The game's
render clock follows the real frame time, and gameplay stays at its 30 Hz worker (both measured in game
memory). `DREAMS_SKIP_INTRO=1` + `DREAMS_USER_DIR` reach the main menu in ~23 s with no input. The
guest thread `OSC` spins one core at 100% at any frame rate (open). Details and numbers: `FPS.md`.

## Latest manual gameplay status

The user reports Please Hug Me working correctly in manual testing.
The earlier cloud-screen report is no longer a confirmed current blocker.
The responsible change, save setup and timing mode are not established by that report.
Automated scene entry/checkpoints and original-PS4 physics/progression comparisons remain unverified.
See `SCENE_TESTS.md`; retain the earlier diagnostics as historical evidence.

**Comic Sands** (Mm 2D platformer): the sky-only defect is reproduced on v2.52 at default
PS4 timing. The pinned scene loads, shows its title and pencil, and reaches the jump prompt,
but the character and ground do not render. The rendering cause is unresolved; no fix is applied.
See `HANDOFF.md` for the reproducible headless route and captures.

## Current blocker

No hard boot blocker. **Correctness is the priority** (`ACCURACY.md`, user requirement 2026-11-16).
The malformed indirect-draw arguments remain unresolved. Guards prevent known hangs but do not
make those draws correct. Editor, physics, logic, save/reload, and original-PS4 comparisons remain open.

The progressive 5 → <1 FPS slowdown now has a concrete cause: an uninitialized GPU fault bitmap.
Its stale bits requested phantom sparse-residency allocations. The constructor now clears the
bitmap and orders that clear before shader access. Two pre-fix runs degraded below 2 FPS.
A fixed sandbox run sustained ~94 FPS in the menu, and a default-timing run measured
30.0 render FPS / 30.0 gameplay updates per second. See `PERF.md` and `WORKLOG.md`.

AvPlayer is runtime-side (`src/runtime_avplayer.c`, `out/libdreams-av.so`). The unused vcore
fallback now defaults OFF.

## Input, video, text entry (CPU runtime, 2026-11-15)

- **AvPlayer**: runtime-side (`src/runtime_avplayer.c` + `src/runtime_avplayer/av_glue.c` ->
  `out/libdreams-av.so`, built by `build-deps.sh` from system FFmpeg, dlopen'ed). The Mm logo
  animation plays fully (wall clock, Dreams fork rule) and Dreams goes on to the indreams.me card
  and the preferences screen, with and without vcore's bbport AvPlayer compiled in. Tutorial
  videos use the same path (not yet seen in a run: they play later in the game).
- **Pad motion**: real DS4/DualSense gyro + accelerometer through SDL3 (`src/runtime_pad/sensors.c`,
  "PadMotion" thread at ~500 Hz, orientation integrated, Dreams' tilt and bias correction
  settings honoured). **The imp follows motion** since the pad timestamp is on the process clock
  (`3a31dd3`; root cause in `notes/input-research.md`): verified headless with synthetic motion
  (`DREAMS_PAD_SYNTH_MOTION`, captures `work/captures/20261115-201803`), and the old clock gives a
  frozen imp (`work/captures/20261115-202118`). Still to confirm with the user's DS4.
- **Keyboard & mouse** (`docs/dreams/INPUT.md`): Dreams key profile (Space/Enter ✕, Backspace ○,
  WASD/IJKL sticks, ...), the mouse moves the imp (click the window to give it the mouse, F1
  releases), mouse buttons R1/R2/L1. Injected mouse motion moves the imp the right way in a headless
  run (`work/captures/20261115-202429`); the window's real mouse capture (relative mode on Wayland)
  can't be tested headless: pending the user.
- **IME**: `src/runtime_ime.c`. sceImeOpen text entry through the game window's title bar
  (Enter/Esc); USB keyboard events from SDL key events. Not yet exercised by the game in a run
  (Dreams opens it when naming a creation).

## Temporary behaviours to remove later

| What | Where | Why it's there | Proper fix |
|---|---|---|---|
| Unimplemented imports return 0 (logged once as `STUB:`) | `src/probe.c` `unresolved()` | shadPS4 parity; unblocks boot | Implement each entry in `STUBS.md` that matters; `DREAMS_STRICT_IMPORTS=1` shows the old stop |
| Ajm AAC needs the optional codec library; MP3 is always silent | `src/runtime_ajm.c`, `src/runtime_codecs.c` | AAC is decoded by fdk-aac from `out/libdreams-codecs.so` (`./build-deps.sh`, dlopen at first use). Without the library AAC is silent (logged: "SILENT decoder"). Dreams creates no MP3 instances so far | Have `build-dreams.sh` run `./build-deps.sh` (incremental, ~6 s clean) before linking; MP3 via minimp3 if Dreams ever needs it. Licence note: fdk-aac (Fraunhofer FDK licence, which the FSF considers GPL-incompatible) and artswap (GPL-3.0-or-later) stay in this separate, locally built library; shadPS4 bundles the same sources |
| RAW AAC = one access unit per Ajm run | `src/runtime_ajm.c` `aac_frame` | fdk's RAW transport can't split concatenated AUs; shadPS4 has the same limit | Only if Dreams submits several raw AUs in one job (check `BB_AUDIO_TRACE=1` for AAC results `0xc0000000`) |
| Audio3d mixes to stereo without spatialisation | `src/runtime_audio3d.c` | port of shadPS4's software Audio3d (object position/spread ignored, objects with no GAIN are silent) | shadPS4's OpenAL variant (`audio3d_openal.cpp`) if positional audio matters |
| IME text shows only in the window title; the window can't cancel text entry when the game closes the IME first | `src/runtime_ime.c`, `vcore/shim/window.cpp` | no on-screen keyboard/overlay in vcore | an overlay text box; `bbgpu_text_input_end()` in vcore (the runtime already calls it if exported, weak symbol) |
| Keyboard/mouse layout is a first guess (INPUT.md); mouse → imp sensitivity default 0.0004 rad/px (≈1:1 at 1080p) | `src/runtime_pad/bindings.c` `dreams_defaults`, `sensors.c` | basic keyboard & mouse | check each binding against Dreams' prompts with the user; tune `DREAMS_MOUSE_IMP_SENS` |
| Audit no-ops (`ok`) | `src/runtime_dreams_services.c` | 0 is right offline (profiler hooks, streaming/sharing, sanitizers, Neo mode off) | Revisit if a feature needs one (e.g. Neo mode for PS4 Pro resolution) |
| `BB_SKIP_GAME_CHECK=1` inside offline generators | `scripts/dreams_prepare.py` | Bypasses the inherited Bloodborne hash check only after the Dreams gate succeeds | Keep the version-independent Dreams gate in `run-dreams.sh`; the legacy flag cannot bypass it |
| Optional bbport AvPlayer fallback | `vcore/CMakeLists.txt` `VCORE_BBPORT_AVPLAYER` | Legacy comparison support only; default OFF. The runtime supplies AvPlayer first | Keep excluded from default builds |
| AvPlayer: no looping, trick speed, HLS; resolution changes mid-file drop frames | `src/runtime_avplayer.c` | Dreams doesn't import SetLooping/SetTrickSpeed | implement if a v2.64 import needs it |
| No ImGui overlay in vcore; presenter blits the frame | ref/shadps4 `vcore` branch, `vk_presenter.cpp` | upstream draws the game through Dear ImGui | Fine as is; add a small overlay later if wanted |

## Numbers to track

| Metric | Value | Source |
|---|---|---|
| Distinct imports served by the return-0 fallback before the GPU stop | 67 (GPU run, before the CPU audit) | `logs-gpu-run1.txt` |
| Same, `--cpu-only` (2026-11-15 audit) | 54 -> **17** (all 17 GnmDriver/VideoOut) | `STUBS.md`, `work/cpu-audit-base.log` -> `work/cpu-audit-2.log` |
| Non-GPU imports nothing resolves at load (`BB_LIST_UNRESOLVED=1`, `--cpu-only`) | 258 -> **154** (Np online, VR, camera, dialogs, Http...) | `$REPO/tools/unresolved_report.py` on the logs above |
| Unresolved imports at load (`BB_LIST_UNRESOLVED=1`, `--cpu-only`) | 402 (incl. 120 GnmDriver/VideoOut that the GPU library provides when not `--cpu-only`) | `work/cpu-only-run4.log` |
| eboot imports | 812 across 63 libraries | `notes/dreams-imports.md` |

## Known risks

- Dreams fork fixes are keyed on v2.64 shader hashes. `DREAMS_SIGNATURE_MATCH` (default on,
  VERSIONS.md) maps the 22 DOC kernels and 4 sculpt-pipeline shaders of any version that has the
  same code or ordered-count signature; 66 other fork hashes (glass/shadow mip fixes, linear-write
  ranges, diagnostics) still only apply on v2.64 until mapped.
- Async compute + GDS ordered-count correctness is still being worked on in the Dreams fork itself.
- 40-bit pointer packing: all host allocations visible to the guest must stay below 1 TiB.
- Stubs returning 0 can hide bugs: when something odd happens, check `STUBS.md` first, or run with
  `DREAMS_STRICT_IMPORTS=1`.
- The CPU audit (2026-11-15) could only exercise boot up to the point `--cpu-only` reaches (the main
  loop waits on GPU fences). Audio3d, AudioOut mastering, the error dialog etc. are first called
  later; they are unit-tested (`tests/run-dreams-tests.sh`) but not yet seen in a real run.
- AvPlayer, pad motion and IME are unit-tested (`tests/run-dreams-tests.sh`: generated MP4,
  SDL virtual gamepad with sensors, fake window); in real runs only the logo video and the DS4's
  sensor stream have been seen so far.

## GPU hang risk (2026-11-15)

Native runs hung the GPU twice (gfx ring timeout in `shadPS4:Present`; amdgpu fell back to a full
reset that kills the desktop). Cause not yet known (see WORKLOG incident entry). Until it is
understood: only one Dreams instance at a time (enforced by run-dreams.sh), every run is logged to
`work/logs/` and captured to `work/captures/`, and agents should save their work before long GPU runs.

## Open issues (2026-11-15, after merging `input` and `fps`)

- **OSC busy-loop fixed:** offline receive waits reduced its CPU usage from 103.8% to 0.4%
  of a core (`PERF.md`).
- **High fps ceiling ~90–100 fps:** limited by the shadPS4 GPU command thread (78% of a core at
  98 fps) while the GPU is ~44% busy. See FPS.md.
- **Patchy object rendering on 2.52:** fixed for the homespace behind the main menu by matching the
  fork's DOC routes on v2.52 (VERSIONS.md, A/B captures). Not yet checked in creation/editor scenes.
- **Untested by a human:** mouse capture on the user's Wayland desktop, the key layout,
  DREAMS_PROSPERO=1, whether
  moving objects look smooth at 60/120 or step at 30 Hz gameplay rate.
- New worktrees don't get the `third_party/LibAtrac9` submodule: run
  `git submodule update --init third_party/LibAtrac9` in the worktree before building.
