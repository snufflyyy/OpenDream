#!/usr/bin/env bash
# Gather evidence after a GPU hang / game crash into work/crash/<YYYYmmdd-HHMMSS>/.
#
#   tools/collect_crash.sh [--minutes N] [--previous-boot]
#
#   --minutes N       window for the filtered kernel log slice (default 30)
#   --previous-boot   also collect `journalctl -k -b -1` (use after a reboot/forced reset)
# Collects: amdgpu devcoredump (vanishes on reboot), kernel journal (full + amdgpu-filtered recent
# slice), newest work/logs/run-*.log tail, newest capture index + last 20 PNGs, run owner file,
# uname, vulkaninfo --summary, and README.txt summarizing hang signatures.
# Never fails because something is missing; safe to run with no crash.
set -uo pipefail
ROOT=$(cd -- "$(dirname -- "$0")/.." && pwd)
WORK=${DREAMS_WORK:-$ROOT/work}
MIN=30; PREV=0
while (($#)); do
  case $1 in
    --minutes) MIN=$2; shift ;;
    --previous-boot) PREV=1 ;;
    -h|--help) sed -n '2,11p' "$0"; exit 0 ;;
    *) echo "unknown option $1" >&2; exit 2 ;;
  esac; shift
done
D=$WORK/crash/$(date +%Y%m%d-%H%M%S); mkdir -p "$D"
note() { echo "$*" | tee -a "$D/README.txt"; }
echo "Crash evidence collected $(date -Is), window ${MIN} min" > "$D/README.txt"

# devcoredump (root-owned sometimes; try plain cp)
for c in /sys/class/drm/card*/device/devcoredump/data; do
  [[ -e $c ]] || continue
  n=$(echo "$c" | sed 's|.*/\(card[0-9]*\)/.*|\1|')
  if cp "$c" "$D/devcoredump-$n.bin" 2>/dev/null && [[ -s $D/devcoredump-$n.bin ]]; then
    note "devcoredump: saved $n ($(stat -c %s "$D/devcoredump-$n.bin") bytes)"
  else rm -f "$D/devcoredump-$n.bin"; note "devcoredump: $c present but unreadable/empty (try sudo)"; fi
done
ls "$D"/devcoredump-* >/dev/null 2>&1 || note "devcoredump: none present"

# kernel journal
slice() {  # slice <label> <journalctl -b arg>
  journalctl -k -b "$2" --no-pager > "$D/kernel-$1-full.log" 2>&1
  journalctl -k -b "$2" --no-pager --since "-${MIN} min" 2>/dev/null | grep -i 'amdgpu\|drm\|gpu' > "$D/kernel-$1-amdgpu-recent.log" || true
  note "kernel $1: $(wc -l < "$D/kernel-$1-full.log") lines, $(wc -l < "$D/kernel-$1-amdgpu-recent.log") amdgpu lines in last ${MIN} min"
}
slice current 0
((PREV)) && { journalctl -k -b -1 --no-pager > "$D/kernel-previous-full.log" 2>&1
  tail -n 3000 "$D/kernel-previous-full.log" | grep -i 'amdgpu\|drm\|gpu' > "$D/kernel-previous-amdgpu-tail.log" || true
  note "kernel previous boot: $(wc -l < "$D/kernel-previous-full.log") lines (amdgpu lines from its last 3000 in kernel-previous-amdgpu-tail.log)"; }

# run log + captures
LOG=$(ls -t "$WORK"/logs/run-*.log 2>/dev/null | head -1 || true)
if [[ -n $LOG ]]; then tail -n 2000 "$LOG" > "$D/$(basename "$LOG").tail"; note "run log: $LOG (tail 2000 lines)"
else note "run log: none"; fi
CAP=$(ls -dt "$WORK"/captures/*/ 2>/dev/null | head -1 || true)
if [[ -n $CAP ]]; then
  mkdir -p "$D/capture"; cp "$CAP/index.csv" "$D/capture/" 2>/dev/null || true
  ls -t "$CAP"/*.png 2>/dev/null | head -20 | xargs -r cp -t "$D/capture/"
  note "capture: $CAP (index.csv + $(ls "$D/capture" | grep -c png) newest PNGs)"
else note "capture: none"; fi

# system context
{ echo "# who"; cat "$WORK/.dreams-run.who" 2>/dev/null || echo "(no .dreams-run.who)"; echo; uname -a; } > "$D/system.txt"
if command -v vulkaninfo >/dev/null; then timeout 20 vulkaninfo --summary > "$D/vulkaninfo.txt" 2>&1 || true; fi

# signatures
note ""; note "== signatures (kernel logs) =="
for pat in 'ring .* timeout' 'GPU reset' 'Process .* pid' 'amdgpu.*(fault|VM_L2|page fault)' 'ring .* (reset|failed)'; do
  c=$(cat "$D"/kernel-*-full.log 2>/dev/null | grep -Eic "$pat")
  note "$pat : $c"
  cat "$D"/kernel-*-full.log 2>/dev/null | grep -Ei "$pat" | tail -3 | sed 's/^/    /' | tee -a "$D/README.txt"
done
note ""; note "files:"; ls -1 "$D" | sed 's/^/  /' | tee -a "$D/README.txt"
echo "evidence: $D"
