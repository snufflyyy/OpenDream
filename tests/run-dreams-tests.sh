#!/usr/bin/env bash
# Dreams port: standalone unit tests for runtime pieces added for Dreams (no game, no GPU).
#   tests/run-dreams-tests.sh            build + run all
# Each test compiles the runtime source it covers directly (see the test file), so it doesn't
# need the full runtime or libbbgpu. Outputs go to out/tests/.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
CC=${CC:-gcc}
CXX=${CXX:-g++}
mkdir -p out/tests
flags=(-std=c11 -O2 -g -Wall -Wextra -Werror -pthread -I. -Isrc)
skipped=0 # optional tests skipped for missing prerequisites (DREAMS_TESTS_STRICT=1 makes it fatal)
# Real shared backing mappings and the vcore page adapter; no Vulkan device is opened.
"$CC" "${flags[@]}" -ffunction-sections -fdata-sections tests/test_memory_backing.c \
    -Wl,--gc-sections -ldl -o out/tests/memory-backing-test
out/tests/memory-backing-test
"$CXX" -std=c++23 -O2 -g -Wall -Wextra -Werror -Ivcore/shim/include -Ivcore/shim \
    -I"${SHADPS4_DIR:-../ref/shadps4}/src" tests/test_backing_pages.cpp \
    vcore/shim/backing_pages.cpp -o out/tests/backing-pages-test
out/tests/backing-pages-test
out/tests/backing-pages-test copy
# Runtime<->vcore ABI contract: runtime_api.h vs the runtime's own declarations (AUDIT A3).
# Compile-only (no link): a signature drift fails to compile.
"$CXX" -std=c++23 -O2 -g -Wall -Wextra -Werror -Ivcore/shim/include -Ivcore/shim \
    -I"${SHADPS4_DIR:-../ref/shadps4}/src" -Isrc tests/test_abi_contract.cpp -o out/tests/abi-contract-test
out/tests/abi-contract-test
python3 -I tests/test_fps_sample.py
python3 -I tests/test_content_inventory.py
python3 -I tests/test_content_closure.py
python3 -I tests/test_dreams_gpu_config.py
python3 -I tests/test_route_table.py
python3 -I tests/test_import_names.py
python3 -I tests/test_doc_lint.py
python3 -I tests/test_dreams_game_check.py
# Optional GPU-fault regression on Mesa's CPU device only (not a software-GPU game run).
host_shaders="${VCORE_BUILD_DIR:-vcore/build}/host_shaders/include"
lavapipe=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json
if [[ -f $host_shaders/video_core/host_shaders/fault_buffer_process_comp.h && -f $lavapipe ]]; then
    "$CXX" -std=c++20 -O2 -g -Wall -Wextra -Werror -Wno-missing-field-initializers \
        tools/fault_buffer_test.cpp -I"$host_shaders" -lvulkan -o out/tests/fault-buffer-test
    VK_DRIVER_FILES="$lavapipe" VK_ICD_FILENAMES="$lavapipe" out/tests/fault-buffer-test
    # Hang guard (docs/dreams/GPU_HANG.md): guarded shader + indirect clamp on the CPU device.
    # The guarded shader is a private shader-cache artifact (work/, not tracked).
    guarded_shader=$(find ../work/vcore-shader-cache -path '*/loop-guard-*/CUSA08010/*5c35f8ee_0.spv' \
        -print -quit 2>/dev/null || true)
    if [[ -n $guarded_shader ]]; then
        "$CXX" -std=c++20 -O2 -g -Wall -Wextra -Werror -Wno-missing-field-initializers \
            tools/loop_guard_test.cpp -I"$host_shaders" -lvulkan -o out/tests/loop-guard-test
        VK_DRIVER_FILES="$lavapipe" VK_ICD_FILENAMES="$lavapipe" out/tests/loop-guard-test "$guarded_shader"
    else
        echo "skip loop-guard-test: needs the guarded 0x5c35f8ee shader in ../work/vcore-shader-cache/loop-guard-*"
        skipped=$((skipped + 1))
    fi
else
    echo "skip fault-buffer-test: needs built vcore host shaders and Mesa lavapipe"
    echo "skip loop-guard-test: needs built vcore host shaders and Mesa lavapipe"
    skipped=$((skipped + 2))
fi
"$CC" "${flags[@]}" tests/test_eventflag.c -o out/tests/eventflag-test
out/tests/eventflag-test
"$CC" "${flags[@]}" tests/test_audio3d.c -lm -o out/tests/audio3d-test
out/tests/audio3d-test
"$CC" "${flags[@]}" tests/test_dreams_services.c -ldl -o out/tests/dreams-services-test
out/tests/dreams-services-test
"$CC" "${flags[@]}" tests/test_dreams_offline.c -ldl -o out/tests/dreams-offline-test
out/tests/dreams-offline-test
# Codec library (./build-deps.sh) + a 1 s stereo 1 kHz sine as ADTS AAC (needs ffmpeg).
if [[ -f out/libdreams-codecs.so ]] && command -v ffmpeg >/dev/null; then
    [[ -f out/tests/sine.aac ]] || ffmpeg -hide_banner -loglevel error -y -f lavfi \
        -i "sine=frequency=1000:sample_rate=48000:duration=1" -ac 2 -c:a aac -b:a 128k -f adts out/tests/sine.aac
    "$CC" "${flags[@]}" -Wno-unused-function tests/test_codecs.c out/libatrac9.a -lm -o out/tests/codecs-test
    out/tests/codecs-test out/libdreams-codecs.so out/tests/sine.aac
else
    echo "skip codecs-test: needs ./build-deps.sh and ffmpeg"
    skipped=$((skipped + 1))
fi
# Pad motion sensors and IME (SDL3 virtual gamepad / fake window, dummy video driver).
read -r -a sdl_cflags <<< "$(pkg-config --cflags sdl3)"
read -r -a sdl_libs <<< "$(pkg-config --libs sdl3)"
"$CC" "${flags[@]}" "${sdl_cflags[@]}" tests/test_pad_motion.c "${sdl_libs[@]}" -lm -o out/tests/pad-motion-test
"$CC" "${flags[@]}" "${sdl_cflags[@]}" tests/test_pad_input.c "${sdl_libs[@]}" -lm -o out/tests/pad-input-test
"$CC" "${flags[@]}" "${sdl_cflags[@]}" tests/test_pad.c "${sdl_libs[@]}" -lm -o out/tests/pad-test
out/tests/pad-test
out/tests/pad-input-test
out/tests/pad-motion-test
"$CC" "${flags[@]}" "${sdl_cflags[@]}" tests/test_ime.c "${sdl_libs[@]}" -o out/tests/ime-test
out/tests/ime-test
# AvPlayer decoder library (./build-deps.sh, FFmpeg) + a 2 s 200x120 red H.264/AAC MP4 (needs ffmpeg).
if [[ -f out/libdreams-av.so ]] && command -v ffmpeg >/dev/null; then
    [[ -f out/tests/clip.mp4 ]] || ffmpeg -hide_banner -loglevel error -y -f lavfi -i "color=c=red:s=200x120:r=30:d=2" \
        -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=2" -ac 2 -c:v libx264 -pix_fmt yuv420p \
        -c:a aac -b:a 128k -metadata:s:a:0 language=eng -shortest out/tests/clip.mp4
    "$CC" "${flags[@]}" tests/test_avplayer.c -ldl -lm -o out/tests/avplayer-test
    out/tests/avplayer-test out/libdreams-av.so out/tests/clip.mp4
else
    echo "skip avplayer-test: needs ./build-deps.sh (FFmpeg) and ffmpeg"
    skipped=$((skipped + 1))
fi
# Optional tests skipped for missing prerequisites are counted, not silently ignored (AUDIT T6).
echo "$skipped optional test(s) skipped"
if (( skipped )) && [[ ${DREAMS_TESTS_STRICT:-0} == 1 ]]; then
    echo "DREAMS_TESTS_STRICT=1: failing because optional tests were skipped" >&2
    exit 1
fi
echo "Dreams runtime tests passed"
