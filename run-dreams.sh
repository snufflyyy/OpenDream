#!/usr/bin/env bash
# Dreams port: offline prep + launch.
#   ./run-dreams.sh [--cpu-only] [extra dreams-probe args]
# Env: DREAMS_GAME (default ../game/CUSA08010), DREAMS_WORK (default ../work),
#      DREAMS_TIMEOUT (watchdog seconds, default 0 = off), BB_LIST_UNRESOLVED=1 (list missing imports).
#      VCORE_LOG_FILTER (vcore GPU log filter, default "*:warning", shadPS4 log_filter syntax).
#      DREAMS_USER_DIR (guest user dir: saves, download0; default $DREAMS_WORK/user). Any other dir
#      is seeded once from a copy of $DREAMS_WORK/user (only read), so test runs never write the
#      player's saves.
#      DREAMS_SKIP_INTRO=1: faster boots for testing (default off), docs/dreams/FPS.md "Skip intro":
#      the Mm logo video ends at once, and in a DREAMS_USER_DIR other than the player's the save's
#      tutorial flags are set, so Dreams goes from the indreams.me card straight to its main menu
#      (no preferences screen, control-scheme popup or first-time intro sequence). The player's
#      own save is changed only with DREAMS_TUTORIAL_SKIP_MY_SAVE=1 (timestamped backup first).
#      Frame rate (docs/dreams/FPS.md): DREAMS_PROSPERO=1 (PS5: 60 fps menus), DREAMS_60HZ=1,
#      DREAMS_FPS=<60..240> (also sets the vblank frequency to that rate).
# See docs/dreams/RUNNING.md and docs/dreams/VCORE.md.
set -euo pipefail
cd -- "$(dirname -- "$0")"
ROOT=$(cd .. && pwd)
GAME=${DREAMS_GAME:-$ROOT/game/CUSA08010}
WORK=${DREAMS_WORK:-$ROOT/work}
OUT=$WORK/out
USERDIR=${DREAMS_USER_DIR:-$WORK/user}
mkdir -p "$OUT" "$WORK/logs"
RUN_ID=$(date +%Y%m%d-%H%M%S)
SERIAL=$(python3 -I -c 'import sys
d=open(sys.argv[1],"rb").read()
i=d.find(b"CUSA"); print(d[i:i+9].decode())' "$GAME/sce_sys/param.sfo")

# One Dreams run at a time (2026-11-15: a user run and an agent test run overlapped on the GPU,
# then the user's run hung the GPU and the amdgpu full reset took the desktop down). The lock is
# held on fd 9, which survives the exec below, so it lasts exactly as long as dreams-probe.
# DREAMS_ALLOW_PARALLEL=1 skips it (don't, unless you know why).
# Roles (DREAMS_RUN_ROLE): "user" (default: the person at the PC) or "agent" (set by
# tools/agent-run.sh: sandboxed, headless). The user always wins: a user launch stops a running
# agent run and takes the GPU; an agent launch while anything runs exits 75 (retry later).
ROLE=${DREAMS_RUN_ROLE:-user}
if [[ ${DREAMS_ALLOW_PARALLEL:-0} != 1 ]]; then
    exec 9>"$WORK/.dreams-run.lock"
    if ! flock -n 9; then
        holder=$(cat "$WORK/.dreams-run.who" 2>/dev/null || echo unknown)
        if [[ $ROLE == user && $holder == *"role agent"* ]]; then
            hpid=$(sed -E 's/^pid ([0-9]+).*/\1/' <<< "$holder")
            echo "run-dreams.sh: stopping the agent test run ($holder) so you can play" >&2
            kill -TERM "$hpid" 2>/dev/null || true
            if ! flock -w 20 9; then kill -KILL "$hpid" 2>/dev/null || true; flock -w 10 9 || { echo "run-dreams.sh: could not take over from the agent run" >&2; exit 75; }; fi
        else
            echo "run-dreams.sh: another Dreams run is active ($holder); not starting a second one" >&2
            exit 75
        fi
    fi
    echo "pid $$ role $ROLE since $RUN_ID" > "$WORK/.dreams-run.who"
fi

# Every run keeps its own log, also when launched from a terminal: work/logs/run-<RUN_ID>.log
# (DREAMS_LOG=0 disables). After a crash, this and the capture below are the evidence.
if [[ ${DREAMS_LOG:-1} != 0 ]]; then
    LOG=$WORK/logs/run-$RUN_ID.log
    exec > >(tee -a "$LOG") 2>&1
    echo "run-dreams.sh: run $RUN_ID, log $LOG"
fi
# A separate guest user dir (DREAMS_USER_DIR) starts as a copy of the player's; DREAMS_SKIP_INTRO=1
# marks its tutorial as done. The player's own dir is never edited here.
if [[ $(realpath -m "$USERDIR") != $(realpath -m "$WORK/user") ]]; then
    python3 -I scripts/dreams_seed_save.py "$USERDIR" "$WORK/user" "$SERIAL" \
        $([[ ${DREAMS_SKIP_INTRO:-0} == 1 ]] && echo --tutorial-skip)
elif [[ ${DREAMS_SKIP_INTRO:-0} == 1 && ${DREAMS_TUTORIAL_SKIP_MY_SAVE:-0} == 1 ]]; then
    # The player's own save, only on this explicit opt-in: a timestamped backup first.
    python3 -I scripts/dreams_seed_save.py "$USERDIR" "$USERDIR" "$SERIAL" --tutorial-skip --in-place-with-backup
elif [[ ${DREAMS_SKIP_INTRO:-0} == 1 ]]; then
    echo "run-dreams.sh: DREAMS_SKIP_INTRO: your own save is left as it is, so the preferences screen, the control-scheme popup and the first-time intro still show (DREAMS_TUTORIAL_SKIP_MY_SAVE=1 changes that, with a backup; docs/dreams/FPS.md)"
fi
mkdir -p "$USERDIR"

# Offline prep: SELF -> flat image, link the game's own libc/Fios2/JobManager prx, AppContent profile.
# BB_SKIP_GAME_CHECK=1: bbport's game_check.py only knows Bloodborne's image hash
# (Dreams game check is TODO, docs/dreams/STATUS.md).
if [[ ! -f $OUT/boot-linked.bin || $GAME/eboot.bin -nt $OUT/boot-linked.bin || scripts/link_modules.py -nt $OUT/boot-linked.bin ]]; then
    BB_SKIP_GAME_CHECK=1 python3 scripts/prepare.py "$GAME" --out "$OUT"
    BB_SKIP_GAME_CHECK=1 python3 scripts/link_modules.py "$GAME" --out "$OUT" \
        --modules libc.prx libSceFios2.prx libSceJobManager.prx
    python3 scripts/content_profile.py "$GAME" --out "$OUT"
fi

# vcore (the shadPS4-based GPU library, docs/dreams/VCORE.md): the same configuration the Dreams
# fork's launcher gives its emulator (tools/run-shadps4-reference.sh, notes/shadps4-dreams-rebase.md
# section 6): per-game config (readbacks_mode=2 Precise, ...) and the SHADPS4_DREAMS_* switches.
if [[ $(cat out/gpu-library.txt 2>/dev/null || echo vcore) == vcore ]]; then
    VUSER=$WORK/vcore-user                       # shadPS4 "user" dir of the GPU library
    mkdir -p "$VUSER/custom_configs" "$WORK/vcore-probe" "$WORK/vcore-shader-cache"
    cp "$ROOT/notes/shadps4-dreams-runtime/CUSA04301.json" "$VUSER/custom_configs/$SERIAL.json"
    # DREAMS_FPS=<n> above 60 (vcore/shim/frame_rate.cpp): the display must flip n times a second.
    fps=${DREAMS_FPS:-0}
    if [[ $fps =~ ^[0-9]+$ ]] && (( fps > 60 && fps <= 240 )); then
        python3 -I -c 'import json,sys
p,n=sys.argv[1],int(sys.argv[2]); c=json.load(open(p)); c["GPU"]["vblank_frequency"]=n
json.dump(c,open(p,"w"),indent=2)' "$VUSER/custom_configs/$SERIAL.json" "$DREAMS_FPS"
        echo "run-dreams.sh: DREAMS_FPS=$DREAMS_FPS: vblank_frequency $DREAMS_FPS"
    fi
    printf 'native' > "$WORK/vcore-probe/mode.txt"
    # Exactly the fork launcher's switches (DreamsCore.cs strips other SHADPS4_* first).
    for v in $(env | grep -o '^SHADPS4_[A-Z0-9_]*' || true); do
        [[ ${VCORE_KEEP_SHADPS4_ENV:-0} == 1 ]] || unset "$v"
    done
    export SHADPS4_ENABLE_IPC=false
    for k in ADDITIONAL_EXACT_DOC SCENE_COMPACT_EXACT_DOC SELECTION_LIST_EXACT_DOC STATEFUL_EXACT_DOC \
             BLOCK_TEXEL_ALIAS EXTENDED_READBACK_CACHE FAIR_GPU_QUEUES GPU_NUMERIC_TABLE LINEAR_WRITE_RANGES \
             POOLED_TILING_ALLOCATIONS PRECISE_FRAME_FENCE_READS PRECISE_SRT_READS PRECISE_WRITE_DATA \
             PREFETCH_CPU_READBACKS PREFETCH_CPU_WRITE_READBACKS STRICT_SCENE_READINESS; do
        export "SHADPS4_DREAMS_$k=1"
    done
    for k in BACKGROUND_CONTROL DEFER_PREFETCH_WAITS FORCE_60HZ_RENDERING PRECISE_CPU_WRITES SYNC_GDS_EXPORT; do
        export "SHADPS4_DREAMS_$k=0"
    done
    # DREAMS_SKIP_INTRO=1: the Dreams fork's logo-video skip (runtime_avplayer.c honours it too).
    # Set after the SHADPS4_* clean-up above, which would otherwise remove it.
    if [[ ${DREAMS_SKIP_INTRO:-0} == 1 ]]; then
        export SHADPS4_DREAMS_SKIP_LOGO_VIDEO=1
    fi
    export SHADPS4_DREAMS_PRIMARY_QUEUE_PROBE_DIR="$WORK/vcore-probe"
    export SHADPS4_DREAMS_PRIVATE_SHADER_CACHE="$WORK/vcore-shader-cache"
    export SHADPS4_USER_DIR="$VUSER"             # path_util.cpp vcore edit
    export VCORE_GAME_DIR="$GAME"                # param.sfo APP_VER/SYSTEM_VER for ElfInfo
    # Frame capture (docs/dreams/CAPTURE.md): ON by default at 1000 ms so every run (also the
    # user's) leaves a picture record. DREAMS_CAPTURE=1 (500 ms), =<ms>, or =0 (off). Small PNGs of
    # changed frames + index.csv in work/captures/<RUN_ID>; review with tools/capture_sheet.py.
    DREAMS_CAPTURE=${DREAMS_CAPTURE:-1000}
    if [[ $DREAMS_CAPTURE != 0 ]]; then
        export VCORE_CAPTURE_MS=$([[ $DREAMS_CAPTURE == 1 ]] && echo 500 || echo "$DREAMS_CAPTURE")
        export VCORE_CAPTURE_DIR=${VCORE_CAPTURE_DIR:-$WORK/captures/$RUN_ID}
    fi
fi
exec out/dreams-probe "$OUT/boot-linked.bin" --content-profile "$OUT/content.bin" \
    --app0 "$GAME" --user "$USERDIR" --timeout "${DREAMS_TIMEOUT:-0}" "$@"
