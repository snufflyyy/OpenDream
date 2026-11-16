#!/usr/bin/env bash
# Dreams port: build the C loader + runtime (out/dreams-probe).
#
# Phase note (milestones 2-5, see docs/dreams/STATUS.md): the GPU library is still bbport's
# libbbgpu.so, built once in ../ref/bbport (see docs/dreams/BUILD.md). It will be replaced by
# "vcore" (latest upstream shadPS4 video_core + Dreams fork patches) at milestone 6.
set -euo pipefail
cd -- "$(dirname -- "$0")"
ROOT=$(cd .. && pwd)
BBGPU_DIR=${BBGPU_DIR:-$ROOT/ref/bbport/out/gpu}
CC=${CC:-gcc}
if [[ ! -f $BBGPU_DIR/libbbgpu.so ]]; then
    echo "missing $BBGPU_DIR/libbbgpu.so: build ref/bbport first (docs/dreams/BUILD.md)" >&2; exit 1
fi
mkdir -p out
read -r -a includes <<< "$(pkg-config --cflags vulkan sdl3)"
read -r -a libraries <<< "$(pkg-config --libs vulkan sdl3)"
# LibAtrac9 (third-party, compiled without -Werror), rebuilt only when its sources change.
if [[ ! -f out/libatrac9.a || -n $(find third_party/LibAtrac9/C/src -newer out/libatrac9.a -name '*.c') ]]; then
    rm -rf out/atrac9 && mkdir -p out/atrac9
    for source in third_party/LibAtrac9/C/src/*.c; do
        "$CC" -std=c99 -O2 -g -w -c "$source" -o "out/atrac9/$(basename "${source%.c}").o"
    done
    ar rcs out/libatrac9.a out/atrac9/*.o
fi
# -no-pie is required: host heap must stay below 1 TiB (src/probe.c, 40-bit pointer packing).
"$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -no-pie "${includes[@]}" -I. -Isrc \
    src/probe.c src/runtime*.c src/vulkan_smoke.c out/libatrac9.a -lm \
    -L"$BBGPU_DIR" -lbbgpu -Wl,-rpath,"$BBGPU_DIR" -rdynamic "${libraries[@]}" -o out/dreams-probe
echo "Built $PWD/out/dreams-probe"
