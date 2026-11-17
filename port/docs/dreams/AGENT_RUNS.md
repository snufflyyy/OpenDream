# Agent test runs (isolated, headless)

Working directory: `$REPO` (your OpenDream checkout). The game runs from `$REPO/port`, the
runner is `$REPO/tools/agent-run.sh`.

The user uses the same PC while agents test. **Agents must run the game only through
`tools/agent-run.sh`** (project root), never `run-dreams.sh` directly, the reference emulator, or
anything that opens a window, plays sound or reads the user's devices.

```
tools/agent-run.sh --seconds 90                         # boot test of ./port, captures every 500 ms
DREAMS_PORT_DIR=$PWD/port-xyz tools/agent-run.sh ...     # test another checkout (git worktree)
DREAMS_CAPTURE=200 tools/agent-run.sh --seconds 60 ...  # denser capture; any run-dreams.sh env works
```
Then: newest log `$REPO/work/logs/run-<id>.log`, newest capture `$REPO/work/captures/<id>/`,
review with `python3 -I $REPO/tools/capture_sheet.py timeline|sheet <dir>` (CAPTURE.md).

What it does:
- **Invisible display:** a private `weston --backend=headless --renderer=gl` (1920×1080) on its own
  `XDG_RUNTIME_DIR`. The game uses SDL's Wayland + Vulkan WSI, the same path as on the user's
  desktop session, but nothing is shown. (Wayland is the target platform: the user's choice.)
  Requires `weston` and `bubblewrap` on the host; see `TEST_HARDWARE.md` for the reference
  machine.
- **Sandbox (bubblewrap):** fresh minimal `/dev` with only `/dev/dri` (no `/dev/input`,
  `/dev/hidraw*`, `/dev/snd`: the user's DS4/keyboard are invisible); `/run/user` replaced (the
  user's PipeWire/Pulse/display-server sockets are unreachable); `--unshare-net` (offline
  guaranteed).
- **Silent audio:** `BB_AUDIO=none` (runtime's timer backend: real pacing, no device).
- **Resource limits:** systemd user scope with `CPUWeight=20 IOWeight=20 MemoryMax=20G`.
- **User priority:** `DREAMS_RUN_ROLE=agent`. All launches share one root-global `flock` in
  `DREAMS_RUN_LOCK_DIR` (default `$REPO/work`, independent of `DREAMS_WORK`); the wrapper forces
  the main checkout's lock directory even for `DREAMS_PORT_DIR`. An agent run exits **75** if
  anything is running (retry later, never override; `DREAMS_ALLOW_PARALLEL=1` with an agent role
  exits 2), and a *user* launch stops a running agent run (SIGTERM, then KILL after 20 s) and takes
  over. `.dreams-run.who` is JSON `{pid,start_ticks,role,since,dev,ino}`; takeover is pidfd-bound
  and only after the live pid start time, fd 9 and its FLOCK record match. Stale, legacy,
  unverifiable or replaced lock state fails closed with 75 and no signal after a bounded wait.
- **Timeouts:** keep `--seconds` comfortably above the inner `DREAMS_TIMEOUT` watchdog (for example
  `DREAMS_TIMEOUT=55` with `--seconds 90` or `120`) so the probe's watchdog fires first and the run
  shuts down; the outer `timeout -s KILL` can kill the wrapper and leave a scope process holding
  the lock. Before each run confirm no `dreams-probe` survived and the lock is free, otherwise the
  next launch exits 75.
- **No input devices**, so use synthetic input for input tests (e.g. `BB_TEST_VPAD=1`, the
  synthetic-motion diagnostics described in `$REPO/notes/input-research.md`).

Gameplay/editor coverage and the agent-facing regression harness contract: `SCENE_TESTS.md`.
A boot to the offline notice is not a gameplay or editor pass.

Limit: it's the same physical GPU. A GPU hang still resets the GPU for the whole desktop
(see WORKLOG incident 2026-11-15). Keep runs short and save work first.
