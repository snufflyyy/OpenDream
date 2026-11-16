#!/usr/bin/env bash
# Isolated test run of the native Dreams port for agents (docs: port/docs/dreams/AGENT_RUNS.md).
#
#   tools/agent-run.sh [--seconds N] [run-dreams.sh args...]      (default 90 s)
#   DREAMS_PORT_DIR=<dir> tools/agent-run.sh ...   run another port checkout (e.g. a diagnostic
#                                                   git worktree) instead of ./port
#
# Why: the user works on this PC while agents test. An agent run must not show a window, play
# sound, read the user's controller/keyboard, go online, or hog the machine. It runs:
#   - inside a private, invisible Wayland compositor (weston --backend=headless --renderer=gl) on
#     its own XDG_RUNTIME_DIR, so the game uses the same SDL Wayland + Vulkan WSI path as on the
#     user's Hyprland desktop, but nothing appears on screen;
#   - in a bubblewrap sandbox with a fresh minimal /dev (only /dev/dri is passed through: no
#     /dev/input, /dev/hidraw*, /dev/snd), no network, and the private runtime dir (the user's
#     PipeWire/Pulse/wayland-1 sockets aren't reachable); SDL audio goes to the dummy driver;
#   - in a systemd user scope with low CPU/IO weight and a memory cap;
#   - as DREAMS_RUN_ROLE=agent: run-dreams.sh refuses to start while anything else runs (exit 75)
#     and a user launch stops this run (the user always wins).
# Frame capture is on (500 ms) so the agent can see the run: tools/capture_sheet.py.
# Saves: agent runs use their own guest user dir (DREAMS_USER_DIR, default work/agent-user, seeded
# once from a copy of the user's work/user) and DREAMS_SKIP_INTRO=1 (main menu in ~23 s). The game
# writes its SYSTEM save while running, so agents must never run against the user's work/user.
# Limit: it's still the same physical GPU. A GPU hang can still reset the GPU for everyone.
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "$0")/.." && pwd)
SECONDS_MAX=90
if [[ ${1:-} == --seconds ]]; then SECONDS_MAX=$2; shift 2; fi
WORK=${DREAMS_WORK:-$ROOT/work}
RUN_DIR=$(mktemp -d "$WORK/agent-rt.XXXXXX")       # private XDG_RUNTIME_DIR for weston + game
chmod 700 "$RUN_DIR"
SOCKET=wayland-dreams
cleanup() {
    [[ -n ${WESTON_PID:-} ]] && kill "$WESTON_PID" 2>/dev/null || true
    wait "${WESTON_PID:-}" 2>/dev/null || true
    rm -rf "$RUN_DIR"
}
trap cleanup EXIT

# Common sandbox: whole filesystem as usual (the project lives in $HOME), but a fresh /dev with
# only the GPU, private /run/user runtime dir, no network.
SANDBOX=(bwrap --bind / / --dev /dev --dev-bind /dev/dri /dev/dri --tmpfs /run/user
         --bind "$RUN_DIR" "$RUN_DIR" --unshare-net --die-with-parent)
SCOPE=(systemd-run --user --scope --quiet --collect -p CPUWeight=20 -p IOWeight=20 -p MemoryMax=20G
       -p MemorySwapMax=0)

# 1. Invisible compositor.
env -i HOME="$HOME" PATH="$PATH" XDG_RUNTIME_DIR="$RUN_DIR" \
    "${SANDBOX[@]}" weston --backend=headless --renderer=gl --width=1920 --height=1080 \
    --socket="$SOCKET" --idle-time=0 --no-config --log="$RUN_DIR/weston.log" &
WESTON_PID=$!
for _ in $(seq 1 100); do [[ -S $RUN_DIR/$SOCKET ]] && break; sleep 0.1; done
if [[ ! -S $RUN_DIR/$SOCKET ]]; then
    echo "agent-run.sh: headless weston did not start:" >&2; cat "$RUN_DIR/weston.log" >&2 2>/dev/null; exit 1
fi

# 2. The game, sandboxed, against that compositor only.
# Order matters: systemd-run talks to the user's session bus (needs the real XDG_RUNTIME_DIR), so
# the scope is created first and the private runtime dir is switched in inside it.
# Audio: BB_AUDIO=none = runtime_audio.c's silent timer backend (real pacing, no device opened).
set +e
timeout -s KILL "$SECONDS_MAX" "${SCOPE[@]}" \
    env -u DISPLAY -u WAYLAND_DISPLAY -u PULSE_SERVER -u PIPEWIRE_REMOTE -u DBUS_SESSION_BUS_ADDRESS \
    XDG_RUNTIME_DIR="$RUN_DIR" WAYLAND_DISPLAY="$SOCKET" SDL_VIDEO_DRIVER=wayland \
    SDL_AUDIO_DRIVER=dummy BB_AUDIO=none DREAMS_RUN_ROLE=agent BB_GAME_PROFILE=none \
    DREAMS_CAPTURE="${DREAMS_CAPTURE:-500}" DREAMS_TIMEOUT="${DREAMS_TIMEOUT:-0}" \
    DREAMS_USER_DIR="${DREAMS_USER_DIR:-$WORK/agent-user}" DREAMS_SKIP_INTRO="${DREAMS_SKIP_INTRO:-1}" \
    "${SANDBOX[@]}" "${DREAMS_PORT_DIR:-$ROOT/port}/run-dreams.sh" "$@"
status=$?
set -e
# A timeout can kill the wrapper while the scope's game process survives. The game inherits the
# single-instance flock (run-dreams.sh fd 9), so a survivor blocks every later run. Kill only a
# game whose --user is THIS run's private dir; the user's own run can never match.
private_user=${DREAMS_USER_DIR:-$WORK/agent-user}
for pid in $(pgrep -f 'dreams-probe' 2>/dev/null || true); do
    if tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null | grep -q -- "--user $private_user"; then
        kill -KILL "$pid" 2>/dev/null || true
    fi
done
echo "agent-run.sh: exit $status (124/137 = stopped at the ${SECONDS_MAX}s limit; 75 = GPU busy, retry later)" >&2
echo "agent-run.sh: newest log: $(ls -t "$WORK"/logs/run-*.log 2>/dev/null | head -1)" >&2
echo "agent-run.sh: newest capture: $(ls -td "$WORK"/captures/*/ 2>/dev/null | head -1)" >&2
exit $status
