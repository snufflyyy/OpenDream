#!/usr/bin/env bash
# Dreams port: build the C loader + runtime (out/dreams-probe) and link it against a GPU library.
#
#   DREAMS_GPU=vcore  (default) port/vcore: latest upstream shadPS4 video_core + shader recompiler +
#                     Dreams-ShadPS4 fork patches (ref/shadps4 branch `vcore`), built here with CMake
#                     into vcore/build/libvcore.so. See docs/dreams/VCORE.md.
#   DREAMS_GPU=bbport bbport's own libbbgpu.so, built once in ../ref/bbport (docs/dreams/BUILD.md);
#                     BBGPU_DIR overrides its location. Kept for comparison runs.
# Env for vcore: VCORE_BUILD_DIR (default vcore/build), SHADPS4_DIR (default ../ref/shadps4),
#   VCORE_CC/VCORE_CXX (default ~/.local/bin/clang{,++} when present: same compiler as the
#   reference shadPS4 build), VCORE_JOBS (default 15).
set -euo pipefail
cd -- "$(dirname -- "$0")"
ROOT=$(cd .. && pwd)
GPU=${DREAMS_GPU:-vcore}
CC=${CC:-gcc}
mkdir -p out
case $GPU in
vcore)
    VCORE_BUILD_DIR=${VCORE_BUILD_DIR:-$PWD/vcore/build}
    if [[ ! -f $VCORE_BUILD_DIR/build.ninja ]]; then
        vcc=${VCORE_CC:-}; vcxx=${VCORE_CXX:-}
        if [[ -z $vcc && -x $HOME/.local/bin/clang ]]; then vcc=$HOME/.local/bin/clang; vcxx=$HOME/.local/bin/clang++; fi
        CC=${vcc:-cc} CXX=${vcxx:-c++} cmake -S vcore -B "$VCORE_BUILD_DIR" -G Ninja \
            -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSHADPS4_DIR="${SHADPS4_DIR:-$ROOT/ref/shadps4}" -Wno-dev
    fi
    ninja -C "$VCORE_BUILD_DIR" -j"${VCORE_JOBS:-15}" vcore
    GPU_DIR=$VCORE_BUILD_DIR GPU_LIB=vcore
    ;;
bbport)
    GPU_DIR=${BBGPU_DIR:-$ROOT/ref/bbport/out/gpu} GPU_LIB=bbgpu
    if [[ ! -f $GPU_DIR/libbbgpu.so ]]; then
        echo "missing $GPU_DIR/libbbgpu.so: build ref/bbport first (docs/dreams/BUILD.md)" >&2; exit 1
    fi
    ;;
*) echo "DREAMS_GPU must be vcore or bbport (got '$GPU')" >&2; exit 1 ;;
esac
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
# Optional run-time codec library (out/libdreams-codecs.so: fdk-aac, ATRAC9 encoder), dlopen'ed by
# src/runtime_codecs.c; skips work when up to date. No link flags.
./build-deps.sh
# -no-pie is required: host heap must stay below 1 TiB (src/probe.c, 40-bit pointer packing).
"$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -no-pie "${includes[@]}" -I. -Isrc \
    src/probe.c src/runtime*.c src/vulkan_smoke.c out/libatrac9.a -lm \
    -L"$GPU_DIR" -l"$GPU_LIB" -Wl,-rpath,"$GPU_DIR" -rdynamic "${libraries[@]}" -o out/dreams-probe
echo "$GPU" > out/gpu-library.txt
echo "Built $PWD/out/dreams-probe (GPU library: $GPU, $GPU_DIR/lib$GPU_LIB.so)"
