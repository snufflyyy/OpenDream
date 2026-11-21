# Running

Working directory: `$REPO/port` (`$REPO` = your OpenDream checkout, e.g.
`REPO=/path/to/OpenDream`). Paths below are relative to that directory unless they start with
`$REPO`.

## Game files

Build the port first (see `BUILD.md`). OpenDream takes an extracted game folder, not a `.pkg`,
archive or standalone `eboot.bin`. Use your own copy. The launcher looks in
`$REPO/game/CUSA08010/` by default. To use another folder, set `DREAMS_GAME` to its absolute path.
Point it at the folder that contains these files and directories directly:

```text
eboot.bin
sce_sys/param.sfo
sce_module/libc.prx
sce_module/libSceFios2.prx
sce_module/libSceJobManager.prx
data/
shaders/
```

The reference game is CUSA08010 v2.52. Keep other versions in separate folders. See `VERSIONS.md`
for shader compatibility. The optional `tools/pkg_extract.py` is a development tool with separate
prerequisites; it is not part of the normal build or launch.

## Commands

```
cd port
./run-dreams.sh --cpu-only                        # no GPU: stops before graphics
BB_LIST_UNRESOLVED=1 ./run-dreams.sh --cpu-only   # also list every import nothing resolves
DREAMS_TIMEOUT=30 ./run-dreams.sh --cpu-only      # watchdog: dump thread stacks after 30 s
```
`run-dreams.sh` validates the Dreams dump before save setup. It then checks the preparation
cache under the single-instance lock. The cache hashes the game inputs, Python generators,
and required outputs. Changing game folders, modules, metadata, or generators triggers a rebuild,
even when timestamps stay unchanged. Failed rebuilds leave no valid manifest.
Outputs and `dreams-prep.json` go to `$REPO/work/out/`. Guest saves remain in `$REPO/work/user/`.

Check cache freshness without rebuilding or running the game:

```sh
python3 -I scripts/dreams_prepare.py "$REPO/game/CUSA08010" --out "$REPO/work/out" --check
```

Exit 0 means current, 1 means stale, and 2 means invalid input or a preparation error.
The first launch after this cache update rebuilds the offline outputs.
Keep game folders unchanged during launch and execution. Install another version in a separate
folder, then set `DREAMS_GAME` to that folder.

The cached `analysis.json` also contains diagnostic directory counts. Asset-only directory changes
can leave those counts stale without changing the prepared executable. Removing only
`$REPO/work/out/dreams-prep.json` forces regeneration on the next launch.

GPU (vcore, the default build), see `VCORE.md` §5:
```
BB_GAME_PROFILE=none DREAMS_TIMEOUT=0 timeout -s KILL 150 ./run-dreams.sh > ../work/run.log 2>&1
DREAMS_CAPTURE=1 ...              # small PNG of changed frames every 500 ms -> $REPO/work/captures/<run>/ (CAPTURE.md)
VCORE_SCREENSHOT_EVERY=10 ...    # full-size PNG every 10 s -> $REPO/work/vcore-user/screenshots/ (slow)
VCORE_LOG_FILTER="*:info Render.Vulkan:debug" ...   # GPU log filter (shadPS4 syntax; default *:warning)
```
`run-dreams.sh` sets up the Dreams fork's emulator configuration for vcore: per-game config
`$REPO/work/vcore-user/custom_configs/<SERIAL>.json` (readbacks Precise, ...), the `SHADPS4_DREAMS_*`
switches, private shader cache `$REPO/work/vcore-shader-cache/`, `SHADPS4_USER_DIR=$REPO/work/vcore-user`.
It opens a window: always wrap agent runs in `timeout -s KILL`. Only one run at a time: a
root-global `flock` in `DREAMS_RUN_LOCK_DIR` (default `$REPO/work`, see *Run lock* below); a second
launch exits 75. Every run logs to
`$REPO/work/logs/run-<id>.log` and captures frames to `$REPO/work/captures/<id>/` (1 s, `DREAMS_CAPTURE=0` off).

## Run lock (`DREAMS_RUN_LOCK_DIR`)

All runs on the machine share one lock. `DREAMS_RUN_LOCK_DIR` (default `$REPO/work`) holds
`.dreams-run.lock` (the `flock`) and `.dreams-run.who`; it defaults to the main checkout's `work`,
so a private `DREAMS_WORK` (test data dir, private cache copies) never creates a second GPU owner.
A second checkout launched directly computes its own `$REPO/work`, so pass `DREAMS_RUN_LOCK_DIR`
explicitly there and never rely on the default. Agent runs cannot opt out: `DREAMS_ALLOW_PARALLEL=1` with an agent role
exits 2, and `$REPO/tools/agent-run.sh` forces the main checkout's lock directory even when
`DREAMS_PORT_DIR` selects another port checkout.

`.dreams-run.who` is JSON: `{pid, start_ticks, role, since, dev, ino}`. It is only a hint that the
holder may be an agent. A user launch stops an agent only after the live `/proc` pid start time,
the holder's fd 9 and its FLOCK record all match the published lock identity, and the signal is
pidfd-bound. Stale, legacy or unverifiable metadata fails closed with exit 75 and no signal after
a bounded wait; so does a lock path that no longer names the regular file fd 9 locked. That check
runs at acquisition time: deleting or replacing `.dreams-run.lock` while a run holds it is
unsupported (no project tool does), and a run that starts in that window would not be refused.

For a bounded run keep the outer `$REPO/tools/agent-run.sh --seconds` limit comfortably above the
inner `DREAMS_TIMEOUT` watchdog (for example `DREAMS_TIMEOUT=55` with `--seconds 90` or `120`) so
the probe's own watchdog fires first and shuts down cleanly. Before each run confirm no leftover
`dreams-probe` and that the lock is free; a surviving probe keeps the lock and the next run exits 75.

Exit codes from the loader: 20 = stopped at an unsupported import (message starts with `STOP:`),
21 = a runtime `STOP:` for an unimplemented case.

## Reading a STOP

```
STOP: first unsupported PS4 import: <NID>#<lib>#<mod> (index N)
Caller in linked module 0 (libc.prx): +0x790
```
- Look the NID up in `$REPO/notes/dreams-imports.json` (name, library, shadPS4 file) or
  `grep -rn '"<NID>"' ref/shadps4/src/core/libraries`.
- The `#lib#mod` suffix is a per-executable letter code, not a stable library name.
- "Caller in linked module k" tells you which image called it (0 = libc.prx, 1 = Fios2,
  2 = JobManager; otherwise the eboot).

## Tutorial-skip save

`$REPO/notes/tutorial-skip-savedata.json` (from the Dreams-on-PC Discord) skips the first-run
tutorial, which doesn't let you progress under emulation. In shadPS4 it goes to
`<shadPS4 user>/savedata/<TITLE_ID>/SYSTEM/savedata.json`. For the native port the equivalent will
be under `$REPO/work/user/` once savedata works (milestone 8); details in
`$REPO/notes/tutorial-skip.md`.

## Runtime switches for video, pad motion and text entry

| Env | Effect |
|---|---|
| `DREAMS_AVPLAYER_TRACE=1` | AvPlayer calls, events and every 300th video frame |
| `DREAMS_AVPLAYER_OFF=1` | AddSource fails: Dreams skips its videos |
| `DREAMS_AVPLAYER_SYNC_READY=1` | StateReady from AddSource itself (bbport's order) instead of the controller thread |
| `SHADPS4_DREAMS_SKIP_LOGO_VIDEO=1` | the Mm logo video ends at once (Dreams fork switch) |
| `DREAMS_AV_LIB=<path>` | AvPlayer decoder library (default `out/libdreams-av.so` next to dreams-probe) |
| `DREAMS_PAD_SENSOR_TRACE=1` | raw SDL gyro/accel samples and what the game reads (first 20, then every 2 s) |
| `DREAMS_PAD_SENSORS=0` | ignore the controller's motion sensors |
| `DREAMS_PAD_READ_TRACE=1` | every 600th scePadRead: game-facing timestamp, orientation, angular velocity |
| `DREAMS_PAD_SYNTH_MOTION=<deg>[,<s>[,<s>]]` | test aid: synthetic figure-eight controller motion (agent runs, no controller) |
| `DREAMS_INPUT_PROFILE`, `DREAMS_MOUSE_MODE`, `DREAMS_MOUSE_IMP_SENS` | keyboard/mouse layout and where the mouse goes: see `INPUT.md` |
| `DREAMS_IME_TEXT=<utf-8>` | sceImeOpen enters this text and confirms at once |
| `DREAMS_IME_KEYBOARD=0` | no USB keyboard events to the game |
| `DREAMS_RENDER_RESOLUTION=<WxH>` | experimental internal scene resolution (`1280x720`); default unset = off, see *Internal render resolution* below |

At pad open the log says which gamepad is used and whether its gyro/accelerometer are on:
`Runtime: pad motion: using "PS4 Controller" (/dev/hidraw11, SDL id 5): gyro enabled (250 Hz), ...`.
`$REPO/tools/sdl_pads.c` (build line in its header) lists SDL's gamepads with their sensors outside the game.


## Helper tools

All in `$REPO/tools/` (the project-root tools, not `port/tools/`), none needs the game running:
- `$REPO/tools/install_game.sh [--dry-run] [--delete-inputs] [--margin-gb N] PKG OUTDIR`: extract a `.pkg`
  (fpkg only; extract archives yourself first) into `OUTDIR` (e.g. `game/CUSA08010-v264`) with a
  disk-space preflight (pkg size + 5 GB margin), PFS digest check, SHA-256 re-verification against
  `OUTDIR.extract.json`, and a TITLE_ID/APP_VER/VERSION printout. `--delete-inputs` removes the pkg
  only after success. Then run with `DREAMS_GAME=<OUTDIR>`.
- `$REPO/tools/collect_crash.sh [--minutes N] [--previous-boot]`: after a GPU hang or crash, copy the
  amdgpu devcoredump, kernel journal (full + amdgpu slice), newest run log tail, newest capture
  (index + last 20 PNGs), run owner, uname, vulkaninfo into `$REPO/work/crash/<timestamp>/` with a
  README.txt of hang signatures. Run it first thing, before rebooting if possible.
- `python3 -I $REPO/tools/clean_work.py [--apply] [--keep N] [--older-than DAYS]`: prune captures, run
  logs, mangohud output, scratch `$REPO/work/*.log|*.out` and unused `$REPO/work/agent-rt.*` dirs.
  Dry run unless `--apply`. Negative or nonfinite retention values are rejected.
  Symlinked candidates and category directories are skipped. Never touches `$REPO/work/out`,
  `user`, `vcore-user`, `vcore-shader-cache`, `shadps4-ref`, `crash`.
  Safe deletion requires no-follow directory descriptors and symlink-resistant FD-relative `rmtree`.
  Unsupported platforms fail closed. Keep agent testing stopped during `--apply`: `/proc` in-use
  detection is best effort. This is not a sandbox against direct same-user renames or bind mounts.

## Frame rate and faster boots (docs/dreams/FPS.md)

| Env | Effect |
|---|---|
| `DREAMS_PRESENT=mailbox\|fifo\|immediate` | presentation mode (default mailbox: no tearing, low latency; immediate = the fork's original) |
| `DREAMS_MANGOHUD=1` | keep MangoHud loaded. **Default: the port disables MangoHud** because its Vulkan layer can leave the window on an older frame while the game renders the newest one (the frame capture still shows the newest frame); all other `LD_PRELOAD` entries are kept |
| `DREAMS_60HZ=1` | 60 fps (the Dreams fork's 60 Hz rendering pair, found by signature on 2.52 and 2.64); gameplay stays 30 Hz |
| `DREAMS_FPS=<61..240>` | the same with a 1/n render-clock floor and an n Hz vblank (~90-100 fps in the menu on the reference machine, `TEST_HARDWARE.md`) |
| `DREAMS_PROSPERO=1` | `sceKernelIsProspero` = 1: Dreams' own 60 fps PS5 UI while browsing the Dreamiverse |
| `DREAMS_SKIP_INTRO=1` | logo video ends at once; in a separate `DREAMS_USER_DIR` the tutorial flags are set, so Dreams goes straight to the main menu (~23 s) |
| `DREAMS_USER_DIR=<dir>` | separate guest user dir (saves), seeded once from a copy of `$REPO/work/user`; use it for test runs |
| `DREAMS_TUTORIAL_SKIP_MY_SAVE=1` | with `DREAMS_SKIP_INTRO=1`: also edit your own save (backup first) |

## Internal render resolution (`DREAMS_RENDER_RESOLUTION`, experimental)

`DREAMS_RENDER_RESOLUTION=<WxH>` makes the game draw its internal scene target at that extent
instead of the extent it picks itself. It is off by default: with the variable unset,
`run-dreams.sh` adds no argument, runs no generator and writes no file, so the launch is unchanged.

| Value | Effect |
|---|---|
| (unset) | off: the game's own resolution |
| `1280x720` | the verified value: internal passes that depend on the engine's default dimension record are drawn at 1280x720 (about 2/3 of the game's own extent in the tested screens) |
| `1920x1080` | explicit no-op: writes the game's own default record back |

Other 16:9 values between 640x360 and 1920x1080 are rejected until they are verified, and so are
malformed or non-16:9 values. The host window and the presented image keep their size: only the
internal drawing extent changes, so the scene is upscaled into the unchanged output. No window or
output scaling is involved.

How it works and what is left untouched:

- `port/tools/dreams_render_resolution.py` locates the engine's dimension constructor in
  `$DREAMS_GAME/eboot.bin` offline (SELF/ELF parsing, `GNU_EH_FRAME` function bounds, decoded
  instructions, NID-anchored `sceVideoOutSetBufferAttribute`/`sceVideoOutRegisterBuffers` calls)
  and emits a 56-byte `BBPATCH2` file: one 16-byte data write at the constructor's shared default
  dimension vector. The loader applies it through its existing `--patches` path, after relocation
  and in memory only. The game folder, the prepared outputs and the shader cache are not modified.
  Generation needs GNU binutils (`objdump`) and runs only when the variable is set.
- The patch and its JSON report are generated for each run into
  `$REPO/work/out/render-resolution/<run id>/` (`render-resolution.bbpatch`,
  `render-resolution.json`). They are published exclusively: an existing patch or report is never
  overwritten, so a stale artifact cannot be reused by a later run.
- An explicit `--patches` forwarded to `run-dreams.sh` is refused with exit 2 before preparation:
  the loader takes one patch file per run, and silently dropping or merging another one would hide
  a requested patch.
- Any generator error, an unsupported value or a missing generated patch stops the run with a
  non-zero exit before the game starts. There is no fallback launch without the patch.

Evidence and limits (from the bounded experiment on the CUSA08010 v2.52 dump; details in
`$REPO/work/render-resolution/experiment-results.md`): the internal scene plus its dependent pass
were drawn at about 2/3 of the baseline extent in the boot/preferences screen, Please Hug Me
gameplay and Dream Shaping editor entry, with unchanged 30 fps cadence, no guard or device loss and
coherent captures; the final composite stayed 1920x1080. Sculpting and painting, other scenes,
deeper editor workflows and original-PS4 comparison remain unverified. The same default vector also
feeds a mode-15 load, an allocation floor and a comparison, so treat this as experimental.

## Game versions (docs/dreams/VERSIONS.md)

| Env | Effect |
|---|---|
| `DREAMS_SIGNATURE_MATCH=1` (default) | the Dreams fork's shader fixes (keyed on v2.64 shader hashes) also apply to other versions: shaders are matched by code hash / ordered-count signature (`Dreams route:` log lines). `=0` = old behaviour (on v2.52: 22 DOC kernels on the atomic fallback, blocky sculpts) |

