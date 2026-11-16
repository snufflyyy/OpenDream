# Seeing a run: test frame capture

Agents can't watch the game window, so a native run can record small images of what it presents.
Use this to verify boots and fixes and to find graphical glitches.

## Record

```
cd port
BB_GAME_PROFILE=none DREAMS_TIMEOUT=0 DREAMS_CAPTURE=1 timeout -s KILL 90 ./run-dreams.sh > ../work/run.log 2>&1
```
- `DREAMS_CAPTURE=1`: a frame every 500 ms; `DREAMS_CAPTURE=<ms>` for another interval (e.g. 100
  for a burst-like look at animation/flicker, 2000 for long soak runs).
- Output: `work/captures/<YYYYmmdd-HHMMSS>/`: `f<seq>_t<seconds>.png` (480 px wide) + `index.csv`
  (`seq,t_ms,saved,diff,file`; saved = 1 saved, 0 unchanged, -1 dropped because the writer was
  behind).
- Only frames that changed are saved: mean absolute luma difference vs the last saved frame >
  `VCORE_CAPTURE_DIFF` (default 1.0, scale 0–255), or at least every `VCORE_CAPTURE_KEEP_S` seconds
  (default 10). A 70 s boot gives ~120 captures, ~66 saved, ~3 MB.
- Saved frames print `CAPTURE #<seq> t=<s> diff=<d> <file>` into the run log, so you can see which
  log lines surround a frame. `t` counts from process start (library load).
- Other knobs: `VCORE_CAPTURE_WIDTH` (default 480), `VCORE_CAPTURE_DIR` (default set by
  run-dreams.sh).

## Review (cheap first, images only where needed)

```
python3 -I tools/capture_sheet.py latest                      # newest capture dir
python3 -I tools/capture_sheet.py timeline <dir>              # text: changes over time, static stretches
python3 -I tools/capture_sheet.py sheet <dir> --every 4       # overview: 4x4 sheet(s) of every 4th frame
python3 -I tools/capture_sheet.py sheet <dir> --from 20 --to 30   # all frames in a window
```
Sheets go to `<dir>/sheets/` (1920×1080 for 4×4 at 480 px). Each tile is labelled
`#seq t=… d=…`. Look at the timeline, then one overview sheet, then a window sheet or single frames
around anything odd. For fine detail, use the full-size `VCORE_SCREENSHOT_EVERY=<s>` instead
(shadPS4's own PNGs in `work/vcore-user/screenshots/`, slow: full-size encode on the presenter).

## How it works

- `port/vcore/shim/kernel_glue.cpp` `TimedScreenshot()`: with `VCORE_CAPTURE_MS` set, asks the
  presenter for a game-only screenshot every interval (it otherwise serves `VCORE_SCREENSHOT_EVERY`).
- `ref/shadps4` branch `vcore`, `video_core/renderer_vulkan/vk_presenter.cpp`
  `SavePendingScreenshot()`: a `// vcore:` hook calls `VideoCore::CaptureSink(rgba, w, h)`. The
  default definition there is **weak** and returns false, so the shadPS4 emulator build behaves as
  upstream.
- `port/vcore/shim/capture.cpp` defines the strong `CaptureSink`: it moves the pixels into a queue
  (max 4; never blocks the presenter) and a background thread box-downscales, diffs, writes the
  PNG (stb) and the index line.

## Uses

- Boot checks after a change: did it still reach the same screen at roughly the same time?
- Glitches: capture at 100–200 ms around the problem and compare tiles. Compare with the reference
  emulator (`tools/run-shadps4-reference.sh`, full-size screenshots) to tell port bugs from shared
  shadPS4 GPU behaviour.
- Future: scripted input replay (planned) for reproducible paths to a scene, and a frame-time
  CSV next to `index.csv` for performance A/B.
