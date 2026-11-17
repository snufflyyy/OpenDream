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
#      DREAMS_VCORE_CONFIG=<file>: per-game vcore config template for this run (default
#      notes/shadps4-dreams-runtime/CUSA04301.json), e.g. a copy with GPU.dump_shaders=true.
#      DREAMS_MANGOHUD=1: keep MangoHud loaded. Default: disable it, because its Vulkan layer can
#      leave the window showing an older frame even though the game renders the newest one.
#      DREAMS_RENDER_RESOLUTION=<WxH>: experimental internal scene resolution (1280x720; default
#      unset = off) as a one-write loader patch generated for this run only; refused together with
#      an explicit --patches argument. docs/dreams/RUNNING.md "Internal render resolution".
# See docs/dreams/RUNNING.md and docs/dreams/VCORE.md.
set -euo pipefail
cd -- "$(dirname -- "$0")"
ROOT=$(cd .. && pwd)
GAME=${DREAMS_GAME:-$ROOT/game/CUSA08010}
WORK=${DREAMS_WORK:-$ROOT/work}
OUT=$WORK/out
USERDIR=${DREAMS_USER_DIR:-$WORK/user}
RUN_ID=$(date +%Y%m%d-%H%M%S)

# DREAMS_RENDER_RESOLUTION (docs/dreams/RUNNING.md) is one patch file per loader invocation, so an
# explicit --patches is refused instead of being dropped or merged. Checked before any work.
RESOLUTION=${DREAMS_RENDER_RESOLUTION:-}
if [[ -n $RESOLUTION ]]; then
    for arg in "$@"; do
        case $arg in
            --patches|--patches=*)
                echo "run-dreams.sh: DREAMS_RENDER_RESOLUTION and an explicit --patches argument cannot be combined (the loader applies one patch file per run); drop one of them" >&2
                exit 2 ;;
        esac
    done
fi

# All data WORK directories share the root lock unless explicitly configured.
# Agents cannot opt out. User override remains an explicit unsafe escape hatch.
ROLE=${DREAMS_RUN_ROLE:-user}
LOCK_DIR=${DREAMS_RUN_LOCK_DIR:-$ROOT/work}
if [[ $ROLE == agent && ${DREAMS_ALLOW_PARALLEL:-0} != 0 ]]; then
    echo "run-dreams.sh: agents cannot bypass the run lock" >&2
    exit 2
fi
if [[ ${DREAMS_ALLOW_PARALLEL:-0} != 1 ]]; then
    mkdir -p "$LOCK_DIR"
    exec 9>"$LOCK_DIR/.dreams-run.lock"
    python3 -I scripts/dreams_run_lock.py "$LOCK_DIR" "$ROLE" "$RUN_ID" "$$"
fi
mkdir -p "$OUT" "$WORK/logs"

# Every run keeps its own log, also when launched from a terminal: work/logs/run-<RUN_ID>.log
# (DREAMS_LOG=0 disables). After a crash, this and the capture below are the evidence.
if [[ ${DREAMS_LOG:-1} != 0 ]]; then
    LOG=$WORK/logs/run-$RUN_ID.log
    exec > >(tee -a "$LOG") 2>&1
    echo "run-dreams.sh: run $RUN_ID, log $LOG"
fi
# Validate and prepare under the run lock before touching guest saves.
python3 -I scripts/dreams_game_check.py "$GAME"
SERIAL=$(python3 -I scripts/dreams_game_check.py --serial "$GAME")
python3 -I scripts/dreams_prepare.py "$GAME" --out "$OUT"
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

# DREAMS_RENDER_RESOLUTION (experimental, docs/dreams/RUNNING.md): one BBPATCH2 data write applied
# by the loader's --patches. The patch and its JSON report are generated for this run only into a
# private directory under OUT, so the game folder, the prepared outputs and the shader cache are
# never written; the variable unset keeps the launch unchanged. Any generator error, invalid value
# or missing patch stops the launch before the game starts.
resolution_patch=()
if [[ -n $RESOLUTION ]]; then
    res_dir=$OUT/render-resolution/$RUN_ID
    mkdir -p "$res_dir"
    res_patch=$res_dir/render-resolution.bbpatch
    res_report=$res_dir/render-resolution.json
    python3 -I tools/dreams_render_resolution.py "$GAME/eboot.bin" --resolution "$RESOLUTION" \
        --patch "$res_patch" --report "$res_report"
    if [[ ! -f $res_patch ]]; then
        echo "run-dreams.sh: DREAMS_RENDER_RESOLUTION=$RESOLUTION: no patch was published ($res_patch)" >&2
        exit 1
    fi
    resolution_patch=(--patches "$res_patch")
    echo "run-dreams.sh: DREAMS_RENDER_RESOLUTION=$RESOLUTION: loader patch $res_patch (report $res_report)"
fi

# vcore (the shadPS4-based GPU library, docs/dreams/VCORE.md): the same configuration the Dreams
# fork's launcher gives its emulator (tools/run-shadps4-reference.sh, notes/shadps4-dreams-rebase.md
# section 6): per-game config (readbacks_mode=2 Precise, ...) and the SHADPS4_DREAMS_* switches.
if [[ $(cat out/gpu-library.txt 2>/dev/null || echo vcore) == vcore ]]; then
    VUSER=$WORK/vcore-user                       # shadPS4 "user" dir of the GPU library
    mkdir -p "$VUSER/custom_configs" "$WORK/vcore-probe" "$WORK/vcore-shader-cache"
    # The per-game config copy (readbacks_mode=2 Precise, ...). DREAMS_VCORE_CONFIG=<file> replaces
    # the template for one run (e.g. a copy with GPU.dump_shaders=true): the shared notes template
    # is never edited by a run.
    vcore_config=${DREAMS_VCORE_CONFIG:-$ROOT/notes/shadps4-dreams-runtime/CUSA04301.json}
    python3 -I tools/dreams_gpu_config.py "$vcore_config" "$VUSER/custom_configs/$SERIAL.json"
    # DREAMS_FPS=<n> above 60 (vcore/shim/frame_rate.cpp): the display must flip n times a second.
    fps=${DREAMS_FPS:-0}
    if [[ $fps =~ ^[0-9]+$ ]] && (( fps > 60 && fps <= 240 )); then
        python3 -I -c 'import json,sys
p,n=sys.argv[1],int(sys.argv[2]); c=json.load(open(p)); c["GPU"]["vblank_frequency"]=n
json.dump(c,open(p,"w"),indent=2)' "$VUSER/custom_configs/$SERIAL.json" "$DREAMS_FPS"
        echo "run-dreams.sh: DREAMS_FPS=$DREAMS_FPS: vblank_frequency $DREAMS_FPS"
    fi
    # Presentation (docs/dreams/PERF.md): DREAMS_PRESENT=mailbox (default: no tearing, newest frame
    # each refresh, low latency), fifo (strict vsync), immediate (the Dreams fork's setting: tears,
    # frame-time wobble shows directly). shadPS4 falls back to FIFO if the mode is unsupported.
    present=${DREAMS_PRESENT:-mailbox}
    case ${present,,} in
        mailbox) present=Mailbox ;; fifo) present=Fifo ;; immediate) present=Immediate ;;
        *) echo "run-dreams.sh: DREAMS_PRESENT must be mailbox, fifo or immediate (got '$present')" >&2; exit 2 ;;
    esac
    python3 -I -c 'import json,sys
p,m=sys.argv[1],sys.argv[2]; c=json.load(open(p)); c["GPU"]["present_mode"]=m
json.dump(c,open(p,"w"),indent=2)' "$VUSER/custom_configs/$SERIAL.json" "$present"
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
    # DREAMS_SIGNATURE_MATCH=1: apply the fork's v2.64 shader routes to other Dreams versions by
    # code hash / DS_ORDERED_COUNT signature (docs/dreams/VERSIONS.md). Default on; =0 turns it off.
    if [[ ${DREAMS_SIGNATURE_MATCH:-1} == 1 ]]; then
        export SHADPS4_DREAMS_SIGNATURE_MATCH=1
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
# --- mangohud-begin
# MangoHud (docs/dreams/PERF.md): its Vulkan layer can leave the game window on an older frame
# while the newest frame is still rendered (DREAMS_CAPTURE shows it). Disable it by default so the
# window matches what the game draws; DREAMS_MANGOHUD=1 keeps it for profiling. Other LD_PRELOAD
# entries are preserved.
if [[ ${DREAMS_MANGOHUD:-0} != 1 ]]; then
    mangohud_seen=0
    [[ ${MANGOHUD:-0} == 1 ]] && mangohud_seen=1
    if [[ ${LD_PRELOAD:-} == *[Mm]ango[Hh]ud* ]]; then
        mangohud_seen=1
        cleaned_preload=
        IFS=: read -r -a _preload_entries <<< "${LD_PRELOAD:-}"
        for _entry in "${_preload_entries[@]}"; do
            [[ -z $_entry ]] && continue
            [[ $_entry == *[Mm]ango[Hh]ud* ]] && continue
            cleaned_preload=${cleaned_preload:+$cleaned_preload:}$_entry
        done
        if [[ -n $cleaned_preload ]]; then export LD_PRELOAD=$cleaned_preload; else unset LD_PRELOAD; fi
    fi
    export MANGOHUD=0
    if (( mangohud_seen )); then
        echo "run-dreams.sh: disabled MangoHud (its Vulkan layer can leave a stale frame on screen); DREAMS_MANGOHUD=1 keeps it" >&2
    fi
fi
# --- mangohud-end
exec out/dreams-probe "$OUT/boot-linked.bin" --content-profile "$OUT/content.bin" \
    --app0 "$GAME" --user "$USERDIR" --timeout "${DREAMS_TIMEOUT:-0}" \
    ${resolution_patch[@]+"${resolution_patch[@]}"} "$@"
