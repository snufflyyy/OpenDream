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
VCORE_SCREENSHOT_EVERY=10 ...    # PNG of the presented frame every 10 s -> work/vcore-user/screenshots/
VCORE_LOG_FILTER="*:info Render.Vulkan:debug" ...   # GPU log filter (shadPS4 syntax; default *:warning)
```
`run-dreams.sh` sets up the Dreams fork's emulator configuration for vcore: per-game config
`work/vcore-user/custom_configs/<SERIAL>.json` (readbacks Precise, ...), the `SHADPS4_DREAMS_*`
switches, private shader cache `work/vcore-shader-cache/`, `SHADPS4_USER_DIR=work/vcore-user`.
It opens a window: always wrap runs in `timeout -s KILL`.

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
