# Running

## Game files

The extracted game lives in `game/CUSA08010/` (eboot.bin, sce_module/{libc,libSceFios2,
libSceJobManager}.prx, sce_sys/, data/, shaders/, ...). It was extracted from the user's fpkg with
`tools/pkg_extract.py` (own extractor; fake-PKG keys read from shadPS4 v0.5.0's keys.h in
`tools/thirdparty/`). Verification:
- PFS image SHA-256 matched the pkg header digest;
- all 56,237 zlib blocks decompressed to 64 KiB with valid checksums;
- every file re-hashed against `game/CUSA08010.extract.json` (1,591 files, 0 mismatches);
- `sce_sys/` (21 files) comes from the PKG entry table, not the PFS, so it's not in the manifest.
The original .pkg was deleted afterwards at the user's request.

To use a different dump (e.g. v2.64), extract it to its own folder and set `DREAMS_GAME`.

## Commands

```
cd port
./run-dreams.sh --cpu-only                        # no GPU: stops before graphics
BB_LIST_UNRESOLVED=1 ./run-dreams.sh --cpu-only   # also list every import nothing resolves
DREAMS_TIMEOUT=30 ./run-dreams.sh --cpu-only      # watchdog: dump thread stacks after 30 s
```
`run-dreams.sh` re-runs the offline prep only when `eboot.bin` or `scripts/link_modules.py`
changed. Outputs go to `work/out/`; the guest's user dir (saves) is `work/user/`.

GPU (vcore, the default build), see `VCORE.md` §5:
```
BB_GAME_PROFILE=none DREAMS_TIMEOUT=0 timeout -s KILL 150 ./run-dreams.sh > ../work/run.log 2>&1
DREAMS_CAPTURE=1 ...              # small PNG of changed frames every 500 ms -> work/captures/<run>/ (CAPTURE.md)
VCORE_SCREENSHOT_EVERY=10 ...    # full-size PNG every 10 s -> work/vcore-user/screenshots/ (slow)
VCORE_LOG_FILTER="*:info Render.Vulkan:debug" ...   # GPU log filter (shadPS4 syntax; default *:warning)
```
`run-dreams.sh` sets up the Dreams fork's emulator configuration for vcore: per-game config
`work/vcore-user/custom_configs/<SERIAL>.json` (readbacks Precise, ...), the `SHADPS4_DREAMS_*`
switches, private shader cache `work/vcore-shader-cache/`, `SHADPS4_USER_DIR=work/vcore-user`.
It opens a window: always wrap agent runs in `timeout -s KILL`. Only one run at a time
(`flock` on `work/.dreams-run.lock`; a second launch exits 75). Every run logs to
`work/logs/run-<id>.log` and captures frames to `work/captures/<id>/` (1 s, `DREAMS_CAPTURE=0` off).

Exit codes from the loader: 20 = stopped at an unsupported import (message starts with `STOP:`),
21 = a runtime `STOP:` for an unimplemented case.

## Reading a STOP

```
STOP: first unsupported PS4 import: <NID>#<lib>#<mod> (index N)
Caller in linked module 0 (libc.prx): +0x790
```
- Look the NID up in `notes/dreams-imports.json` (name, library, shadPS4 file) or
  `grep -rn '"<NID>"' ref/shadps4/src/core/libraries`.
- The `#lib#mod` suffix is a per-executable letter code, not a stable library name.
- "Caller in linked module k" tells you which image called it (0 = libc.prx, 1 = Fios2,
  2 = JobManager; otherwise the eboot).

## Tutorial-skip save

`notes/tutorial-skip-savedata.json` (from the Dreams-on-PC Discord) skips the first-run tutorial,
which doesn't let you progress under emulation. In shadPS4 it goes to
`<shadPS4 user>/savedata/<TITLE_ID>/SYSTEM/savedata.json`. For the native port the equivalent will
be under `work/user/` once savedata works (milestone 8); details in `notes/tutorial-skip.md`.

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

At pad open the log says which gamepad is used and whether its gyro/accelerometer are on:
`Runtime: pad motion: using "PS4 Controller" (/dev/hidraw11, SDL id 5): gyro enabled (250 Hz), ...`.
`tools/sdl_pads.c` (build line in its header) lists SDL's gamepads with their sensors outside the game.


## Helper tools

All in `tools/`, none needs the game running:
- `tools/install_game.sh [--dry-run] [--delete-inputs] [--margin-gb N] PKG OUTDIR`: extract a `.pkg`
  (fpkg only; extract archives yourself first) into `OUTDIR` (e.g. `game/CUSA08010-v264`) with a
  disk-space preflight (pkg size + 5 GB margin), PFS digest check, SHA-256 re-verification against
  `OUTDIR.extract.json`, and a TITLE_ID/APP_VER/VERSION printout. `--delete-inputs` removes the pkg
  only after success. Then run with `DREAMS_GAME=<OUTDIR>`.
- `tools/collect_crash.sh [--minutes N] [--previous-boot]`: after a GPU hang or crash, copy the
  amdgpu devcoredump, kernel journal (full + amdgpu slice), newest run log tail, newest capture
  (index + last 20 PNGs), run owner, uname, vulkaninfo into `work/crash/<timestamp>/` with a
  README.txt of hang signatures. Run it first thing, before rebooting if possible.
- `python3 -I tools/clean_work.py [--apply] [--keep N] [--older-than DAYS]`: prune captures, run
  logs, mangohud output, scratch `work/*.log|*.out` and unused `work/agent-rt.*` dirs. Dry run
  unless `--apply`. Never touches `work/out`, `user`, `vcore-user`, `vcore-shader-cache`,
  `shadps4-ref`, `crash`.
