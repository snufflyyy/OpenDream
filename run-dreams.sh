#!/usr/bin/env bash
# Dreams port: offline prep + launch.
#   ./run-dreams.sh [--cpu-only] [extra dreams-probe args]
# Env: DREAMS_GAME (default ../game/CUSA08010), DREAMS_WORK (default ../work),
#      DREAMS_TIMEOUT (watchdog seconds, default 0 = off), BB_LIST_UNRESOLVED=1 (list missing imports).
# See docs/dreams/RUNNING.md.
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
exec out/dreams-probe "$OUT/boot-linked.bin" --content-profile "$OUT/content.bin" \
    --app0 "$GAME" --user "$WORK/user" --timeout "${DREAMS_TIMEOUT:-0}" "$@"
