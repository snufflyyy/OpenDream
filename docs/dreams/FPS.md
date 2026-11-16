# Frame rate (60 / 120 fps) and faster test boots

Working directory: `$REPO/port` (`$REPO` = your Dreams-Recomp checkout). Every measurement below
was taken on the reference machine (`TEST_HARDWARE.md`).

Status 2026-11-15, CUSA08010 v2.52, sandboxed agent runs (`$REPO/tools/agent-run.sh`: headless
weston 1920x1080, low CPU weight). Research and sources: `$REPO/notes/high-fps-research.md`.
Code: `vcore/shim/frame_rate.cpp`, `run-dreams.sh`, `scripts/dreams_seed_save.py`,
`src/runtime_dreams_services.c` (`kernel_is_prospero`).

## Switches (all off by default)

| Env | Effect | Where |
|---|---|---|
| `DREAMS_60HZ=1` | The Dreams fork's `FORCE_60HZ_RENDERING` pair: flip rate 0 + render-clock floor 1/60. **60 fps, correct motion speed, gameplay stays 30 Hz** (measured below) | `vcore/shim/frame_rate.cpp` via `bbgpu_patch_image` |
| `DREAMS_FPS=<61..240>` | The pair plus a render-clock floor of 1/n; run-dreams.sh also sets the vblank frequency to n (per-game config copy) | same + `run-dreams.sh` |
| `DREAMS_PROSPERO=1` | `sceKernelIsProspero` answers 1: Dreams' own PS5 path (2.28+: "we upped the UI to 60fps"). Only consulted while moving around the Dreamiverse UI (gate `[0x79c92ec]`), so a static menu stays 30 | `src/runtime_dreams_services.c` |
| `DREAMS_SKIP_INTRO=1` | The logo video ends at once (the fork's `SHADPS4_DREAMS_SKIP_LOGO_VIDEO`, which run-dreams.sh's `SHADPS4_*` clean-up otherwise removes); in a separate user dir also the tutorial flags (see below) | `run-dreams.sh` |
| `DREAMS_USER_DIR=<dir>` | Guest user dir (saves, download0) other than `work/user`; seeded once from a **copy** of `work/user` | `run-dreams.sh`, `scripts/dreams_seed_save.py` |
| `DREAMS_TUTORIAL_SKIP_MY_SAVE=1` | With `DREAMS_SKIP_INTRO=1`: apply the tutorial flags to the player's own save too, after a timestamped backup (`savedata.json.before-tutorial-skip-<time>`) | same |

Startup log lines to check: `vcore: Dreams frame rate 60: flip rate 0 at eboot +0xe34d73, render-clock
floor 1/60 at +0xe72d81 ...`, `run-dreams.sh: DREAMS_FPS=120: vblank_frequency 120`,
`Runtime: sceKernelIsProspero answers 1` (only once the game asks), `dreams_seed_save.py: ...`,
`AvPlayer: ... Mm-Dreams-logo.mp4 ... (Dreams logo: skipped)`.

## How Dreams paces (summary of the research)

- Every frame it calls `sceVideoOutSetFlipRate(handle, rate)` (eboot `+0xe34da7`), with rate 1
  (30 fps) on a base PS4 in 2D. The choice is `[flag30 0x58aa968]` (`+0xe34d73`). PS VR modes and
  PS5 UI mode pick 0.
- The render clock is measured wall time, smoothed and **floored** at 1/30 or 1/60 by the same flag
  (`+0xe72d81`, table `{1/30, 1/60}` at `0x16ff098`), stored at `0x26e4f90`. Faster frames than the
  floor would make render-side motion run fast; that is why the flip rate and the floor change together.
- Gameplay runs on an independent 30 Hz worker. Counter `0x6f738e8` (v2.52; the fork's "game FPS"
  counter is v2.64 `+0x4558688`) counts its updates: 30/s in every run below. The render frame
  counter is `0x5845980` (incremented once per frame next to the render clock).
- The patches are found by signature: opcodes fixed, displacements free, both sites must name the same
  flag, and the table must hold 1/30 and 1/60. They are applied as a pair or not at all, as in the
  fork, so they match v2.52 and v2.64 (the fork's offsets `+0xc2075e`/`+0xc502d1`).

## Measurements (main menu with the offline notice, 1920x1080 headless, no input)

Read from the running game (`/proc/<pid>/mem`, read-only, every 0.5 s; t=35-78 s):

| Run | Render frames/s | Gameplay updates/s (`0x6f738e8`) | Render dt (`0x26e4f90`) | Flip rate | GPU busy | Hottest threads |
|---|---|---|---|---|---|---|
| default (`20261115-204608`) | **30.0** | 30.0 | 0.03335 | 1 | 22% (clock mostly idle) | OSC 99%, GpuComm 29%, Main 7% |
| `DREAMS_PROSPERO=1` (`-202836`, sticks injected) | 30.0 | 30.0 | 0.03335 | 1 | – | gate never opened headless |
| `DREAMS_60HZ=1` (`-203034`) | **60.0** | 30.0 | 0.01668 | 0 | – | – |
| `DREAMS_FPS=120` (`-203155`) | **90.4** (boot screens 105.8) | 30.0 | 0.01105 | 0 | – | – |
| `DREAMS_FPS=240` (`-204425`) | **97.8** | 30.0 | 0.01010 | 0 | 44% at ~1.8 GHz | OSC 99%, shadPS4 GPU thread 78%, Main 21% |

- **The render clock matches measured menu cadence.** The render dt matches the real frame
  interval (1/60.0 = 0.01667, 1/90.4 = 0.01106, 1/97.8 = 0.01022).
  The captures show the same menu and notice animation at 60 and 120.
  This does not establish correct animation speed or simulation behavior in every creation.
- **Gameplay stays at 30 Hz** (updates/s unchanged). Whether objects driven by gameplay *interpolate*
  between updates at 60/120 or step at 30 can't be judged on a static menu. That needs a scene with
  moving objects (user test).
- **Above ~90-100 fps the limit is the CPU side of vcore, not the GPU.**
  - The shadPS4 GPU command thread goes from 29% of a core at 30 fps to 78% at 98 fps (≈8-10 ms of
    CPU per frame on one thread). The GPU stays under half busy.
  - The sandbox's `CPUWeight=20` and other load on the host lower these numbers somewhat.
  - Next steps: Lantern's parallel draw pipeline (`ref/lantern/engine/docs/parallel_gpu.md`) and
    presenting off the vblank thread.
- **OSC busy-loop fixed:** the runtime now waits on offline receives instead of failing instantly.
  Its CPU usage fell from 103.8% to 0.4% of a core (`PERF.md`).
- `DREAMS_PROSPERO=1` is plumbed, but no headless run reached the code that asks. The flip-rate
  decision only consults `sceKernelIsProspero` in UI mode while `[0x79c92ec]` is set
  (`+0xede364`: a positive UI value, "moving around the Dreamiverse"). Scripted sticks could not
  dismiss the offline notice to get there. It needs a user test in the Dreamiverse menus.
- MangoHud did not load in the sandbox (no CSV), hence the in-game counters above.

## Skip intro (DREAMS_SKIP_INTRO=1)

Research:
- The **Mm logo video** is skippable cleanly (fork switch, AvPlayer ends the stream). It saves ~12 s.
- The **indreams.me card** is timed (about 10 s, ends by itself; ✕ ends it early).
- The **preferences screen** ("Dreams comes with a host of preferences... ✕ CONTINUE") and the
  **Control Scheme popup** ("Wireless Controller (Motion Sensor Function)...") wait for ✕. They are
  the first-run boot flow (`DreamUIPageBootFlowOptionsScreen` in the eboot). They are **not** gated by an
  ini setting (`/app0/unicorn.ini` holds only CurrentSKU/DefaultLanguage/AgeRestriction; others are
  "not settable in final"). They **are** gated by the save's tutorial progress: with
  `played_tutorial_vo`/`played_tutorial_cutscene`/`progress_unlocked` set (the community
  tutorial-skip edit, `$REPO/notes/tutorial-skip.md`), Dreams goes from the indreams.me card straight to
  the main menu. The same flags also skip the first-time intro sequence ("The idea could be
  anything... Light... color...", ~40 s), which otherwise plays after the boot screens.

Result with a seeded dir (`20261115-200656`):
- t=6.9 s: Mm logo (still frame);
- t=11 s: indreams.me card;
- **t=22.9 s: main menu (Dreamiverse hub)**;
- ~t=28 s: the offline notice ("You are in offline mode and playing the Restricted Dreams
  Experience", back/✓ continue), which needs ✕ on every boot while offline.

Without the flags the same point needs ✕ three times (card, preferences, control scheme) and
comes after the intro sequence: ~95 s in `20261115-195936` (logo skipped, ✕ pressed by script).

The player's own `work/user` is never changed unless `DREAMS_TUTORIAL_SKIP_MY_SAVE=1` is set, and then a
backup is taken first. Timed ✕ injection through `BB_PAD_FILE` (tried in `20261115-195936`: ✕ at
the card, the preferences and the control scheme) works but was dropped in favour of the save flags.

## Accuracy constraint (2026-11-16)

Original-PS4 behavior takes priority over FPS (`ACCURACY.md`). High-FPS rendering remains opt-in.
The menu counters below establish update cadence, not full physics, logic, animation, or editor parity.
Do not describe 60/120 FPS as fully accurate until representative creations pass those checks.

## Recommended use

- Agents: `DREAMS_USER_DIR=$WORK/agent-user DREAMS_SKIP_INTRO=1` for every test run, so they
  never write the player's saves. Before this, agent runs and the user shared `work/user`, and the game
  writes its SYSTEM save during a run.
- Player: use default timing as the PS4 control. `DREAMS_60HZ=1` uses the fork's 60 FPS mode.
  `DREAMS_FPS=120` previously reached ~90–100 FPS in the menu on the reference machine
  (`TEST_HARDWARE.md`).
  Both modes need behavior checks in representative creations. A static menu is insufficient.
  Presentation now defaults to Mailbox (`PERF.md`), not Immediate.

## Next steps

1. User test of `DREAMS_60HZ=1` / `DREAMS_FPS=120` in homespace, edit mode and a creation with moving
   objects: do gameplay-driven objects step at 30 Hz? Do the camera, imp and UI feel right? Is audio in sync?
2. User test of `DREAMS_PROSPERO=1` while browsing the Dreamiverse.
3. Trace malformed indirect-draw arguments. A clamp prevents hangs but does not restore correct rendering.
4. vcore CPU cost per frame (GPU command thread), so heavy scenes and 120 fps get headroom. See the
   research notes, §6 stage B.
5. v2.64: the signatures already match the fork's offsets. Re-check the startup log line.
