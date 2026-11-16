#!/usr/bin/env bash
# Cheap, host-independent preflight for agent edits to the port.
#
#   tools/preflight.sh          # run all checks
#
# Runs in seconds and needs no GPU/game. Use it before handing work off or
# committing. It is deliberately a subset of tests/run-dreams-tests.sh: full
# runtime/GPU behaviour is covered there, not here.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."

fail=0
step() { printf '\n== %s\n' "$*"; }
ok()   { printf 'ok: %s\n' "$*"; }
bad()  { printf 'FAIL: %s\n' "$*" >&2; fail=1; }

step "shell syntax"
while IFS= read -r -d '' f; do
    if bash -n "$f" 2>/dev/null; then ok "$f"; else bad "bash -n $f"; fi
done < <(find . -path ./.git -prune -o -path ./out -prune -o -path ./vcore/build -prune \
              -o -name '*.sh' -print0)

step "Python syntax"
if command -v python3 >/dev/null; then
    if python3 -I -m py_compile tools/*.py scripts/*.py tests/*.py 2>/dev/null; then
        ok "py_compile tools/ scripts/ tests/"
    else
        bad "py_compile failed (see above)"
    fi
else
    bad "python3 not found"
fi

step "documentation is host-independent"
if [[ -f tools/doc_lint.py ]]; then
    if python3 -I tools/doc_lint.py; then ok "doc_lint"; else bad "doc_lint"; fi
else
    printf 'skip: tools/doc_lint.py not present\n'
fi

step "shader route table matches its source"
if [[ -f tests/test_route_table.py ]]; then
    if python3 -I tests/test_route_table.py >/dev/null 2>&1; then
        ok "route table sync"
    else
        bad "route table sync (run: python3 -I tests/test_route_table.py)"
    fi
fi

step "no absolute home paths in tracked code/tools/scripts/tests"
if git grep -InE '/home/[A-Za-z0-9._-]+/|/Users/[A-Za-z0-9._-]+/' \
       -- 'src/*' 'tools/*' 'scripts/*' 'tests/*' '*.sh' >/dev/null 2>&1; then
    bad "absolute home path found"
    git grep -InE '/home/[A-Za-z0-9._-]+/|/Users/[A-Za-z0-9._-]+/' \
        -- 'src/*' 'tools/*' 'scripts/*' 'tests/*' '*.sh' || true
else
    ok "no absolute home paths"
fi

step "whitespace"
if git diff --check >/dev/null 2>&1; then ok "git diff --check"; else bad "git diff --check"; fi

if (( fail )); then
    printf '\npreflight FAILED\n' >&2
    exit 2
fi
printf '\npreflight passed\n'
