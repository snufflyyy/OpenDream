#!/usr/bin/env bash
# Extract and verify a Dreams fpkg into a game folder.
#
#   tools/install_game.sh [--dry-run] [--delete-inputs] [--margin-gb N] PKG OUTDIR
#
#   PKG     a .pkg (fpkg) file; archives (.rar etc.) must be extracted by you first
#   OUTDIR  e.g. game/CUSA08010-v264   (manifest goes to OUTDIR.extract.json)
#
# Steps: disk preflight (pkg size ~ extracted size, plus margin, default 5 GB) ->
# pkg_extract.py extract --digest (PFS digest + zlib checks) -> re-hash every file against the
# manifest -> print TITLE_ID / APP_VER / VERSION from sce_sys/param.sfo.
# --dry-run        print the plan after the preflight; extract nothing
# --delete-inputs  after successful verification remove the PKG (never by default)
# Use the result with DREAMS_GAME=<OUTDIR> (RUNNING.md).
set -euo pipefail
TOOLS=$(cd -- "$(dirname -- "$0")" && pwd)
DRY=0; DEL=0; MARGIN_GB=5; ARGS=()
while (($#)); do
  case $1 in
    --dry-run) DRY=1 ;;
    --delete-inputs) DEL=1 ;;
    --margin-gb) MARGIN_GB=$2; shift ;;
    -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
    -*) echo "unknown option $1" >&2; exit 2 ;;
    *) ARGS+=("$1") ;;
  esac; shift
done
((${#ARGS[@]} == 2)) || { sed -n '2,5p' "$0" >&2; exit 2; }
IN=${ARGS[0]}; OUT=${ARGS[1]%/}
[[ -f $IN ]] || { echo "input not found: $IN" >&2; exit 1; }
[[ ${IN,,} == *.pkg ]] || { echo "expects a .pkg file; extract archives yourself first (got: $IN)" >&2; exit 2; }
[[ -e $OUT ]] && { echo "output exists: $OUT (refusing to overwrite)" >&2; exit 1; }
PY=$TOOLS/.venv/bin/python; [[ -x $PY ]] || PY=python3

# (a) disk preflight on the nearest existing parent of OUT
PARENT=$(dirname -- "$OUT"); while [[ ! -d $PARENT ]]; do PARENT=$(dirname -- "$PARENT"); done
NEED=$(stat -c %s "$IN")
FREE=$(df -B1 --output=avail "$PARENT" | tail -1 | tr -d ' ')
MARGIN=$((MARGIN_GB * 1024 * 1024 * 1024))
gb() { awk -v b="$1" 'BEGIN {printf "%.1f GB", b/1073741824}'; }
echo "input: $IN  need: $(gb "$NEED") + $(gb "$MARGIN") margin  free on $PARENT: $(gb "$FREE")"
if ((FREE < NEED + MARGIN)); then
  echo "REFUSING: not enough free space (short by $(gb $((NEED + MARGIN - FREE))))" >&2; exit 1
fi
if ((DRY)); then
  echo "plan: pkg_extract.py extract '$IN' '$OUT' --digest  (manifest: $OUT.extract.json)"
  echo "      re-hash all files vs manifest; print param.sfo$( ((DEL)) && echo '; delete the pkg')"
  exit 0
fi

# (b) extract with digest check
"$PY" -I "$TOOLS/pkg_extract.py" extract "$IN" "$OUT" --digest | tail -40

# (c) re-hash against the manifest, (d) print SFO fields
"$PY" -I - "$OUT" <<'PYEOF'
import hashlib, json, os, struct, sys
out = sys.argv[1]
m = json.load(open(out + ".extract.json"))
if m["report"].get("errors"):
    sys.exit("extractor reported errors: %s" % m["report"]["errors"][:5])
bad = 0
for f in m["files"]:
    h = hashlib.sha256()
    with open(os.path.join(out, f["path"]), "rb") as fh:
        while c := fh.read(1 << 22):
            h.update(c)
    bad += h.hexdigest() != f["sha256"]
print("verified %d files, %d mismatches" % (len(m["files"]), bad))
if bad:
    sys.exit(1)
d = open(os.path.join(out, "sce_sys/param.sfo"), "rb").read()
_, _, kt, dt, n = struct.unpack_from("<4sIIII", d, 0)
for i in range(n):
    ko, fmt, ln, _, do = struct.unpack_from("<HHIII", d, 20 + 16 * i)
    k = d[kt + ko:d.index(b"\0", kt + ko)].decode()
    if k in ("TITLE_ID", "APP_VER", "VERSION", "TITLE"):
        print("%-9s %s" % (k, d[dt + do:dt + do + ln].split(b"\0")[0].decode(errors="replace")))
PYEOF

# (e) opt-in cleanup, only after everything above succeeded
((DEL)) && { rm -f -- "$IN"; echo "pkg deleted"; }
echo "done: DREAMS_GAME=$OUT"
