#!/usr/bin/env bash
# Dreams port: standalone unit tests for runtime pieces added for Dreams (no game, no GPU).
#   tests/run-dreams-tests.sh            build + run all
# Each test compiles the runtime source it covers directly (see the test file), so it doesn't
# need the full runtime or libbbgpu. Outputs go to out/tests/.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
CC=${CC:-gcc}
mkdir -p out/tests
flags=(-std=c11 -O2 -g -Wall -Wextra -Werror -pthread -I. -Isrc)
"$CC" "${flags[@]}" tests/test_eventflag.c -o out/tests/eventflag-test
out/tests/eventflag-test
"$CC" "${flags[@]}" tests/test_audio3d.c -lm -o out/tests/audio3d-test
out/tests/audio3d-test
"$CC" "${flags[@]}" tests/test_dreams_services.c -ldl -o out/tests/dreams-services-test
out/tests/dreams-services-test
# Codec library (./build-deps.sh) + a 1 s stereo 1 kHz sine as ADTS AAC (needs ffmpeg).
if [[ -f out/libdreams-codecs.so ]] && command -v ffmpeg >/dev/null; then
    [[ -f out/tests/sine.aac ]] || ffmpeg -hide_banner -loglevel error -y -f lavfi \
        -i "sine=frequency=1000:sample_rate=48000:duration=1" -ac 2 -c:a aac -b:a 128k -f adts out/tests/sine.aac
    "$CC" "${flags[@]}" -Wno-unused-function tests/test_codecs.c out/libatrac9.a -lm -o out/tests/codecs-test
    out/tests/codecs-test out/libdreams-codecs.so out/tests/sine.aac
else
    echo "skip codecs-test: needs ./build-deps.sh and ffmpeg"
fi
echo "Dreams runtime tests passed"
