#!/usr/bin/env bash
# Dreams port: offline prep + launch.
#   ./run-dreams.sh [--cpu-only] [extra dreams-probe args]
# Env: DREAMS_GAME (default ../game/CUSA08010), DREAMS_WORK (default ../work),
#      DREAMS_TIMEOUT (watchdog seconds, default 0 = off), BB_LIST_UNRESOLVED=1 (list missing imports).
#      VCORE_LOG_FILTER (vcore GPU log filter, default "*:warning", shadPS4 log_filter syntax).
# See docs/dreams/RUNNING.md and docs/dreams/VCORE.md.
set -euo pipefail
cd -- "$(dirname -- "$0")"
ROOT=$(cd .. && pwd)
GAME=${DREAMS_GAME:-$ROOT/game/CUSA08010}
WORK=${DREAMS_WORK:-$ROOT/work}
OUT=$WORK/out
mkdir -p "$OUT" "$WORK/user"
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
    SERIAL=$(python3 -I -c 'import sys
d=open(sys.argv[1],"rb").read()
i=d.find(b"CUSA"); print(d[i:i+9].decode())' "$GAME/sce_sys/param.sfo")
    VUSER=$WORK/vcore-user                       # shadPS4 "user" dir of the GPU library
    mkdir -p "$VUSER/custom_configs" "$WORK/vcore-probe" "$WORK/vcore-shader-cache"
    cp "$ROOT/notes/shadps4-dreams-runtime/CUSA04301.json" "$VUSER/custom_configs/$SERIAL.json"
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
    export SHADPS4_DREAMS_PRIMARY_QUEUE_PROBE_DIR="$WORK/vcore-probe"
    export SHADPS4_DREAMS_PRIVATE_SHADER_CACHE="$WORK/vcore-shader-cache"
    export SHADPS4_USER_DIR="$VUSER"             # path_util.cpp vcore edit
    export VCORE_GAME_DIR="$GAME"                # param.sfo APP_VER/SYSTEM_VER for ElfInfo
    # Test capture (docs/dreams/CAPTURE.md): DREAMS_CAPTURE=1 (every 500 ms) or =<ms>. Small PNGs of
    # changed frames + index.csv in work/captures/<run>; review with tools/capture_sheet.py.
    if [[ -n ${DREAMS_CAPTURE:-} && ${DREAMS_CAPTURE} != 0 ]]; then
        export VCORE_CAPTURE_MS=$([[ $DREAMS_CAPTURE == 1 ]] && echo 500 || echo "$DREAMS_CAPTURE")
        export VCORE_CAPTURE_DIR=${VCORE_CAPTURE_DIR:-$WORK/captures/$(date +%Y%m%d-%H%M%S)}
    fi
fi
exec out/dreams-probe "$OUT/boot-linked.bin" --content-profile "$OUT/content.bin" \
    --app0 "$GAME" --user "$WORK/user" --timeout "${DREAMS_TIMEOUT:-0}" "$@"
