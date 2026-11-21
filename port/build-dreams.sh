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
shadps4_dir=${SHADPS4_DIR:-$ROOT/ref/shadps4}
if [[ ! -f third_party/LibAtrac9/C/src/decoder.c ]]; then
    echo 'Missing LibAtrac9 sources. Run port/tools/setup-build.sh from the checkout root.' >&2
    exit 1
fi
if ! command -v pkg-config >/dev/null || ! pkg-config --exists vulkan sdl3; then
    echo 'Missing pkg-config, Vulkan or SDL3 development files (see port/docs/dreams/BUILD.md).' >&2
    exit 1
fi
mkdir -p out
case $GPU in
vcore)
    if [[ ! -f $shadps4_dir/src/shader_recompiler/dreams_route_table.inc ]]; then
        printf 'Missing Dreams renderer in %s. Run port/tools/setup-build.sh from the checkout root.\n' "$shadps4_dir" >&2
        exit 1
    fi
    shadps4_dir=$(cd -- "$shadps4_dir" && pwd -P)
    managed=no
    if [[ -f $shadps4_dir/.git/opendream-vcore ]]; then
        source vcore/source-lock.sh
        if ! check_vcore_overlay "$shadps4_dir"; then
            echo 'The renderer does not match the pinned source tree. Use a fresh checkout.' >&2
            exit 1
        fi
        managed=yes
    fi
    VCORE_BUILD_DIR=${VCORE_BUILD_DIR:-$PWD/vcore/build}
    mkdir -p -- "$VCORE_BUILD_DIR"
    VCORE_BUILD_DIR=$(cd -- "$VCORE_BUILD_DIR" && pwd -P)
    if [[ ! -f $VCORE_BUILD_DIR/build.ninja ]] ||
        ! grep -Fxq "SHADPS4_DIR:PATH=$shadps4_dir" "$VCORE_BUILD_DIR/CMakeCache.txt"; then
        vcc=${VCORE_CC:-}; vcxx=${VCORE_CXX:-}
        if [[ -z $vcc && -x $HOME/.local/bin/clang ]]; then vcc=$HOME/.local/bin/clang; fi
        if [[ -z $vcxx && -x $HOME/.local/bin/clang++ ]]; then vcxx=$HOME/.local/bin/clang++; fi
        CC=${vcc:-clang} CXX=${vcxx:-clang++} cmake -S vcore -B "$VCORE_BUILD_DIR" -G Ninja \
            -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSHADPS4_DIR="$shadps4_dir" -Wno-dev
    fi
    ninja -C "$VCORE_BUILD_DIR" -j"${VCORE_JOBS:-15}" vcore
    GPU_DIR=$VCORE_BUILD_DIR GPU_LIB=vcore
    # AUDIT A7: provenance of the GPU renderer a build linked, so a test result can be traced to
    # exact vcore sources (the fault manager is called out because the audit's hang fix lives there).
    gpu_branch=$(git -C "$shadps4_dir" branch --show-current)
    gpu_head=$(git -C "$shadps4_dir" rev-parse HEAD)
    # Tracked modifications only: the reference tree has a required untracked marker file
    # (i-didnt-check-my-work.md); an untracked file must not make every build look dirty.
    gpu_dirty=no
    if [[ $managed != yes && -n $(git -C "$shadps4_dir" status --porcelain --untracked-files=no) ]]; then
        gpu_dirty=yes
    fi
    gpu_fault_manager_blob=$(git -C "$shadps4_dir" hash-object src/video_core/buffer_cache/fault_manager.cpp)
    gpu_built=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    revision=("branch=$gpu_branch" "head=$gpu_head" "dirty=$gpu_dirty"
              "fault_manager_blob=$gpu_fault_manager_blob" "built=$gpu_built")
    if [[ $managed == yes ]]; then revision+=("overlay_tree=$VCORE_TREE"); fi
    printf '%s\n' "${revision[@]}" > out/gpu-revision.txt
    # AUDIT A2: libvcore.so must expose the bbgpu C API, not shadPS4's emulator/front-end classes
    # (that would mean a shim pulled in the wrong layer). Advisory: prints the symbols and warns.
    # Frontend::WindowSDL and Core::Emulator::FillElfInfo are vcore's own shim implementations
    # (port/vcore/shim/window.cpp, shim/bbgpu.cpp), so lines naming them are expected and excluded.
    if command -v nm >/dev/null; then
        leaks=$({ nm -CD --defined-only "$GPU_DIR/lib$GPU_LIB.so"
                  nm -CD --undefined-only "$GPU_DIR/lib$GPU_LIB.so"; } 2>/dev/null |
                grep -E 'Emulator::|Frontend::WindowSDL' |
                grep -vE 'Frontend::WindowSDL|Core::Emulator::FillElfInfo' | sort -u || true)
        if [[ -n $leaks ]]; then
            echo "WARNING: libvcore.so exposes shadPS4 emulator/front-end symbols (the vcore seam should be the bbgpu C API only):" >&2
            printf '  %s\n' "$leaks" >&2
        fi
    else
        echo "skip vcore seam symbol check: nm not found"
    fi
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
SHADPS4_EXTERNALS=${SHADPS4_EXTERNALS:-$shadps4_dir/externals} ./build-deps.sh
# -no-pie is required: host heap must stay below 1 TiB (src/probe.c, 40-bit pointer packing).
"$CC" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -no-pie "${includes[@]}" -I. -Isrc \
    src/probe.c src/runtime*.c src/vulkan_smoke.c out/libatrac9.a -lm \
    -L"$GPU_DIR" -l"$GPU_LIB" -Wl,-rpath,"$GPU_DIR" -rdynamic "${libraries[@]}" -o out/dreams-probe
echo "$GPU" > out/gpu-library.txt
echo "Built $PWD/out/dreams-probe (GPU library: $GPU, $GPU_DIR/lib$GPU_LIB.so)"
if [[ $GPU == vcore ]]; then
    echo "GPU revision ($shadps4_dir, copied to out/gpu-revision.txt):"
    printf '  %s\n' "${revision[@]}"
    if [[ $managed != yes && ( $gpu_branch != vcore || $gpu_dirty == yes ) ]]; then
        echo "WARNING: vcore sources in $shadps4_dir are not a clean 'vcore' branch (branch=$gpu_branch dirty=$gpu_dirty); this build is not reproducible from a commit alone." >&2
        if [[ ${DREAMS_REQUIRE_CLEAN_GPU:-0} == 1 ]]; then
            echo "DREAMS_REQUIRE_CLEAN_GPU=1: refusing a build of dirty/non-vcore GPU sources." >&2
            exit 1
        fi
    fi
fi
