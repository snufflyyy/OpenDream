#!/usr/bin/env bash
# Fetch the exact source dependencies for the default native Dreams build. No game data needed.
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "$0")/../.." && pwd -P)
# shellcheck source=../vcore/source-lock.sh
source "$ROOT/port/vcore/source-lock.sh"
renderer=$ROOT/ref/shadps4

fetch_externals() {
    git -C "$1" submodule update --init --recursive --depth 1 --jobs "${FETCH_JOBS:-4}" -- \
        externals/{abseil-cpp,ext-boost,fmt,half,json,magic_enum,miniz,robin-map,sirit,toml11,tracy,vma,vulkan-headers,xbyak,xxhash,zydis} \
        externals/aacdec/fdk-aac
}

if [[ -e $renderer || -L $renderer ]]; then
    if ! check_vcore_overlay "$renderer"; then
        printf 'Not replacing existing %s. Use a fresh OpenDream checkout for the pinned public build.\n' "$renderer" >&2
        exit 1
    fi
    fetch_externals "$renderer"
    touch "$renderer/.git/opendream-vcore"
else
    mkdir -p -- "$ROOT/ref"
    tmp=$(mktemp -d "$ROOT/ref/.shadps4-setup.XXXXXX")
    trap 'rm -rf -- "$tmp"' EXIT
    git init -q "$tmp"
    git -C "$tmp" remote add origin https://github.com/shadps4-emu/shadPS4.git
    git -C "$tmp" fetch --depth 1 origin "$VCORE_BASE"
    git -C "$tmp" switch --detach "$VCORE_BASE"
    git -C "$tmp" apply --index --whitespace=nowarn "$ROOT/port/vcore/patches/shadps4-vcore.patch"
    check_vcore_overlay "$tmp"
    fetch_externals "$tmp"
    check_vcore_overlay "$tmp"
    touch "$tmp/.git/opendream-vcore"
    mv -nT -- "$tmp" "$renderer"
    if [[ -d $tmp ]]; then
        echo 'Another setup created ref/shadps4; rerun setup-build.sh to check it.' >&2
        exit 1
    fi
fi

git -C "$ROOT" submodule update --init --depth 1 -- port/third_party/LibAtrac9
printf 'Build sources ready. Run port/build-dreams.sh from the checkout root.\n'
