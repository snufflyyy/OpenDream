# Agent test runs (isolated, headless)

Working directory: `$REPO` (your Dreams-Recomp checkout). The game runs from `$REPO/port`, the
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
- **User priority:** `DREAMS_RUN_ROLE=agent`. run-dreams.sh lets one run at a time; an agent run
  exits **75** if anything is running (retry later, never override), and a *user* launch stops a
  running agent run (SIGTERM, then KILL after 20 s) and takes over.
- **No input devices**, so use synthetic input for input tests (e.g. `BB_TEST_VPAD=1`, the
  synthetic-motion diagnostics described in `$REPO/notes/input-research.md`).

Gameplay/editor coverage and the agent-facing regression harness contract: `SCENE_TESTS.md`.
A boot to the offline notice is not a gameplay or editor pass.

Limit: it's the same physical GPU. A GPU hang still resets the GPU for the whole desktop
(see WORKLOG incident 2026-11-15). Keep runs short and save work first.
