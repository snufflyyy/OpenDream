#!/usr/bin/env bash
# Reference run: the rebased shadPS4 *emulator* (ref/shadps4 branch `dreams`, latest upstream +
# Dreams fork patches) on our extracted game, configured exactly like the Dreams fork's launcher
# (notes/shadps4-dreams-rebase.md §6, notes/shadps4-dreams-runtime/).
#
# Purpose: validate the rebase and give the native port a known-good reference for GPU behaviour
# (milestone 6/7). This is NOT the native port.
#
#   tools/run-shadps4-reference.sh [extra shadps4 args]
# Env: DREAMS_GAME (default game/CUSA08010), SHADPS4_BIN (default ref/shadps4/build/shadps4)
# State lives in work/shadps4-ref/user/ (shadPS4 "portable" mode: ./user in the cwd).
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "$0")/.." && pwd)
GAME=${DREAMS_GAME:-$ROOT/game/CUSA08010}
# Default: a frozen copy of the known-good dreams-branch build (ref/shadps4/build may be rebuilt by vcore work).
BIN=${SHADPS4_BIN:-$ROOT/work/shadps4-ref/bin/shadps4}
RUN=$ROOT/work/shadps4-ref
SERIAL=$(python3 -I -c 'import sys,struct
d=open(sys.argv[1],"rb").read()
i=d.find(b"CUSA"); print(d[i:i+9].decode())' "$GAME/sce_sys/param.sfo")
mkdir -p "$RUN/user/custom_configs" "$RUN/probe" "$RUN/shader-cache"
# The fork's per-game config (readbacks_mode=2 Precise is essential), saved under our serial.
cp "$ROOT/notes/shadps4-dreams-runtime/CUSA04301.json" "$RUN/user/custom_configs/$SERIAL.json"
printf 'native' > "$RUN/probe/mode.txt"
# Tutorial skip (first run otherwise can't progress): only seed if no save exists yet.
SAVE="$RUN/user/home/1000/savedata/$SERIAL/SYSTEM"   # current shadPS4 layout (old user/savedata/ triggers a blocking "Save Migration" dialog)
if [[ ! -f $SAVE/savedata.json ]]; then
    mkdir -p "$SAVE" && cp "$ROOT/notes/tutorial-skip-savedata.json" "$SAVE/savedata.json"
fi
# Exactly the fork launcher's environment (DreamsCore.cs strips other SHADPS4_* first).
for v in $(env | grep -o '^SHADPS4_[A-Z0-9_]*' || true); do unset "$v"; done
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
export SHADPS4_DREAMS_PRIMARY_QUEUE_PROBE_DIR="$RUN/probe"
export SHADPS4_DREAMS_PRIVATE_SHADER_CACHE="$RUN/shader-cache"
cd "$RUN"
exec "$BIN" --fullscreen false -g "$GAME" "$@" < /dev/null
